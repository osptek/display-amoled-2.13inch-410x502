#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lv_adapter.h"
#include "lv_demos.h"

#include "esp_lcd_st7801.h"
#include "st7801_init_cmds.h"
#include "esp_lcd_touch_cst820.h"

/* LCD size */
#define EXAMPLE_LCD_H_RES   (410)
#define EXAMPLE_LCD_V_RES   (502)

#if LV_COLOR_DEPTH == 16
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_FMT_RGB565)
#define BSP_LCD_COLOR_DEPTH (16)
#elif LV_COLOR_DEPTH == 24
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_FMT_RGB888)
#define BSP_LCD_COLOR_DEPTH (24)
#endif

/* “VDD_MIPI_DPHY”应供电 2.5V，可从内部 LDO 稳压器或外部 LDO 芯片获取电源 */
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN 3 // LDO_VO3 连接至 VDD_MIPI_DPHY
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define EXAMPLE_PIN_NUM_LCD_RST  -1

/* Touch settings */
#define EXAMPLE_TOUCH_I2C_NUM       (0)
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (400000)

/* LCD touch pins */
#define EXAMPLE_TOUCH_I2C_SCL       (GPIO_NUM_8)
#define EXAMPLE_TOUCH_I2C_SDA       (GPIO_NUM_7)
#define EXAMPLE_TOUCH_RST           (GPIO_NUM_NC)
#define EXAMPLE_TOUCH_INT           (GPIO_NUM_NC)

/* 旋转与防撕裂（MIPI 默认 TRIPLE_PARTIAL） */
#define EXAMPLE_LV_ROTATION         ESP_LV_ADAPTER_ROTATE_0
#define EXAMPLE_TEAR_AVOID_MODE     ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI

static const char *TAG = "EXAMPLE";

/* LCD IO and panel */
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

static void example_bsp_enable_dsi_phy_power(void)
{
    // 打开 MIPI DSI PHY 的电源，使其从“无电”状态进入“关机”状态
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif
}

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    example_bsp_enable_dsi_phy_power();

    // 根据防撕裂模式与旋转计算所需帧缓冲数量
    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
        EXAMPLE_TEAR_AVOID_MODE, EXAMPLE_LV_ROTATION);

    // 首先创建 MIPI DSI 总线，它还将初始化 DSI PHY
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 1,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = 420,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "LCD init failed");

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // 我们使用DBI接口发送LCD命令和参数
    esp_lcd_dbi_io_config_t dbi_config = ST7801_PANEL_IO_DBI_CONFIG();

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle), err, TAG, "LCD init failed");

    // 创建ST7801控制面板
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 15,
        .virtual_channel = 0,
        .in_color_format = MIPI_DPI_PX_FORMAT,
        .num_fbs = num_fbs,
        .video_timing = {
            .h_size = EXAMPLE_LCD_H_RES,
            .v_size = EXAMPLE_LCD_V_RES,
            .hsync_back_porch = 20,
            .hsync_pulse_width = 4,
            .hsync_front_porch = 20,
            .vsync_back_porch = 20,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 20,
        },
    };

    st7801_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_DEPTH,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7801(io_handle, &panel_config, &lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_st7801_set_brightness(lcd_panel, 80), err, TAG, "AMOLED init failed");

    return ret;

err:
    if (lcd_panel) {
        esp_lcd_panel_del(lcd_panel);
        lcd_panel = NULL;
    }
    return ret;
}

static esp_err_t app_touch_init(void)
{
    /* Initialize I2C */
    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
        .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_TOUCH_RST,
        .int_gpio_num = EXAMPLE_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    return esp_lcd_touch_new_i2c_cst820(tp_io_handle, &tp_cfg, &touch_handle);
}

static esp_err_t app_lvgl_init(void)
{
    /* 初始化 esp_lvgl_adapter */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    // 如需更大栈或把栈放 PSRAM，可在此修改，例如：
    // adapter_cfg.task_stack_size = 16 * 1024;
    // adapter_cfg.stack_in_psram = true;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_cfg), TAG, "esp_lv_adapter_init failed");

    /* 注册 MIPI DSI 显示 */
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        lcd_panel,
        io_handle,
        EXAMPLE_LCD_H_RES,
        EXAMPLE_LCD_V_RES,
        EXAMPLE_LV_ROTATION
    );
    // 如需自定义 buffer_height / use_psram 等，可在宏展开后修改：
    // disp_cfg.profile.buffer_height = 100;
    // disp_cfg.profile.use_psram = true;
    // disp_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL; // 与 EXAMPLE_TEAR_AVOID_MODE 保持一致

    lvgl_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "esp_lv_adapter_register_display failed");
        return ESP_FAIL;
    }

    /* 注册触摸 */
    if (touch_handle) {
        esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lvgl_disp, touch_handle);
        lvgl_touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
        if (lvgl_touch_indev == NULL) {
            ESP_LOGW(TAG, "esp_lv_adapter_register_touch failed");
        }
    }

    /* 启动 adapter 任务 */
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "esp_lv_adapter_start failed");

    return ESP_OK;
}

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());

    /* Touch initialization */
    ESP_ERROR_CHECK(app_touch_init());

    /* LVGL / adapter initialization */
    ESP_ERROR_CHECK(app_lvgl_init());

    /* Show LVGL objects（必须在 lock 保护下调用 LVGL API） */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        // lv_demo_music();
        lv_demo_widgets();
        esp_lv_adapter_unlock();
    }
}