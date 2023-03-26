/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This library uses CMUX to create multiple data channels, called DLCIs, on a single serial bus.
 * Each DLCI has an address from 1 to 63. DLCI address 0 is reserved for control commands.
 *
 * Design overview:
 *
 *     DLCI1 <-----------+                              +-------> DLCI1
 *                       v                              v
 *     DLCI2 <---> CMUX instance <--> Serial bus <--> Client <--> DLCI2
 *                       ^                              ^
 *     DLCI3 <-----------+                              +-------> DLCI3
 *
 * Writing to and from the CMUX instances is done using a generic PIPE interface.
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_DRIVERS_MODEM_MODEM_CMUX
#define ZEPHYR_DRIVERS_MODEM_MODEM_CMUX

struct modem_cmux;

enum modem_cmux_state {
	MODEM_CMUX_STATE_DISCONNECTED = 0,
	MODEM_CMUX_STATE_CONNECTING,
	MODEM_CMUX_STATE_CONNECTED,
	MODEM_CMUX_STATE_DISCONNECTING,
};

enum modem_cmux_event {
	MODEM_CMUX_EVENT_CONNECTED = 0,
	MODEM_CMUX_EVENT_DISCONNECTED,
};

typedef void (*modem_cmux_callback)(struct modem_cmux *cmux, enum modem_cmux_event event,
				    void *user_data);

enum modem_cmux_receive_state {
	MODEM_CMUX_RECEIVE_STATE_SOF = 0,
	MODEM_CMUX_RECEIVE_STATE_RESYNC_0,
	MODEM_CMUX_RECEIVE_STATE_RESYNC_1,
	MODEM_CMUX_RECEIVE_STATE_RESYNC_2,
	MODEM_CMUX_RECEIVE_STATE_RESYNC_3,
	MODEM_CMUX_RECEIVE_STATE_ADDRESS,
	MODEM_CMUX_RECEIVE_STATE_ADDRESS_CONT,
	MODEM_CMUX_RECEIVE_STATE_CONTROL,
	MODEM_CMUX_RECEIVE_STATE_LENGTH,
	MODEM_CMUX_RECEIVE_STATE_LENGTH_CONT,
	MODEM_CMUX_RECEIVE_STATE_DATA,
	MODEM_CMUX_RECEIVE_STATE_FCS,
	MODEM_CMUX_RECEIVE_STATE_DROP,
	MODEM_CMUX_RECEIVE_STATE_EOF,
};

enum modem_cmux_dlci_state {
	MODEM_CMUX_DLCI_STATE_CLOSED,
	MODEM_CMUX_DLCI_STATE_OPENING,
	MODEM_CMUX_DLCI_STATE_OPEN,
	MODEM_CMUX_DLCI_STATE_CLOSING,
};

enum modem_cmux_dlci_event {
	MODEM_CMUX_DLCI_EVENT_OPENED,
	MODEM_CMUX_DLCI_EVENT_CLOSED,
};

struct modem_cmux_dlci;

struct modem_cmux_dlci_work {
	struct k_work_delayable dwork;
	struct modem_cmux_dlci *dlci;
};

struct modem_cmux_dlci {
	sys_snode_t node;

	/* Pipe */
	struct modem_pipe pipe;

	/* Context */
	uint16_t dlci_address;
	struct modem_cmux *cmux;

	/* Receive buffer */
	struct ring_buf receive_rb;
	struct k_mutex receive_rb_lock;

	/* Work */
	struct modem_cmux_dlci_work open_work;
	struct modem_cmux_dlci_work close_work;

	/* State */
	enum modem_cmux_dlci_state state;
};

struct modem_cmux_frame {
	uint16_t dlci_address;
	bool cr;
	bool pf;
	uint8_t type;
	const uint8_t *data;
	uint16_t data_len;
};

struct modem_cmux_work {
	struct k_work_delayable dwork;
	struct modem_cmux *cmux;
};

struct modem_cmux {
	/* Bus pipe */
	struct modem_pipe *pipe;

	/* Event handler */
	modem_cmux_callback callback;
	void *user_data;

	/* DLCI channel contexts */
	sys_slist_t dlcis;

	/* State */
	enum modem_cmux_state state;
	bool flow_control_on;

	/* Receive state*/
	enum modem_cmux_receive_state receive_state;

	/* Receive buffer */
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	uint16_t receive_buf_len;

	/* Transmit buffer */
	struct ring_buf transmit_rb;
	struct k_mutex transmit_rb_lock;

	/* Received frame */
	struct modem_cmux_frame frame;
	uint8_t frame_header[5];
	uint16_t frame_header_len;

	/* Work */
	struct modem_cmux_work receive_work;
	struct modem_cmux_work transmit_work;
	struct modem_cmux_work connect_work;
	struct modem_cmux_work disconnect_work;

	/* Synchronize actions */
	struct k_event event;
};

/**
 * @brief Contains CMUX instance confuguration data
 * @param callback Invoked when event occurs
 * @param user_data Free to use pointer passed to event handler when invoked
 * @param receive_buf Receive buffer
 * @param receive_buf_size Size of receive buffer in bytes [127, ...]
 * @param transmit_buf Transmit buffer
 * @param transmit_buf_size Size of transmit buffer in bytes [149, ...]
 * @param receive_timeout Timeout from data is received until data is read
 */
struct modem_cmux_config {
	modem_cmux_callback callback;
	void *user_data;
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	uint8_t *transmit_buf;
	uint16_t transmit_buf_size;
};

/**
 * @brief Initialize CMUX instance
 */
void modem_cmux_init(struct modem_cmux *cmux, const struct modem_cmux_config *config);

/**
 * @brief CMUX DLCI configuration
 * @param dlci_address DLCI channel address
 * @param receive_buf Receive buffer used by pipe
 * @param receive_buf_size Size of receive buffer used by pipe [127, ...]
 */
struct modem_cmux_dlci_config {
	uint8_t dlci_address;
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
};

/**
 * @brief Initialize DLCI instance and register it with CMUX instance
 * @param cmux CMUX instance
 * @param dlci DLCI instance
 * @param config DLCI configuration
 */
struct modem_pipe *modem_cmux_dlci_init(struct modem_cmux *cmux, struct modem_cmux_dlci *dlci,
					const struct modem_cmux_dlci_config *config);

/**
 * @brief Initialize CMUX instance
 */
int modem_cmux_attach(struct modem_cmux *cmux, struct modem_pipe *pipe);

/**
 * @brief Connect CMUX instance
 * @details This will send a CMUX connect request to target on the serial bus. If successful,
 * DLCI channels can be now be opened using modem_pipe_open()
 * @param cmux CMUX instance
 * @param pipe pipe used to transmit data to and from bus
 * @note When connected, the bus pipe must not be used directly
 */
int modem_cmux_connect(struct modem_cmux *cmux);

/**
 * @brief Connect CMUX instance asynchronously
 * @details This will send a CMUX connect request to target on the serial bus. If successful,
 * DLCI channels can be now be opened using modem_pipe_open().
 * @param cmux CMUX instance
 * @param pipe pipe used to transmit data to and from bus
 * @note When connected, the bus pipe must not be used directly
 */
int modem_cmux_connect_async(struct modem_cmux *cmux);

/**
 * @brief Close down and disconnect CMUX instance
 * @details This will close all open DLCI channels, and close down the CMUX connection.
 * @note When disconnected, the bus pipe can be used directly again
 */
int modem_cmux_disconnect(struct modem_cmux *cmux);

/**
 * @brief Close down and disconnect CMUX instance asynchronously
 * @details This will close all open DLCI channels, and close down the CMUX connection.
 * @note When disconnected, the bus pipe can be used directly again
 */
int modem_cmux_disconnect_async(struct modem_cmux *cmux);

void modem_cmux_release(struct modem_cmux *cmux);

#endif /* ZEPHYR_DRIVERS_MODEM_MODEM_CMUX */
