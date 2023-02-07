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

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_DRIVERS_MODEM_MODEM_CMUX
#define ZEPHYR_DRIVERS_MODEM_MODEM_CMUX

struct modem_cmux;

enum modem_cmux_event_type {
	MODEM_CMUX_EVENT_CONNECTED = 0,
	MODEM_CMUX_EVENT_DISCONNECTED,
	MODEM_CMUX_EVENT_OPENED,
	MODEM_CMUX_EVENT_CLOSED,
	MODEM_CMUX_EVENT_MODEM_STATUS,
};

struct modem_cmux_event {
	uint16_t dlci_address;
	enum modem_cmux_event_type type;
};

typedef void (*modem_cmux_event_handler_t)(struct modem_cmux *cmux, struct modem_cmux_event event,
					   void *user_data);

enum modem_cmux_dlci_state {
	MODEM_CMUX_DLCI_STATE_CLOSED = 0,
	MODEM_CMUX_DLCI_STATE_OPENING,
	MODEM_CMUX_DLCI_STATE_OPEN,
	MODEM_CMUX_DLCI_STATE_CLOSING,
};

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
	MODEM_CMUX_RECEIVE_STATE_EOF,
};

struct modem_cmux_dlci {
	/* Configuration */
	uint16_t dlci_address;
	struct modem_cmux *cmux;
	struct modem_pipe *pipe;
	modem_pipe_event_handler_t pipe_event_handler;
	void *pipe_event_handler_user_data;

	/* Receive buffer */
	struct ring_buf receive_rb;
	struct k_mutex receive_rb_lock;

	/* State */
	enum modem_cmux_dlci_state state;
	bool allocated;
};

struct modem_cmux_frame {
	uint16_t dlci_address;
	bool cr;
	bool pf;
	uint8_t type;
	const uint8_t *data;
	uint16_t data_len;
};

enum modem_cmux_state {
	MODEM_CMUX_STATE_DISCONNECTED = 0,
	MODEM_CMUX_STATE_CONNECTING,
	MODEM_CMUX_STATE_CONNECTED,
	MODEM_CMUX_STATE_DISCONNECTING,
};

struct modem_cmux_work_delayable {
	struct k_work_delayable dwork;
	struct modem_cmux *cmux;
};

struct modem_cmux {
	/* Bus pipe */
	struct modem_pipe *pipe;

	/* Event handler */
	modem_cmux_event_handler_t event_handler;
	void *event_handler_user_data;

	/* Synchronization */
	struct k_mutex lock;

	/* DLCI channel contexts */
	struct modem_cmux_dlci *dlcis;
	uint16_t dlcis_cnt;

	/* Status */
	enum modem_cmux_state state;

	/* Receive state*/
	enum modem_cmux_receive_state receive_state;

	/* Receive buffer */
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	uint16_t receive_buf_len;

	/* Work buffer */
	uint8_t work_buf[64];
	uint16_t work_buf_len;

	/* Received frame */
	struct modem_cmux_frame frame;
	uint8_t frame_header[5];
	uint16_t frame_header_len;

	/* Work */
	struct modem_cmux_work_delayable process_received;
	k_timeout_t receive_timeout;
};

/**
 * @brief Contains CMUX instance confuguration data
 * @param event_handler Invoked when event occurs
 * @param event_handler_user_data Free to use pointer passed to event handler when invoked
 * @param dlcis Array of DLCI channel contexts used internally
 * @param dlcis_cnt Length of array of DLCI channel contexts
 * @param receive_buf Receive buffer
 * @param receive_buf_size Sice of receive buffer in bytes
 * @param receive_timeout Timeout from data is received until data is read
 */
struct modem_cmux_config {
	modem_cmux_event_handler_t event_handler;
	void *event_handler_user_data;
	struct modem_cmux_dlci *dlcis;
	uint16_t dlcis_cnt;
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	k_timeout_t receive_timeout;
};

/**
 * @brief Initialize CMUX instance
 */
int modem_cmux_init(struct modem_cmux *cmux, const struct modem_cmux_config *config);

/**
 * @brief Connect CMUX instance
 * @details This will send a CMUX connect request to target on the serial bus. If successful,
 * DLCI channels can be now be opened using modem_cmux_dlci_open().
 * @param cmux CMUX instance
 * @param pipe pipe used to transmit data to and from bus
 * @note When connected, the bus pipe must not be used directly
 */
int modem_cmux_connect(struct modem_cmux *cmux, struct modem_pipe *pipe);

/**
 * @brief CMUX DLCI configuration
 * @param dlci_address DLCI channel address
 * @param receive_buf Receive buffer used by pipe
 * @param receive_buf_size Size of receive buffer used by pipe
 */
struct modem_cmux_dlci_config {
	uint16_t dlci_address;
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
};

/**
 * @brief Open DLCI channel on connected CMUX instance
 * @param cmux CMUX instance
 * @param config CMUX DLCI channel configuration
 * @param pipe Pipe context
 */
int modem_cmux_dlci_open(struct modem_cmux *cmux, struct modem_cmux_dlci_config *config,
			 struct modem_pipe *pipe);

/**
 * @brief Close DLCI channel on connected CMUX instance
 */
int modem_cmux_dlci_close(struct modem_pipe *pipe);

/**
 * @brief Close down and disconnect CMUX instance
 * @details This will close all open DLCI channels, and close down the CMUX connection.
 * @note When disconnected, the bus pipe can be used directly again
 */
int modem_cmux_disconnect(struct modem_cmux *cmux);

#endif /* ZEPHYR_DRIVERS_MODEM_MODEM_CMUX */
