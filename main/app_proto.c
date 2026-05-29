#include "app_proto.h"

#include <string.h>

/* Frame build/parse helpers and CRC16. */

/* Compute CRC16-CCITT over a byte buffer. */
static uint16_t app_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if ((crc & 0x8000U) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Format bytes into an uppercase hex string. */
void app_proto_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (data == NULL || len == 0) {
        return;
    }

    size_t max_bytes = (out_size - 1) / 2;
    if (len > max_bytes) {
        len = max_bytes;
    }

    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out[i * 2] = hex[(b >> 4) & 0x0F];
        out[i * 2 + 1] = hex[b & 0x0F];
    }
    out[len * 2] = '\0';
}

/* Serialize a frame and append CRC. */
size_t app_proto_build_frame(uint8_t *out, size_t out_size, const app_proto_frame_t *in)
{
    if (out == NULL || in == NULL || out_size < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        return 0;
    }

    size_t max_payload_by_out = out_size - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE;
    uint8_t pl = in->payload_len > APP_PROTO_MAX_USER_PAYLOAD ? APP_PROTO_MAX_USER_PAYLOAD : in->payload_len;
    if (pl > max_payload_by_out) {
        pl = (uint8_t)max_payload_by_out;
    }
    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;

    memset(out, 0, out_size);
    out[0] = APP_PROTO_MAGIC0;
    out[1] = APP_PROTO_MAGIC1;
    out[2] = APP_PROTO_VER;
    out[3] = (uint8_t)(in->seq & 0xFF);
    out[4] = (uint8_t)((in->seq >> 8) & 0xFF);
    out[5] = pl;
    out[6] = in->flags;
    out[7] = 0;
    if (pl > 0) {
        memcpy(&out[APP_PROTO_HEADER_SIZE], in->payload, pl);
    }

    uint16_t crc = app_crc16_ccitt(out, APP_PROTO_HEADER_SIZE + pl);
    out[APP_PROTO_HEADER_SIZE + pl] = (uint8_t)(crc & 0xFF);
    out[APP_PROTO_HEADER_SIZE + pl + 1] = (uint8_t)((crc >> 8) & 0xFF);
    return used;
}

/* Validate and parse a frame from raw bytes. */
app_proto_parse_result_t app_proto_parse_frame(const uint8_t *buf, size_t len, app_proto_frame_t *out)
{
    if (buf == NULL || out == NULL || len < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    if (buf[0] != APP_PROTO_MAGIC0 || buf[1] != APP_PROTO_MAGIC1 || buf[2] != APP_PROTO_VER) {
        return APP_PROTO_PARSE_ERR_MAGIC;
    }

    uint8_t pl = buf[5];
    if (pl > APP_PROTO_MAX_USER_PAYLOAD) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;
    if (used > len) {
        return APP_PROTO_PARSE_ERR_LEN;
    }

    uint16_t crc_rx = (uint16_t)buf[APP_PROTO_HEADER_SIZE + pl] |
                      ((uint16_t)buf[APP_PROTO_HEADER_SIZE + pl + 1] << 8);
    uint16_t crc_calc = app_crc16_ccitt(buf, APP_PROTO_HEADER_SIZE + pl);
    if (crc_rx != crc_calc) {
        return APP_PROTO_PARSE_ERR_CRC;
    }

    out->seq = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    out->payload_len = pl;
    out->flags = buf[6];
    if (pl > 0) {
        memcpy(out->payload, &buf[APP_PROTO_HEADER_SIZE], pl);
    }
    return APP_PROTO_PARSE_OK;
}
