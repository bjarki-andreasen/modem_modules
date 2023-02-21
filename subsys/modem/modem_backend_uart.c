/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/modem/modem_backend_uart.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_backend_uart);

#include <string.h>

static void modem_backend_uart_irq_handler_rx_ready(struct modem_backend_uart *backend)
{
	/* Variables */
	uint32_t size;
	uint8_t *buffer;
	int ret;

	/* Reserve rx ring buffer */
	size = ring_buf_put_claim(&backend->rx_rdb[backend->rx_rdb_used], &buffer, UINT32_MAX);

	/* Validate rx ring buffer is not full */
	if (size == 0) {
		/* Release rx ring buffer */
		ring_buf_put_finish(&backend->rx_rdb[backend->rx_rdb_used], 0);

		/* Disable rx IRQ */
		uart_irq_rx_disable(backend->uart);

		LOG_WRN("RX buffer overrun");

		return;
	}

	/* Try to read from fifo */
	ret = uart_fifo_read(backend->uart, buffer, size);

	/* Release rx ring buffer */
	ring_buf_put_finish(&backend->rx_rdb[backend->rx_rdb_used], (uint32_t)ret);

	/* Invoke event if data was received */
	if (0 < ret) {
		k_work_submit(&backend->receive_ready_work.work);
	}
}

static void modem_backend_uart_irq_handler_tx_ready(struct modem_backend_uart *backend)
{
	/* Variables */
	uint32_t size;
	uint8_t *buffer;
	int ret;

	/* Check if tx ring buffer is empty */
	if (ring_buf_size_get(&backend->tx_rb) == 0) {
		/* Disable UART tx IRQ */
		uart_irq_tx_disable(backend->uart);

		/* Release tx ring buffer */
		ring_buf_get_finish(&backend->tx_rb, 0);

		return;
	}

	/* Reserve tx ring buffer */
	size = ring_buf_get_claim(&backend->tx_rb, &buffer, UINT32_MAX);

	/* Try to write to fifo */
	ret = uart_fifo_fill(backend->uart, buffer, size);

	/* Release tx ring buffer */
	ring_buf_get_finish(&backend->tx_rb, (uint32_t)ret);
}

static void modem_backend_uart_irq_handler(const struct device *uart, void *user_data)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)user_data;

	if (uart_irq_update(uart) != 1) {
		return;
	}

	if (uart_irq_rx_ready(uart) == 1) {
		modem_backend_uart_irq_handler_rx_ready(backend);
	}

	if (uart_irq_tx_ready(uart) == 1) {
		modem_backend_uart_irq_handler_tx_ready(backend);
	}
}

static void modem_backend_uart_flush(struct modem_backend_uart *backend)
{
	uint8_t c;

	while (0 < uart_fifo_read(backend->uart, &c, 1)) {
		continue;
	}
}

static int modem_backend_uart_open(void *data)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;

	ring_buf_reset(&backend->rx_rdb[0]);
	ring_buf_reset(&backend->rx_rdb[1]);
	ring_buf_reset(&backend->tx_rb);

	modem_backend_uart_flush(backend);

	uart_irq_rx_enable(backend->uart);
	uart_irq_tx_enable(backend->uart);

	modem_pipe_notify_opened(&backend->pipe);

	return 0;
}

static int modem_backend_uart_transmit(void *data, const uint8_t *buf, uint32_t size)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;

	int written;

	uart_irq_tx_disable(backend->uart);

	written = ring_buf_put(&backend->tx_rb, buf, size);

	uart_irq_tx_enable(backend->uart);

	return written;
}

static int modem_backend_uart_receive(void *data, uint8_t *buf, uint32_t size)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;
	uint32_t read_bytes = 0;
	uint8_t rx_rdb_unused = (backend->rx_rdb_used == 1) ? 0 : 1;

	/* Read data from unused ring double buffer first */
	read_bytes += ring_buf_get(&backend->rx_rdb[rx_rdb_unused], buf, size);

	/* Check if data remains in unused buffer */
	if (ring_buf_is_empty(&backend->rx_rdb[rx_rdb_unused]) == false) {
		return (int)read_bytes;
	}

	/* Swap rx ring double buffer */
	uart_irq_rx_disable(backend->uart);

	backend->rx_rdb_used = rx_rdb_unused;

	uart_irq_rx_enable(backend->uart);

	/* Read data from previously used buffer */
	rx_rdb_unused = (backend->rx_rdb_used == 1) ? 0 : 1;

	read_bytes += ring_buf_get(&backend->rx_rdb[rx_rdb_unused], &buf[read_bytes],
				   (size - read_bytes));

	uart_irq_rx_disable(backend->uart);

	/* Invoke receive ready event if data remains */
	if (ring_buf_is_empty(&backend->rx_rdb[backend->rx_rdb_used]) == false) {
		k_work_submit(&backend->receive_ready_work.work);
	}

	uart_irq_rx_enable(backend->uart);

	return (int)read_bytes;
}

static int modem_backend_uart_close(void *data)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;

	uart_irq_rx_disable(backend->uart);
	uart_irq_tx_disable(backend->uart);

	modem_pipe_notify_closed(&backend->pipe);

	return 0;
}

struct modem_pipe_api modem_backend_uart_api = {
	.open = modem_backend_uart_open,
	.transmit = modem_backend_uart_transmit,
	.receive = modem_backend_uart_receive,
	.close = modem_backend_uart_close,
};

static void modem_backend_uart_receive_ready_handler(struct k_work *item)
{
	struct modem_backend_uart_work *backend_work =
		(struct modem_backend_uart_work *)item;

	struct modem_backend_uart *backend = backend_work->backend;

	modem_pipe_notify_receive_ready(&backend->pipe);
}

struct modem_pipe *modem_backend_uart_init(struct modem_backend_uart *backend,
					   const struct modem_backend_uart_config *config)
{
	uint32_t rx_double_buf_size;

	__ASSERT_NO_MSG(config->uart != NULL);
	__ASSERT_NO_MSG(config->rx_buf != NULL);
	__ASSERT_NO_MSG(config->rx_buf_size > 1);
	__ASSERT_NO_MSG((config->rx_buf_size % 2) == 0);
	__ASSERT_NO_MSG(config->tx_buf != NULL);
	__ASSERT_NO_MSG(config->tx_buf_size > 0);
	__ASSERT_NO_MSG(config->uart != NULL);

	memset(backend, 0x00, sizeof(*backend));

	backend->uart = config->uart;

	rx_double_buf_size = config->rx_buf_size / 2;

	ring_buf_init(&backend->rx_rdb[0], rx_double_buf_size, config->rx_buf);
	ring_buf_init(&backend->rx_rdb[1], rx_double_buf_size, &config->rx_buf[rx_double_buf_size]);
	ring_buf_init(&backend->tx_rb, config->tx_buf_size, config->tx_buf);

	uart_irq_rx_disable(backend->uart);
	uart_irq_tx_disable(backend->uart);

	uart_irq_callback_user_data_set(backend->uart, modem_backend_uart_irq_handler, backend);

	modem_pipe_init(&backend->pipe, backend, &modem_backend_uart_api);

	backend->receive_ready_work.backend = backend;
	k_work_init(&backend->receive_ready_work.work, modem_backend_uart_receive_ready_handler);

	return &backend->pipe;
}
