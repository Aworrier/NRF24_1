#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * app_proto.h — 应用层自定义通信协议定义
 *
 * 协议分层视角：
 *   - 本文件定义了 NRF24 空口载荷之上的一层简单成帧协议。
 *   - 底层驱动（nrf24.c）负责将原始字节通过 SPI 写入 NRF24 芯片并通过空口发送；
 *     本协议层则在载荷中加入了帧头（魔数+版本）、序列号、CRC 校验等字段，
 *     使上层能够检测帧边界、校验数据完整性、追踪丢包/乱序。
 *
 * 帧格式（最大 APP_PROTO_MAX_FRAME_SIZE 字节）：
 *   +--------+--------+--------+------+------+------+-------+------+---------+------+------+
 *   | MAGIC0 | MAGIC1 |  VER   | SEQ_L| SEQ_H| PL_LEN| FLAGS | RSV  | PAYLOAD | CRC_L| CRC_H|
 *   |  0xA5  |  0x5A  |  0x01  | low  | high |       |       |  0   | 变长    | low  | high |
 *   +--------+--------+--------+------+------+------+-------+------+---------+------+------+
 *     字节0    字节1    字节2    字节3  字节4  字节5   字节6   字节7  8..8+PL-1  ...   ...
 *   |<------------ 帧头 APP_PROTO_HEADER_SIZE(8) ------------>|<- 载荷 ->|<- CRC(2) ->|
 *
 * 字段说明：
 *   MAGIC0/MAGIC1: 帧同步魔数 (0xA5 0x5A)，用于在字节流中定位帧起始边界。
 *   VER:           协议版本号，接收端校验版本是否匹配。
 *   SEQ (16bit):   帧序列号，低字节在前（小端序），用于检测丢包/重复/乱序。
 *   PL_LEN:        实际用户载荷长度 (0..APP_PROTO_MAX_USER_PAYLOAD)。
 *   FLAGS:         标志位，预留用于控制信息（如 ACK 请求、紧急标记等）。
 *   RSV:           保留字节，当前固定为 0。
 *   PAYLOAD:       用户载荷数据，长度由 PL_LEN 字段指定。
 *   CRC (16bit):   CRC16-CCITT 校验值，覆盖帧头+载荷，低字节在前。
 *
 * 常量说明：
 *   APP_PROTO_MAX_FRAME_SIZE = 32: 与 NRF24 最大载荷长度对齐。
 *   APP_PROTO_MAX_USER_PAYLOAD = 22: 每帧可承载的最大用户数据量。
 *     （计算公式: 32 - 8(帧头) - 2(CRC) = 22）
 *
 * 新手阅读建议：
 *   1) 先看宏定义和常量，了解帧格式的硬约束。
 *   2) 再看 app_proto_frame_t 结构体，这是应用层操作的帧对象。
 *   3) 然后看 app_proto_build_frame / app_proto_parse_frame 的函数声明。
 *   4) 最后结合 app_proto.c 的实现理解 CRC 计算和序列化细节。
 */

/* 帧同步魔数：用于在字节流中定位帧起始边界。 */
#define APP_PROTO_MAGIC0 0xA5
#define APP_PROTO_MAGIC1 0x5A

/* 当前协议版本号。接收端会校验版本是否匹配，不匹配则丢弃。 */
#define APP_PROTO_VER 0x01

/* 帧头固定长度（字节）：2(魔数) + 1(版本) + 2(序列号) + 1(载荷长度) + 1(标志) + 1(保留) */
#define APP_PROTO_HEADER_SIZE 8

/* CRC 校验长度（字节）：CRC16-CCITT 占 2 字节 */
#define APP_PROTO_CRC_SIZE 2

/* 单帧最大总长度：与 NRF24 硬件载荷上限对齐（32 字节） */
#define APP_PROTO_MAX_FRAME_SIZE 32

/*
 * 每帧可承载的最大用户数据量。
 * 计算: 32(总长) - 8(帧头) - 2(CRC) = 22 字节。
 * 上层应用发送超过此长度的数据需要自行分包。
 */
#define APP_PROTO_MAX_USER_PAYLOAD (APP_PROTO_MAX_FRAME_SIZE - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE)

/*
 * 应用层协议帧对象。
 *
 * 这是上层代码（TX/RX/控制台）操作帧时的统一数据结构。
 * 在发送侧：上层填充 seq/flags/payload 字段，调用 app_proto_build_frame 序列化为字节数组。
 * 在接收侧：app_proto_parse_frame 将字节数组反序列化为此结构体。
 */
typedef struct {
    uint16_t seq;       /* 帧序列号，用于丢包/乱序检测 */
    uint8_t flags;      /* 标志位字段，预留 */
    uint8_t payload_len;/* 实际载荷长度（字节） */
    uint8_t payload[APP_PROTO_MAX_USER_PAYLOAD]; /* 用户载荷数据缓冲区 */
} app_proto_frame_t;

/*
 * 帧解析结果枚举。
 *
 * 接收端调用 app_proto_parse_frame 后，根据返回值判断帧是否有效。
 * 无效原因分为三类：长度不足、魔数/版本不匹配、CRC 校验失败。
 */
typedef enum {
    APP_PROTO_PARSE_OK = 0,      /* 解析成功，帧完整且校验通过 */
    APP_PROTO_PARSE_ERR_LEN,     /* 长度不足：缓冲区太小或载荷长度字段异常 */
    APP_PROTO_PARSE_ERR_MAGIC,   /* 魔数/版本不匹配：非本协议帧或版本不一致 */
    APP_PROTO_PARSE_ERR_CRC,     /* CRC 校验失败：数据在传输中损坏 */
} app_proto_parse_result_t;

/*
 * 将帧对象序列化为字节数组（发送侧调用）。
 *
 * 参数:
 *   out:      输出缓冲区，存放序列化后的完整帧字节。
 *   out_size: 输出缓冲区大小（字节），必须 >= APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE。
 *   in:       待序列化的帧对象指针。
 *
 * 返回值:
 *   实际写入的字节数（帧头 + 载荷 + CRC），如果参数无效或缓冲区不足则返回 0。
 *
 * 注意: 该函数会自动裁剪 payload_len 到 APP_PROTO_MAX_USER_PAYLOAD 以内。
 */
size_t app_proto_build_frame(uint8_t *out, size_t out_size, const app_proto_frame_t *in);

/*
 * 从字节数组解析帧（接收侧调用）。
 *
 * 参数:
 *   buf: 原始接收字节缓冲区。
 *   len: 缓冲区有效字节数。
 *   out: 输出帧对象指针，解析成功后填充。
 *
 * 返回值:
 *   APP_PROTO_PARSE_OK: 解析成功，out 中为有效帧数据。
 *   其他值：解析失败原因。
 *
 * 校验流程: 长度检查 -> 魔数/版本校验 -> CRC16 校验 -> 提取字段。
 */
app_proto_parse_result_t app_proto_parse_frame(const uint8_t *buf, size_t len, app_proto_frame_t *out);

/*
 * 将字节数组格式化为十六进制字符串（调试/日志用）。
 *
 * 参数:
 *   data:     原始字节数组。
 *   len:      字节数。
 *   out:      输出的十六进制字符串缓冲区。
 *   out_size: 输出缓冲区大小（字节），包含末尾 '\0'。
 *
 * 输出格式: 大写十六进制，每字节两个字符，如 "A55A01000000"。
 * 如果缓冲区不足以容纳全部数据，会截断到可容纳的最大长度。
 */
void app_proto_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size);
