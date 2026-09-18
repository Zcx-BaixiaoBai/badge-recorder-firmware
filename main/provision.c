// main/provision.c —— SoftAP 网页配网（首次使用 / 改配置）
//
// 工牌开一个开放热点 Badge-Recorder-Setup，手机/电脑连上后浏览器打开
// http://192.168.4.1 填 WiFi + 录音服务器地址/令牌，提交后写 NVS 并重启进 STA。
// 配网是独立的一次启动模式（app_main 开机判定），避免 AP/STA 在同一次启动里混初始化。
#include "provision.h"
#include "badge_cfg.h"
#include "ui_badge.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "provision";
#define AP_SSID "Badge-Recorder-Setup"

static const char *HTML_FORM =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>工牌录音 配网</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#0f1419;color:#e6edf3;"
    "margin:0;padding:24px}h2{color:#2f81f7}h3{color:#4ade80;margin:20px 0 6px}"
    "label{display:block;margin:14px 0 4px;color:#8b98a5;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:10px;"
    "border:1px solid #2a3542;border-radius:8px;background:#1a222c;color:#e6edf3;"
    "font-size:16px}button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;"
    "background:#2f81f7;color:#fff;font-size:16px;font-weight:600}"
    ".tip{color:#6e7c8c;font-size:13px;line-height:1.6}"
    ".feat{color:#e6edf3;font-size:14px;line-height:1.8;margin:8px 0}"
    ".warn{color:#fbbf24;font-size:13px;margin:12px 0;padding:10px;"
    "border:1px solid #fbbf24;border-radius:6px;background:rgba(251,191,36,.08)}</style>"
    "<h2>工牌录音 配网</h2>"
    "<h3>这是什么？</h3>"
    "<div class=feat>这是一台<b>静默录音工牌</b>。佩戴后自动录下你一整天"
    "的对话和会议，音频自动上传到你的服务器进行分析：</div>"
    "<div class=feat>· 每日自动生成<b>会议纪要</b>（谁说了什么、议题、决议）<br>"
    "· 每日自动生成<b>工作日报</b>（做了什么、待办、需要注意的话）<br>"
    "· 支持<b>声纹识别</b>（自动区分不同说话人）<br>"
    "· 全文<b>搜索</b>历史对话</div>"
    "<h3>怎么用？</h3>"
    "<div class=tip><b>长按上键</b> = 开始录音（30 分钟一节）<br>"
    "<b>长按下键</b> = 开始连续录音（每 30 分钟一节自动续录）<br>"
    "<b>长按 OK</b> = 结束录音 / 进设置<br>"
    "录音中短按任意键 = 亮屏看状态（10 秒后自动熄屏省电）</div>"
    "<div class=warn>· 连续录音模式下每 30 分钟有约 1 分钟同步间隔"
    "（上传已录音频），期间不录音<br>"
    "· 录音无任何指示（不发光、不发声），佩戴者无感知<br>"
    "· 他人对话的录音与分析需遵守当地法律法规</div>"
    "<h3>服务器地址</h3>"
    "<div class=tip>录音数据上传到你部署的 recorder-server（"
    "github.com/Zcx-BaixiaoBai/badge-recorder-firmware 仓库 README 有部署教程）。"
    "填运行 recorder-server 那台主机的地址。</div>"
    "<h3>填写 WiFi 和服务器信息</h3>"
    "<form method=post action=/save>"
    "<label>WiFi 名称（仅 2.4G）</label><input name=ssid required autocomplete=off>"
    "<label>WiFi 密码</label><input name=pass type=password autocomplete=off>"
    "<label>服务器地址</label><input name=gw_url required placeholder='http://192.168.1.20:8787' autocomplete=off>"
    "<p class=tip>= 运行 recorder-server 主机的 IP + :8787。"
    "电脑 cmd 输入 ipconfig 查看（192.168.x.x / 10.x.x.x）。</p>"
    "<label>访问令牌</label><input name=token autocomplete=off>"
    "<p class=tip>recorder-server 配置文件里的 token。</p>"
    "<label style='color:#e6edf3'><input type=checkbox name=mute value=1 style='width:auto'> 静音（不播报语音，仅显示文字）</label>"
    "<button>保存并重启工牌</button></form>";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {