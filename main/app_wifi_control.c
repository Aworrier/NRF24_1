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
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* SoftAP + TCP control server, forwards lines to app_control. */

static const char *TAG = "nrf24_app";

#ifndef CONFIG_NRF24_CONTROL_WIFI_RX_SSID
#define CONFIG_NRF24_CONTROL_WIFI_RX_SSID "NRF24_RX"
#endif

#ifndef CONFIG_NRF24_CONTROL_WIFI_TX_PREFIX
#define CONFIG_NRF24_CONTROL_WIFI_TX_PREFIX "NRF24_TX"
#endif

#ifndef CONFIG_NRF24_CONTROL_WIFI_TX_ID
#define CONFIG_NRF24_CONTROL_WIFI_TX_ID 1
#endif

#define APP_CONTROL_LINE_MAX 256
#define APP_WIFI_CONTROL_MAX_CLIENTS 1

#if CONFIG_NRF24_CONTROL_WIFI_ENABLE
static esp_netif_t *s_wifi_ap_netif;

/* Send a line to the TCP client socket. */
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

/* Dispatch a pre-formatted line to the client. */
static void app_control_reply(const app_control_io_t *io, const char *line)
{
    if (io == NULL || io->send_line == NULL || line == NULL) {
        return;
    }

    io->send_line(io->user, line);
}

/* Trim leading whitespace in-place and return pointer. */
static char *app_trim_left(char *s)
{
    while (*s != '\0' && isspace((int)(unsigned char)*s)) {
        ++s;
    }
    return s;
}

/* Trim trailing whitespace in-place. */
static void app_trim_right(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((int)(unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

/* Build AP SSID based on role and menuconfig. */
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

/* Read a \n-terminated line from the socket. */
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
            return false;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            return false;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            buf[used] = '\0';
            return true;
        }

        buf[used++] = ch;
    }

    buf[used] = '\0';
    return true;
}

/* Initialize NVS/Wi-Fi and start SoftAP. */
static void app_wifi_control_start_impl(void)
{
    static bool s_wifi_ready = false;
    if (s_wifi_ready) {
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(s_wifi_ap_netif != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    wifi_config_t ap_config = {0};
    const char *password = CONFIG_NRF24_CONTROL_WIFI_PASSWORD;
    char ssid[sizeof(ap_config.ap.ssid)] = {0};
    app_build_wifi_ssid(ssid, sizeof(ssid));
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    if (ssid_len > sizeof(ap_config.ap.ssid)) {
        ssid_len = sizeof(ap_config.ap.ssid);
    }
    memcpy(ap_config.ap.ssid, ssid, ssid_len);
    ap_config.ap.ssid_len = (uint8_t)ssid_len;

    if (pass_len > sizeof(ap_config.ap.password) - 1) {
        pass_len = sizeof(ap_config.ap.password) - 1;
    }
    memcpy(ap_config.ap.password, password, pass_len);
    ap_config.ap.password[pass_len] = '\0';

    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = APP_WIFI_CONTROL_MAX_CLIENTS;
    ap_config.ap.beacon_interval = 100;
    ap_config.ap.authmode = pass_len >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

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

/* TCP server task: accept client, auth, and forward commands. */
static void app_tcp_control_task(void *arg)
{
    (void)arg;
    const int listen_port = CONFIG_NRF24_CONTROL_TCP_PORT;
    const char *token = CONFIG_NRF24_CONTROL_TOKEN;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "TCP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP control server listening on %d", listen_port);

    while (1) {
        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            continue;
        }

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const app_control_io_t io = {
            .send_line = app_control_send_socket,
            .user = (void *)(intptr_t)client_fd,
        };

        app_control_reply(&io, "READY NRF24 TCP CONTROL");
        app_control_reply(&io, "AUTH required");

        bool authed = false;
        char line[APP_CONTROL_LINE_MAX] = {0};

        while (app_socket_read_line(client_fd, line, sizeof(line))) {
            char *cmd = line;
            cmd = app_trim_left(cmd);
            app_trim_right(cmd);
            if (*cmd == '\0') {
                continue;
            }

            if (!authed) {
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

            app_control_handle_line(&io, cmd);
        }

        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        ESP_LOGI(TAG, "TCP client disconnected");
    }
}
#endif

/* Start Wi-Fi control if enabled in menuconfig. */
void app_wifi_control_start(void)
{
#if CONFIG_NRF24_CONTROL_WIFI_ENABLE
    app_wifi_control_start_impl();
    xTaskCreate(app_tcp_control_task, "tcp_ctrl", 4096, NULL, 8, NULL);
#endif
}
