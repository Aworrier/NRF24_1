#include "app_proto.h"

#include <string.h>

/*
 * app_proto.c — 应用层自定义协议实现
 *
 * 模块职责：
 *   1. CRC16-CCITT 校验计算（app_crc16_ccitt）
 *   2. 发送侧帧序列化（app_proto_build_frame）：将帧对象编码为字节数组 + 附加 CRC
 *   3. 接收侧帧反序列化（app_proto_parse_frame）：校验魔数/版本/CRC，提取帧字段
 *   4. 调试辅助函数（app_proto_bytes_to_hex）：将二进制数据转为可读的十六进制字符串
 *
 * CRC16 算法选型说明：
 *   选用 CRC16-CCITT（多项式 0x1021），因为它是嵌入式通信中广泛使用的标准算法，
 *   对 32 字节以内的短帧具有良好的错误检测能力。
 *   注意：这不是加密/安全校验，仅用于检测无线传输中的比特错误。
 */

/*
 * 计算 CRC16-CCITT 校验值。
 *
 * 多项式: x^16 + x^12 + x^5 + 1 (0x1021)
 * 初始值: 0xFFFF
 * 输入数据逐字节处理，不进行反射（reflection），不异或最终值。
 *
 * 参数:
 *   data: 待校验数据缓冲区指针。
 *   len:  数据长度（字节）。
 *
 * 返回: 16 位 CRC 校验值。
 */
static uint16_t app_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;                      /* CRC 初始值为全 1 */
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;          /* 将当前字节异或到 CRC 高字节 */
        for (int b = 0; b < 8; ++b) {           /* 逐位处理 */
            if ((crc & 0x8000U) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U); /* 最高位为 1：左移后异或多项式 */
            } else {
                crc <<= 1;                              /* 最高位为 0：仅左移 */
            }
        }
    }
    return crc;
}

/*
 * 将字节数组格式化为大写十六进制字符串。
 *
 * 用途：将 NRF24 接收到的原始二进制载荷转为可打印的 hex 字符串，
 *       方便在串口日志或 TCP 控制台中显示。
 *
 * 参数:
 *   data:     原始字节数据指针（可为 NULL）。
 *   len:      数据长度。
 *   out:      输出字符串缓冲区。
 *   out_size: 输出缓冲区总大小（含末尾 '\0'）。
 *
 * 安全设计:
 *   - 如果 data 为 NULL 或 len 为 0，输出空字符串 ""。
 *   - 如果缓冲区不足以容纳全部 hex 字符，会截断到最大可容纳长度。
 *   - 始终保证以 '\0' 结尾。
 */
void app_proto_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (data == NULL || len == 0) {
        return;
    }

    /* 计算输出缓冲区能容纳的最大字节数。
     * 每字节需要 2 个 hex 字符，out_size-1 是因为末尾需要 '\0'。 */
    size_t max_bytes = (out_size - 1) / 2;
    if (len > max_bytes) {
        len = max_bytes;
    }

    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out[i * 2]     = hex[(b >> 4) & 0x0F];  /* 高 4 位 -> hex 字符 */
        out[i * 2 + 1] = hex[b & 0x0F];          /* 低 4 位 -> hex 字符 */
    }
    out[len * 2] = '\0';                          /* 末尾追加字符串终止符 */
}

/*
 * 帧序列化：将帧对象编码为可在空口发送的字节数组。
 *
 * 序列化流程:
 *   1. 校验参数有效性（缓冲区指针、最小长度）。
 *   2. 裁剪载荷长度到协议允许的最大值（APP_PROTO_MAX_USER_PAYLOAD）。
 *   3. 按帧格式依次写入：魔数(2) + 版本(1) + 序列号(2) + 载荷长度(1) + 标志(1) + 保留(1) + 载荷(变长)。
 *   4. 对帧头+载荷计算 CRC16，追加到帧尾（低字节在前）。
 *
 * 参数:
 *   out:      输出字节数组缓冲区。
 *   out_size: 输出缓冲区大小。
 *   in:       待编码的帧对象指针。
 *
 * 返回值:
 *   实际使用的字节数（帧头 + 载荷 + CRC）；参数无效时返回 0。
 */
size_t app_proto_build_frame(uint8_t *out, size_t out_size, const app_proto_frame_t *in)
{
    /* 参数有效性检查：缓冲区至少需要容纳帧头和 CRC */
    if (out == NULL || in == NULL || out_size < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        return 0;
    }

    /* 裁剪载荷长度：不超过协议最大载荷，也不超过输出缓冲区容量 */
    size_t max_payload_by_out = out_size - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE;
    uint8_t pl = in->payload_len > APP_PROTO_MAX_USER_PAYLOAD ? APP_PROTO_MAX_USER_PAYLOAD : in->payload_len;
    if (pl > max_payload_by_out) {
        pl = (uint8_t)max_payload_by_out;
    }
    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;

    /* 先清空整个输出缓冲区，保证未使用的字节为 0 */
    memset(out, 0, out_size);

    /* 按帧格式逐字段填充 */
    out[0] = APP_PROTO_MAGIC0;          /* 字节0: 同步魔数高位 */
    out[1] = APP_PROTO_MAGIC1;          /* 字节1: 同步魔数低位 */
    out[2] = APP_PROTO_VER;             /* 字节2: 协议版本 */
    out[3] = (uint8_t)(in->seq & 0xFF);       /* 字节3: 序列号低字节 */
    out[4] = (uint8_t)((in->seq >> 8) & 0xFF);/* 字节4: 序列号高字节（小端序） */
    out[5] = pl;                        /* 字节5: 载荷长度 */
    out[6] = in->flags;                 /* 字节6: 标志位 */
    out[7] = 0;                         /* 字节7: 保留字节 */

    /* 拷贝载荷数据 */
    if (pl > 0) {
        memcpy(&out[APP_PROTO_HEADER_SIZE], in->payload, pl);
    }

    /* 计算 CRC16（覆盖帧头 + 载荷），追加到帧尾，低字节在前 */
    uint16_t crc = app_crc16_ccitt(out, APP_PROTO_HEADER_SIZE + pl);
    out[APP_PROTO_HEADER_SIZE + pl]     = (uint8_t)(crc & 0xFF);
    out[APP_PROTO_HEADER_SIZE + pl + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return used;
}

/*
 * 帧反序列化：校验并解析接收到的字节数组。
 *
 * 校验流程（按顺序，一旦失败立即返回对应错误码）:
 *   1. 参数有效性检查（buf/out 非空，len 至少 >= 帧头+CRC）。
 *   2. 魔数+版本匹配：检查字节0==0xA5 && 字节1==0x5A && 字节2==0x01。
 *   3. 载荷长度字段合法性：不能超过协议最大载荷限制。
 *   4. 实际帧长度校验：缓冲区长度需 >= 帧头 + PL + CRC。
 *   5. CRC16 校验：重新计算帧头+载荷的 CRC，与帧尾的 CRC 字段比对。
 *   6. 上述全部通过后，提取各字段到输出结构体。
 *
 * 参数:
 *   buf: 接收到的原始字节数组。
 *   len: 缓冲区有效字节数。
 *   out: 解析成功后的帧对象输出。
 *
 * 返回值:
 *   APP_PROTO_PARSE_OK:       帧有效，out 已填充。
 *   APP_PROTO_PARSE_ERR_LEN:  长度不足。
 *   APP_PROTO_PARSE_ERR_MAGIC:魔数或版本不匹配。
 *   APP_PROTO_PARSE_ERR_CRC:  CRC 校验失败。
 */
app_proto_parse_result_t app_proto_parse_frame(const uint8_t *buf, size_t len, app_proto_frame_t *out)
{
    /* 步骤1: 参数有效性检查 */
    if (buf == NULL || out == NULL || len < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    /* 步骤2: 魔数和版本校验
     * 接收端必须严格匹配魔数和版本号，否则说明：
     *   - 不是本协议的帧（魔数不匹配）
     *   - 发送端使用了不同版本的协议（版本不匹配） */
    if (buf[0] != APP_PROTO_MAGIC0 || buf[1] != APP_PROTO_MAGIC1 || buf[2] != APP_PROTO_VER) {
        return APP_PROTO_PARSE_ERR_MAGIC;
    }

    /* 步骤3: 载荷长度字段合法性检查 */
    uint8_t pl = buf[5];
    if (pl > APP_PROTO_MAX_USER_PAYLOAD) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    /* 步骤4: 实际帧长度校验
     * used = 帧头(8) + 载荷(pl) + CRC(2)，必须 <= 缓冲区的 len */
    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;
    if (used > len) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    /* 步骤5: CRC 校验
     * 从帧尾读取发送端附加的 CRC 值（低字节在前），
     * 与重新计算的 CRC 比对，不一致说明数据在传输中损坏 */
    uint16_t crc_rx = (uint16_t)buf[APP_PROTO_HEADER_SIZE + pl] |
                      ((uint16_t)buf[APP_PROTO_HEADER_SIZE + pl + 1] << 8);
    uint16_t crc_calc = app_crc16_ccitt(buf, APP_PROTO_HEADER_SIZE + pl);
    if (crc_rx != crc_calc) {
        return APP_PROTO_PARSE_ERR_CRC;
    }

    /* 步骤6: 所有校验通过，提取字段 */
    out->seq = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);  /* 序列号：低字节在前 */
    out->payload_len = pl;
    out->flags = buf[6];
    if (pl > 0) {
        memcpy(out->payload, &buf[APP_PROTO_HEADER_SIZE], pl);
    }
    return APP_PROTO_PARSE_OK;
}
