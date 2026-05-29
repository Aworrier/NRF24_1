#pragma once

#include <stddef.h>

typedef void (*app_control_send_fn_t)(void *user, const char *line);

typedef struct {
    app_control_send_fn_t send_line;
    void *user;
} app_control_io_t;

void app_control_start_uart(void);
void app_control_handle_line(const app_control_io_t *io, char *line);
