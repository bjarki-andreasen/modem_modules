/*
 * Copyright (c) 2023 Bjarki Arge Andreasen
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/cmux.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/modem//backend/uart.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_cellular);

#include <string.h>
#include <stdlib.h>

enum modem_cellular_state {
	MODEM_CELLULAR_STATE_IDLE = 0,
	MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT,
	MODEM_CELLULAR_STATE_CONNECT_CMUX,
	MODEM_CELLULAR_STATE_OPEN_DLCI1,
	MODEM_CELLULAR_STATE_OPEN_DLCI2,
	MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT,
	MODEM_CELLULAR_STATE_REGISTER,
	MODEM_CELLULAR_STATE_ROAMING,
	MODEM_CELLULAR_STATE_CLOSE_DLCI2,
	MODEM_CELLULAR_STATE_CLOSE_DLCI1,
	MODEM_CELLULAR_STATE_DISCONNECT_CMUX,
};

enum modem_cellular_event {
	MODEM_CELLULAR_EVENT_RESUME = 0,
	MODEM_CELLULAR_EVENT_SUSPEND,
	MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS,
	MODEM_CELLULAR_EVENT_SCRIPT_FAILED,
	MODEM_CELLULAR_EVENT_CMUX_CONNECTED,
	MODEM_CELLULAR_EVENT_DLCI1_OPENED,
	MODEM_CELLULAR_EVENT_DLCI1_CLOSED,
	MODEM_CELLULAR_EVENT_DLCI2_OPENED,
	MODEM_CELLULAR_EVENT_DLCI2_CLOSED,
	MODEM_CELLULAR_EVENT_CMUX_DISCONNECTED,
	MODEM_CELLULAR_EVENT_TIMEOUT,
};

struct modem_cellular_data {
	/* UART backend */
	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[512];
	uint8_t uart_backend_transmit_buf[512];

	/* CMUX */
	struct modem_cmux cmux;
	uint8_t cmux_receive_buf[128];
	uint8_t cmux_transmit_buf[256];
	struct modem_cmux_dlci dlci1;
	struct modem_cmux_dlci dlci2;
	struct modem_pipe *dlci1_pipe;
	struct modem_pipe *dlci2_pipe;
	uint8_t dlci1_receive_buf[128];
	uint8_t dlci2_receive_buf[256];

	/* Modem chat */
	struct modem_chat chat;
	uint8_t chat_receive_buf[128];
	uint8_t chat_delimiter[1];
	uint8_t chat_filter[1];
	uint8_t *chat_argv[32];

	/* Status */
	uint8_t imei[15];
	uint8_t hwinfo[64];
	uint8_t access_tech;
	uint8_t registration_status;
	uint8_t packet_service_attached;

	/* PPP */
	struct modem_ppp *ppp;

	enum modem_cellular_state state;
	const struct device *dev;
	struct k_work_delayable timeout_work;

	/* Event dispatcher */
	struct k_work event_dispatch_work;
	uint8_t event_buf[8];
	struct ring_buf event_rb;
	struct k_mutex event_rb_lock;
};

struct modem_cellular_config {
	const struct device *uart;
};

static const char *modem_cellular_state_str[] = {
	"idle",
	"run init script",
	"connect cmux",
	"open dlci1",
	"open dlci2",
	"run dial script",
	"register",
	"roaming",
	"close dlci2",
	"close dlci1",
	"disconnect cmux"
};

static const char *modem_cellular_event_str[] = {
	"resume",
	"suspend",
	"script success",
	"script failed",
	"cmux connected",
	"dlci1 opened",
	"dlci1 closed",
	"dlci2 opened",
	"dlci2 closed",
	"cmux disconnected",
	"timeout"
};

static void modem_cellular_enter_state(struct modem_cellular_data *data,
				       enum modem_cellular_state state);

static void modem_cellular_delegate_event(struct modem_cellular_data *data,
					  enum modem_cellular_event evt);

static void modem_cellular_event_handler(struct modem_cellular_data *data,
					 enum modem_cellular_event evt);

static void modem_cellular_dlci1_pipe_handler(struct modem_pipe *pipe,
					      enum modem_pipe_event event,
					      void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event)
	{
	case MODEM_PIPE_EVENT_OPENED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI1_OPENED);

		break;

	case MODEM_PIPE_EVENT_CLOSED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI1_CLOSED);

		break;

	default:
		break;
	}
}

static void modem_cellular_dlci2_pipe_handler(struct modem_pipe *pipe,
					      enum modem_pipe_event event,
					      void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event)
	{
	case MODEM_PIPE_EVENT_OPENED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI2_OPENED);

		break;

	case MODEM_PIPE_EVENT_CLOSED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI2_CLOSED);

		break;

	default:
		break;
	}
}

static void modem_cellular_chat_callback_handler(struct modem_chat *chat,
						 enum modem_chat_script_result result,
						 void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (result == MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS);
	} else {
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_SCRIPT_FAILED);
	}
}

static void modem_cellular_chat_on_imei(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	if (strlen(argv[1]) != 15) {
		return;
	}

	for (uint8_t i = 0; i < 15; i++) {
		data->imei[i] = argv[1][i] - '0';
	}
}

static void modem_cellular_chat_on_cgmm(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	strncpy(data->hwinfo, argv[1], sizeof(data->hwinfo) - 1);
}

static void modem_cellular_chat_on_creg(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 3) {
		return;
	}

	data->access_tech = atoi(argv[1]);
	data->registration_status = atoi(argv[2]);
}

static void modem_cellular_chat_on_cgatt(struct modem_chat *chat, char **argv, uint16_t argc,
					 void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	data->packet_service_attached = atoi(argv[1]);
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", NULL);
MODEM_CHAT_MATCH_DEFINE(imei_match, "", "", modem_cellular_chat_on_imei);
MODEM_CHAT_MATCH_DEFINE(cgmm_match, "", "", modem_cellular_chat_on_cgmm);
MODEM_CHAT_MATCH_DEFINE(creg_match, "+CREG: ", ",", modem_cellular_chat_on_creg);
MODEM_CHAT_MATCH_DEFINE(cgatt_match, "+CGATT: ", ",", modem_cellular_chat_on_cgatt);
MODEM_CHAT_MATCH_DEFINE(connect_match, "CONNECT ", "", NULL);

MODEM_CHAT_MATCHES_DEFINE(abort_matches, MODEM_CHAT_MATCH("ERROR", "", NULL),
			  MODEM_CHAT_MATCH("BUSY", "", NULL),
			  MODEM_CHAT_MATCH("NO ANSWER", "", NULL),
			  MODEM_CHAT_MATCH("NO CARRIER", "", NULL),
			  MODEM_CHAT_MATCH("NO DIALTONE", "", NULL));

MODEM_CHAT_SCRIPT_CMDS_DEFINE(init_chat_script_cmds, MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATH", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGSN", imei_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGMM", cgmm_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMUX=0,0,5,127,10,3,30,10,2",
							 ok_match));

MODEM_CHAT_SCRIPT_DEFINE(init_chat_script, init_chat_script_cmds, abort_matches,
			 modem_cellular_chat_callback_handler, 10);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(net_stat_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", creg_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGATT?", cgatt_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(net_stat_chat_script, net_stat_chat_script_cmds, abort_matches,
			 modem_cellular_chat_callback_handler, 10);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(connect_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\","
							 "\""CONFIG_MODEM_CELLULAR_APN"\","
							 "\""CONFIG_MODEM_CELLULAR_USERNAME"\","
							 "\""CONFIG_MODEM_CELLULAR_PASSWORD"\"",
							 ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATD*99#", connect_match));

MODEM_CHAT_SCRIPT_DEFINE(connect_chat_script, connect_chat_script_cmds, abort_matches,
			 modem_cellular_chat_callback_handler, 120);

static void modem_cellular_log_state_changed(enum modem_cellular_state last_state,
					     enum modem_cellular_state new_state)
{
	LOG_INF("switch from %s to %s", modem_cellular_state_str[last_state],
		modem_cellular_state_str[new_state]);
}

static void modem_cellular_log_event(enum modem_cellular_event evt)
{
	LOG_INF("event %s", modem_cellular_event_str[evt]);
}

static void modem_cellular_start_timer(struct modem_cellular_data *data, k_timeout_t timeout)
{
	k_work_schedule(&data->timeout_work, timeout);
}

static void modem_cellular_stop_timer(struct modem_cellular_data *data)
{
	k_work_cancel_delayable(&data->timeout_work);
}

static void modem_cellular_timeout_handler(struct k_work *item)
{
	struct modem_cellular_data *data =
		CONTAINER_OF(item, struct modem_cellular_data, timeout_work);

	modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_TIMEOUT);
}

static void modem_cellular_event_dispatch_handler(struct k_work *item)
{
	struct modem_cellular_data *data =
		CONTAINER_OF(item, struct modem_cellular_data, event_dispatch_work);

	uint8_t events[sizeof(data->event_buf)];
	uint8_t events_cnt;

	k_mutex_lock(&data->event_rb_lock, K_FOREVER);

	events_cnt = (uint8_t)ring_buf_get(&data->event_rb, events, sizeof(data->event_buf));

	k_mutex_unlock(&data->event_rb_lock);

	for (uint8_t i = 0; i < events_cnt; i++) {
		modem_cellular_event_handler(data, (enum modem_cellular_event)events[i]);
	}
}

static void modem_cellular_delegate_event(struct modem_cellular_data *data,
					  enum modem_cellular_event evt)
{
	k_mutex_lock(&data->event_rb_lock, K_FOREVER);

	ring_buf_put(&data->event_rb, (uint8_t *)&evt, 1);

	k_mutex_unlock(&data->event_rb_lock);

	k_work_submit(&data->event_dispatch_work);
}

static void modem_cellular_idle_event_handler(struct modem_cellular_data *data,
					      enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_RESUME:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_init_script_state_enter(struct modem_cellular_data *data)
{
	if (modem_pipe_open(data->uart_pipe) < 0) {
		return -EAGAIN;
	}

	if (modem_chat_attach(&data->chat, data->uart_pipe) < 0) {
		return -EAGAIN;
	}

	return modem_chat_script_run(&data->chat, &init_chat_script);
}

static void modem_cellular_run_init_script_event_handler(struct modem_cellular_data *data,
							 enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CONNECT_CMUX);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_init_script_state_leave(struct modem_cellular_data *data)
{
	net_if_set_link_addr(modem_ppp_get_iface(data->ppp), data->imei, ARRAY_SIZE(data->imei),
			     NET_LINK_UNKNOWN);

	modem_chat_release(&data->chat);

	return 0;
}

static int modem_cellular_on_connect_cmux_state_enter(struct modem_cellular_data *data)
{
	if (modem_cmux_attach(&data->cmux, data->uart_pipe) < 0) {
		return -EAGAIN;
	}

	return modem_cmux_connect_async(&data->cmux);
}

static void modem_cellular_connect_cmux_event_handler(struct modem_cellular_data *data,
						      enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_CMUX_CONNECTED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_OPEN_DLCI1);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci1_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_attach(data->dlci1_pipe, modem_cellular_dlci1_pipe_handler, data);

	return modem_pipe_open_async(data->dlci1_pipe);
}

static void modem_cellular_open_dlci1_event_handler(struct modem_cellular_data *data,
						    enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_DLCI1_OPENED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_OPEN_DLCI2);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci1_state_leave(struct modem_cellular_data *data)
{
	modem_pipe_release(data->dlci1_pipe);

	return 0;
}

static int modem_cellular_on_open_dlci2_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_attach(data->dlci2_pipe, modem_cellular_dlci2_pipe_handler, data);

	return modem_pipe_open_async(data->dlci2_pipe);
}

static void modem_cellular_open_dlci2_event_handler(struct modem_cellular_data *data,
						    enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_DLCI2_OPENED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci2_state_leave(struct modem_cellular_data *data)
{
	modem_pipe_release(data->dlci2_pipe);

	return 0;
}

static int modem_cellular_on_run_dial_script_state_enter(struct modem_cellular_data *data)
{
	if (modem_chat_attach(&data->chat, data->dlci2_pipe) < 0) {
		return -EAGAIN;
	}

	return modem_chat_script_run(&data->chat, &connect_chat_script);
}

static void modem_cellular_run_dial_script_event_handler(struct modem_cellular_data *data,
							 enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_REGISTER);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_dial_script_state_leave(struct modem_cellular_data *data)
{
	modem_chat_release(&data->chat);

	return modem_ppp_attach(data->ppp, data->dlci2_pipe);
}

static int modem_cellular_on_register_state_enter(struct modem_cellular_data *data)
{
	if (modem_chat_attach(&data->chat, data->dlci1_pipe) < 0) {
		return -EAGAIN;
	}

	modem_cellular_start_timer(data, K_SECONDS(2));

	return modem_chat_script_run(&data->chat, &net_stat_chat_script);
}

static bool modem_cellular_is_registered(struct modem_cellular_data *data)
{
	return (data->registration_status == 5) && (data->packet_service_attached == 1);
}

static void modem_cellular_register_event_handler(struct modem_cellular_data *data,
						  enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		if (modem_cellular_is_registered(data)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_ROAMING);
		}

		break;

	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_cellular_start_timer(data, K_SECONDS(2));

		modem_chat_script_run(&data->chat, &net_stat_chat_script);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_register_state_leave(struct modem_cellular_data *data)
{
	modem_cellular_stop_timer(data);

	modem_chat_release(&data->chat);

	return 0;
}

static int modem_cellular_on_roaming_state_enter(struct modem_cellular_data *data)
{
	modem_chat_attach(&data->chat, data->dlci1_pipe);

	modem_chat_script_run(&data->chat, &net_stat_chat_script);

	modem_cellular_start_timer(data, K_SECONDS(4));

	net_ppp_carrier_on(modem_ppp_get_iface(data->ppp));

	return 0;
}

static void modem_cellular_roaming_event_handler(struct modem_cellular_data *data,
						 enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_SUSPEND:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CLOSE_DLCI2);

		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		if (modem_cellular_is_registered(data) == false) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT);
		}

		break;

	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_chat_script_run(&data->chat, &net_stat_chat_script);

		modem_cellular_start_timer(data, K_SECONDS(4));

		break;

	default:
		break;
	}
}

static int modem_cellular_on_roaming_state_leave(struct modem_cellular_data *data)
{
	modem_chat_release(&data->chat);

	modem_cellular_stop_timer(data);

	net_ppp_carrier_off(modem_ppp_get_iface(data->ppp));

	return 0;
}

static int modem_cellular_on_close_dlci2_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_close_async(data->dlci2_pipe);

	return 0;
}

static void modem_cellular_close_dlci2_event_handler(struct modem_cellular_data *data,
						     enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_DLCI2_CLOSED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CLOSE_DLCI1);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_close_dlci1_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_close_async(data->dlci1_pipe);

	return 0;
}

static void modem_cellular_close_dlci1_event_handler(struct modem_cellular_data *data,
						     enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_DLCI1_CLOSED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_DISCONNECT_CMUX);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_disconnect_cmux_state_enter(struct modem_cellular_data *data)
{
	modem_cmux_disconnect_async(&data->cmux);

	return 0;
}

static void modem_cellular_disconnect_cmux_event_handler(struct modem_cellular_data *data,
							 enum modem_cellular_event evt)
{
	switch (evt)
	{
	case MODEM_CELLULAR_EVENT_CMUX_DISCONNECTED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_IDLE);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_state_enter(struct modem_cellular_data *data)
{
	int ret;

	switch (data->state)
	{
	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		ret = modem_cellular_on_run_init_script_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_CONNECT_CMUX:
		ret = modem_cellular_on_connect_cmux_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		ret = modem_cellular_on_open_dlci1_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		ret = modem_cellular_on_open_dlci2_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		ret = modem_cellular_on_run_dial_script_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		ret = modem_cellular_on_register_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_ROAMING:
		ret = modem_cellular_on_roaming_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_CLOSE_DLCI2:
		ret = modem_cellular_on_close_dlci2_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_CLOSE_DLCI1:
		ret = modem_cellular_on_close_dlci1_state_enter(data);

		break;

	case MODEM_CELLULAR_STATE_DISCONNECT_CMUX:
		ret = modem_cellular_on_disconnect_cmux_state_enter(data);

		break;

	default:
		ret = 0;

		break;
	}

	return ret;
}

static int modem_cellular_on_state_leave(struct modem_cellular_data *data)
{
	int ret;

	switch (data->state)
	{
	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		ret = modem_cellular_on_run_init_script_state_leave(data);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		ret = modem_cellular_on_open_dlci1_state_leave(data);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		ret = modem_cellular_on_open_dlci2_state_leave(data);

		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		ret = modem_cellular_on_run_dial_script_state_leave(data);

		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		ret = modem_cellular_on_register_state_leave(data);

		break;

	case MODEM_CELLULAR_STATE_ROAMING:
		ret = modem_cellular_on_roaming_state_leave(data);

		break;

	default:
		ret = 0;

		break;
	}

	return ret;
}

static void modem_cellular_enter_state(struct modem_cellular_data *data,
				       enum modem_cellular_state state)
{
	int ret;

	ret = modem_cellular_on_state_leave(data);

	if (ret < 0) {
		LOG_WRN("failed to leave state");

		return;
	}

	data->state = state;

	ret = modem_cellular_on_state_enter(data);

	if (ret < 0) {
		LOG_WRN("failed to enter state");
	}
}

static void modem_cellular_event_handler(struct modem_cellular_data *data,
					 enum modem_cellular_event evt)
{
	enum modem_cellular_state state;

	state = data->state;

	modem_cellular_log_event(evt);

	switch (data->state)
	{
	case MODEM_CELLULAR_STATE_IDLE:
		modem_cellular_idle_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		modem_cellular_run_init_script_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_CONNECT_CMUX:
		modem_cellular_connect_cmux_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		modem_cellular_open_dlci1_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		modem_cellular_open_dlci2_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		modem_cellular_run_dial_script_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		modem_cellular_register_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_ROAMING:
		modem_cellular_roaming_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_CLOSE_DLCI2:
		modem_cellular_close_dlci2_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_CLOSE_DLCI1:
		modem_cellular_close_dlci1_event_handler(data, evt);

		break;

	case MODEM_CELLULAR_STATE_DISCONNECT_CMUX:
		modem_cellular_disconnect_cmux_event_handler(data, evt);

		break;
	}

	if (state != data->state) {
		modem_cellular_log_state_changed(state, data->state);
	}
}

static void modem_cellular_cmux_handler(struct modem_cmux *cmux, enum modem_cmux_event event,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event)
	{
	case MODEM_CMUX_EVENT_CONNECTED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_CMUX_CONNECTED);

		break;

	case MODEM_CMUX_EVENT_DISCONNECTED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_CMUX_DISCONNECTED);

		break;

	default:
		break;
	}
}

static int modem_cellular_init(const struct device *dev)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)dev->data;
	struct modem_cellular_config *config = (struct modem_cellular_config *)dev->config;

	data->dev = dev;

	k_work_init_delayable(&data->timeout_work, modem_cellular_timeout_handler);

	k_work_init(&data->event_dispatch_work, modem_cellular_event_dispatch_handler);
	ring_buf_init(&data->event_rb, sizeof(data->event_buf), data->event_buf);

	{
		const struct modem_backend_uart_config uart_backend_config = {
			.uart = config->uart,
			.receive_buf = data->uart_backend_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->uart_backend_receive_buf),
			.transmit_buf = data->uart_backend_transmit_buf,
			.transmit_buf_size = ARRAY_SIZE(data->uart_backend_transmit_buf),
		};

		data->uart_pipe = modem_backend_uart_init(&data->uart_backend,
							  &uart_backend_config);
	}

	{
		const struct modem_cmux_config cmux_config = {
			.callback = modem_cellular_cmux_handler,
			.user_data = data,
			.receive_buf = data->cmux_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->cmux_receive_buf),
			.transmit_buf = data->cmux_transmit_buf,
			.transmit_buf_size = ARRAY_SIZE(data->cmux_transmit_buf),
		};

		modem_cmux_init(&data->cmux, &cmux_config);
	}

	{
		const struct modem_cmux_dlci_config dlci1_config = {
			.dlci_address = 1,
			.receive_buf = data->dlci1_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->dlci1_receive_buf),
		};

		data->dlci1_pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci1,
							&dlci1_config);
	}

	{
		const struct modem_cmux_dlci_config dlci2_config = {
			.dlci_address = 2,
			.receive_buf = data->dlci2_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->dlci2_receive_buf),
		};

		data->dlci2_pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci2,
							&dlci2_config);
	}

	{
		const struct modem_chat_config chat_config = {
			.user_data = data,
			.receive_buf = data->chat_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->chat_receive_buf),
			.delimiter = data->chat_delimiter,
			.delimiter_size = ARRAY_SIZE(data->chat_delimiter),
			.filter = data->chat_filter,
			.filter_size = ARRAY_SIZE(data->chat_filter),
			.argv = data->chat_argv,
			.argv_size = ARRAY_SIZE(data->chat_argv),
			.unsol_matches = NULL,
			.unsol_matches_size = 0,
			.process_timeout = K_MSEC(2),
		};

		modem_chat_init(&data->chat, &chat_config);
	}

	modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_RESUME);

	return 0;
}

#define MODEM_CELLULAR_DEVICE(node, inst)						\
	MODEM_PPP_DEFINE(ppp, NULL, 98, 1500, 64, 8);					\
											\
	static struct modem_cellular_data modem_cellular_data_##inst = {		\
		.chat_delimiter = {'\r'},						\
		.chat_filter = {'\n'},							\
		.ppp = &ppp,								\
	};										\
											\
	static struct modem_cellular_config modem_cellular_config_##inst = {		\
		.uart = DEVICE_DT_GET(DT_BUS(node)),					\
	};										\
											\
	DEVICE_DT_DEFINE(node, modem_cellular_init, NULL,				\
			      &modem_cellular_data_##inst,				\
			      &modem_cellular_config_##inst, POST_KERNEL, 99, NULL);

#define MODEM_CELLULAR_DEVICE_FROM_NODE(node) MODEM_CELLULAR_DEVICE(node, 0)

DT_FOREACH_STATUS_OKAY(quectel_bg9x, MODEM_CELLULAR_DEVICE_FROM_NODE)
DT_FOREACH_STATUS_OKAY(zephyr_gsm_ppp, MODEM_CELLULAR_DEVICE_FROM_NODE)
DT_FOREACH_STATUS_OKAY(simcom_sim7080, MODEM_CELLULAR_DEVICE_FROM_NODE)
DT_FOREACH_STATUS_OKAY(ublox_sara_r4, MODEM_CELLULAR_DEVICE_FROM_NODE)
