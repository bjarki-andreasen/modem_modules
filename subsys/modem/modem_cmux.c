/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_cmux);

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <string.h>

#include <zephyr/modem/modem_cmux.h>

#define MODEM_CMUX_N1 256
#define MODEM_CMUX_N2 3

#define MODEM_CMUX_FCS_POLYNOMIAL 0xE0
#define MODEM_CMUX_FCS_INIT_VALUE 0xFF

#define MODEM_CMUX_EA 0x01
#define MODEM_CMUX_CR 0x02
#define MODEM_CMUX_PF 0x10

#define MODEM_CMUX_DLCI_ADDRESS_MIN	 (1U)
#define MODEM_CMUX_DLCI_ADDRESS_MAX	 (32767U)
#define MODEM_CMUX_FRAME_SIZE_MIN	 (6U)
#define MODEM_CMUX_FRAME_HEADER_SIZE_MAX (6U)
#define MODEM_CMUX_FRAME_TAIL_SIZE	 (2U)
#define MODEM_CMUX_RECEIVE_BUF_SIZE_MIN                                                            \
	(128U + MODEM_CMUX_FRAME_HEADER_SIZE_MAX + MODEM_CMUX_FRAME_TAIL_SIZE)

#define MODEM_CMUX_DLCI_RECEIVE_BUF_SIZE_MIN                                                       \
	(MODEM_CMUX_FRAME_HEADER_SIZE_MAX + MODEM_CMUX_FRAME_TAIL_SIZE)

#define MODEM_CMUX_FRAME_TRANSMIT_INTERVAL_MIN_MS (10U)

#define MODEM_CMUX_EVENT_BUS_PIPE_RECEIVE_READY_BIT (0)

#define MODEM_CMUX_FRAME_SIZE_MAX (8U)

enum modem_cmux_frame_types {
	MODEM_CMUX_FRAME_TYPE_RR = 0x01,
	MODEM_CMUX_FRAME_TYPE_UI = 0x03,
	MODEM_CMUX_FRAME_TYPE_RNR = 0x05,
	MODEM_CMUX_FRAME_TYPE_REJ = 0x09,
	MODEM_CMUX_FRAME_TYPE_DM = 0x0F,
	MODEM_CMUX_FRAME_TYPE_SABM = 0x2F,
	MODEM_CMUX_FRAME_TYPE_DISC = 0x43,
	MODEM_CMUX_FRAME_TYPE_UA = 0x63,
	MODEM_CMUX_FRAME_TYPE_UIH = 0xEF,
};

enum modem_cmux_command_types {
	MODEM_CMUX_COMMAND_NSC = 0x04,
	MODEM_CMUX_COMMAND_TEST = 0x08,
	MODEM_CMUX_COMMAND_PSC = 0x10,
	MODEM_CMUX_COMMAND_RLS = 0x14,
	MODEM_CMUX_COMMAND_FCOFF = 0x18,
	MODEM_CMUX_COMMAND_PN = 0x20,
	MODEM_CMUX_COMMAND_RPN = 0x24,
	MODEM_CMUX_COMMAND_FCON = 0x28,
	MODEM_CMUX_COMMAND_CLD = 0x30,
	MODEM_CMUX_COMMAND_SNC = 0x34,
	MODEM_CMUX_COMMAND_MSC = 0x38,
};

struct modem_cmux_command_type {
	uint8_t ea: 1;
	uint8_t cr: 1;
	uint8_t value: 6;
};

struct modem_cmux_command_length {
	uint8_t ea: 1;
	uint8_t value: 7;
};

struct modem_cmux_command {
	struct modem_cmux_command_type type;
	struct modem_cmux_command_length length;
	uint8_t value[];
};

static int modem_cmux_wrap_command(struct modem_cmux_command **command, const uint8_t *data,
				   uint16_t data_len)
{
	if ((data == NULL) || (data_len < 2)) {
		return -EINVAL;
	}

	(*command) = (struct modem_cmux_command *)data;

	if (((*command)->length.ea == 0) || ((*command)->type.ea == 0)) {
		return -EINVAL;
	}

	if ((*command)->length.value != (data_len - 2)) {
		return -EINVAL;
	}

	return 0;
}

static struct modem_cmux_command *modem_cmux_command_wrap(uint8_t *data)
{
	return (struct modem_cmux_command *)data;
}

static void modem_cmux_log_unknown_frame(struct modem_cmux *cmux)
{
	char data[24];
	uint8_t data_cnt = (cmux->frame.data_len < 8) ? cmux->frame.data_len : 8;
	for (uint8_t i = 0; i < data_cnt; i++) {
		snprintk(&data[i * 3], sizeof(data) - (i * 3), "%02X,", cmux->frame.data[i]);
	}

	/* Remove trailing */
	if (data_cnt > 0) {
		data[(data_cnt * 3) - 1] = '\0';
	}

	LOG_DBG("ch:%u, type:%u, data:%s", cmux->frame.dlci_address, cmux->frame.type, data);
}

static void modem_cmux_dlci_pipe_raise_event(struct modem_cmux_dlci *dlci,
					     enum modem_pipe_event event)
{
	/* Notify pipe closed */
	if (dlci->pipe_callback != NULL) {
		dlci->pipe_callback(dlci->pipe, event, dlci->pipe_callback_user_data);
	}
}

static struct modem_cmux_dlci *modem_cmux_dlci_alloc(struct modem_cmux *cmux)
{
	for (uint16_t i = 0; i < cmux->dlcis_size; i++) {
		if (cmux->dlcis[i].allocated == true) {
			continue;
		}

		cmux->dlcis[i].allocated = true;

		return &cmux->dlcis[i];
	}

	return NULL;
}

static void modem_cmux_dlci_free(struct modem_cmux *cmux, struct modem_cmux_dlci *dlci)
{
	dlci->allocated = false;
}

/* Find allocated DLCI channel context with matcing DLCI channel address */
static struct modem_cmux_dlci *modem_cmux_dlci_find(struct modem_cmux *cmux, uint16_t dlci_address)
{
	for (uint16_t i = 0; i < cmux->dlcis_size; i++) {
		if (cmux->dlcis[i].allocated == false) {
			continue;
		}

		if (cmux->dlcis[i].dlci_address != dlci_address) {
			continue;
		}

		return &cmux->dlcis[i];
	}

	return NULL;
}

static void modem_cmux_dlci_deinit(struct modem_cmux_dlci *dlci)
{
	/* Notify pipe closed */
	modem_cmux_dlci_pipe_raise_event(dlci, MODEM_PIPE_EVENT_CLOSED);

	/* Release pipe */
	modem_pipe_callback_set(dlci->pipe, NULL, NULL);

	dlci->dlci_address = 0;
	dlci->cmux = NULL;
	dlci->pipe = NULL;
	dlci->pipe_callback = NULL;
	dlci->pipe_callback_user_data = NULL;
	dlci->state = MODEM_CMUX_DLCI_STATE_CLOSED;
}

static void modem_cmux_dlci_close_all(struct modem_cmux *cmux)
{
	struct modem_cmux_dlci *dlci;

	for (uint16_t i = 0; i < cmux->dlcis_size; i++) {
		/* Ease readability */
		dlci = &cmux->dlcis[i];

		/* Check if allocated */
		if (dlci->allocated == false) {
			continue;
		}

		/* Check if already closed */
		if (dlci->state == MODEM_CMUX_DLCI_STATE_CLOSED) {
			continue;
		}

		/* Deinit CMUX DLCI channel context and pipe */
		modem_cmux_dlci_deinit(dlci);

		/* Free DLCI channel context */
		modem_cmux_dlci_free(cmux, dlci);
	}
}

static void modem_cmux_raise_event(struct modem_cmux *cmux, struct modem_cmux_event event)
{
	/* Validate event handler set */
	if (cmux->callback == NULL) {
		return;
	}

	/* Invoke event handler */
	cmux->callback(cmux, event, cmux->user_data);
}

static void modem_cmux_bus_callback(struct modem_pipe *pipe, enum modem_pipe_event event,
				    void *user_data)
{
	struct modem_cmux *cmux = (struct modem_cmux *)user_data;

	if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
		k_work_schedule(&cmux->process_received.dwork, cmux->receive_timeout);
	}
}

static uint16_t modem_cmux_transmit_frame(struct modem_cmux *cmux,
					  const struct modem_cmux_frame *frame, bool allow_partial)
{
	uint8_t byte;
	uint8_t fcs;
	uint16_t space;
	uint16_t data_len;

	k_mutex_lock(&cmux->transmit_rb_lock, K_FOREVER);

	space = ring_buf_space_get(&cmux->transmit_rb);

	if (space < MODEM_CMUX_FRAME_SIZE_MAX) {
		k_mutex_unlock(&cmux->transmit_rb_lock);

		return 0;
	}

	space -= MODEM_CMUX_FRAME_SIZE_MAX;

	if ((allow_partial == false) && (space < frame->data_len)) {
		k_mutex_unlock(&cmux->transmit_rb_lock);

		return 0;
	}

	data_len = (space < frame->data_len) ? space : frame->data_len;

	/* SOF */
	byte = 0xF9;

	ring_buf_put(&cmux->transmit_rb, &byte, 1);

	/* DLCI Address (Max 63) */
	byte = 0x01 | (frame->cr << 1) | (frame->dlci_address << 2);

	fcs = crc8(&byte, 1, MODEM_CMUX_FCS_POLYNOMIAL, MODEM_CMUX_FCS_INIT_VALUE, true);

	ring_buf_put(&cmux->transmit_rb, &byte, 1);

	/* Frame type and poll/final */
	byte = frame->type | (frame->pf << 4);

	fcs = crc8(&byte, 1, MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);

	ring_buf_put(&cmux->transmit_rb, &byte, 1);

	/* Data length */
	if (127 < data_len) {
		byte = data_len << 1;

		fcs = crc8(&byte, 1, MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);

		ring_buf_put(&cmux->transmit_rb, &byte, 1);

		byte = 0x01 | (data_len >> 7);

		ring_buf_put(&cmux->transmit_rb, &byte, 1);
	} else {
		byte = 0x01 | (data_len << 1);

		ring_buf_put(&cmux->transmit_rb, &byte, 1);
	}

	/* FCS final */
	if (frame->type == MODEM_CMUX_FRAME_TYPE_UIH) {
		fcs = 0xFF - crc8(&byte, 1, MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);
	} else {
		fcs = crc8(&byte, 1, MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);

		fcs = 0xFF - crc8(frame->data, data_len, MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);
	}

	/* Data */
	ring_buf_put(&cmux->transmit_rb, frame->data, data_len);

	/* FCS */
	ring_buf_put(&cmux->transmit_rb, &fcs, 1);

	/* EOF */
	byte = 0xF9;

	ring_buf_put(&cmux->transmit_rb, &byte, 1);

	k_mutex_unlock(&cmux->transmit_rb_lock);

	k_work_submit(&cmux->transmit_work.work);

	return data_len;
}

static void modem_cmux_process_on_frame_received_ua_control(struct modem_cmux *cmux)
{
	/* Update cmux state */
	cmux->state = MODEM_CMUX_STATE_CONNECTED;

	/* Notify cmux state updated */
	struct modem_cmux_event cmux_event = {.dlci_address = 0,
					      .type = MODEM_CMUX_EVENT_CONNECTED};

	modem_cmux_raise_event(cmux, cmux_event);

	return;
}

static void modem_cmux_process_on_frame_received_ua(struct modem_cmux *cmux)
{
	struct modem_cmux_dlci *dlci;

	/* Find DLCI channel context using DLCI address */
	dlci = modem_cmux_dlci_find(cmux, cmux->frame.dlci_address);
	if (dlci == NULL) {
		return;
	}

	if (dlci->state == MODEM_CMUX_DLCI_STATE_OPENING) {
		/* Update CMXU DLCI channel state */
		dlci->state = MODEM_CMUX_DLCI_STATE_OPEN;

		/* Notify CMXU DLCI channel state changed */
		struct modem_cmux_event cmux_event = {.dlci_address = cmux->frame.dlci_address,
						      .type = MODEM_CMUX_EVENT_OPENED};

		modem_cmux_raise_event(cmux, cmux_event);

		return;
	}

	if (dlci->state == MODEM_CMUX_DLCI_STATE_CLOSING) {
		/* Notify CMXU DLCI channel state changed */
		struct modem_cmux_event cmux_event = {.dlci_address = cmux->frame.dlci_address,
						      .type = MODEM_CMUX_EVENT_CLOSED};

		modem_cmux_raise_event(cmux, cmux_event);

		/* Deinit CMUX DLCI channel context and pipe */
		modem_cmux_dlci_deinit(dlci);

		/* Free DLCI channel context */
		modem_cmux_dlci_free(cmux, dlci);

		return;
	}
}

static void modem_cmux_acknowledge_received_frame(struct modem_cmux *cmux)
{
	struct modem_cmux_command *command;
	struct modem_cmux_frame frame;
	uint8_t data[8];

	if (sizeof(data) < cmux->frame.data_len) {
		LOG_WRN("Command acknowledge buffer overrun");

		return;
	}

	memcpy(&frame, &cmux->frame, sizeof(cmux->frame));
	memcpy(data, cmux->frame.data, cmux->frame.data_len);

	modem_cmux_wrap_command(&command, data, cmux->frame.data_len);

	command->type.cr = 0;
	frame.data = data;
	frame.data_len = cmux->frame.data_len;

	if (modem_cmux_transmit_frame(cmux, &frame, false) < 1) {
		LOG_WRN("Command acknowledge buffer overrun");
	}
}

static void modem_cmux_process_on_frame_received_uih_control(struct modem_cmux *cmux)
{
	struct modem_cmux_command *command;

	if (modem_cmux_wrap_command(&command, cmux->frame.data, cmux->frame.data_len) < 0) {
		LOG_WRN("Invalid command");

		return;
	}

	/* Close down */
	if ((command->type.value == MODEM_CMUX_COMMAND_CLD) && (command->type.cr == 1) &&
	    (cmux->state == MODEM_CMUX_STATE_DISCONNECTING)) {
		/* Update CMUX state */
		cmux->state = MODEM_CMUX_STATE_DISCONNECTED;

		/* Notify CMUX state changed */
		struct modem_cmux_event cmux_event = {.dlci_address = 0,
						      .type = MODEM_CMUX_EVENT_DISCONNECTED};

		modem_cmux_raise_event(cmux, cmux_event);

		/* Release bus pipe */
		modem_pipe_callback_set(cmux->pipe, NULL, NULL);
		cmux->pipe = NULL;

		return;
	}

	/* Modem status command */
	if ((command->type.value == MODEM_CMUX_COMMAND_MSC) && (command->type.cr == 1) &&
	    (command->type.ea == 1)) {
		modem_cmux_acknowledge_received_frame(cmux);

		return;
	}

	/* Notify unknown frame */
	modem_cmux_log_unknown_frame(cmux);
}

static void modem_cmux_process_on_frame_received_uih(struct modem_cmux *cmux)
{
	struct modem_cmux_dlci *dlci;

	/* Find DLCI channel context using DLCI address */
	dlci = modem_cmux_dlci_find(cmux, cmux->frame.dlci_address);
	if (dlci == NULL) {
		return;
	}

	/* Copy data to DLCI channel receive buffer */
	k_mutex_lock(&dlci->receive_rb_lock, K_FOREVER);

	ring_buf_put(&dlci->receive_rb, cmux->frame.data, cmux->frame.data_len);

	k_mutex_unlock(&dlci->receive_rb_lock);

	/* Notify data received */
	modem_cmux_dlci_pipe_raise_event(dlci, MODEM_PIPE_EVENT_RECEIVE_READY);

	return;
}

static void modem_cmux_process_on_frame_received(struct modem_cmux *cmux)
{
	if (cmux->frame.dlci_address == 0) {
		switch (cmux->frame.type) {
		case MODEM_CMUX_FRAME_TYPE_UA:
			modem_cmux_process_on_frame_received_ua_control(cmux);
			break;
		case MODEM_CMUX_FRAME_TYPE_UIH:
			modem_cmux_process_on_frame_received_uih_control(cmux);
			break;
		default:
			break;
		}

		return;
	}

	switch (cmux->frame.type) {
	case MODEM_CMUX_FRAME_TYPE_UA:
		modem_cmux_process_on_frame_received_ua(cmux);
		break;
	case MODEM_CMUX_FRAME_TYPE_UIH:
		modem_cmux_process_on_frame_received_uih(cmux);
		break;
	default:
		break;
	}
}

static void modem_cmux_process_received_byte(struct modem_cmux *cmux, uint8_t byte)
{
	uint8_t fcs;
	static const uint8_t resync[3] = {0xF9, 0xF9, 0xF9};

	switch (cmux->receive_state) {
	case MODEM_CMUX_RECEIVE_STATE_SOF:
		if (byte == 0xF9) {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_ADDRESS;

			break;
		}

		/* Send resync flags */
		modem_pipe_transmit(cmux->pipe, resync, sizeof(resync));

		/* Await resync flags */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_0;

		break;

	case MODEM_CMUX_RECEIVE_STATE_RESYNC_0:
		if (byte == 0xF9) {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_1;
		}

		break;

	case MODEM_CMUX_RECEIVE_STATE_RESYNC_1:
		if (byte == 0xF9) {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_2;
		} else {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_0;
		}

		break;

	case MODEM_CMUX_RECEIVE_STATE_RESYNC_2:
		if (byte == 0xF9) {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_3;
		} else {
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_RESYNC_0;
		}

		break;

	case MODEM_CMUX_RECEIVE_STATE_RESYNC_3:
		if (byte == 0xF9) {
			break;
		}

	case MODEM_CMUX_RECEIVE_STATE_ADDRESS:
		/* Initialize */
		cmux->receive_buf_len = 0;
		cmux->frame_header_len = 0;

		/* Store header for FCS */
		cmux->frame_header[cmux->frame_header_len] = byte;
		cmux->frame_header_len++;

		/* Get CR */
		cmux->frame.cr = (byte & 0x02) ? true : false;

		/* Get DLCI address */
		cmux->frame.dlci_address = (byte >> 2) & 0x3F;

		/* Await control */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_CONTROL;

		break;

	case MODEM_CMUX_RECEIVE_STATE_CONTROL:
		/* Store header for FCS */
		cmux->frame_header[cmux->frame_header_len] = byte;
		cmux->frame_header_len++;

		/* Get PF */
		cmux->frame.pf = (byte & MODEM_CMUX_PF) ? true : false;

		/* Get frame type */
		cmux->frame.type = byte & (~MODEM_CMUX_PF);

		/* Await data length */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_LENGTH;

		break;

	case MODEM_CMUX_RECEIVE_STATE_LENGTH:
		/* Store header for FCS */
		cmux->frame_header[cmux->frame_header_len] = byte;
		cmux->frame_header_len++;

		/* Get first 7 bits of data length */
		cmux->frame.data_len = (byte >> 1);

		/* Check if length field continues */
		if ((byte & MODEM_CMUX_EA) == 0) {
			/* Await continued length field */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_LENGTH_CONT;

			break;
		}

		/* Check if no data field */
		if (cmux->frame.data_len == 0) {
			/* Await FCS */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_FCS;

			break;
		}

		/* Await data */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_DATA;

		break;

	case MODEM_CMUX_RECEIVE_STATE_LENGTH_CONT:
		/* Store header for FCS */
		cmux->frame_header[cmux->frame_header_len] = byte;
		cmux->frame_header_len++;

		/* Get last 8 bits of data length */
		cmux->frame.data_len |= ((uint16_t)byte) << 7;

		/* Await data */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_DATA;

		break;

	case MODEM_CMUX_RECEIVE_STATE_DATA:
		/* Copy byte to data */
		cmux->receive_buf[cmux->receive_buf_len] = byte;
		cmux->receive_buf_len++;

		/* Check if datalen reached */
		if (cmux->frame.data_len == cmux->receive_buf_len) {
			/* Await FCS */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_FCS;

			break;
		}

		/* Check if receive buffer overrun */
		if (cmux->receive_buf_len == cmux->receive_buf_size) {
			LOG_DBG("Receive buf overrun");

			/* Drop frame */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_EOF;

			break;
		}

		break;

	case MODEM_CMUX_RECEIVE_STATE_FCS:
		/* Compute FCS */
		if (cmux->frame.type == MODEM_CMUX_FRAME_TYPE_UIH) {
			fcs = 0xFF - crc8(cmux->frame_header, cmux->frame_header_len,
					  MODEM_CMUX_FCS_POLYNOMIAL, MODEM_CMUX_FCS_INIT_VALUE,
					  true);
		} else {
			fcs = crc8(cmux->frame_header, cmux->frame_header_len,
				   MODEM_CMUX_FCS_POLYNOMIAL, MODEM_CMUX_FCS_INIT_VALUE, true);

			fcs = 0xFF - crc8(cmux->frame.data, cmux->frame.data_len,
					  MODEM_CMUX_FCS_POLYNOMIAL, fcs, true);
		}

		/* Validate FCS */
		if (fcs != byte) {
			LOG_DBG("Frame FCS err");

			/* Drop frame */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_SOF;

			break;
		}

		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_EOF;

		break;

	case MODEM_CMUX_RECEIVE_STATE_EOF:
		/* Validate byte is EOF */
		if (byte != 0xF9) {
			/* Unexpected byte */
			cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_SOF;

			break;
		}

		LOG_DBG("Received frame");

		/* Process frame */
		cmux->frame.data = cmux->receive_buf;
		modem_cmux_process_on_frame_received(cmux);

		/* Await start of next frame */
		cmux->receive_state = MODEM_CMUX_RECEIVE_STATE_SOF;

		break;

	default:
		break;
	}
}

static void modem_cmux_process_received(struct k_work *item)
{
	struct modem_cmux_work_delayable *cmux_process = (struct modem_cmux_work_delayable *)item;
	struct modem_cmux *cmux = cmux_process->cmux;
	uint8_t buf[16];
	int ret;

	/* Receive data from pipe */
	ret = modem_pipe_receive(cmux->pipe, buf, sizeof(buf));

	if (ret < 1) {
		return;
	}

	/* Process received data */
	for (uint16_t i = 0; i < (uint16_t)ret; i++) {
		modem_cmux_process_received_byte(cmux, buf[i]);
	}

	/* Reschedule received work */
	k_work_schedule(&cmux->process_received.dwork, K_NO_WAIT);
}

static void modem_cmux_transmit_handler(struct k_work *item)
{
	struct modem_cmux_work *cmux_work = (struct modem_cmux_work *)item;
	struct modem_cmux *cmux = cmux_work->cmux;
	uint8_t *reserved;
	uint32_t reserved_size;
	int ret;

	k_mutex_lock(&cmux->transmit_rb_lock, K_FOREVER);

	/* Reserve data to transmit from transmit ring buffer */
	reserved_size = ring_buf_get_claim(&cmux->transmit_rb, &reserved, UINT32_MAX);

	/* Transmit reserved data */
	ret = modem_pipe_transmit(cmux->pipe, reserved, reserved_size);

	if (ret < 1) {
		ring_buf_get_finish(&cmux->transmit_rb, 0);

		k_mutex_unlock(&cmux->transmit_rb_lock);

		return;
	}

	/* Release remaining reserved data */
	ring_buf_get_finish(&cmux->transmit_rb, ret);

	/* Resubmit transmit work if data remains */
	if (ring_buf_is_empty(&cmux->transmit_rb) == false) {
		k_work_submit(&cmux->transmit_work.work);
	}

	k_mutex_unlock(&cmux->transmit_rb_lock);
}

static int modem_cmux_dlci_pipe_callback_set(struct modem_pipe *pipe, modem_pipe_callback handler,
					     void *user_data)
{
	struct modem_cmux_dlci *dlci = (struct modem_cmux_dlci *)pipe->data;
	struct modem_cmux *cmux = dlci->cmux;

	k_mutex_lock(&cmux->lock, K_FOREVER);

	dlci->pipe_callback = handler;
	dlci->pipe_callback_user_data = user_data;

	k_mutex_unlock(&cmux->lock);

	return 0;
}

static int modem_cmux_dlci_pipe_transmit(struct modem_pipe *pipe, const uint8_t *buf, uint32_t size)
{
	struct modem_cmux_dlci *dlci = (struct modem_cmux_dlci *)pipe->data;
	struct modem_cmux *cmux = dlci->cmux;
	struct modem_cmux_frame frame = {
		.dlci_address = dlci->dlci_address,
		.cr = false,
		.pf = false,
		.type = MODEM_CMUX_FRAME_TYPE_UIH,
		.data = buf,
		.data_len = size,
	};

	return modem_cmux_transmit_frame(cmux, &frame, true);
}

static int modem_cmux_dlci_pipe_receive(struct modem_pipe *pipe, uint8_t *buf, uint32_t size)
{
	struct modem_cmux_dlci *dlci = (struct modem_cmux_dlci *)pipe->data;
	uint32_t ret;

	k_mutex_lock(&dlci->receive_rb_lock, K_FOREVER);

	ret = ring_buf_get(&dlci->receive_rb, buf, size);

	k_mutex_unlock(&dlci->receive_rb_lock);

	return (int)ret;
}

struct modem_pipe_driver_api modem_cmux_dlci_pipe_api = {
	.callback_set = modem_cmux_dlci_pipe_callback_set,
	.transmit = modem_cmux_dlci_pipe_transmit,
	.receive = modem_cmux_dlci_pipe_receive,
};

static void modem_cmux_dlci_init(struct modem_cmux *cmux, struct modem_cmux_dlci *dlci,
				 const struct modem_cmux_dlci_config *config,
				 struct modem_pipe *pipe)
{
	dlci->dlci_address = config->dlci_address;
	dlci->cmux = cmux;
	dlci->pipe = pipe;
	dlci->pipe->data = dlci;
	dlci->pipe->api = &modem_cmux_dlci_pipe_api;
	dlci->pipe_callback = NULL;
	dlci->pipe_callback_user_data = NULL;
	ring_buf_init(&dlci->receive_rb, config->receive_buf_size, config->receive_buf);
	k_mutex_init(&dlci->receive_rb_lock);
	dlci->state = MODEM_CMUX_DLCI_STATE_CLOSED;
}

int modem_cmux_init(struct modem_cmux *cmux, const struct modem_cmux_config *config)
{
	/* Validate arguments */
	if (cmux == NULL || config == NULL) {
		return -EINVAL;
	}

	/* Validate config */
	if ((config->callback == NULL) || (config->dlcis == NULL) || (config->dlcis_size == 0) ||
	    (config->receive_buf == NULL) || (config->receive_buf_size == 0) ||
	    (config->transmit_buf == NULL) || (config->transmit_buf_size == 0)) {
		return -EINVAL;
	}

	/* Clear CMUX context */
	memset(cmux, 0x00, sizeof(*cmux));

	/* Clear DLCI channel contexts */
	for (uint16_t i = 0; i < config->dlcis_size; i++) {
		memset(&config->dlcis[i], 0x00, sizeof(config->dlcis[0]));
	}

	/* Copy configuration to context */
	cmux->callback = config->callback;
	cmux->user_data = config->user_data;
	cmux->dlcis = config->dlcis;
	cmux->dlcis_size = config->dlcis_size;
	cmux->receive_buf = config->receive_buf;
	cmux->receive_buf_size = config->receive_buf_size;
	cmux->receive_timeout = config->receive_timeout;

	/* Initialize delayable work */
	cmux->process_received.cmux = cmux;
	k_work_init_delayable(&cmux->process_received.dwork, modem_cmux_process_received);

	/* Initialize work */
	cmux->transmit_work.cmux = cmux;
	k_work_init(&cmux->transmit_work.work, modem_cmux_transmit_handler);

	ring_buf_init(&cmux->transmit_rb, config->transmit_buf_size, config->transmit_buf);

	k_mutex_init(&cmux->transmit_rb_lock);

	return 0;
}

int modem_cmux_connect(struct modem_cmux *cmux, struct modem_pipe *pipe)
{
	int ret;

	struct modem_cmux_frame frame = {
		.dlci_address = 0,
		.cr = true,
		.pf = true,
		.type = MODEM_CMUX_FRAME_TYPE_SABM,
		.data = NULL,
		.data_len = 0,
	};

	k_mutex_lock(&cmux->lock, K_FOREVER);

	/* Verify cmux disconnected */
	if (cmux->state != MODEM_CMUX_STATE_DISCONNECTED) {
		k_mutex_unlock(&cmux->lock);

		return -EPERM;
	}

	/* Attach bus pipe */
	cmux->pipe = pipe;

	ret = modem_pipe_callback_set(cmux->pipe, modem_cmux_bus_callback, cmux);

	if (ret < 0) {
		k_mutex_unlock(&cmux->lock);

		return ret;
	}

	/* Send connection request */
	ret = modem_cmux_transmit_frame(cmux, &frame, false);

	if (ret == 0) {
		k_mutex_unlock(&cmux->lock);

		return ret;
	}

	/* Update CMUX state */
	cmux->state = MODEM_CMUX_STATE_CONNECTING;

	k_mutex_unlock(&cmux->lock);

	return 0;
}

int modem_cmux_dlci_open(struct modem_cmux *cmux, const struct modem_cmux_dlci_config *config,
			 struct modem_pipe *pipe)
{
	int ret;
	struct modem_cmux_dlci *dlci;

	struct modem_cmux_frame frame = {
		.dlci_address = config->dlci_address,
		.cr = true,
		.pf = true,
		.type = MODEM_CMUX_FRAME_TYPE_SABM,
		.data = NULL,
		.data_len = 0,
	};

	/* Validate arguments */
	if ((cmux == NULL) || (config == NULL) || (pipe == NULL)) {
		return -EINVAL;
	}

	/* Validate configuration */
	if ((config->receive_buf == NULL) ||
	    (config->receive_buf_size == MODEM_CMUX_DLCI_RECEIVE_BUF_SIZE_MIN) ||
	    (MODEM_CMUX_DLCI_ADDRESS_MAX < config->dlci_address) ||
	    (config->dlci_address < MODEM_CMUX_DLCI_ADDRESS_MIN)) {
		return -EINVAL;
	}

	k_mutex_lock(&cmux->lock, K_FOREVER);

	/* Verify cmux connected */
	if (cmux->state != MODEM_CMUX_STATE_CONNECTED) {
		k_mutex_unlock(&cmux->lock);
		return -EPERM;
	}

	/* Verify channel not already open */
	dlci = modem_cmux_dlci_find(cmux, config->dlci_address);
	if (dlci != NULL) {
		k_mutex_unlock(&cmux->lock);
		return -EPERM;
	}

	/* Try to alloc DLCI channel context */
	dlci = modem_cmux_dlci_alloc(cmux);
	if (dlci == NULL) {
		k_mutex_unlock(&cmux->lock);
		return -ENOMEM;
	}

	/* Initialize DLCI channel context and pipe */
	modem_cmux_dlci_init(cmux, dlci, config, pipe);

	/* Update DLCI channel context state */
	dlci->state = MODEM_CMUX_DLCI_STATE_OPENING;

	/* Try to open DLCI channel */
	ret = modem_cmux_transmit_frame(cmux, &frame, false);
	if (ret < 0) {
		modem_cmux_dlci_free(cmux, dlci);
		k_mutex_unlock(&cmux->lock);
		return ret;
	}

	k_mutex_unlock(&cmux->lock);
	return 0;
}

int modem_cmux_dlci_close(struct modem_pipe *pipe)
{
	struct modem_cmux_dlci *dlci = (struct modem_cmux_dlci *)pipe->data;
	struct modem_cmux *cmux = dlci->cmux;

	int ret;

	/* Validate argument */
	if ((pipe == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&cmux->lock, K_FOREVER);

	/* Validate pipe is open */
	if (dlci->state != MODEM_CMUX_DLCI_STATE_OPEN) {
		k_mutex_unlock(&cmux->lock);
		return -EPERM;
	}

	/* Try to close DLCI channel */
	struct modem_cmux_frame frame = {
		.dlci_address = dlci->dlci_address,
		.cr = true,
		.pf = true,
		.type = MODEM_CMUX_FRAME_TYPE_DISC,
		.data = NULL,
		.data_len = 0,
	};

	/* Update CMUX DLCI channel state */
	dlci->state = MODEM_CMUX_DLCI_STATE_CLOSING;

	ret = modem_cmux_transmit_frame(cmux, &frame, false);
	if (ret < 0) {
		k_mutex_unlock(&cmux->lock);
		return ret;
	}

	k_mutex_unlock(&cmux->lock);
	return 0;
}

int modem_cmux_disconnect(struct modem_cmux *cmux)
{
	int ret;
	uint8_t data[2];
	struct modem_cmux_command *command;

	command = modem_cmux_command_wrap(data);
	command->type.ea = 1;
	command->type.cr = 1;
	command->type.value = MODEM_CMUX_COMMAND_CLD;
	command->length.ea = 1;
	command->length.value = 0;

	struct modem_cmux_frame frame = {
		.dlci_address = 0,
		.cr = true,
		.pf = false,
		.type = MODEM_CMUX_FRAME_TYPE_UIH,
		.data = data,
		.data_len = sizeof(data),
	};

	k_mutex_lock(&cmux->lock, K_FOREVER);

	/* Verify cmux connected */
	if (cmux->state != MODEM_CMUX_STATE_CONNECTED) {
		k_mutex_unlock(&cmux->lock);
		return -EPERM;
	}

	/* Close all open DLCI channels */
	modem_cmux_dlci_close_all(cmux);

	/* Update state */
	cmux->state = MODEM_CMUX_STATE_DISCONNECTING;

	/* Request disconnect */
	ret = modem_cmux_transmit_frame(cmux, &frame, false);
	if (ret < 0) {
		k_mutex_unlock(&cmux->lock);
		return ret;
	}

	/* Lazy await disconnect */
	k_msleep(300);

	/* Cancel work */
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&cmux->process_received.dwork, &sync);

	/* Release bus pipe */
	if (cmux->pipe != NULL) {
		modem_pipe_callback_set(cmux->pipe, NULL, NULL);
	}

	k_mutex_unlock(&cmux->lock);

	return 0;
}
