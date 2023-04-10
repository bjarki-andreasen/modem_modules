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
#include <zephyr/net/net_if.h>

#include <string.h>

/*************************************************************************************************/
/*                                         Definitions                                           */
/*************************************************************************************************/
#warning "Configure the following before running the test"
#define TEST_MODEM_E2E_SERVER_IP_ADDR "87.62.97.174"

#define TEST_MODEM_E2E_SERVER_UPLOAD_PORT	(7777)
#define TEST_MODEM_E2E_SERVER_ECHO_PORT		(7778)
#define TEST_MODEM_E2E_SERVER_DOWNLOAD_PORT	(7779)

#define TEST_MODEM_E2E_TEST_PKT_CNT		(100)
#define TEST_MODEM_E2E_TEST_PKT_SIZE		(1024)

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

static int socket_fd;
static struct sockaddr_in socket_addr;

/*************************************************************************************************/
/*                                         Test setup                                            */
/*************************************************************************************************/
static void *test_modem_e2e_setup(void)
{
	uint32_t raised_event;
	const void *info;
	size_t info_len;
	int ret;

	printk("Waiting for L4 connected\n");

	ret = net_mgmt_event_wait_on_iface(net_if_get_first_by_type(&NET_L2_GET_NAME(PPP)),
					   NET_EVENT_L4_CONNECTED, &raised_event, &info,
					   &info_len, K_SECONDS(30));

	__ASSERT(ret == 0, "L4 was not connected in time");

	test_modem_e2e_init_test_packet_data();

	printk("Opening socket\n");

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

		printk("sent %u frame\n", (i + 1));
	}
}

ZTEST(modem_e2e, echo)
{
	test_modem_e2e_socket_set_destination(TEST_MODEM_E2E_SERVER_IP_ADDR,
					      TEST_MODEM_E2E_SERVER_ECHO_PORT);

	for (size_t i = 0; i < TEST_MODEM_E2E_TEST_PKT_CNT; i++) {
		zassert_true(test_modem_e2e_socket_send(),
			     "Failed to send test packet data");

		printk("sent %u frame\n", (i + 1));

		zassert_true(test_modem_e2e_socket_recv(), "Failed to receive packet");

		zassert_true(test_modem_e2e_validate_recv_buf(), "Invalid packet data received");

		printk("received %u frames\n", (i + 1));
	}
}

ZTEST(modem_e2e, download)
{
	uint32_t uptime;

	test_modem_e2e_socket_set_destination(TEST_MODEM_E2E_SERVER_IP_ADDR,
					      TEST_MODEM_E2E_SERVER_DOWNLOAD_PORT);

	zassert_true(test_modem_e2e_socket_send(),
			"Failed to send test packet data");

	for (size_t i = 0; i < TEST_MODEM_E2E_TEST_PKT_CNT; i++) {
		uptime = k_uptime_get_32();
		printk("wait for frame\n");
		zassert_true(test_modem_e2e_socket_recv(), "Failed to receive packet");
		printk("waited for %ums\n", k_uptime_get_32() - uptime);
		uptime = k_uptime_get_32();
		printk("validate frame\n");
		zassert_true(test_modem_e2e_validate_recv_buf(), "Invalid packet data received");
		printk("validate took %ums\n", k_uptime_get_32() - uptime);
		printk("received %u frames\n", (i + 1));
	}
}

ZTEST_SUITE(modem_e2e, NULL, test_modem_e2e_setup, NULL, NULL, NULL);
