#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_proto.h"

typedef struct {
    uint32_t burst_queued;
    uint32_t frame_sent;
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t retries_sum;
    uint32_t retries_max;
    uint16_t next_seq;
} app_tx_stats_t;

typedef struct {
    uint32_t rx_packets;
    uint32_t frame_ok;
    uint32_t crc_fail;
    uint32_t magic_fail;
    uint32_t len_fail;
    uint32_t seq_dup;
    uint32_t seq_out_of_order;
    uint32_t seq_gap;
    bool has_last_seq;
    uint16_t last_seq;
} app_rx_stats_t;

void app_stats_reset(void);
app_tx_stats_t *app_stats_tx(void);
app_rx_stats_t *app_stats_rx(void);
const app_tx_stats_t *app_stats_tx_get(void);
const app_rx_stats_t *app_stats_rx_get(void);

void app_rx_stats_on_parse_result(app_rx_stats_t *stats, app_proto_parse_result_t result);
void app_rx_stats_on_frame_ok(app_rx_stats_t *stats, uint16_t seq);
