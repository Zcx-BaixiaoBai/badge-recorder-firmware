// main/badge_console.c —— USB-Serial-JTAG 命令行实现（自研，零外部依赖）。
//
// 不用 esp_console REPL：其依赖 linenoise/argtable3，IDF 5.5 已移出核心且
// registry 无 espressif/linenoise。这里直接装 USB-Serial-JTAG 驱动读行——
// PC 侧用 pyserial 发命令即可（无需行编辑/历史），printf 输出走同一控制台。
//
// 命令：help / rec-start / rec-stop / rec-status / batt / time / kick / pending / reboot
#include "badge_console.h"

#include "bsp_battery.h"
#include "frec_store.h"
#include "rec_mode.h"
#include "rec_upload.h"
#include "time_sync.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "console";

#define RX_BUF   256
#define LINE_MAX 64

typedef struct {
    const char *name;
    const char *help;
    int (*fn)(int argc, char **argv);
} cmd_t;

static int cmd_rec_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t e = rec_mode_start();
    printf("rec_start: %s%s\n", e == ESP_OK ? "OK" : "FAIL",
           e == ESP_OK ? "" : rec_mode_last_error());
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
    printf("recording=%d pending_upload=%d mounted=%d heap=%uB\n",
           rec_mode_active(), rec_upload_pending(), frec_store_is_mounted(),
           (unsigned)esp_get_free_heap_size());
    if (rec_mode_last_error()[0]) printf("last_err=%s\n", rec_mode_last_error());
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

static const cmd_t CMDS[] = {
    { "rec-start",  "进入熄屏录音模式",        cmd_rec_start },
    { "rec-stop",   "结束录音并亮屏",          cmd_rec_stop },
    { "rec-status", "录音/上传/存储/内存状态", cmd_rec_status },
    { "batt",       "CW2017 电量/电压",        cmd_batt },
    { "time",       "当前墙钟毫秒（0=未校时）", cmd_time },
    { "kick",       "通知上传任务排空待传段",  cmd_kick },
    { "reboot",     "重启设备",                cmd_reboot },
};

static void dispatch(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char *argv[8];
    int argc = 0;
    char *save = NULL;
    for (char *p = strtok_r(line, " ", &save); p && argc < 8; p = strtok_r(NULL, " ", &save)) {
        argv[argc++] = p;
    }
    if (argc == 0) return;
    if (strcmp(argv[0], "help") == 0) {
        for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
            printf("%-12s %s\n", CMDS[i].name, CMDS[i].help);
        }
        return;
    }
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (strcmp(argv[0], CMDS[i].name) == 0) {
            CMDS[i].fn(argc, argv);
            fflush(stdout);
            return;
        }
    }
    printf("unknown: %s（help 看命令表）\n", argv[0]);
    fflush(stdout);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    size_t n = 0;
    uint8_t ch;
    for (;;) {
        int r = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (r <= 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (n > 0) {
                line[n] = '\0';
                dispatch(line);
                n = 0;
            }
            continue;
        }
        if (ch == 0x08 || ch == 0x7F) {          // 退格
            if (n > 0) n--;
            continue;
        }
        if (n + 1 < sizeof(line)) line[n++] = (char)ch;
    }
}

esp_err_t badge_console_start(void)
{
    static bool s_started;
    if (s_started) return ESP_OK;

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_BUF,
        .tx_buffer_size = 256,
    };
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG 驱动安装失败: %s", esp_err_to_name(e));
        return e;
    }
    if (xTaskCreate(console_task, "badge_cli", 4096, NULL, 4, NULL) != pdPASS) {
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    printf("\nbadge> 命令行就绪（help 查看命令表）\n");
    fflush(stdout);
    return ESP_OK;
}
