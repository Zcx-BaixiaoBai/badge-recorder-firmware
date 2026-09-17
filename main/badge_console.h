// main/badge_console.h —— USB-Serial-JTAG 控制台（开发/测试驱动入口）。
//
// 动机：熄屏录音入口在设置菜单里需要物理按键，自动化端到端验证无法按键。
// 固件已启 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG（COM 口），在此挂 esp_console
// REPL，提供录音/电量/校时/上传等命令——测试与调试均可从 PC 直驱。
#pragma once

#include "esp_err.h"

// 初始化并启动 REPL（幂等）。在 app_main 末尾调用。
esp_err_t badge_console_start(void);
