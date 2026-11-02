# SPDX-License-Identifier: Apache-2.0
"""Exercise Tab5's actual early power setup without hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Tab5PowerTest(unittest.TestCase):
    def test_radio_power_and_failure_paths(self):
        source = (ROOT / "components/muse/boards/board_m5stack_tab5.c").read_text()
        start = source.index("#define RF_EXTERNAL")
        end = source.index("static lv_display_t *display_start")
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int esp_io_expander_handle_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define IO_EXPANDER_PIN_NUM_0 (1u << 0)
#define IO_EXPANDER_PIN_NUM_5 (1u << 5)
#define IO_EXPANDER_PIN_NUM_7 (1u << 7)
#define IO_EXPANDER_OUTPUT 1
#define IO_EXPANDER_OUTPUT_MODE_PUSH_PULL 1
#define BSP_FEATURE_WIFI 5
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { (void)(tag); int err = (expr); if (err) return err; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, tag, ...) do { (void)(tag); if (!(expr)) return err; } while (0)
static int failure, calls, radio, waited;
static uint32_t levels[3], dirs[3], push_pull[3];
static int step(void) { return ++calls == failure ? ESP_FAIL : ESP_OK; }
static int bsp_i2c_init(void) { return step(); }
static int bsp_io_expander_init(void) { return 1; }
static int bsp_io_expander1_init(void) { return 2; }
static int esp_io_expander_set_level(int io, uint32_t mask, int high) {
    int err = step();
    if (!err) levels[io] = (levels[io] & ~mask) | (high ? mask : 0);
    return err;
}
static int esp_io_expander_set_dir(int io, uint32_t mask, int direction) {
    assert(direction == IO_EXPANDER_OUTPUT);
    int err = step(); if (!err) dirs[io] |= mask; return err;
}
static int esp_io_expander_set_output_mode(int io, uint32_t mask, int mode) {
    assert(mode == IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    int err = step(); if (!err) push_pull[io] |= mask; return err;
}
static int bsp_feature_enable(int feature, bool enable) {
    assert(feature == BSP_FEATURE_WIFI && enable);
    assert(!(levels[1] & 1) && (dirs[1] & 1) && (push_pull[1] & 1));
    assert((levels[2] & 0xa0) == 0xa0);
    assert((dirs[2] & 0xa0) == 0xa0 && (push_pull[2] & 0xa0) == 0xa0);
    int err = step(); if (!err) radio++; return err;
}
static void vTaskDelay(int ticks) { assert(radio == 1 && ticks >= 100); waited++; }
'''
        code += source[start:end]
        code += r'''
int main(void) {
    /* Preserve unrelated outputs, including the shutdown pulse and USB power. */
    levels[1] = levels[2] = 0xff;
    assert(early_init() == ESP_OK && radio == 1 && waited == 1);
    assert(levels[1] == 0xfe && levels[2] == 0xff);
    assert(dirs[1] == 1 && dirs[2] == 0xa0);
    int total = calls;
    for (failure = 1; failure <= total; failure++) {
        calls = radio = waited = 0;
        assert(early_init() == ESP_FAIL);
        assert(calls == failure && radio == 0 && waited == 0);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "tab5.c", Path(tmp) / "tab5"
            src.write_text(code)
            cc = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_early_hook_finishes_before_link_starts(self):
        main = (ROOT / "main/main.c").read_text()
        self.assertLess(main.index("muse_glue_start();"), main.index("app_run();"))
        glue = (ROOT / "main/muse_glue.c").read_text().split("void muse_glue_start(void)", 1)[1]
        self.assertLess(glue.index("muse_input_power_init();"), glue.index("board->early_init()"))
        self.assertLess(glue.index("ESP_ERROR_CHECK(board->early_init());"), glue.index("xTaskCreatePinnedToCore(boot_task"))
