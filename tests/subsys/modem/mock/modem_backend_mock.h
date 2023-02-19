
/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/modem/modem_pipe.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#ifndef ZEPHYR_DRIVERS_MODEM_MODEM_PIPE_MOCK
#define ZEPHYR_DRIVERS_MODEM_MODEM_PIPE_MOCK

struct modem_backend_mock;

struct modem_backend_mock_work {
	struct k_work work;
	struct modem_backend_mock *mock;
};

struct modem_backend_mock {
	/* Pipe */
	struct modem_pipe pipe;

	/* Ring buffers */
	struct ring_buf rx_rb;
	struct ring_buf tx_rb;

	/* Work */
	struct modem_backend_mock_work received_work_item;

	/* Max allowed read/write size */
	size_t limit;
};

struct modem_backend_mock_config {
	uint8_t *rx_buf;
	size_t rx_buf_size;
	uint8_t *tx_buf;
	size_t tx_buf_size;
	size_t limit;
};

struct modem_pipe *modem_backend_mock_init(struct modem_backend_mock *mock,
					   const struct modem_backend_mock_config *config);

void modem_backend_mock_reset(struct modem_backend_mock *mock);

int modem_backend_mock_get(struct modem_backend_mock *mock, uint8_t *buf, size_t size);

void modem_backend_mock_put(struct modem_backend_mock *mock, const uint8_t *buf, size_t size);

#endif /* ZEPHYR_DRIVERS_MODEM_MODEM_PIPE_MOCK */
