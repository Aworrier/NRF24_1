#include "app_control.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_stats.h"
#include "app_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* UART command parser and STAT/HELP responses. */

/* Send a single line to UART output. */
static void app_control_send_uart(void *user, const char *line)
{
    (void)user;
    if (line == NULL) {
        return;
    }
    printf("%s\n", line);
}

/* Dispatch a pre-formatted response line. */
static void app_control_reply(const app_control_io_t *io, const char *line)
{
    if (io == NULL || io->send_line == NULL || line == NULL) {
        return;
    }

    io->send_line(io->user, line);
}

/* Format and dispatch a response line. */
static void app_control_replyf(const app_control_io_t *io, const char *fmt, ...)
{
    if (io == NULL || io->send_line == NULL || fmt == NULL) {
        return;
    }

    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    app_control_reply(io, line);
}

/* Trim leading whitespace in-place and return the first non-space. */
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

/* Parse hex text into a byte payload. */
static bool app_parse_hex_payload(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t hex_len = strlen(hex);
    if ((hex_len == 0) || (hex_len % 2 != 0) || (hex_len / 2 > 32)) {
        return false;
    }

    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; ++i) {
        char hi = (char)toupper((int)(unsigned char)hex[i * 2]);
        char lo = (char)toupper((int)(unsigned char)hex[i * 2 + 1]);
        if (!isxdigit((int)hi) || !isxdigit((int)lo)) {
            return false;
        }
        uint8_t v_hi = (uint8_t)(hi <= '9' ? hi - '0' : hi - 'A' + 10);
        uint8_t v_lo = (uint8_t)(lo <= '9' ? lo - '0' : lo - 'A' + 10);
        out[i] = (uint8_t)((v_hi << 4) | v_lo);
    }

    *out_len = n;
    return true;
}

/* Parse next unsigned integer token and advance cursor. */
static bool app_parse_u32_token(char **p, uint32_t *out)
{
    char *s = app_trim_left(*p);
    if (*s == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }

    *out = (uint32_t)v;
    *p = end;
    return true;
}

/* Case-insensitive token compare. */
static bool app_token_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (toupper((int)(unsigned char)*a) != toupper((int)(unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Emit STAT line based on current role and counters. */
static void app_reply_stats(const app_control_io_t *io)
{
#if defined(CONFIG_NRF24_ROLE_TX)
    app_mac_mode_t mode = APP_MAC_ALOHA;
    uint8_t q = 0;
    uint32_t slot_ms = 0;
    uint32_t csma_win = 0;
    app_tx_get_mac_config(&mode, &q);
    app_tx_get_slot_params(&slot_ms, &csma_win);

    const app_tx_stats_t *tx = app_stats_tx_get();
    app_control_replyf(io, "STAT role=TX enabled=%d mac=%s q=%u slot_ms=%lu csma_win=%lu slot_limit=%lu queued=%lu sent=%lu ack_ok=%lu ack_fail=%lu retries_sum=%lu retries_max=%lu next_seq=%u",
                       app_tx_is_enabled() ? 1 : 0,
                       app_tx_mac_mode_name(mode),
                       (unsigned)q,
                       (unsigned long)slot_ms,
                       (unsigned long)csma_win,
                       (unsigned long)app_tx_get_slot_limit(),
                       (unsigned long)tx->burst_queued,
                       (unsigned long)tx->frame_sent,
                       (unsigned long)tx->tx_ok,
                       (unsigned long)tx->tx_fail,
                       (unsigned long)tx->retries_sum,
                       (unsigned long)tx->retries_max,
                       (unsigned)tx->next_seq);
#else
    const app_rx_stats_t *rx = app_stats_rx_get();
    app_control_replyf(io, "STAT role=RX rx_pkt=%lu frame_ok=%lu crc_fail=%lu magic_fail=%lu len_fail=%lu dup=%lu ooo=%lu gap=%lu last_seq=%u",
                       (unsigned long)rx->rx_packets,
                       (unsigned long)rx->frame_ok,
                       (unsigned long)rx->crc_fail,
                       (unsigned long)rx->magic_fail,
                       (unsigned long)rx->len_fail,
                       (unsigned long)rx->seq_dup,
                       (unsigned long)rx->seq_out_of_order,
                       (unsigned long)rx->seq_gap,
                       (unsigned)rx->last_seq);
#endif
}

/* Parse and execute one control command line. */
void app_control_handle_line(const app_control_io_t *io, char *line)
{
    app_trim_right(line);
    char *cmd = app_trim_left(line);
    if (*cmd == '\0') {
        return;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        app_reply_stats(io);
        return;
    }

    if (strcmp(cmd, "RESETSTATS") == 0) {
        app_stats_reset();
        app_control_reply(io, "OK RESET");
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
#if defined(CONFIG_NRF24_ROLE_TX)
        app_control_reply(io, "CMD: ENABLE <0|1>, MAC <ALOHA|CSMA> <q_percent>, SLOT <slot_ms> <csma_window>, SLOTLIMIT <max_slots>, BURST <count> <interval_ms> <ascii>, BURSTHEX <count> <interval_ms> <hex>, STOP, STATUS, RESETSTATS");
#else
        app_control_reply(io, "CMD: STATUS, RESETSTATS");
#endif
        return;
    }

#if defined(CONFIG_NRF24_ROLE_TX)
    if (strncmp(cmd, "ENABLE", 6) == 0) {
        char *p = cmd + 6;
        uint32_t enabled = 0;
        if (!app_parse_u32_token(&p, &enabled) || (enabled > 1)) {
            app_control_reply(io, "ERR usage: ENABLE <0|1>");
            return;
        }
        app_tx_set_enabled(enabled == 1);
        app_control_reply(io, enabled ? "OK ENABLED" : "OK DISABLED");
        return;
    }

    if (strncmp(cmd, "MAC", 3) == 0) {
        char *p = app_trim_left(cmd + 3);
        if (*p == '\0') {
            app_mac_mode_t mode = APP_MAC_ALOHA;
            uint8_t q = 0;
            app_tx_get_mac_config(&mode, &q);
            app_control_replyf(io, "OK MAC mode=%s q=%u", app_tx_mac_mode_name(mode), (unsigned)q);
            return;
        }

        char mode_token[12] = {0};
        size_t idx = 0;
        while (*p != '\0' && !isspace((int)(unsigned char)*p) && idx + 1 < sizeof(mode_token)) {
            mode_token[idx++] = *p++;
        }
        mode_token[idx] = '\0';
        p = app_trim_left(p);

        app_mac_mode_t mode = APP_MAC_ALOHA;
        if (app_token_eq(mode_token, "ALOHA")) {
            mode = APP_MAC_ALOHA;
        } else if (app_token_eq(mode_token, "CSMA")) {
            mode = APP_MAC_CSMA;
        } else {
            app_control_reply(io, "ERR usage: MAC <ALOHA|CSMA> <q_percent>");
            return;
        }

        uint8_t current_q = 0;
        app_tx_get_mac_config(NULL, &current_q);

        if (*p != '\0') {
            uint32_t q = 0;
            if (!app_parse_u32_token(&p, &q) || q > 100) {
                app_control_reply(io, "ERR q_percent must be 0..100");
                return;
            }
            app_tx_set_mac_config(mode, (uint8_t)q);
        } else {
            app_tx_set_mac_config(mode, current_q);
        }

        {
            uint8_t q = 0;
            app_tx_get_mac_config(&mode, &q);
            app_control_replyf(io, "OK MAC mode=%s q=%u", app_tx_mac_mode_name(mode), (unsigned)q);
        }
        return;
    }

    if (strncmp(cmd, "SLOTLIMIT", 9) == 0) {
        char *p = app_trim_left(cmd + 9);
        if (*p == '\0') {
            app_control_replyf(io, "OK SLOTLIMIT %lu", (unsigned long)app_tx_get_slot_limit());
            return;
        }

        uint32_t limit = 0;
        if (!app_parse_u32_token(&p, &limit)) {
            app_control_reply(io, "ERR slot_limit must be >= 0");
            return;
        }
        app_tx_set_slot_limit(limit);
        app_control_replyf(io, "OK SLOTLIMIT %lu", (unsigned long)app_tx_get_slot_limit());
        return;
    }

    if (strncmp(cmd, "SLOT", 4) == 0) {
        char *p = app_trim_left(cmd + 4);
        if (*p == '\0') {
            uint32_t slot_ms = 0;
            uint32_t csma_win = 0;
            app_tx_get_slot_params(&slot_ms, &csma_win);
            app_control_replyf(io, "OK SLOT ms=%lu csma_win=%lu", (unsigned long)slot_ms, (unsigned long)csma_win);
            return;
        }

        uint32_t slot_ms = 0;
        uint32_t win = 0;
        if (!app_parse_u32_token(&p, &slot_ms) || slot_ms == 0) {
            app_control_reply(io, "ERR slot_ms must be >= 1");
            return;
        }
        if (!app_parse_u32_token(&p, &win) || win == 0) {
            app_control_reply(io, "ERR csma_window must be >= 1");
            return;
        }

        app_tx_set_slot_params(slot_ms, win);
        app_control_replyf(io, "OK SLOT ms=%lu csma_win=%lu", (unsigned long)slot_ms, (unsigned long)win);
        return;
    }

    if (strcmp(cmd, "STOP") == 0) {
        app_tx_abort();
        app_control_reply(io, "OK STOPPED");
        return;
    }

    bool is_hex = false;
    if (strncmp(cmd, "BURSTHEX", 8) == 0) {
        is_hex = true;
        cmd += 8;
    } else if (strncmp(cmd, "BURST", 5) == 0) {
        cmd += 5;
    } else {
        app_control_reply(io, "ERR unknown command");
        return;
    }

    uint32_t count = 0;
    uint32_t interval_ms = 0;
    if (!app_parse_u32_token(&cmd, &count) || count == 0) {
        app_control_reply(io, "ERR invalid count");
        return;
    }
    if (!app_parse_u32_token(&cmd, &interval_ms)) {
        app_control_reply(io, "ERR invalid interval_ms");
        return;
    }

    char *payload = app_trim_left(cmd);
    if (*payload == '\0') {
        app_control_reply(io, "ERR empty payload");
        return;
    }

    uint8_t data[32] = {0};
    size_t data_len = 0;
    if (is_hex) {
        if (!app_parse_hex_payload(payload, data, &data_len)) {
            app_control_reply(io, "ERR invalid hex payload");
            return;
        }
    } else {
        size_t src_len = strlen(payload);
        data_len = src_len < sizeof(data) ? src_len : sizeof(data);
        memcpy(data, payload, data_len);
    }

    if (!app_tx_submit_burst(count, interval_ms, data, data_len)) {
        app_control_reply(io, "ERR queue full");
        return;
    }

    app_control_reply(io, "OK queued");
#else
    app_control_reply(io, "ERR RX role: only STATUS/RESETSTATS supported");
#endif
}

/* UART console task: read lines and dispatch commands. */
static void app_uart_cmd_task(void *arg)
{
    (void)arg;
    const app_control_io_t io = {
        .send_line = app_control_send_uart,
        .user = NULL,
    };

    char line[192] = {0};
    app_control_reply(&io, "READY type HELP for commands");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        app_control_handle_line(&io, line);
    }
}

/* Start the UART command task. */
void app_control_start_uart(void)
{
    xTaskCreate(app_uart_cmd_task, "uart_cmd", 4096, NULL, 9, NULL);
}
