#pragma once

#include <stddef.h>

/*
 * app_control.h — 命令控制台接口
 *
 * 模块职责:
 *   提供统一的文本命令解析与分发框架，支持两种物理传输通道：
 *     1. UART 串口控制台（stdin/stdout）
 *     2. WIFI TCP 控制台（socket）
 *
 * 设计思路：
 *   本模块通过 app_control_io_t 抽象了"发送响应行"这一操作，
 *   使得 app_control_handle_line() 命令解析逻辑与物理传输通道解耦。
 *   无论是 UART 还是 TCP，命令处理逻辑完全相同。
 *
 * 支持的命令（TX 角色）:
 *   STATUS     - 查询当前运行统计
 *   RESETSTATS - 重置所有统计计数器
 *   ENABLE     - 启用/禁用 TX 发送
 *   MAC        - 设置/查询 MAC 协议模式（ALOHA/CSMA）和概率门限 q
 *   SLOT       - 设置/查询时隙参数（时隙长度、CSMA 退避窗口）
 *   SLOTLIMIT  - 设置/查询发送总时隙上限
 *   BURST      - 提交 ASCII 载荷的批量发送任务
 *   BURSTHEX   - 提交十六进制载荷的批量发送任务
 *   STOP       - 中止当前发送任务
 *   HELP       - 打印命令列表
 *
 * 支持的命令（RX 角色）:
 *   STATUS     - 查询当前运行统计
 *   RESETSTATS - 重置所有统计计数器
 *   HELP       - 打印命令列表
 */

/*
 * 发送一行响应文本的回调函数类型。
 *
 * 参数:
 *   user: 调用者自定义上下文指针（UART 模式下为 NULL，TCP 模式下为 socket fd）。
 *   line: 待发送的字符串（不含换行符，回调内部负责追加）。
 *
 * 不同通道的实现:
 *   UART:  printf("%s\n", line)
 *   TCP:   write(sock, line, len) + write(sock, "\n", 1)
 */
typedef void (*app_control_send_fn_t)(void *user, const char *line);

/*
 * 控制台 I/O 抽象结构体。
 *
 * 将"发送响应"操作抽象为回调，使得同一套命令解析逻辑
 * 可以同时服务于 UART 控制台和 TCP 控制台。
 */
typedef struct {
    app_control_send_fn_t send_line; /* 发送一行的回调函数指针 */
    void *user;                       /* 回调函数的用户上下文参数 */
} app_control_io_t;

/*
 * 启动 UART 串口控制台任务。
 *
 * 创建一个 FreeRTOS 任务，循环读取 stdin (fgets)，
 * 将每一行文本传递给 app_control_handle_line 处理。
 *
 * 任务栈大小: 4096 字节
 * 任务优先级: 9
 */
void app_control_start_uart(void);

/*
 * 解析并执行一行命令文本。
 *
 * 这是控制台的核心处理函数。它接收一行原始文本，
 * 执行命令匹配、参数解析和动作分发。
 *
 * 参数:
 *   io:   控制台 I/O 接口（用于发送响应）。
 *   line: 原始命令行文本（会被原地修改，trim 处理）。
 *
 * 命令匹配逻辑（按优先级）:
 *   1. "STATUS"          → 查询统计并格式化输出
 *   2. "RESETSTATS"      → 清空统计计数器
 *   3. "HELP"            → 打印命令帮助
 *   4. "ENABLE <0|1>"    → 启用/禁用 TX
 *   5. "MAC [ALOHA|CSMA] <q>" → MAC 协议配置
 *   6. "SLOTLIMIT <n>"   → 设置时隙上限
 *   7. "SLOT <ms> <win>" → 设置时隙参数
 *   8. "STOP"            → 中止当前 burst
 *   9. "BURST <count> <interval_ms> <ascii_payload>" → ASCII 发送
 *   10. "BURSTHEX <count> <interval_ms> <hex>"        → Hex 发送
 */
void app_control_handle_line(const app_control_io_t *io, char *line);
