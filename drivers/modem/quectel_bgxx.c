/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*

 */

/************************************************************************
 * Dependencies
 ************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

/************************************************************************
 * Definitions
 ************************************************************************/

/************************************************************************
 * Logging
 ************************************************************************/
LOG_MODULE_REGISTER(quectel_bg95, CONFIG_LOG_DEFAULT_LEVEL);

/************************************************************************
 * Instance configuration struct
 ************************************************************************/
struct quectel_bgxx_config {
	const struct device *uart;
};

/************************************************************************
 * Instance data struct
 ************************************************************************/
struct quectel_bgxx_data {

	/* device */
	const struct device *uart_dev;
	struct k_mutex state_mut;
};

static int wrap_uart_quectel_bgxx_poll_in(const struct device *dev, unsigned char *c)
{
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	return uart_poll_in(data->uart_dev, c);
}

static void wrap_uart_quectel_bgxx_poll_out(const struct device *dev, unsigned char c)
{
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	uart_poll_out(data->uart_dev, c);
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int wrap_uart_quectel_bgxx_uart_configure(const struct device *dev,
						 const struct uart_config *cfg)
{
	return uart_configure(dev, cfg);
}

static int wrap_uart_quectel_bgxx_uart_config_get(const struct device *dev, struct uart_config *cfg)
{
	return uart_config_get(dev, cfg);
}
#endif

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int wrap_uart_quectel_bgxx_uart_fifo_fill(const struct device *dev, const uint8_t *tx_data,
						 int size)
{
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	return uart_fifo_fill(data->uart_dev, tx_data, size);
}

static int wrap_uart_quectel_bgxx_uart_fifo_read(const struct device *dev, uint8_t *rx_data,
						 const int size)
{
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	return uart_fifo_read(data->uart_dev, rx_data, size);
}

static void wrap_uart_quectel_bgxx_uart_irq_tx_enable(const struct device *dev)
{
	uart_irq_tx_enable(dev);
}

static void wrap_uart_quectel_bgxx_uart_irq_tx_disable(const struct device *dev)
{
	uart_irq_tx_disable(dev);
}

static int wrap_uart_quectel_bgxx_uart_irq_tx_ready(const struct device *dev)
{
	return uart_irq_tx_ready(dev);
}

static void wrap_uart_quectel_bgxx_uart_irq_rx_enable(const struct device *dev)
{
	uart_irq_rx_enable(dev);
}

static void wrap_uart_quectel_bgxx_uart_irq_rx_disable(const struct device *dev)
{
	uart_irq_rx_disable(dev);
}

static int wrap_uart_quectel_bgxx_uart_irq_tx_complete(const struct device *dev)
{
	return uart_irq_tx_complete(dev);
}

static int wrap_uart_quectel_bgxx_uart_irq_rx_ready(const struct device *dev)
{
	return uart_irq_rx_ready(dev);
}

static int wrap_uart_quectel_bgxx_uart_irq_is_pending(const struct device *dev)
{
	return uart_irq_is_pending(dev);
}

static int wrap_uart_quectel_bgxx_uart_irq_update(const struct device *dev)
{
	return uart_irq_update(dev);
}

static void wrap_uart_quectel_bgxx_uart_irq_callback_set(const struct device *dev,
							 uart_irq_callback_user_data_t cb,
							 void *user_data)
{
	uart_irq_callback_user_data_set(dev, cb, user_data);
}
#endif

static const struct uart_driver_api quectel_bgxx_uart_api = {
	.poll_in = wrap_uart_quectel_bgxx_poll_in,
	.poll_out = wrap_uart_quectel_bgxx_poll_out,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = wrap_uart_quectel_bgxx_uart_configure,
	.config_get = wrap_uart_quectel_bgxx_uart_config_get,
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = wrap_uart_quectel_bgxx_uart_fifo_fill,
	.fifo_read = wrap_uart_quectel_bgxx_uart_fifo_read,
	.irq_tx_enable = wrap_uart_quectel_bgxx_uart_irq_tx_enable,
	.irq_tx_disable = wrap_uart_quectel_bgxx_uart_irq_tx_disable,
	.irq_tx_ready = wrap_uart_quectel_bgxx_uart_irq_tx_ready,
	.irq_rx_enable = wrap_uart_quectel_bgxx_uart_irq_rx_enable,
	.irq_rx_disable = wrap_uart_quectel_bgxx_uart_irq_rx_disable,
	.irq_tx_complete = wrap_uart_quectel_bgxx_uart_irq_tx_complete,
	.irq_rx_ready = wrap_uart_quectel_bgxx_uart_irq_rx_ready,
	.irq_is_pending = wrap_uart_quectel_bgxx_uart_irq_is_pending,
	.irq_update = wrap_uart_quectel_bgxx_uart_irq_update,
	.irq_callback_set = wrap_uart_quectel_bgxx_uart_irq_callback_set,
#endif
};

static int quectel_bgxx_init(const struct device *dev)
{
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;

	/* Initialize mutexes */
	k_mutex_init(&data->state_mut);

	return 0;
}

/* Enter section in which cellular state is accessed or altered */
static void quectel_bgxx_state_section_enter(struct quectel_bgxx_data *bgxx_data)
{
	/* Get exclusive access to GNSS commands */
	k_mutex_lock(&bgxx_data->state_mut, K_FOREVER);
}

/* Leave section in which cellular state is accessed or altered */
static void quectel_bgxx_state_section_leave(struct quectel_bgxx_data *bgxx_data)
{
	k_mutex_unlock(&bgxx_data->state_mut);
}

/* Power on modem, enable CMUX */
static int quectel_bgxx_resume(const struct device *dev)
{
	// power domain (mosfet) -> device tree
	// power on by PWR_KEY
	// check status line if on made reboot?
	// change baudrate
	// uart domains

	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	pm_device_runtime_get(data->uart_dev);

	// char read_char;
	// uart_poll_in(data->uart_dev,&read_char);

	return 0;
}

/* modem to low power state */
static int quectel_bgxx_suspend(const struct device *dev)
{
	// oposite to resume
	struct quectel_bgxx_data *data = (struct quectel_bgxx_data *)dev->data;
	pm_device_runtime_put(data->uart_dev);
	return 0;
}

static int quectel_bgxx_power_on(const struct device *dev)
{
	// config gpio to input, outputs
	return 0;
}

static int quectel_bgxx_power_off(const struct device *dev)
{
	// all gpio to sleep
	return 0;
}

static int quectel_bgxx_pm_action(const struct device *dev, enum pm_device_action action)
{
	/* Variables */
	struct quectel_bgxx_data *bgxx_data = (struct quectel_bgxx_data *)dev->data;
	int rc;

	quectel_bgxx_state_section_enter(bgxx_data);

	/* Handle action */
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		rc = quectel_bgxx_suspend(dev);
		break;

	case PM_DEVICE_ACTION_RESUME:
		rc = quectel_bgxx_resume(dev);
		if (rc < 0) {
			break;
		}

		break;

	case PM_DEVICE_ACTION_TURN_OFF:
		rc = quectel_bgxx_power_off(dev);
		break;

	case PM_DEVICE_ACTION_TURN_ON:
		rc = quectel_bgxx_power_on(dev);
		break;

	default:
		rc = -ENOTSUP;
	}

	quectel_bgxx_state_section_leave(bgxx_data);

	return rc;
}

/************************************************************************
 * Instanciation parent device macro
 ************************************************************************/
// #define DT_DRV_COMPAT quectel_bgxx

#define BGXX_DEVICE(node_id)                                                                       \
	static struct quectel_bgxx_config quectel_bgxx_config_##node_id = {                        \
		.uart = DEVICE_DT_GET(DT_BUS(node_id)),                                            \
	};                                                                                         \
                                                                                                   \
	static struct quectel_bgxx_data quectel_bgxx_data_##node_id = {};                          \
                                                                                                   \
	PM_DEVICE_DT_DEFINE(node_id, quectel_bgxx_pm_action);                                      \
                                                                                                   \
	DEVICE_DT_DEFINE(node_id, quectel_bgxx_init, PM_DEVICE_DT_GET(node_id),                    \
			 &quectel_bgxx_data_##node_id, &quectel_bgxx_config_##node_id,             \
			 POST_KERNEL, 42, &quectel_bgxx_uart_api);

DT_FOREACH_STATUS_OKAY(quectel_bgxx, BGXX_DEVICE)

// this driver
// resume -> should power modem + uart domains + power domain (mosfet)
