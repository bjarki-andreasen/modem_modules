/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*************************************************************************************************/
/*                                        Dependencies                                           */
/*************************************************************************************************/
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/socket.h>
#include <zephyr/modem/modem_chat.h>
#include <zephyr/modem/modem_cmux.h>
#include <zephyr/modem/modem_pipe.h>
#include <zephyr/modem/modem_ppp.h>
#include <zephyr/modem/modem_backend_tty.h>

#include <string.h>
#include <stdlib.h>

/*************************************************************************************************/
/*                                         Definitions                                           */
/*************************************************************************************************/
#warning "Configure the following before running the test"
#define TEST_MODEM_E2E_APN ""
#define TEST_MODEM_E2E_SERVER_IP_ADDR ""
#define TEST_MODEM_E2E_TTY_PATH ""

#define TEST_MODEM_E2E_CMUX			("AT+CMUX=0,0,5,127,10,3,30,10,2")
#define TEST_MODEM_E2E_SERVER_UPLOAD_PORT	(7777)
#define TEST_MODEM_E2E_SERVER_ECHO_PORT		(7778)
#define TEST_MODEM_E2E_SERVER_DOWNLOAD_PORT	(7779)

#define TEST_MODEM_E2E_TEST_PKT_CNT		(100)
#define TEST_MODEM_E2E_TEST_PKT_SIZE		(1024)

/*************************************************************************************************/
/*                                            Events                                             */
/*************************************************************************************************/
#define TEST_MODEM_E2E_EVENT_SCRIPT_SUCCESS	 BIT(0)
#define TEST_MODEM_E2E_EVENT_SCRIPT_ABORT	 BIT(1)
#define TEST_MODEM_E2E_EVENT_SCRIPT_TIMEOUT	 BIT(2)
#define TEST_MODEM_E2E_EVENT_CMUX_CONNECTED	 BIT(3)
#define TEST_MODEM_E2E_EVENT_CMUX_DLCI1_OPENED	 BIT(4)
#define TEST_MODEM_E2E_EVENT_CMUX_DLCI1_CLOSED	 BIT(5)
#define TEST_MODEM_E2E_EVENT_CMUX_DLCI2_OPENED	 BIT(6)
#define TEST_MODEM_E2E_EVENT_CMUX_DLCI2_CLOSED	 BIT(7)
#define TEST_MODEM_E2E_EVENT_CMUX_DISCONNECTED	 BIT(8)
#define TEST_MODEM_E2E_EVENT_NET_L4_CONNECTED	 BIT(9)
#define TEST_MODEM_E2E_EVENT_NET_L4_DISCONNECTED BIT(10)

static struct k_event test_modem_e2e_event;

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
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_CMUX_CONNECTED);
		break;

	case MODEM_CMUX_EVENT_DISCONNECTED:
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_CMUX_DISCONNECTED);
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
MODEM_PPP_DEFINE(ppp, NULL, 41, 1500, 64, 8);

/*************************************************************************************************/
/*                                           Socket                                              */
/*************************************************************************************************/
static int socket_fd;
static struct sockaddr_in socket_addr;

/*************************************************************************************************/
/*                                     Chat script matches                                       */
/*************************************************************************************************/
static uint8_t imei[15];
static uint8_t hwinfo[64];
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

static void on_cgmm(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	if (argc != 2) {
		return;
	}

	strncpy(hwinfo, argv[1], sizeof(hwinfo) - 1);
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
MODEM_CHAT_MATCH_DEFINE(cgmm_match, "", "", on_cgmm);
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
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_SCRIPT_SUCCESS);
		break;

	case MODEM_CHAT_SCRIPT_RESULT_ABORT:
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_SCRIPT_ABORT);
		break;

	case MODEM_CHAT_SCRIPT_RESULT_TIMEOUT:
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_SCRIPT_TIMEOUT);
		break;
	}
}

/*************************************************************************************************/
/*                                 Initialization chat script                                    */
/*************************************************************************************************/
MODEM_CHAT_SCRIPT_CMDS_DEFINE(init_chat_script_cmds, MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATH", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGSN", imei_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGMM", cgmm_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP(TEST_MODEM_E2E_CMUX, ok_match));

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
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\",\""
			      				 TEST_MODEM_E2E_APN"\",",
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
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_NET_L4_CONNECTED);
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		k_event_post(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_NET_L4_DISCONNECTED);
	}
}

/*************************************************************************************************/
/*                                           Helpers                                             */
/*************************************************************************************************/
static void chat_script_reset(void)
{
	k_event_clear(&test_modem_e2e_event, (TEST_MODEM_E2E_EVENT_SCRIPT_SUCCESS | TEST_MODEM_E2E_EVENT_SCRIPT_ABORT |
				      TEST_MODEM_E2E_EVENT_SCRIPT_TIMEOUT));
}

static bool chat_script_wait(void)
{
	uint32_t events;

	events = k_event_wait(&test_modem_e2e_event,
			      (TEST_MODEM_E2E_EVENT_SCRIPT_SUCCESS | TEST_MODEM_E2E_EVENT_SCRIPT_ABORT |
			       TEST_MODEM_E2E_EVENT_SCRIPT_TIMEOUT),
			      false, K_SECONDS(10));

	return ((events & TEST_MODEM_E2E_EVENT_SCRIPT_SUCCESS) == 0) ? false : true;
}

/*************************************************************************************************/
/*                             Psudo random packet datagenerator                                 */
/*************************************************************************************************/
static uint8_t test_packet_data[TEST_MODEM_E2E_TEST_PKT_SIZE];
static uint32_t prng_state = 1234;
static uint8_t recv_buf[TEST_MODEM_E2E_TEST_PKT_SIZE];

static uint8_t test_modem_e2e_prng_random(void)
{
	prng_state = ((1103515245 * prng_state) + 12345) % (1U << 31);

	return (uint8_t)(prng_state & 0xFF);
}

static void test_modem_e2e_init_test_packet_data(void)
{
	for (size_t i = 0; i < sizeof(test_packet_data); i++) {
		test_packet_data[i] = test_modem_e2e_prng_random();
	}
}

static bool test_modem_e2e_validate_recv_buf(void)
{
	for (size_t i = 0; i < sizeof(test_packet_data); i++) {
		if (test_packet_data[i] != recv_buf[i]) {
			return false;
		}
	}

	return true;
}

/*************************************************************************************************/
/*                                         Test setup                                            */
/*************************************************************************************************/
static void *test_modem_e2e_setup(void)
{
	test_modem_e2e_init_test_packet_data();

	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_event_callback_handler,
				     (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED));

	net_mgmt_add_event_callback(&mgmt_cb);

	const struct modem_backend_tty_config backend_tty_config = {
		.tty_path = TEST_MODEM_E2E_TTY_PATH,
	};

	tty_pipe = modem_backend_tty_init(&tty_backend, &backend_tty_config);

	__ASSERT_NO_MSG(modem_pipe_open(tty_pipe) == 0);

	const struct modem_cmux_config cmux_config = {
		.callback = modem_cmux_callback_handler,
		.user_data = NULL,
		.receive_buf = cmux_receive_buf,
		.receive_buf_size = ARRAY_SIZE(cmux_receive_buf),
		.transmit_buf = cmux_transmit_buf,
		.transmit_buf_size = ARRAY_SIZE(cmux_transmit_buf),
	};

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

	__ASSERT_NO_MSG(modem_chat_init(&chat, &chat_config) == 0);

	k_event_init(&test_modem_e2e_event);

	__ASSERT_NO_MSG(modem_chat_attach(&chat, tty_pipe) == 0);

	chat_script_reset();

	__ASSERT_NO_MSG(modem_chat_script_run(&chat, &init_chat_script) == 0);
	__ASSERT_NO_MSG(chat_script_wait() == true);

	/* Set IMEI as net link address */
	__ASSERT_NO_MSG(net_if_set_link_addr(modem_ppp_get_iface(&ppp), imei, ARRAY_SIZE(imei),
			NET_LINK_UNKNOWN) == 0);

	/* Print hardware info */
	printk("Modem: %s\n", hwinfo);

	/* Release bus pipe */
	modem_chat_release(&chat);

	/* Give modem time to enter CMUX mode */
	k_msleep(300);

	__ASSERT_NO_MSG(modem_cmux_attach(&cmux, tty_pipe) == 0);

	__ASSERT_NO_MSG(modem_cmux_connect(&cmux) == 0);

	__ASSERT_NO_MSG(k_event_wait(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_CMUX_CONNECTED, false, K_MSEC(3000)) > 0);

	printk("CMUX connected\n");

	/* Open CMUX channels */
	__ASSERT_NO_MSG(modem_pipe_open(dlci1_pipe) == 0);
	__ASSERT_NO_MSG(modem_pipe_open(dlci2_pipe) == 0);

	printk("Opened DLCI CMUX channels\n");

	__ASSERT_NO_MSG(modem_chat_attach(&chat, dlci2_pipe) == 0);

	printk("Chat connected to DLCI2\n");

	chat_script_reset();

	/* Run connect script */
	__ASSERT_NO_MSG(modem_chat_script_run(&chat, &connect_chat_script) == 0);

	/* Wait for script execution complete */
	__ASSERT_NO_MSG(chat_script_wait() == true);

	/* Release modem chat module from DLCI channel 2 */
	modem_chat_release(&chat);

	printk("Chat disconnected\n");

	/* Attach modem chat module to DLCI channel 1 */
	__ASSERT_NO_MSG(modem_chat_attach(&chat, dlci1_pipe) == 0);
	__ASSERT_NO_MSG(modem_ppp_attach(&ppp, dlci2_pipe) == 0);

	printk("Chat connected to DLCI1\n");
	printk("PPP connected to DLCI2\n");

	/* Wait for cellular modem registered to network */
	for (uint8_t i = 0; i < 10; i++) {
		chat_script_reset();

		/* Run net stat script */
		__ASSERT_NO_MSG(modem_chat_script_run(&chat, &net_stat_chat_script) == 0);

		/* Wait for script execution complete */
		__ASSERT_NO_MSG(chat_script_wait() == true);

		/* Check network status */
		if ((registration_status == 5) && (packet_service_attached == 1)) {
			printk("Modem registered to network\n");

			break;
		}

		k_msleep(3000);
	}

	__ASSERT_NO_MSG((registration_status == 5) && (packet_service_attached == 1));

	printk("Bringing up network\n");

	/* Bring up PPP network interface */
	net_ppp_carrier_on(modem_ppp_get_iface(&ppp));

	/* Wait for network layer 4 connected */
	__ASSERT_NO_MSG(k_event_wait(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_NET_L4_CONNECTED,
				     false, K_SECONDS(20)));

	printk("Network L4 connected\n");

	socket_fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	__ASSERT_NO_MSG(socket_fd > -1);

	printk("Socket opened\n");

	return NULL;
}

static void test_modem_e2e_socket_set_destination(const char *addr, int16_t port)
{
	memset(&socket_addr, 0, sizeof(socket_addr));

	socket_addr.sin_family = AF_INET;
	socket_addr.sin_port = htons(port);
	zsock_inet_pton(AF_INET, addr, &socket_addr.sin_addr);
}

static bool test_modem_e2e_socket_send(void)
{
	int ret = zsock_sendto(socket_fd, test_packet_data, sizeof(test_packet_data), 0,
			       (struct sockaddr *)&socket_addr, sizeof(socket_addr));

	return (ret == sizeof(test_packet_data));
}

static bool test_modem_e2e_socket_recv(void)
{
	int ret = zsock_recv(socket_fd, recv_buf, sizeof(recv_buf), 0);

	return ret == sizeof(recv_buf);
}

static void test_modem_e2e_teardown(void *f)
{
	/* Bring down PPP network interface */
	net_ppp_carrier_off(modem_ppp_get_iface(&ppp));

	k_event_wait(&test_modem_e2e_event, TEST_MODEM_E2E_EVENT_NET_L4_CONNECTED, false,
				     K_SECONDS(5));

	printk("Network L4 disconnected\n");
	printk("Releasing chat and PPP\n");

	/* Release modem chat module from DLCI channel 1 */
	modem_chat_release(&chat);

	/* Release modem PPP module from DLCI channel 2 */
	modem_ppp_release(&ppp);

	printk("Closing DLCI 1 and 2\n");

	/* Close CMUX channels */
	modem_pipe_close(dlci1_pipe);
	modem_pipe_close(dlci2_pipe);

	printk("Disconnecting CMUX\n");

	modem_cmux_disconnect(&cmux);

	modem_cmux_release(&cmux);
}

/*************************************************************************************************/
/*                                             Tests                                             */
/*************************************************************************************************/
ZTEST(modem_e2e, send)
{
	test_modem_e2e_socket_set_destination(TEST_MODEM_E2E_SERVER_IP_ADDR,
					      TEST_MODEM_E2E_SERVER_UPLOAD_PORT);

	for (size_t i = 0; i < TEST_MODEM_E2E_TEST_PKT_CNT; i++) {
		zassert_true(test_modem_e2e_socket_send(),
			     "Failed to send test packet data");
	}
}

ZTEST(modem_e2e, echo)
{
	test_modem_e2e_socket_set_destination(TEST_MODEM_E2E_SERVER_IP_ADDR,
					      TEST_MODEM_E2E_SERVER_ECHO_PORT);

	for (size_t i = 0; i < TEST_MODEM_E2E_TEST_PKT_CNT; i++) {
		zassert_true(test_modem_e2e_socket_send(),
			     "Failed to send test packet data");

		zassert_true(test_modem_e2e_socket_recv(), "Failed to receive packet");

		zassert_true(test_modem_e2e_validate_recv_buf(), "Invalid packet data received");
	}
}

ZTEST(modem_e2e, download)
{
	test_modem_e2e_socket_set_destination(TEST_MODEM_E2E_SERVER_IP_ADDR,
					      TEST_MODEM_E2E_SERVER_DOWNLOAD_PORT);

	zassert_true(test_modem_e2e_socket_send(),
			"Failed to send test packet data");

	for (size_t i = 0; i < TEST_MODEM_E2E_TEST_PKT_CNT; i++) {
		zassert_true(test_modem_e2e_socket_recv(), "Failed to receive packet");
		zassert_true(test_modem_e2e_validate_recv_buf(), "Invalid packet data received");
	}
}

ZTEST_SUITE(modem_e2e, NULL, test_modem_e2e_setup, NULL, NULL, test_modem_e2e_teardown);
