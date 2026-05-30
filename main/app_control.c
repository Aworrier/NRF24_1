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

/*
 * app_control.c — UART 命令控制台实现
 *
 * 模块职责：
 *   1. 提供 UART 串口命令行解析（app_control_handle_line）。
 *   2. 提供 STAT/HELP 等查询命令的响应格式化。
 *   3. 提供 BURST/BURSTHEX 等发送命令的参数解析和执行。
 *
 * 命令控制流:
 *   用户输入一行文本（以 \n 结尾）
 *     → app_control_handle_line() 解析命令词
 *     → 匹配具体命令（strcmp/strncmp）
 *     → 解析参数（app_parse_u32_token / app_parse_hex_payload）
 *     → 调用对应的 app_tx_* / app_stats_* API
 *     → 通过 app_control_io_t 通道回复 OK/ERR/STAT 结果
 *
 * 通道抽象:
 *   app_control_io_t 将"发送响应"与"解析命令"解耦。
 *   当前有两个消费者：
 *     - app_control_start_uart()（本文件）：字符设备 UART。
 *     - app_tcp_control_task()（app_wifi_control.c）：TCP socket。
 *   两套通道共享同一个 app_control_handle_line() 解析引擎。
 */

/*
 * UART 通道的发送实现：直接 printf 到 stdout。
 * 注意 user 参数在 UART 模式下未使用。
 */
static void app_control_send_uart(void *user, const char *line)
{
    (void)user;
    if (line == NULL) {
        return;
    }
    printf("%s\n", line);
}

/*
 * 通过 I/O 通道发送一行固定字符串。
 * 简单的转发函数，用于不需要格式化的响应（如 "OK"、"ERR"）。
 */
static void app_control_reply(const app_control_io_t *io, const char *line)
{
    if (io == NULL || io->send_line == NULL || line == NULL) {
        return;
    }

    io->send_line(io->user, line);
}

/*
 * 通过 I/O 通道发送格式化字符串。
 * 内部使用 vsnprintf 格式化，缓冲区 256 字节，超长截断。
 * 用于需要嵌入动态数值的响应（如 STAT 数据）。
 */
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

/*
 * 去除字符串左侧（开头的）空白字符。
 * 返回第一个非空白字符的指针（不会修改原字符串内容）。
 *
 * 空白字符定义：isspace() 为 true 的字符（空格、\t、\r、\n 等）。
 */
static char *app_trim_left(char *s)
{
    while (*s != '\0' && isspace((int)(unsigned char)*s)) {
        ++s;
    }
    return s;
}

/*
 * 原地去除字符串右侧（末尾的）空白字符。
 * 通过将末尾的空白字符替换为 '\0' 实现截断。
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
 * 将 hex 文本解析为字节数组。
 *
 * 解析规则:
 *   - hex 字符串长度必须为偶数且不为 0。
 *   - 每两个 hex 字符合成一个字节，最大支持 32 字节（NRF24 载荷上限）。
 *   - 字符必须为 0-9 或 A-F（大小写均可）。
 *
 * 参数:
 *   hex:     输入 hex 字符串（以 '\0' 结尾）。
 *   out:     输出字节数组。
 *   out_len: 输出实际字节数。
 *
 * 返回: true=成功, false=格式错误。
 */
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

/*
 * 从字符串中解析一个无符号整数 token 并推进游标。
 *
 * 这是命令参数解析的核心工具函数。
 * 流程: 跳过空白 → strtoul 解析数字 → 更新游标到下一个 token 的开头。
 *
 * 参数:
 *   p:   字符串游标指针的指针（既是输入也是输出）。
 *        - 输入：指向待解析字符串中的当前解析位置。
 *        - 输出：指向解析完数字后的下一个字符位置（可能是空白或 '\0'）。
 *   out: 输出解析结果。
 *
 * 返回: true=解析成功, false=未找到合法数字。
 */
static bool app_parse_u32_token(char **p, uint32_t *out)
{
    char *s = app_trim_left(*p);
    if (*s == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        /* strtoul 没有消费任何字符，说明不是数字 */
        return false;
    }

    *out = (uint32_t)v;
    *p = end;
    return true;
}

/*
 * 大小写不敏感的 token 比较。
 *
 * 用于命令词匹配（如 "ALOHA" vs "aloha" vs "Aloha" 均认为相等）。
 * 注意：只在两个 token 都完全消费完时才返回 true。
 *
 * 参数:
 *   a, b: 待比较的两个字符串（均需以 '\0' 结尾）。
 *
 * 返回: true=相等（忽略大小写）, false=不相等。
 */
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

/*
 * 生成并发送 STATUS 查询响应。
 *
 * 根据编译角色（TX/RX）输出不同格式的统计信息行：
 *   TX: STAT role=TX enabled=... mac=... q=... sent=... ack_ok=... ack_fail=... retries=...
 *   RX: STAT role=RX rx_pkt=... frame_ok=... crc_fail=... dup=... gap=...
 *
 * 各字段含义详见 app_stats.h 中的结构体注释。
 */
static void app_reply_stats(const app_control_io_t *io)
{
#if defined(CONFIG_NRF24_ROLE_TX)
    /* TX 角色：输出发送统计 */
    app_mac_mode_t mode = APP_MAC_ALOHA;
    uint8_t q = 0;
    uint32_t slot_ms = 0;
    uint32_t csma_win = 0;
    app_tx_get_mac_config(&mode, &q);
    app_tx_get_slot_params(&slot_ms, &csma_win);

    const app_tx_stats_t *tx = app_stats_tx_get();
    app_control_replyf(io,
        "STAT role=TX enabled=%d mac=%s q=%u slot_ms=%lu csma_win=%lu slot_limit=%lu "
        "queued=%lu sent=%lu ack_ok=%lu ack_fail=%lu retries_sum=%lu retries_max=%lu next_seq=%u",
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
    /* RX 角色：输出接收统计 */
    const app_rx_stats_t *rx = app_stats_rx_get();
    app_control_replyf(io,
        "STAT role=RX rx_pkt=%lu frame_ok=%lu crc_fail=%lu magic_fail=%lu "
        "len_fail=%lu dup=%lu ooo=%lu gap=%lu last_seq=%u",
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

/*
 * 命令解析主函数：匹配命令词并分发到对应处理器。
 *
 * 命令处理流程:
 *   1. 去除行尾空白（app_trim_right）。
 *   2. 跳过行首空白（app_trim_left），得到纯命令文本。
 *   3. 空行直接返回（不做任何响应）。
 *   4. 依次匹配各命令词，进入对应的处理分支。
 *   5. 未知命令回复 "ERR unknown command"。
 *
 * 命令参数解析采用"游标推进"模式：
 *   每个命令处理函数从 cmd 指针处开始读取参数，
 *   使用 app_parse_u32_token 逐 token 解析并推进指针。
 *
 * 安全性:
 *   - 所有整数参数使用 strtoul（而非 atoi），可以正确处理溢出和非数字输入。
 *   - 输出缓冲区使用 snprintf，防止栈溢出。
 *   - 输入字符串在原地修改（trim），不需要额外内存分配。
 */
void app_control_handle_line(const app_control_io_t *io, char *line)
{
    /* 步骤1-2: 去除前后空白 */
    app_trim_right(line);
    char *cmd = app_trim_left(line);
    if (*cmd == '\0') {
        return;
    }

    /* 命令: STATUS — 查询运行统计 */
    if (strcmp(cmd, "STATUS") == 0) {
        app_reply_stats(io);
        return;
    }

    /* 命令: RESETSTATS — 重置统计计数器 */
    if (strcmp(cmd, "RESETSTATS") == 0) {
        app_stats_reset();
        app_control_reply(io, "OK RESET");
        return;
    }

    /* 命令: HELP — 打印命令列表 */
    if (strcmp(cmd, "HELP") == 0) {
#if defined(CONFIG_NRF24_ROLE_TX)
        app_control_reply(io,
            "CMD: ENABLE <0|1>, "
            "MAC <ALOHA|CSMA> <q_percent>, "
            "SLOT <slot_ms> <csma_window>, "
            "SLOTLIMIT <max_slots>, "
            "BURST <count> <interval_ms> <ascii>, "
            "BURSTHEX <count> <interval_ms> <hex>, "
            "STOP, "
            "STATUS, "
            "RESETSTATS");
#else
        app_control_reply(io, "CMD: STATUS, RESETSTATS");
#endif
        return;
    }

    /*
     * 以下命令仅 TX 角色支持。
     * RX 角色在此处收到未知命令后会走到末尾的 "ERR unknown command"。
     */
#if defined(CONFIG_NRF24_ROLE_TX)
    /* 命令: ENABLE <0|1> — 启用或禁用 TX 发送 */
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

    /* 命令: MAC [ALOHA|CSMA] <q_percent> — 配置 MAC 协议和概率门限 */
    if (strncmp(cmd, "MAC", 3) == 0) {
        char *p = app_trim_left(cmd + 3);

        /* 无参数：查询当前 MAC 配置 */
        if (*p == '\0') {
            app_mac_mode_t mode = APP_MAC_ALOHA;
            uint8_t q = 0;
            app_tx_get_mac_config(&mode, &q);
            app_control_replyf(io, "OK MAC mode=%s q=%u", app_tx_mac_mode_name(mode), (unsigned)q);
            return;
        }

        /* 解析 MAC 模式 token（ALOHA 或 CSMA） */
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

        /* 获取当前的 q 值作为默认 */
        uint8_t current_q = 0;
        app_tx_get_mac_config(NULL, &current_q);

        /* 解析 q 参数（可选，不提供则保持原值） */
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

    /* 命令: SLOTLIMIT <max_slots> — 设置/查询发送总时隙上限 */
    if (strncmp(cmd, "SLOTLIMIT", 9) == 0) {
        char *p = app_trim_left(cmd + 9);
        if (*p == '\0') {
            /* 无参数：查询当前值 */
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

    /* 命令: SLOT <slot_ms> <csma_window> — 设置/查询时隙参数 */
    if (strncmp(cmd, "SLOT", 4) == 0) {
        char *p = app_trim_left(cmd + 4);
        if (*p == '\0') {
            /* 无参数：查询当前时隙设置 */
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

    /* 命令: STOP — 立即中止当前 burst 发送 */
    if (strcmp(cmd, "STOP") == 0) {
        app_tx_abort();
        app_control_reply(io, "OK STOPPED");
        return;
    }

    /*
     * 命令: BURST / BURSTHEX — 提交批量发送任务
     *
     * BURST    <count> <interval_ms> <ascii_payload>
     * BURSTHEX <count> <interval_ms> <hex_payload>
     *
     * count:       帧数（发送多少帧）。
     * interval_ms: 帧间间隔（毫秒），在第一帧发出后开始计时。
     * payload:     载荷数据，BURST 是 ASCII 文本，BURSTHEX 是十六进制。
     */
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

    /* 解析载荷数据 */
    uint8_t data[32] = {0};
    size_t data_len = 0;
    if (is_hex) {
        /* Hex 模式：两个 hex 字符 → 一个字节 */
        if (!app_parse_hex_payload(payload, data, &data_len)) {
            app_control_reply(io, "ERR invalid hex payload");
            return;
        }
    } else {
        /* ASCII 模式：直接拷贝字符串到载荷缓冲区 */
        size_t src_len = strlen(payload);
        data_len = src_len < sizeof(data) ? src_len : sizeof(data);
        memcpy(data, payload, data_len);
    }

    /* 提交 burst 到 TX 任务队列 */
    if (!app_tx_submit_burst(count, interval_ms, data, data_len)) {
        app_control_reply(io, "ERR queue full");
        return;
    }

    app_control_reply(io, "OK queued");
#else
    /* RX 角色：仅支持 STATUS 和 RESETSTATS */
    app_control_reply(io, "ERR RX role: only STATUS/RESETSTATS supported");
#endif
}

/*
 * UART 控制台任务主函数。
 *
 * 任务行为:
 *   1. 初始化 UART 通道的 I/O 接口。
 *   2. 发送 "READY" 欢迎消息。
 *   3. 循环读取 stdin（fgets 阻塞读取），每读一行调用 app_control_handle_line。
 *
 * 限制:
 *   - 单行最大 191 字符（缓冲区 192 字节）。
 *   - 读取失败时短暂休眠 20ms，避免忙等。
 */
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
            /* 读取失败（可能是 EOF 或临时错误），休眠后重试 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        app_control_handle_line(&io, line);
    }
}

/*
 * 启动 UART 命令控制台任务。
 *
 * 直接在 FreeRTOS 中创建一个独立任务，使用 ESP-IDF 的 stdin/stdout
 * （通常通过 USB 串口/JTAG 连接到开发主机）。
 *
 * 任务参数:
 *   - 入口函数: app_uart_cmd_task
 *   - 任务名:   "uart_cmd"（用于调试和堆栈追踪）
 *   - 栈大小:   4096 字节
 *   - 优先级:   9（高于大多数系统任务）
 */
void app_control_start_uart(void)
{
    xTaskCreate(app_uart_cmd_task, "uart_cmd", 4096, NULL, 9, NULL);
}
