/*
 * Copyright (c) 2023 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/modem/modem_backend_uart.h>

#ifndef ZEPHYR_MODEM_BACKENDS_MODEM_BACKEND_UART_ASYNC
#define ZEPHYR_MODEM_BACKENDS_MODEM_BACKEND_UART_ASYNC

bool modem_backend_uart_async_is_supported(struct modem_backend_uart *backend);

void modem_backend_uart_async_init(struct modem_backend_uart *backend,
				   const struct modem_backend_uart_config *config);

#endif /* ZEPHYR_MODEM_BACKENDS_MODEM_BACKEND_UART_ASYNC */
