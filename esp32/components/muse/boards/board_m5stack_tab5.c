/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * M5Stack Tab5: ESP32-P4, 16 MB flash, 32 MB PSRAM; C6 Wi-Fi/BLE over SDIO.
 * Espressif's m5stack_tab5_noglib BSP 1.3.2 owns the 720x1280 DSI LCD (ILI9881C,
 * ST7123 or ST7121, auto-detected), touch, ES8388 speaker and ES7210 mics:
 * https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5
 * Expander pins and power sequencing follow M5Unified's Power_Class.inl
 * (board_M5Tab5) and M5Tab5-UserDemo's platforms/tab5/components/m5stack_tab5:
 * https://github.com/m5stack/M5Unified
 * https://github.com/m5stack/M5Tab5-UserDemo
 * RESET/BOOT belongs to the board's power controller, not a user GPIO.
 * Pairing and push-to-talk use the touchscreen; PWR keeps its hardware role.
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

#define RF_EXTERNAL IO_EXPANDER_PIN_NUM_0 /* 0x43: low selects the internal antenna */
#define CHARGE_QC_N IO_EXPANDER_PIN_NUM_5 /* 0x44: high disables Quick Charge */
#define CHARGE_EN IO_EXPANDER_PIN_NUM_7  /* 0x44: high enables charging */

static const char *TAG = "board";

static esp_err_t early_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    esp_io_expander_handle_t io = bsp_io_expander_init();
    esp_io_expander_handle_t io1 = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(io && io1, ESP_FAIL, TAG, "io expanders");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, RF_EXTERNAL, 0), TAG, "internal antenna");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, RF_EXTERNAL, IO_EXPANDER_OUTPUT), TAG, "antenna direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io, RF_EXTERNAL, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),
                        TAG, "antenna output");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io1, CHARGE_QC_N | CHARGE_EN, 1), TAG, "charger enable");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io1, CHARGE_QC_N | CHARGE_EN, IO_EXPANDER_OUTPUT),
                        TAG, "charger direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io1, CHARGE_QC_N | CHARGE_EN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),
                        TAG, "charger output");
    /* Link requests the C6's STA MAC before Muse's normal init() runs. */
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG, "C6 power");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_core_id = MUSE_UI_CORE;
    cfg.task_priority = MUSE_UI_PRIORITY;
    cfg.task_stack_size = 32 * 1024;
    if (esp_lv_adapter_init(&cfg) != ESP_OK) {
        return NULL;
    }
    /* The BSP uses 965 Mbps for ST7121, below the other panels' 1 Gbps.
     * It still covers the 70 MHz RGB565 pixel clock with two DSI lanes. */
    const bsp_display_config_t lcd_cfg = { .dsi_bus = { .lane_bit_rate_mbps = 965 } };
    bsp_lcd_handles_t lcd;
    if (bsp_display_new_with_handles(&lcd_cfg, &lcd) != ESP_OK
        || esp_lcd_panel_disp_on_off(lcd.panel, true) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t dc = {
        .panel = lcd.panel,
        .panel_io = lcd.io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&dc);
    const bsp_touch_config_t tc = { 0 };
    esp_lcd_touch_handle_t tp;
    if (!disp || bsp_touch_new(&tc, &tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t ic = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&ic);
    return *touch && esp_lv_adapter_start() == ESP_OK ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Use Muse's duplex, two-slot format instead of the BSP's 48 kHz mono. */
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "duplex audio init");
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void audio_power(bool on)
{
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_SPEAKER, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker power: %s", esp_err_to_name(err));
    }
}

static unsigned poll_buttons(void)
{
    return 0; /* The on-screen mic provides talk edges. */
}

static const muse_board_t s_board = {
    .name = "M5Stack Tab5",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 5.0f,
    .talk_button = "screen microphone",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -40 },
    .frame_ms = 40,
    .early_init = early_init,
    .init = bsp_i2c_init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    /* Backlight-only sleep preserves touch to wake this buttonless board. */
    .audio_init = audio_init,
    .mic_slot = -1,
    .audio_power = audio_power,
    .poll_buttons = poll_buttons,
    /* No software shutdown until the expander's power-off pulse is verified. */
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
