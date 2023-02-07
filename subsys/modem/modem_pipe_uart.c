/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_pipe_uart);

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>

#include "modem_pipe_uart.h"

#define MODEM_UART_EVENTS_RX_READY (BIT(0))
#define MODEM_UART_EVENTS_TX_IDLE  (BIT(1))

static void modem_pipe_uart_invoke_receive_ready_event(struct modem_pipe_uart *context)
{
	/* Validate event handler set */
	if (context->pipe_event_handler == NULL) {
		return;
	}

	context->pipe_event_handler(context->pipe, MODEM_PIPE_EVENT_RECEIVE_READY,
				    context->pipe_event_handler_user_data);
}

static void modem_pipe_uart_irq_handler_rx_ready(struct modem_pipe_uart *context)
{
	/* Variables */
	uint32_t size;
	uint8_t *buffer;
	int ret;

	/* Reserve rx ring buffer */
	size = ring_buf_put_claim(&context->rx_rdb[context->rx_rdb_used], &buffer, UINT32_MAX);

	/* Validate rx ring buffer is not full */
	if (size == 0) {
		/* Release rx ring buffer */
		ring_buf_put_finish(&context->rx_rdb[context->rx_rdb_used], 0);

		/* Disable rx IRQ */
		uart_irq_rx_disable(context->uart);

		LOG_WRN("RX buffer overrun");

		return;
	}

	/* Try to read from fifo */
	ret = uart_fifo_read(context->uart, buffer, size);

	/* Release rx ring buffer */
	ring_buf_put_finish(&context->rx_rdb[context->rx_rdb_used], (uint32_t)ret);

	/* Invoke event if data was received */
	if (0 < ret) {
		modem_pipe_uart_invoke_receive_ready_event(context);
	}
}

static void modem_pipe_uart_irq_handler_tx_ready(struct modem_pipe_uart *context)
{
	/* Variables */
	uint32_t size;
	uint8_t *buffer;
	int ret;

	/* Check if tx ring buffer is empty */
	if (ring_buf_size_get(&context->tx_rb) == 0) {
		/* Disable UART tx IRQ */
		uart_irq_tx_disable(context->uart);

		/* Release tx ring buffer */
		ring_buf_get_finish(&context->tx_rb, 0);

		return;
	}

	/* Reserve tx ring buffer */
	size = ring_buf_get_claim(&context->tx_rb, &buffer, UINT32_MAX);

	/* Try to write to fifo */
	ret = uart_fifo_fill(context->uart, buffer, size);

	/* Release tx ring buffer */
	ring_buf_get_finish(&context->tx_rb, (uint32_t)ret);
}

static void modem_pipe_uart_irq_handler(const struct device *uart, void *user_data)
{
	struct modem_pipe_uart *context = (struct modem_pipe_uart *)user_data;

	if (uart_irq_update(uart) != 1) {
		return;
	}

	if (uart_irq_rx_ready(uart) == 1) {
		modem_pipe_uart_irq_handler_rx_ready(context);
	}

	if (uart_irq_tx_ready(uart) == 1) {
		modem_pipe_uart_irq_handler_tx_ready(context);
	}
}

static void modem_pipe_uart_flush(struct modem_pipe_uart *context)
{
	uint8_t c;

	while (0 < uart_fifo_read(context->uart, &c, 1)) {
		continue;
	}
}

static int modem_pipe_uart_pipe_event_handler_set(struct modem_pipe *pipe,
						  modem_pipe_event_handler_t handler,
						  void *user_data)
{
	struct modem_pipe_uart *context = (struct modem_pipe_uart *)pipe->data;

	/* Validate pipe belongs to this context */
	if ((context->pipe != pipe)) {
		return -EPERM;
	}

	uart_irq_rx_disable(context->uart);
	uart_irq_tx_disable(context->uart);

	context->pipe = pipe;
	context->pipe_event_handler = handler;
	context->pipe_event_handler_user_data = user_data;

	uart_irq_rx_enable(context->uart);
	uart_irq_tx_enable(context->uart);

	return 0;
}

static int modem_pipe_uart_transmit(struct modem_pipe *pipe, const uint8_t *buf, uint32_t size)
{
	struct modem_pipe_uart *context = (struct modem_pipe_uart *)pipe->data;

	int written;

	uart_irq_tx_disable(context->uart);

	written = ring_buf_put(&context->tx_rb, buf, size);

	uart_irq_tx_enable(context->uart);

	return written;
}

static int modem_pipe_uart_receive(struct modem_pipe *pipe, uint8_t *buf, uint32_t size)
{
	struct modem_pipe_uart *context = (struct modem_pipe_uart *)pipe->data;
	uint32_t read_bytes = 0;
	uint8_t rx_rdb_unused = (context->rx_rdb_used == 1) ? 0 : 1;

	/* Read data from unused ring double buffer first */
	read_bytes += ring_buf_get(&context->rx_rdb[rx_rdb_unused], buf, size);

	/* Check if data remains in unused buffer */
	if (ring_buf_is_empty(&context->rx_rdb[rx_rdb_unused]) == false) {
		return (int)read_bytes;
	}

	/* Swap rx ring double buffer */
	uart_irq_rx_disable(context->uart);

	context->rx_rdb_used = rx_rdb_unused;

	uart_irq_rx_enable(context->uart);

	/* Read data from previously used buffer */
	rx_rdb_unused = (context->rx_rdb_used == 1) ? 0 : 1;

	read_bytes += ring_buf_get(&context->rx_rdb[rx_rdb_unused], &buf[read_bytes],
				   (size - read_bytes));

	uart_irq_rx_disable(context->uart);

	/* Invoke receive ready event if data remains */
	if (ring_buf_is_empty(&context->rx_rdb[context->rx_rdb_used]) == false) {
		modem_pipe_uart_invoke_receive_ready_event(context);
	}

	uart_irq_rx_enable(context->uart);

	return (int)read_bytes;
}

struct modem_pipe_driver_api modem_pipe_uart_api = {
	.event_handler_set = modem_pipe_uart_pipe_event_handler_set,
	.transmit = modem_pipe_uart_transmit,
	.receive = modem_pipe_uart_receive,
};

int modem_pipe_uart_init(struct modem_pipe_uart *context,
			 const struct modem_pipe_uart_config *config)
{
	/* Validate arguments */
	if (context == NULL || config == NULL) {
		return -EINVAL;
	}

	/* Validate config */
	if ((config->uart == NULL) || (config->rx_buf == NULL) || (config->rx_buf_size < 1) ||
	    (config->tx_buf == NULL) || (config->tx_buf_size < 1)) {
		return -EINVAL;
	}

	/* Clear CMUX context */
	memset(context, 0x00, sizeof(*context));

	/* Copy configuration to context */
	context->uart = config->uart;

	/* Initialize rx ring double buffers */
	uint32_t rx_double_buf_size = config->rx_buf_size / 2;
	ring_buf_init(&context->rx_rdb[0], rx_double_buf_size, config->rx_buf);
	ring_buf_init(&context->rx_rdb[1], rx_double_buf_size, &config->rx_buf[rx_double_buf_size]);

	/* Initialize tx ring buffer */
	ring_buf_init(&context->tx_rb, config->tx_buf_size, config->tx_buf);

	uart_irq_rx_disable(context->uart);
	uart_irq_tx_disable(context->uart);

	uart_irq_callback_user_data_set(context->uart, modem_pipe_uart_irq_handler, context);

	return 0;
}

int modem_pipe_uart_open(struct modem_pipe_uart *context, struct modem_pipe *pipe)
{
	/* Validate arguments */
	if ((context == NULL) || (pipe == NULL)) {
		return -EINVAL;
	}

	/* Validate closed */
	if (context->opened == true) {
		return -EPERM;
	}

	/* Configure pipe */
	pipe->data = context;
	pipe->api = &modem_pipe_uart_api;

	/* Update context */
	context->pipe = pipe;
	context->opened = true;

	/* Flush UART */
	modem_pipe_uart_flush(context);

	/* Enable UART */
	uart_irq_rx_enable(context->uart);
	uart_irq_tx_enable(context->uart);

	return 0;
}

int modem_pipe_uart_close(struct modem_pipe *pipe)
{
	struct modem_pipe_uart *context = (struct modem_pipe_uart *)pipe->data;

	/* Validate arguments */
	if (pipe == NULL) {
		return -EINVAL;
	}

	/* Validate opened */
	if (context->opened == false) {
		return -EPERM;
	}

	/* Clear pipe */
	memset(pipe, 0x00, sizeof(*pipe));

	/* Update context */
	context->pipe = NULL;
	context->pipe_event_handler = NULL;
	context->pipe_event_handler_user_data = NULL;
	context->opened = false;

	uart_irq_rx_disable(context->uart);
	uart_irq_tx_disable(context->uart);

	return 0;
}
