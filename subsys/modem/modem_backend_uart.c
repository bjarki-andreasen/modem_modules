/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/modem/modem_backend_uart.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_backend_uart);

#include <string.h>

static void modem_backend_uart_irq_handler_receive_ready(struct modem_backend_uart *backend)
{
	uint32_t size;
	uint8_t *buffer;
	int ret;

	size = ring_buf_put_claim(&backend->receive_rdb[backend->receive_rdb_used], &buffer, UINT32_MAX);

	if (size == 0) {
		LOG_WRN("receive buffer overrun");

		ring_buf_put_finish(&backend->receive_rdb[backend->receive_rdb_used], 0);

		ring_buf_reset(&backend->receive_rdb[backend->receive_rdb_used]);

		size = ring_buf_put_claim(&backend->receive_rdb[backend->receive_rdb_used], &buffer,
					  UINT32_MAX);
	}

	ret = uart_fifo_read(backend->uart, buffer, size);

	if (ret < 0) {
		ring_buf_put_finish(&backend->receive_rdb[backend->receive_rdb_used], 0);
	} else {
		ring_buf_put_finish(&backend->receive_rdb[backend->receive_rdb_used], (uint32_t)ret);
	}

	if (ret > 0) {
		k_work_submit(&backend->receive_ready_work.work);
	}
}

static void modem_backend_uart_irq_handler_transmit_ready(struct modem_backend_uart *backend)
{
	uint32_t size;
	uint8_t *buffer;
	int ret;

	if (ring_buf_is_empty(&backend->transmit_rb) == true) {
		uart_irq_tx_disable(backend->uart);

		return;
	}

	size = ring_buf_get_claim(&backend->transmit_rb, &buffer, UINT32_MAX);

	ret = uart_fifo_fill(backend->uart, buffer, size);

	if (ret < 0) {
		ring_buf_get_finish(&backend->transmit_rb, 0);
	} else {
		ring_buf_get_finish(&backend->transmit_rb, (uint32_t)ret);

		/* Update transmit buf capacity tracker */
		atomic_sub(&backend->transmit_buf_len, (uint32_t)ret);
	}
}

static void modem_backend_uart_irq_handler(const struct device *uart, void *user_data)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)user_data;

	if (uart_irq_update(uart) < 1) {
		return;
	}

	if (uart_irq_rx_ready(uart)) {
		modem_backend_uart_irq_handler_receive_ready(backend);
	}

	if (uart_irq_tx_ready(uart)) {
		modem_backend_uart_irq_handler_transmit_ready(backend);
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

	ring_buf_reset(&backend->receive_rdb[0]);
	ring_buf_reset(&backend->receive_rdb[1]);
	ring_buf_reset(&backend->transmit_rb);

	atomic_set(&backend->transmit_buf_len, 0);

	modem_backend_uart_flush(backend);

	uart_irq_rx_enable(backend->uart);
	uart_irq_tx_enable(backend->uart);

	modem_pipe_notify_opened(&backend->pipe);

	return 0;
}

static bool modem_backend_transmit_buf_above_limit(struct modem_backend_uart *backend)
{
	return (backend->transmit_buf_put_limit < atomic_get(&backend->transmit_buf_len));
}

static int modem_backend_uart_transmit(void *data, const uint8_t *buf, uint32_t size)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;
	int written;

	if (modem_backend_transmit_buf_above_limit(backend) == true) {
		return 0;
	}

	uart_irq_tx_disable(backend->uart);

	written = ring_buf_put(&backend->transmit_rb, buf, size);

	uart_irq_tx_enable(backend->uart);

	/* Update transmit buf capacity tracker */
	atomic_add(&backend->transmit_buf_len, written);

	return written;
}

static int modem_backend_uart_receive(void *data, uint8_t *buf, uint32_t size)
{
	struct modem_backend_uart *backend = (struct modem_backend_uart *)data;
	uint32_t read_bytes = 0;
	uint8_t receive_rdb_unused = (backend->receive_rdb_used == 1) ? 0 : 1;

	/* Read data from unused ring double buffer first */
	read_bytes += ring_buf_get(&backend->receive_rdb[receive_rdb_unused], buf, size);

	if (ring_buf_is_empty(&backend->receive_rdb[receive_rdb_unused]) == false) {
		return (int)read_bytes;
	}

	/* Swap receive ring double buffer */
	uart_irq_rx_disable(backend->uart);

	backend->receive_rdb_used = receive_rdb_unused;

	uart_irq_rx_enable(backend->uart);

	/* Read data from previously used buffer */
	receive_rdb_unused = (backend->receive_rdb_used == 1) ? 0 : 1;

	read_bytes += ring_buf_get(&backend->receive_rdb[receive_rdb_unused], &buf[read_bytes],
				   (size - read_bytes));

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
	uint32_t receive_double_buf_size;

	__ASSERT_NO_MSG(config->uart != NULL);
	__ASSERT_NO_MSG(config->receive_buf != NULL);
	__ASSERT_NO_MSG(config->receive_buf_size > 1);
	__ASSERT_NO_MSG((config->receive_buf_size % 2) == 0);
	__ASSERT_NO_MSG(config->transmit_buf != NULL);
	__ASSERT_NO_MSG(config->transmit_buf_size > 0);
	__ASSERT_NO_MSG(config->uart != NULL);

	memset(backend, 0x00, sizeof(*backend));

	backend->uart = config->uart;

	backend->transmit_buf_put_limit = config->transmit_buf_size - (config->transmit_buf_size / 4);

	receive_double_buf_size = config->receive_buf_size / 2;

	ring_buf_init(&backend->receive_rdb[0], receive_double_buf_size, &config->receive_buf[0]);

	ring_buf_init(&backend->receive_rdb[1], receive_double_buf_size,
		      &config->receive_buf[receive_double_buf_size]);

	ring_buf_init(&backend->transmit_rb, config->transmit_buf_size, config->transmit_buf);

	atomic_set(&backend->transmit_buf_len, 0);

	uart_irq_rx_disable(backend->uart);
	uart_irq_tx_disable(backend->uart);

	uart_irq_callback_user_data_set(backend->uart, modem_backend_uart_irq_handler, backend);

	modem_pipe_init(&backend->pipe, backend, &modem_backend_uart_api);

	backend->receive_ready_work.backend = backend;
	k_work_init(&backend->receive_ready_work.work, modem_backend_uart_receive_ready_handler);

	return &backend->pipe;
}
