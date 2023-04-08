/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_MODEM_MODEM_BACKEND_TTY_
#define ZEPHYR_MODEM_MODEM_BACKEND_TTY_

#ifdef __cplusplus
extern "C" {
#endif

struct modem_backend_tty;

struct modem_backend_tty_work {
	struct k_work_delayable dwork;
	struct modem_backend_tty *backend;
};

struct modem_backend_tty {
	const char *tty_path;
	int tty_fd;
	struct modem_pipe pipe;
	struct modem_backend_tty_work receive_ready_work;
};

struct modem_backend_tty_config {
	const char *tty_path;
};

struct modem_pipe *modem_backend_tty_init(struct modem_backend_tty *backend,
					  const struct modem_backend_tty_config *config);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MODEM_MODEM_BACKEND_TTY_ */
