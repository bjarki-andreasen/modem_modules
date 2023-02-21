/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/modem/modem_backend_tty.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_backend_tty);

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>

static int modem_backend_tty_open(void *data)
{
	struct modem_backend_tty *backend = (struct modem_backend_tty *)data;

	backend->tty_fd = open(backend->tty_path, (O_RDWR | O_NONBLOCK), 0644);

	if (backend->tty_fd < 0) {
		return -EPERM;
	}

	k_work_schedule(&backend->receive_ready_work.dwork, K_MSEC(10));

	modem_pipe_notify_opened(&backend->pipe);

	return 0;
}

static int modem_backend_tty_transmit(void *data, const uint8_t *buf, uint32_t size)
{
	struct modem_backend_tty *backend = (struct modem_backend_tty *)data;

	return write(backend->tty_fd, buf, size);
}

static int modem_backend_tty_receive(void *data, uint8_t *buf, uint32_t size)
{
	int ret;

	struct modem_backend_tty *backend = (struct modem_backend_tty *)data;

	ret = read(backend->tty_fd, buf, size);

	return (ret < 0) ? 0 : ret;
}

static int modem_backend_tty_close(void *data)
{
	struct modem_backend_tty *backend = (struct modem_backend_tty *)data;
	struct k_work_sync sync;

	k_work_cancel_delayable_sync(&backend->receive_ready_work.dwork, &sync);

	close(backend->tty_fd);

	modem_pipe_notify_closed(&backend->pipe);

	return 0;
}

struct modem_pipe_api modem_backend_tty_api = {
	.open = modem_backend_tty_open,
	.transmit = modem_backend_tty_transmit,
	.receive = modem_backend_tty_receive,
	.close = modem_backend_tty_close,
};

static void modem_backend_tty_receive_ready_handler(struct k_work *item)
{
	struct modem_backend_tty_work *backend_work = (struct modem_backend_tty_work *)item;

	struct modem_backend_tty *backend = backend_work->backend;
	struct pollfd pollfd;

	pollfd.fd = backend->tty_fd;
	pollfd.events = POLLIN;

	if (poll(&pollfd, 1, 0) < 0) {
		k_work_schedule(&backend->receive_ready_work.dwork, K_MSEC(10));

		return;
	}

	if (pollfd.revents & POLLIN) {
		modem_pipe_notify_receive_ready(&backend->pipe);
	}

	k_work_schedule(&backend->receive_ready_work.dwork, K_MSEC(10));
}

struct modem_pipe *modem_backend_tty_init(struct modem_backend_tty *backend,
					  const struct modem_backend_tty_config *config)
{
	__ASSERT_NO_MSG(backend != NULL);
	__ASSERT_NO_MSG(config != NULL);
	__ASSERT_NO_MSG(config->tty_path != NULL);

	memset(backend, 0x00, sizeof(*backend));

	backend->tty_path = config->tty_path;

	modem_pipe_init(&backend->pipe, backend, &modem_backend_tty_api);

	backend->receive_ready_work.backend = backend;
	k_work_init_delayable(&backend->receive_ready_work.dwork,
			      modem_backend_tty_receive_ready_handler);

	return &backend->pipe;
}
