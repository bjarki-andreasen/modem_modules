/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_MODEM_MODEM_PIPE_UART
#define ZEPHYR_MODEM_MODEM_PIPE_UART

struct modem_pipe_uart {
	const struct device *uart;
	struct ring_buf rx_rdb[2];
	uint8_t rx_rdb_used;
	struct ring_buf tx_rb;
	struct modem_pipe *pipe;
	modem_pipe_event_handler_t pipe_event_handler;
	void *pipe_event_handler_user_data;
	bool opened;
};

struct modem_pipe_uart_config {
	const struct device *uart;
	uint8_t *rx_buf;
	uint32_t rx_buf_size;
	uint8_t *tx_buf;
	uint32_t tx_buf_size;
};

int modem_pipe_uart_init(struct modem_pipe_uart *context,
			 const struct modem_pipe_uart_config *config);

int modem_pipe_uart_open(struct modem_pipe_uart *context, struct modem_pipe *pipe);

int modem_pipe_uart_config(struct modem_pipe *pipe, const struct uart_config *cfg);

int modem_pipe_uart_close(struct modem_pipe *pipe);

#endif /* ZEPHYR_MODEM_MODEM_PIPE_UART */
