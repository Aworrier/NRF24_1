#include "app_stats.h"

#include <string.h>

/* Shared TX/RX counters and seq tracking helpers. */

static app_tx_stats_t s_tx_stats = {0};
static app_rx_stats_t s_rx_stats = {0};

/* Clear all TX/RX statistics. */
void app_stats_reset(void)
{
    memset(&s_tx_stats, 0, sizeof(s_tx_stats));
    memset(&s_rx_stats, 0, sizeof(s_rx_stats));
}

/* Return mutable TX stats pointer. */
app_tx_stats_t *app_stats_tx(void)
{
    return &s_tx_stats;
}

/* Return mutable RX stats pointer. */
app_rx_stats_t *app_stats_rx(void)
{
    return &s_rx_stats;
}

/* Return read-only TX stats pointer. */
const app_tx_stats_t *app_stats_tx_get(void)
{
    return &s_tx_stats;
}

/* Return read-only RX stats pointer. */
const app_rx_stats_t *app_stats_rx_get(void)
{
    return &s_rx_stats;
}

/* Update RX error counters by parse result. */
void app_rx_stats_on_parse_result(app_rx_stats_t *stats, app_proto_parse_result_t result)
{
    if (stats == NULL) {
        return;
    }

    switch (result) {
        case APP_PROTO_PARSE_ERR_LEN:
            stats->len_fail++;
            break;
        case APP_PROTO_PARSE_ERR_MAGIC:
            stats->magic_fail++;
            break;
        case APP_PROTO_PARSE_ERR_CRC:
            stats->crc_fail++;
            break;
        default:
            break;
    }
}

/* Update RX sequence tracking after a valid frame. */
void app_rx_stats_on_frame_ok(app_rx_stats_t *stats, uint16_t seq)
{
    if (stats == NULL) {
        return;
    }

    stats->frame_ok++;
    if (!stats->has_last_seq) {
        stats->has_last_seq = true;
        stats->last_seq = seq;
        return;
    }

    uint16_t expected = (uint16_t)(stats->last_seq + 1U);
    if (seq == stats->last_seq) {
        stats->seq_dup++;
    } else if (seq == expected) {
        /* Continuous frame. */
    } else if (seq > expected) {
        stats->seq_gap += (uint32_t)(seq - expected);
    } else {
        stats->seq_out_of_order++;
    }
    stats->last_seq = seq;
}
