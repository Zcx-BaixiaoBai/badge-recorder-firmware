// main/badge_console.c —— USB-Serial-JTAG 控制台实现。
//
// 命令集（录音产品线驱动 + 调试）：
//   rec-start / rec-stop / rec-status   熄屏录音控制与状态
//   batt                                CW2017 电量/电压
//   time                                当前墙钟毫秒（未校时=0）
//   kick                                通知上传任务（排空待传段）
//   pending                             待上传段数
//   reboot                              重启
#include "badge_console.h"

#include "bsp_battery.h"
#include "frec_store.h"
#include "rec_mode.h"
#include "rec_upload.h"
#include "time_sync.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "console";

static int cmd_rec_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t e = rec_mode_start();
    if (e == ESP_OK) {
        printf("rec_start: OK\n");
    } else {
        printf("rec_start: FAIL %s\n", rec_mode_last_error());
    }
    return 0;
}

static int cmd_rec_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    rec_mode_stop();
    printf("rec_stop: done%s\n", rec_mode_last_error()[0] ? "（有错误，见日志）" : "");
    return 0;
}

static int cmd_rec_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("recording=%d pending_upload=%d last_err=%s mounted=%d\n",
           rec_mode_active(), rec_upload_pending(),
           rec_mode_last_error()[0] ? rec_mode_last_error() : "-",
           frec_store_is_mounted());
    printf("heap=%u B\n", (unsigned)esp_get_free_heap_size());
    return 0;
}

static int cmd_batt(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("batt: %d%% (%dmV)\n", bsp_battery_soc(), bsp_battery_mv());
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("now_ms: %llu\n", (unsigned long long)time_now_ms());
    return 0;
}

static int cmd_kick(int argc, char **argv)
{
    (void)argc; (void)argv;
    rec_upload_kick();
    printf("upload kicked (pending=%d)\n", rec_upload_pending());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

esp_err_t badge_console_start(void)
{
    static bool s_started;
    if (s_started) return ESP_OK;

    esp_console_repl_config_t repl = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl.prompt = "badge> ";
    repl.max_history_len = 8;

    esp_console_cmd_t cmds[] = {
        { .command = "rec-start", .help = "进入熄屏录音模式", .hint = NULL, .func = cmd_rec_start },
        { .command = "rec-stop", .help = "结束录音并亮屏", .hint = NULL, .func = cmd_rec_stop },
        { .command = "rec-status", .help = "录音/上传/存储状态", .hint = NULL, .func = cmd_rec_status },
        { .command = "batt", .help = "CW2017 电量/电压", .hint = NULL, .func = cmd_batt },
        { .command = "time", .help = "当前墙钟毫秒（0=未校时）", .hint = NULL, .func = cmd_time },
        { .command = "kick", .help = "通知上传任务排空待传段", .hint = NULL, .func = cmd_kick },
        { .command = "reboot", .help = "重启设备", .hint = NULL, .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_repl_t *repl_if = NULL;
    esp_err_t e = esp_console_new_repl_usb_serial_jtag(&repl, &repl_if);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "REPL 创建失败: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_console_start_repl(repl_if);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "REPL 启动失败: %s", esp_err_to_name(e));
        return e;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB 控制台就绪（rec-start/rec-stop/rec-status/batt/time/kick/reboot）");
    return ESP_OK;
}
