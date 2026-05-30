#include "app_wifi_control.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "app_control.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/*
 * app_wifi_control.c — WIFI TCP 控制台实现
 *
 * 模块职责:
 *   1. Wi-Fi SoftAP 初始化与启动。
 *   2. TCP 服务器监听与客户端管理。
 *   3. 客户端认证（AUTH 命令）。
 *   4. 将 TCP 收到的文本行转发给 app_control_handle_line 处理。
 *
 * 系统架构视图:
 *
 *   PC/手机 (TCP Client)
 *       |
 *       | Wi-Fi (802.11)
 *       |
 *   ESP32 SoftAP (本模块)
 *       |
 *       | TCP socket (读取文本行)
 *       |
 *   app_tcp_control_task()        ← 本模块的 TCP 服务器任务
 *       |
 *       | app_control_handle_line() ← 复用 UART 的命令解析引擎
 *       |
 *   app_tx_* / app_stats_*       ← 实际的业务逻辑
 *
 * 网络栈初始化顺序:
 *   nvs_flash_init()          ← NVS（非易失存储），Wi-Fi 校准数据存储需要
 *   esp_netif_init()          ← TCP/IP 协议栈初始化
 *   esp_event_loop_create()   ← 事件循环（Wi-Fi 事件分发需要）
 *   esp_netif_create_wifi_ap()← 创建 AP 网络接口
 *   esp_wifi_init()           ← Wi-Fi 驱动初始化
 *   esp_wifi_set_mode(AP)     ← 设置为 AP 模式
 *   esp_wifi_set_config()     ← 设置 SSID/密码等参数
 *   esp_wifi_start()          ← 启动 Wi-Fi
 */

static const char *TAG = "nrf24_app";

/*
 * 编译期默认 SSID：
 *   RX 角色: "NRF24_RX"
 *   TX 角色: "NRF24_TX_<ID>" （如 NRF24_TX_1）
 * 这些值可在 menuconfig 中覆盖。
 */
#ifndef CONFIG_NRF24_CONTROL_WIFI_RX_SSID
#define CONFIG_NRF24_CONTROL_WIFI_RX_SSID "NRF24_RX"
#endif

#ifndef CONFIG_NRF24_CONTROL_WIFI_TX_PREFIX
#define CONFIG_NRF24_CONTROL_WIFI_TX_PREFIX "NRF24_TX"
#endif

#ifndef CONFIG_NRF24_CONTROL_WIFI_TX_ID
#define CONFIG_NRF24_CONTROL_WIFI_TX_ID 1
#endif

/* TCP 单行命令最大长度 */
#define APP_CONTROL_LINE_MAX 256

/* 最大同时 TCP 客户端数（SoftAP 资源有限，设为 1） */
#define APP_WIFI_CONTROL_MAX_CLIENTS 1

#if CONFIG_NRF24_CONTROL_WIFI_ENABLE

/* Wi-Fi AP 网络接口句柄，用于后续可能的动态配置 */
static esp_netif_t *s_wifi_ap_netif;

/*
 * TCP 通道的发送实现：通过 socket write 发送一行文本。
 *
 * 与 UART 通道的 printf 不同，这里使用 POSIX write() 写入 socket。
 * 注意：write 可能在 TCP 缓冲区满时阻塞，但在此应用中客户端
 * 读取速度通常远快于 NRF24 发送速度，不会成为瓶颈。
 *
 * 参数:
 *   user: 包含 socket fd（通过 intptr_t 转换），必须 >= 0。
 *   line: 待发送的文本行。
 */
static void app_control_send_socket(void *user, const char *line)
{
    if (user == NULL || line == NULL) {
        return;
    }

    int sock = (int)(intptr_t)user;
    if (sock < 0) {
        return;
    }

    size_t len = strlen(line);
    if (len > 0) {
        (void)write(sock, line, len);
    }
    (void)write(sock, "\n", 1);
}

/*
 * TCP 通道的回复发送：通过 app_control_io_t 接口发送一行。
 * 这是 app_control_send_socket 的简单封装。
 */
static void app_control_reply(const app_control_io_t *io, const char *line)
{
    if (io == NULL || io->send_line == NULL || line == NULL) {
        return;
    }

    io->send_line(io->user, line);
}

/*
 * 去除字符串左侧空白字符。返回第一个非空白字符的指针。
 */
static char *app_trim_left(char *s)
{
    while (*s != '\0' && isspace((int)(unsigned char)*s)) {
        ++s;
    }
    return s;
}

/*
 * 原地去除字符串右侧空白字符。
 */
static void app_trim_right(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((int)(unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

/*
 * 根据编译角色构建 Wi-Fi 热点 SSID。
 *
 * TX 角色: "<PREFIX>_<ID>" 例如 "NRF24_TX_1"
 *   这样当有多台 TX 设备时可以通过 ID 区分。
 *
 * RX 角色: 直接使用固定 SSID 如 "NRF24_RX"
 *   因为通常只有一台接收端。
 *
 * 参数:
 *   ssid:      输出 SSID 字符串缓冲区。
 *   ssid_size: 缓冲区大小（应 >= 32 字节以容纳 Wi-Fi SSID 上限）。
 */
static void app_build_wifi_ssid(char *ssid, size_t ssid_size)
{
    if (ssid == NULL || ssid_size == 0) {
        return;
    }

#if defined(CONFIG_NRF24_ROLE_TX)
    snprintf(ssid, ssid_size, "%s_%d", CONFIG_NRF24_CONTROL_WIFI_TX_PREFIX, CONFIG_NRF24_CONTROL_WIFI_TX_ID);
#else
    snprintf(ssid, ssid_size, "%s", CONFIG_NRF24_CONTROL_WIFI_RX_SSID);
#endif
}

/*
 * 从 TCP socket 读取一行（以 '\n' 结尾）。
 *
 * 这是 TCP 控制台的字符级读取函数。
 * 逐字节 read，直到遇到 '\n' 或缓冲区满或连接断开。
 *
 * 特殊处理:
 *   - '\r' (回车): 跳过不存储（兼容 Windows 风格的 \r\n 换行）。
 *   - '\n' (换行): 行结束标志，在末尾加 '\0' 后返回 true。
 *   - read 返回 0: 对端关闭连接，返回 false。
 *   - read 返回 < 0:
 *       - EINTR: 信号中断，继续读取。
 *       - EAGAIN/EWOULDBLOCK: socket 设置了超时，暂无数据，休眠 10ms 后继续。
 *       - 其他错误: 返回 false（通常是对端断开）。
 *
 * 参数:
 *   sock:     TCP 客户端 socket 描述符。
 *   buf:      行缓冲区。
 *   buf_size: 缓冲区大小。
 *
 * 返回: true=成功读取一行（以 '\0' 结尾存入 buf），false=连接断开或缓冲区不足。
 */
static bool app_socket_read_line(int sock, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 2) {
        return false;
    }

    size_t used = 0;
    while (used + 1 < buf_size) {
        char ch = 0;
        int ret = (int)read(sock, &ch, 1);

        if (ret == 0) {
            /* 对端正常关闭连接（发送了 FIN） */
            return false;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                /* 被信号中断，继续读取 */
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* socket 设置了接收超时，暂无数据，休眠后继续 */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            /* 其他不可恢复错误 */
            return false;
        }

        /* 跳过回车符（兼容 Windows \r\n） */
        if (ch == '\r') {
            continue;
        }
        /* 换行符：行结束 */
        if (ch == '\n') {
            buf[used] = '\0';
            return true;
        }

        buf[used++] = ch;
    }

    /* 缓冲区满（未遇到换行则强制截断） */
    buf[used] = '\0';
    return true;
}

/*
 * Wi-Fi SoftAP 初始化与启动。
 *
 * 初始化流程（按顺序，任一步失败都会触发 ESP_ERROR_CHECK 的 abort）:
 *   1. NVS 初始化：Wi-Fi 子系统需要 NVS 存储校准数据。
 *      如果 NVS 分区损坏或版本不匹配，先擦除再重新初始化。
 *   2. TCP/IP 协议栈初始化（esp_netif_init）。
 *   3. 创建默认事件循环（esp_event_loop_create_default）。
 *   4. 创建 Wi-Fi AP 网络接口（esp_netif_create_default_wifi_ap）。
 *   5. Wi-Fi 驱动初始化（esp_wifi_init）。
 *   6. 配置 AP 参数：SSID、密码、信道、加密模式、最大连接数等。
 *      如果密码长度 >= 8，使用 WPA2-PSK 加密；
 *      否则使用开放式网络（无密码）。
 *   7. 设置 Wi-Fi 为 AP 模式并启动。
 *
 * 注意: 此函数使用 static bool 标记保证只初始化一次。
 */
static void app_wifi_control_start_impl(void)
{
    static bool s_wifi_ready = false;
    if (s_wifi_ready) {
        return;
    }

    /*
     * 步骤1: NVS 初始化
     * Wi-Fi 的 PHY 校准数据存储在 NVS 分区中，
     * 如果分区空间不足或版本更新，需要先擦除再初始化。
     */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 步骤2-3: TCP/IP 协议栈与事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 步骤4: 创建 AP 网络接口 */
    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(s_wifi_ap_netif != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* 步骤5: Wi-Fi 驱动初始化（使用默认配置） */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    /* 步骤6: 配置 AP 参数 */
    wifi_config_t ap_config = {0};
    const char *password = CONFIG_NRF24_CONTROL_WIFI_PASSWORD;

    /* 构建 SSID 字符串 */
    char ssid[sizeof(ap_config.ap.ssid)] = {0};
    app_build_wifi_ssid(ssid, sizeof(ssid));
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    /* SSID 长度截断保护 */
    if (ssid_len > sizeof(ap_config.ap.ssid)) {
        ssid_len = sizeof(ap_config.ap.ssid);
    }
    memcpy(ap_config.ap.ssid, ssid, ssid_len);
    ap_config.ap.ssid_len = (uint8_t)ssid_len;

    /* 密码长度截断保护 */
    if (pass_len > sizeof(ap_config.ap.password) - 1) {
        pass_len = sizeof(ap_config.ap.password) - 1;
    }
    memcpy(ap_config.ap.password, password, pass_len);
    ap_config.ap.password[pass_len] = '\0';

    /* 信道: 1（2.412 GHz），与其他 Wi-Fi 网络共存最常用的信道 */
    ap_config.ap.channel = 1;

    /* 最大连接数: 1（仅支持单一控制终端） */
    ap_config.ap.max_connection = APP_WIFI_CONTROL_MAX_CLIENTS;

    /* Beacon 间隔: 100ms（标准值） */
    ap_config.ap.beacon_interval = 100;

    /*
     * 加密模式:
     *   密码 >= 8 字符: WPA2-PSK（安全加密）
     *   密码 < 8 字符:  开放网络（无加密，调试用）
     */
    ap_config.ap.authmode = pass_len >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* 不要求 PMF（Protected Management Frame，简化兼容性） */
    ap_config.ap.pmf_cfg.required = false;

    /* 步骤7: 设置 AP 模式并启动 Wi-Fi */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=%s password=%s TCP=%d token=%s",
             ssid,
             CONFIG_NRF24_CONTROL_WIFI_PASSWORD,
             CONFIG_NRF24_CONTROL_TCP_PORT,
             CONFIG_NRF24_CONTROL_TOKEN);

    s_wifi_ready = true;
}

/*
 * TCP 控制服务器任务。
 *
 * 任务生命周期:
 *   1. 创建 TCP 监听 socket（IPv4, SOCK_STREAM）。
 *   2. 设置 SO_REUSEADDR 以便快速重启。
 *   3. bind 到指定端口（menuconfig 中的 CONFIG_NRF24_CONTROL_TCP_PORT）。
 *   4. listen 等待连接（backlog=1，仅支持单客户端）。
 *   5. 循环 accept 客户端连接：
 *      a. 为每个客户端设置 1 秒接收超时。
 *      b. 发送欢迎消息和 AUTH 提示。
 *      c. 等待客户端发送 "AUTH <token>" 认证。
 *      d. 认证通过后进入命令循环：
 *         - 逐行读取 socket 数据。
 *         - 每行调用 app_control_handle_line 处理。
 *      e. 客户端断开或认证失败后关闭连接，回到 accept 等待下一个客户端。
 *
 * 连接管理:
 *   - 同一时间只有一个客户端处于活跃状态。
 *   - 如果新客户端在旧客户端断开前就尝试连接，
 *     TCP 协议栈会排队（backlog=1），旧客户端断开后自动 accept。
 */
static void app_tcp_control_task(void *arg)
{
    (void)arg;
    const int listen_port = CONFIG_NRF24_CONTROL_TCP_PORT;
    const char *token = CONFIG_NRF24_CONTROL_TOKEN;

    /* 创建 TCP socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "TCP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    /* 设置地址重用，避免重启后端口被 TIME_WAIT 占用 */
    int reuse = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 绑定地址和端口 */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "TCP bind failed port=%d", listen_port);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    /* 开始监听（backlog=1） */
    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP control server listening on %d", listen_port);

    /* 主循环：等待并处理客户端连接 */
    while (1) {
        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            continue;
        }

        /* 设置接收超时：1 秒。
         * 如果客户端静默 1 秒没有发送数据，
         * app_socket_read_line 中的 read 会返回 EAGAIN，触发 10ms 休眠重试。
         * 这保证了在没有数据时任务不会阻塞在 read 上。 */
        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        /* 构造 I/O 接口：使用 socket send_line 回调 */
        const app_control_io_t io = {
            .send_line = app_control_send_socket,
            .user = (void *)(intptr_t)client_fd,
        };

        /* 发送欢迎消息和认证提示 */
        app_control_reply(&io, "READY NRF24 TCP CONTROL");
        app_control_reply(&io, "AUTH required");

        bool authed = false;
        char line[APP_CONTROL_LINE_MAX] = {0};

        /*
         * 命令/认证循环:
         *   未认证时只能发送 AUTH 命令；
         *   认证通过后可以发送任意命令。
         */
        while (app_socket_read_line(client_fd, line, sizeof(line))) {
            char *cmd = line;
            cmd = app_trim_left(cmd);
            app_trim_right(cmd);

            /* 跳过空行 */
            if (*cmd == '\0') {
                continue;
            }

            /* 未认证：只接受 AUTH 命令 */
            if (!authed) {
                /*
                 * AUTH 命令格式: "AUTH <token>"
                 * 注意 strncmp 匹配 "AUTH " (5 字符，含空格)，
                 * 然后 strcmp 比较后续 token 字符串。
                 */
                if (strncmp(cmd, "AUTH ", 5) == 0 && strcmp(cmd + 5, token) == 0) {
                    authed = true;
                    app_control_reply(&io, "OK AUTH");
                    app_control_reply(&io, "READY type HELP for commands");
                } else {
                    app_control_reply(&io, "ERR auth");
                    break;
                }
                continue;
            }

            /* 已认证：转发给通用命令处理器 */
            app_control_handle_line(&io, cmd);
        }

        /* 客户端断开：先 shutdown 确保数据发送完毕，再关闭 socket */
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        ESP_LOGI(TAG, "TCP client disconnected");
    }
}
#endif

/*
 * 启动 Wi-Fi 控制台（对外接口）。
 *
 * 仅在 menuconfig 中启用了 CONFIG_NRF24_CONTROL_WIFI_ENABLE 时才生效。
 * 启动 SoftAP 并创建 TCP 服务器任务。
 */
void app_wifi_control_start(void)
{
#if CONFIG_NRF24_CONTROL_WIFI_ENABLE
    app_wifi_control_start_impl();
    xTaskCreate(app_tcp_control_task, "tcp_ctrl", 4096, NULL, 8, NULL);
#endif
}
