/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*************************************************************************************************/
/*                                        Dependencies                                           */
/*************************************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/modem/modem_chat.h>
#include <zephyr/modem/modem_cmux.h>
#include <zephyr/modem/modem_pipe.h>
#include <zephyr/modem/modem_ppp.h>
#include <zephyr/modem/modem_backend_tty.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/gpio.h>

#include <string.h>
#include <stdlib.h>

/*************************************************************************************************/
/*                                         Definitions                                           */
/*************************************************************************************************/
#warning "Please update the following defines to match your modem"
#define SAMPLE_APN  "\"iBASIS.iot\""
#define SAMPLE_CMUX "AT+CMUX=0,0,5,127,10,3,30,10,2"
#define SAMPLE_TTY_PATH "/dev/ttyUSB0"

/*************************************************************************************************/
/*                                            Events                                             */
/*************************************************************************************************/
#define SAMPLE_EVENT_SCRIPT_SUCCESS	 BIT(0)
#define SAMPLE_EVENT_SCRIPT_ABORT	 BIT(1)
#define SAMPLE_EVENT_SCRIPT_TIMEOUT	 BIT(2)
#define SAMPLE_EVENT_CMUX_CONNECTED	 BIT(3)
#define SAMPLE_EVENT_CMUX_DLCI1_OPENED	 BIT(4)
#define SAMPLE_EVENT_CMUX_DLCI1_CLOSED	 BIT(5)
#define SAMPLE_EVENT_CMUX_DLCI2_OPENED	 BIT(6)
#define SAMPLE_EVENT_CMUX_DLCI2_CLOSED	 BIT(7)
#define SAMPLE_EVENT_CMUX_DISCONNECTED	 BIT(8)
#define SAMPLE_EVENT_NET_L4_CONNECTED	 BIT(9)
#define SAMPLE_EVENT_NET_L4_DISCONNECTED BIT(10)
#define SAMPLE_EVENT_NET_L4_DISCONNECTED BIT(10)
#define SAMPLE_EVENT_NET_L4_DISCONNECTED BIT(10)

static struct k_event sample_event;

/*************************************************************************************************/
/*                                        Modem pipe UART                                        */
/*************************************************************************************************/
static struct modem_backend_tty tty_backend;
static struct modem_pipe *tty_pipe;

/*************************************************************************************************/
/*                                         Modem CMUX                                            */
/*************************************************************************************************/
static struct modem_cmux cmux;
static uint8_t cmux_receive_buf[128];
static uint8_t cmux_transmit_buf[256];
static struct modem_cmux_dlci dlci1;
static struct modem_cmux_dlci dlci2;
static struct modem_pipe *dlci1_pipe;
static struct modem_pipe *dlci2_pipe;

static uint8_t dlci1_receive_buf[128];
static uint8_t dlci2_receive_buf[128];

static void modem_cmux_callback_handler(struct modem_cmux *cmux, enum modem_cmux_event event,
					void *user_data)
{
	switch (event) {
	case MODEM_CMUX_EVENT_CONNECTED:
		k_event_post(&sample_event, SAMPLE_EVENT_CMUX_CONNECTED);
		break;

	case MODEM_CMUX_EVENT_DISCONNECTED:
		k_event_post(&sample_event, SAMPLE_EVENT_CMUX_DISCONNECTED);
		break;
	}
}

/*************************************************************************************************/
/*                                         Modem chat                                            */
/*************************************************************************************************/
static struct modem_chat chat;
static uint8_t chat_receive_buf[128];
static uint8_t chat_delimiter[] = {'\r'};
static uint8_t chat_filter[1] = {'\n'};
static uint8_t *chat_argv[32];

/*************************************************************************************************/
/*                                          Modem PPP                                            */
/*************************************************************************************************/
static void ppp_iface_init(struct net_if *iface)
{
}

MODEM_PPP_DEFINE(ppp, ppp_iface_init, 41, 1500, 64, 8);

/*************************************************************************************************/
/*                                     Chat script matches                                       */
/*************************************************************************************************/
static uint8_t imei[15];
static uint8_t access_tech;
static uint8_t registration_status;
static uint8_t packet_service_attached;

static void on_imei(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	if (argc != 2) {
		return;
	}

	if (strlen(argv[1]) != 15) {
		return;
	}

	for (uint8_t i = 0; i < 15; i++) {
		imei[i] = argv[1][i] - '0';
	}
}

static void on_creg(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	if (argc != 3) {
		return;
	}

	access_tech = atoi(argv[1]);
	registration_status = atoi(argv[2]);
}

static void on_cgatt(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	if (argc != 2) {
		return;
	}

	packet_service_attached = atoi(argv[1]);
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", NULL);
MODEM_CHAT_MATCH_DEFINE(imei_match, "", "", on_imei);
MODEM_CHAT_MATCH_DEFINE(creg_match, "+CREG: ", ",", on_creg);
MODEM_CHAT_MATCH_DEFINE(cgatt_match, "+CGATT: ", ",", on_cgatt);
MODEM_CHAT_MATCH_DEFINE(connect_match, "CONNECT ", "", NULL);

/*************************************************************************************************/
/*                                  Chat script abort matches                                    */
/*************************************************************************************************/
MODEM_CHAT_MATCHES_DEFINE(abort_matches, MODEM_CHAT_MATCH("ERROR", "", NULL),
			  MODEM_CHAT_MATCH("BUSY", "", NULL),
			  MODEM_CHAT_MATCH("NO ANSWER", "", NULL),
			  MODEM_CHAT_MATCH("NO CARRIER", "", NULL),
			  MODEM_CHAT_MATCH("NO DIALTONE", "", NULL));

/*************************************************************************************************/
/*                                    Chat script callback                                       */
/*************************************************************************************************/
static void modem_chat_callback_handler(struct modem_chat *chat,
					enum modem_chat_script_result result, void *user_data)
{
	switch (result) {
	case MODEM_CHAT_SCRIPT_RESULT_SUCCESS:
		k_event_post(&sample_event, SAMPLE_EVENT_SCRIPT_SUCCESS);
		break;

	case MODEM_CHAT_SCRIPT_RESULT_ABORT:
		k_event_post(&sample_event, SAMPLE_EVENT_SCRIPT_ABORT);
		break;

	case MODEM_CHAT_SCRIPT_RESULT_TIMEOUT:
		k_event_post(&sample_event, SAMPLE_EVENT_SCRIPT_TIMEOUT);
		break;
	}
}

/*************************************************************************************************/
/*                                 Initialization chat script                                    */
/*************************************************************************************************/
MODEM_CHAT_SCRIPT_CMDS_DEFINE(init_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT"),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT"),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT"),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATH", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGSN", imei_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP(SAMPLE_CMUX, ok_match));

MODEM_CHAT_SCRIPT_DEFINE(init_chat_script, init_chat_script_cmds, abort_matches,
			 modem_chat_callback_handler, 10);

/*************************************************************************************************/
/*                                    Network status script                                      */
/*************************************************************************************************/
MODEM_CHAT_SCRIPT_CMDS_DEFINE(net_stat_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", creg_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGATT?", cgatt_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(net_stat_chat_script, net_stat_chat_script_cmds, abort_matches,
			 modem_chat_callback_handler, 10);

/*************************************************************************************************/
/*                                     Connect chat script                                       */
/*************************************************************************************************/
MODEM_CHAT_SCRIPT_CMDS_DEFINE(connect_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\"," SAMPLE_APN,
							 ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATD*99#", connect_match));

MODEM_CHAT_SCRIPT_DEFINE(connect_chat_script, connect_chat_script_cmds, abort_matches,
			 modem_chat_callback_handler, 120);

/*************************************************************************************************/
/*                                       Network manager                                         */
/*************************************************************************************************/
static struct net_mgmt_event_callback mgmt_cb;

static void net_mgmt_event_callback_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event,
					    struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		k_event_post(&sample_event, SAMPLE_EVENT_NET_L4_CONNECTED);
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		k_event_post(&sample_event, SAMPLE_EVENT_NET_L4_DISCONNECTED);
	}
}

/*************************************************************************************************/
/*                                           Helpers                                             */
/*************************************************************************************************/
static void chat_script_reset(void)
{
	k_event_clear(&sample_event, (SAMPLE_EVENT_SCRIPT_SUCCESS | SAMPLE_EVENT_SCRIPT_ABORT |
				      SAMPLE_EVENT_SCRIPT_TIMEOUT));
}

static bool chat_script_wait(void)
{
	uint32_t events;

	events = k_event_wait(&sample_event,
			      (SAMPLE_EVENT_SCRIPT_SUCCESS | SAMPLE_EVENT_SCRIPT_ABORT |
			       SAMPLE_EVENT_SCRIPT_TIMEOUT),
			      false, K_FOREVER);

	return ((events & SAMPLE_EVENT_SCRIPT_SUCCESS) == 0) ? false : true;
}

static bool event_wait_all(uint32_t events, bool reset)
{
	uint32_t match_events;

	match_events = k_event_wait_all(&sample_event, events, reset, K_FOREVER);

	return (match_events == 0) ? false : true;
}

/*************************************************************************************************/
/*                                           Program                                             */
/*************************************************************************************************/
void main(void)
{
	int ret;
	bool result;

	/*
	 * Initialize network management event callback. It is not a requirement.
	 */
	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_event_callback_handler,
				     (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED));

	net_mgmt_add_event_callback(&mgmt_cb);

	/*
	 * Initialize the modem pipe to UART module. This pipe is used to send raw data to the
	 * cellular modem using the UART.
	 */
	const struct modem_backend_tty_config backend_tty_config = {
		.tty_path = SAMPLE_TTY_PATH,
	};

	tty_pipe = modem_backend_tty_init(&tty_backend, &backend_tty_config);

	ret = modem_pipe_open(tty_pipe);
	if (ret < 0) {
		return;
	}

	/*
	 * Initialize the modem CMUX module. CMUX is used to create multiple virtual channels
	 * between the modem and the host. This allows for sending PPP traffic alongside AT
	 * commands on a single serial connection like UART.
	 */
	const struct modem_cmux_config cmux_config = {
		.callback = modem_cmux_callback_handler,
		.user_data = NULL,
		.receive_buf = cmux_receive_buf,
		.receive_buf_size = ARRAY_SIZE(cmux_receive_buf),
		.transmit_buf = cmux_transmit_buf,
		.transmit_buf_size = ARRAY_SIZE(cmux_transmit_buf),
	};

	/* Initialize DLCI channels */
	const struct modem_cmux_dlci_config dlci1_config = {
		.dlci_address = 1,
		.receive_buf = dlci1_receive_buf,
		.receive_buf_size = ARRAY_SIZE(dlci1_receive_buf),
	};

	const struct modem_cmux_dlci_config dlci2_config = {
		.dlci_address = 2,
		.receive_buf = dlci2_receive_buf,
		.receive_buf_size = ARRAY_SIZE(dlci2_receive_buf),
	};

	modem_cmux_init(&cmux, &cmux_config);

	dlci1_pipe = modem_cmux_dlci_init(&cmux, &dlci1, &dlci1_config);

	dlci2_pipe = modem_cmux_dlci_init(&cmux, &dlci2, &dlci2_config);

	/*
	 * Initialize the modem chat module. Chat scripts are executed using this module. A chat
	 * script is used to send and receive text commands to and from the modem
	 */
	const struct modem_chat_config chat_config = {
		.user_data = NULL,
		.receive_buf = chat_receive_buf,
		.receive_buf_size = ARRAY_SIZE(chat_receive_buf),
		.delimiter = chat_delimiter,
		.delimiter_size = ARRAY_SIZE(chat_delimiter),
		.filter = chat_filter,
		.filter_size = 1,
		.argv = chat_argv,
		.argv_size = ARRAY_SIZE(chat_argv),
		.unsol_matches = NULL,
		.unsol_matches_size = 0,
		.process_timeout = K_MSEC(2),
	};

	ret = modem_chat_init(&chat, &chat_config);
	if (ret < 0) {
		return;
	}

	/* Initialize chat script result callback sem */
	k_event_init(&sample_event);

	/* Power on cellular modem
	pm_device_action_run(modem_uart, PM_DEVICE_ACTION_TURN_ON);
	pm_device_action_run(modem_uart, PM_DEVICE_ACTION_RESUME);
	*/

	/* Attach modem chat module to bus pipe */
	ret = modem_chat_attach(&chat, tty_pipe);
	if (ret < 0) {
		return;
	}

	chat_script_reset();

	/* Send initialization script */
	ret = modem_chat_script_run(&chat, &init_chat_script);
	if (ret < 0) {
		return;
	}

	/* Wait for script execution complete */
	if (chat_script_wait() == false) {
		return;
	}

	/* Set IMEI as net link address */
	net_if_set_link_addr(modem_ppp_get_iface(&ppp), imei, ARRAY_SIZE(imei), NET_LINK_UNKNOWN);

	/* Release bus pipe */
	ret = modem_chat_release(&chat);
	if (ret < 0) {
		return;
	}

	/* Give modem time to enter CMUX mode */
	k_msleep(300);

	ret = modem_cmux_attach(&cmux, tty_pipe);
	if (ret < 0) {
		return;
	}

	/* Attach CMUX module to bus pipe which is now in CMUX mode and set up CMUX */
	ret = modem_cmux_connect(&cmux);
	if (ret < 0) {
		return;
	}

	/* Wait for CMUX connected */
	if (event_wait_all(SAMPLE_EVENT_CMUX_CONNECTED, false) == false) {
		return;
	}

	printk("CMUX connected\n");

	/* Open CMUX channels */
	ret = modem_pipe_open(dlci1_pipe);
	if (ret < 0) {
		return;
	}

	ret = modem_pipe_open(dlci2_pipe);
	if (ret < 0) {
		return;
	}

	printk("Opened DLCI CMUX channels\n");

	/* Attach modem chat module to DLCI channel 2 */
	ret = modem_chat_attach(&chat, dlci2_pipe);
	if (ret < 0) {
		return;
	}

	printk("Chat connected to DLCI2\n");

	chat_script_reset();

	/* Run connect script */
	ret = modem_chat_script_run(&chat, &connect_chat_script);
	if (ret < 0) {
		return;
	}

	/* Wait for script execution complete */
	if (chat_script_wait() == false) {
		return;
	}

	/* Release modem chat module from DLCI channel 2 */
	ret = modem_chat_release(&chat);
	if (ret < 0) {
		return;
	}

	k_msleep(500);

	printk("Chat disconnected\n");

	/* Attach modem chat module to DLCI channel 1 */
	ret = modem_chat_attach(&chat, dlci1_pipe);
	if (ret < 0) {
		return;
	}

	/* Attach modem PPP module to DLCI channel 2 which is now in PPP mode */
	ret = modem_ppp_attach(&ppp, dlci2_pipe);
	if (ret < 0) {
		return;
	}

	k_msleep(500);

	printk("Chat connected to DLCI1\n");
	printk("PPP connected to DLCI2\n");

	/* Wait for cellular modem registered to network */
	while (1) {
		chat_script_reset();

		/* Run net stat script */
		ret = modem_chat_script_run(&chat, &net_stat_chat_script);
		if (ret < 0) {
			return;
		}

		/* Wait for script execution complete */
		if (chat_script_wait() == false) {
			return;
		}

		/* Check network status */
		if ((registration_status == 5) && (packet_service_attached == 1)) {
			printk("Modem registered to network\n");

			break;
		}

		k_msleep(5000);
	}

	printk("Bringing up network\n");

	/* Bring up PPP network interface */
	net_ppp_carrier_on(modem_ppp_get_iface(&ppp));

	/* Wait for network layer 4 connected */
	if (event_wait_all(SAMPLE_EVENT_NET_L4_CONNECTED, false) == false) {
		return;
	}

	printk("Network L4 connected\n");

	k_msleep(5000);

	/* Bring down PPP network interface */
	net_ppp_carrier_off(modem_ppp_get_iface(&ppp));

	/* Wait for network layer 4 disconnected */
	if (event_wait_all(SAMPLE_EVENT_NET_L4_DISCONNECTED, false) == false) {
		return;
	}

	printk("Network L4 disconnected\n");

	k_msleep(500);

	printk("Releasing chat and PPP\n");

	/* Release modem chat module from DLCI channel 1 */
	ret = modem_chat_release(&chat);
	if (ret < 0) {
		return;
	}

	/* Release modem PPP module from DLCI channel 2 */
	ret = modem_ppp_release(&ppp);
	if (ret < 0) {
		return;
	}

	printk("Closing DLCI 1 and 2\n");

	/* Close CMUX channels */
	ret = modem_pipe_close(dlci1_pipe);
	if (ret < 0) {
		return;
	}

	ret = modem_pipe_close(dlci2_pipe);
	if (ret < 0) {
		return;
	}

	k_msleep(500);

	printk("Disconnecting CMUX\n");

	/* Disconnect CMUX module */
	modem_cmux_disconnect(&cmux);

	k_msleep(500);

	modem_cmux_release(&cmux);

	printk("Sample complete\n");
}
