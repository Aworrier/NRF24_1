#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_PROTO_MAGIC0 0xA5
#define APP_PROTO_MAGIC1 0x5A
#define APP_PROTO_VER 0x01
#define APP_PROTO_HEADER_SIZE 8
#define APP_PROTO_CRC_SIZE 2
#define APP_PROTO_MAX_FRAME_SIZE 32
#define APP_PROTO_MAX_USER_PAYLOAD (APP_PROTO_MAX_FRAME_SIZE - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE)

typedef struct {
    uint16_t seq;
    uint8_t flags;
    uint8_t payload_len;
    uint8_t payload[APP_PROTO_MAX_USER_PAYLOAD];
} app_proto_frame_t;

typedef enum {
    APP_PROTO_PARSE_OK = 0,
    APP_PROTO_PARSE_ERR_LEN,
    APP_PROTO_PARSE_ERR_MAGIC,
    APP_PROTO_PARSE_ERR_CRC,
} app_proto_parse_result_t;

size_t app_proto_build_frame(uint8_t *out, size_t out_size, const app_proto_frame_t *in);
app_proto_parse_result_t app_proto_parse_frame(const uint8_t *buf, size_t len, app_proto_frame_t *out);
void app_proto_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size);
