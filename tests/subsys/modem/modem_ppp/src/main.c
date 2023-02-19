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
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include "zephyr/net/net_l2.h"
#include "zephyr/net/ppp.h"
#include <string.h>

#include <zephyr/modem/modem_ppp.h>
#include <modem_backend_mock.h>

#define TEST_MODEM_PPP_BUF_SIZE		     (16)
#define TEST_MODEM_PPP_TX_PKT_BUF_SIZE	     (5)
#define TEST_MODEM_PPP_MOCK_PIPE_RX_BUF_SIZE (4096)
#define TEST_MODEM_PPP_MOCK_PIPE_TX_BUF_SIZE (4096)

/*************************************************************************************************/
/*                                          Mock pipe                                            */
/*************************************************************************************************/
static struct modem_backend_mock mock;
static uint8_t mock_rx_buf[TEST_MODEM_PPP_MOCK_PIPE_RX_BUF_SIZE];
static uint8_t mock_tx_buf[TEST_MODEM_PPP_MOCK_PIPE_TX_BUF_SIZE];
static struct modem_pipe *mock_pipe;

/*************************************************************************************************/
/*                                         PPP frames                                            */
/*************************************************************************************************/
static uint8_t ppp_frame_wrapped[] = {0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x21, 0x7D,
				      0x21, 0x7D, 0x20, 0x7D, 0x24, 0xD1, 0xB5, 0x7E};

static uint8_t ppp_frame_unwrapped[] = {0xC0, 0x21, 0x01, 0x01, 0x00, 0x04};

static uint8_t ip_frame_wrapped[] = {
	0x7E, 0xFF, 0x7D, 0x23, 0x7D, 0x20, 0x21, 0x45, 0x7D, 0x20, 0x7D, 0x20, 0x29, 0x87, 0x6E,
	0x40, 0x7D, 0x20, 0xE8, 0x7D, 0x31, 0xC1, 0xE9, 0x7D, 0x23, 0xFB, 0x7D, 0x25, 0x20, 0x7D,
	0x2A, 0x2B, 0x36, 0x26, 0x25, 0x7D, 0x32, 0x8C, 0x3E, 0x7D, 0x20, 0x7D, 0x35, 0xBD, 0xF3,
	0x2D, 0x7D, 0x20, 0x7D, 0x2B, 0x7D, 0x20, 0x7D, 0x27, 0x7D, 0x20, 0x7D, 0x24, 0x7D, 0x20,
	0x7D, 0x24, 0x7D, 0x2A, 0x7D, 0x20, 0x7D, 0x2A, 0x7D, 0x20, 0xD4, 0x31, 0x7E};

static uint8_t ip_frame_unwrapped[] = {
	0x45, 0x00, 0x00, 0x29, 0x87, 0x6E, 0x40, 0x00, 0xE8, 0x11, 0xC1, 0xE9, 0x03, 0xFB,
	0x05, 0x20, 0x0A, 0x2B, 0x36, 0x26, 0x25, 0x12, 0x8C, 0x3E, 0x00, 0x15, 0xBD, 0xF3,
	0x2D, 0x00, 0x0B, 0x00, 0x07, 0x00, 0x04, 0x00, 0x04, 0x0A, 0x00, 0x0A, 0x00};

static uint8_t ip_frame_unwrapped_with_protocol[] = {
	0x00, 0x21, 0x45, 0x00, 0x00, 0x29, 0x87, 0x6E, 0x40, 0x00, 0xE8, 0x11, 0xC1, 0xE9, 0x03,
	0xFB, 0x05, 0x20, 0x0A, 0x2B, 0x36, 0x26, 0x25, 0x12, 0x8C, 0x3E, 0x00, 0x15, 0xBD, 0xF3,
	0x2D, 0x00, 0x0B, 0x00, 0x07, 0x00, 0x04, 0x00, 0x04, 0x0A, 0x00, 0x0A, 0x00};

static uint8_t corrupt_start_end_ppp_frame_wrapped[] = {0x2A, 0x46, 0x7E, 0x7E, 0xFF, 0x7D, 0x23,
							0xC0, 0x21, 0x7D, 0x21, 0x7D, 0x21, 0x7D,
							0x20, 0x7D, 0x24, 0xD1, 0xB5, 0x7E};

/*************************************************************************************************/
/*                                          Buffers                                              */
/*************************************************************************************************/
static struct net_pkt *received_packets[12];
static size_t received_packets_len;
static uint8_t buffer[4096];

/*************************************************************************************************/
/*                                  Mock network interface                                       */
/*************************************************************************************************/
static uint8_t test_net_link_addr[] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x01};

static enum net_verdict test_net_l2_recv(struct net_if *iface, struct net_pkt *pkt)
{
	/* Validate buffer not overflowing */
	zassert_true(received_packets_len < ARRAY_SIZE(received_packets),
		     "Mock network interface receive buffer limit reached");

	/* Store pointer to received packet */
	received_packets[received_packets_len] = pkt;
	received_packets_len++;

	return NET_OK;
}

/* This emulates the layer two API */
static struct net_l2 test_net_l2 = {
	.recv = test_net_l2_recv,
};

/* This emulates the network interface device which will receive unwrapped network packets */
static struct net_if_dev test_net_if_dev = {
	.l2 = &test_net_l2,
	.link_addr.addr = test_net_link_addr,
	.link_addr.len = sizeof(test_net_link_addr),
	.link_addr.type = NET_LINK_DUMMY,
	.mtu = 1500,
	.oper_state = NET_IF_OPER_UP,
};

/* This emulates the network interface which contains the network interface device */
static struct net_if test_iface = {
	.if_dev = &test_net_if_dev,
};

/*************************************************************************************************/
/*                                         Modem PPP                                             */
/*************************************************************************************************/
/*
 * The following initialization happens automatically when MODEM_PPP_DEFINE is used. However,
 * since we are emulating the network interface, we can't use that macro, and have to initialize
 * it manually here.
 */
static uint8_t ppp_receive_buf[TEST_MODEM_PPP_BUF_SIZE];
static uint8_t ppp_transmit_buf[TEST_MODEM_PPP_BUF_SIZE];
static struct net_pkt *ppp_tx_net_pkt_buf[TEST_MODEM_PPP_TX_PKT_BUF_SIZE];

static struct modem_ppp ppp = {
	.iface = &test_iface,
	.receive_buf = ppp_receive_buf,
	.transmit_buf = ppp_transmit_buf,
	.buf_size = TEST_MODEM_PPP_BUF_SIZE,
	.tx_pkt_buf = ppp_tx_net_pkt_buf,
	.tx_pkt_buf_size = TEST_MODEM_PPP_TX_PKT_BUF_SIZE,
};

/*************************************************************************************************/
/*                                     Modem PPP net device                                      */
/*************************************************************************************************/
extern const struct ppp_api modem_ppp_ppp_api;

static const struct device ppp_net_dev = {.data = &ppp};

static int test_net_send(struct net_pkt *pkt)
{
	return modem_ppp_ppp_api.send(&ppp_net_dev, pkt);
}

/*************************************************************************************************/
/*                                     Modem PPP net device                                      */
/*************************************************************************************************/
static void *test_modem_ppp_setup(void)
{
	/*
	 * Manually run internal init function which would normally be performed by kernel as
	 * result of using the macro MODEM_PPP_DEFINE()
	 */
	zassert_true(modem_ppp_init_internal(&ppp_net_dev) == 0, "Failed to run internal init");

	net_if_flag_set(modem_ppp_get_iface(&ppp), NET_IF_UP);

	const struct modem_backend_mock_config mock_config = {
		.rx_buf = mock_rx_buf,
		.rx_buf_size = sizeof(mock_rx_buf),
		.tx_buf = mock_tx_buf,
		.tx_buf_size = sizeof(mock_tx_buf),
		.limit = 8,
	};

	mock_pipe = modem_backend_mock_init(&mock, &mock_config);

	zassert_true(modem_pipe_open_sync(mock_pipe) == 0, "Failed to open mock pipe");

	modem_ppp_attach(&ppp, mock_pipe);

	return NULL;
}

static void test_modem_ppp_before(void *f)
{
	/* Unreference packets */
	for (size_t i = 0; i < received_packets_len; i++) {
		net_pkt_unref(received_packets[i]);
	}

	/* Reset packets received buffer */
	received_packets_len = 0;

	/* Reset mock pipe */
	modem_backend_mock_reset(&mock);
}

ZTEST(modem_ppp, ppp_frame_receive)
{
	struct net_pkt *pkt;
	size_t pkt_len;

	/* Put wrapped frame */
	modem_backend_mock_put(&mock, ppp_frame_wrapped, sizeof(ppp_frame_wrapped));

	/* Give modem ppp time to process received frame */
	k_msleep(1000);

	/* Validate frame received on mock network interface */
	zassert_true(received_packets_len == 1, "Expected to receive one network packet");

	pkt = received_packets[0];
	pkt_len = net_pkt_get_len(pkt);

	/* Validate length of received frame */
	zassert_true(pkt_len == sizeof(ppp_frame_unwrapped), "Received net pkt data len incorrect");

	/* Validate data of received frame */
	net_pkt_cursor_init(pkt);
	net_pkt_read(pkt, buffer, pkt_len);

	zassert_true(memcmp(buffer, ppp_frame_unwrapped, pkt_len) == 0,
		     "Recevied net pkt data incorrect");
}

ZTEST(modem_ppp, corrupt_start_end_ppp_frame_receive)
{
	struct net_pkt *pkt;
	size_t pkt_len;

	/* Put wrapped frame */
	modem_backend_mock_put(&mock, corrupt_start_end_ppp_frame_wrapped,
			       sizeof(corrupt_start_end_ppp_frame_wrapped));

	/* Give modem ppp time to process received frame */
	k_msleep(1000);

	/* Validate frame received on mock network interface */
	zassert_true(received_packets_len == 1, "Expected to receive one network packet");

	pkt = received_packets[0];
	pkt_len = net_pkt_get_len(pkt);

	/* Validate length of received frame */
	zassert_true(pkt_len == sizeof(ppp_frame_unwrapped), "Received net pkt data len incorrect");

	/* Validate data of received frame */
	net_pkt_cursor_init(pkt);
	net_pkt_read(pkt, buffer, pkt_len);

	zassert_true(memcmp(buffer, ppp_frame_unwrapped, pkt_len) == 0,
		     "Recevied net pkt data incorrect");
}

ZTEST(modem_ppp, ppp_frame_send)
{
	struct net_pkt *pkt;
	int ret;

	/* Allocate net pkt */
	pkt = net_pkt_alloc_with_buffer(&test_iface, 256, AF_UNSPEC, 0, K_NO_WAIT);

	zassert_true(pkt != NULL, "Failed to allocate network packet");

	/* Set network packet data */
	net_pkt_cursor_init(pkt);
	ret = net_pkt_write(pkt, ppp_frame_unwrapped, sizeof(ppp_frame_unwrapped));

	zassert_true(ret == 0, "Failed to write data to allocated network packet");

	net_pkt_set_ppp(pkt, true);

	/* Send network packet */

	zassert_true(test_net_send(pkt) == 0, "Failed to send PPP pkt");

	/* Give modem ppp time to wrap and send frame */
	k_msleep(1000);

	/* Get any sent data */
	ret = modem_backend_mock_get(&mock, buffer, sizeof(buffer));

	zassert_true(ret == sizeof(ppp_frame_wrapped), "Wrapped frame length incorrect");

	zassert_true(memcmp(buffer, ppp_frame_wrapped, ret) == 0,
		     "Wrapped frame content is incorrect");
}

ZTEST(modem_ppp, ip_frame_receive)
{
	struct net_pkt *pkt;
	size_t pkt_len;

	/* Put wrapped frame */
	modem_backend_mock_put(&mock, ip_frame_wrapped, sizeof(ip_frame_wrapped));

	/* Give modem ppp time to process received frame */
	k_msleep(1000);

	/* Validate frame received on mock network interface */
	zassert_true(received_packets_len == 1, "Expected to receive one network packet");

	pkt = received_packets[0];
	pkt_len = net_pkt_get_len(pkt);

	/* Validate length of received frame */
	zassert_true(pkt_len == sizeof(ip_frame_unwrapped_with_protocol),
		     "Received net pkt data len incorrect");

	/* Validate data of received frame */
	net_pkt_cursor_init(pkt);
	net_pkt_read(pkt, buffer, pkt_len);

	zassert_true(memcmp(buffer, ip_frame_unwrapped_with_protocol, pkt_len) == 0,
		     "Recevied net pkt data incorrect");
}

ZTEST(modem_ppp, ip_frame_send)
{
	struct net_pkt *pkt;
	int ret;

	/* Allocate net pkt */
	pkt = net_pkt_alloc_with_buffer(&test_iface, 256, AF_UNSPEC, 0, K_NO_WAIT);

	zassert_true(pkt != NULL, "Failed to allocate network packet");

	/* Set network packet data */
	net_pkt_cursor_init(pkt);
	ret = net_pkt_write(pkt, ip_frame_unwrapped, sizeof(ip_frame_unwrapped));

	zassert_true(ret == 0, "Failed to write data to allocated network packet");

	net_pkt_set_family(pkt, AF_INET);

	/* Send network packet */
	test_net_send(pkt);

	/* Give modem ppp time to wrap and send frame */
	k_msleep(100);

	/* Get any sent data */
	ret = modem_backend_mock_get(&mock, buffer, sizeof(buffer));

	zassert_true(ret == sizeof(ip_frame_wrapped), "Wrapped frame length incorrect");

	zassert_true(memcmp(buffer, ip_frame_wrapped, ret) == 0,
		     "Wrapped frame content is incorrect");
}

#define MODEM_PPP_TEST_IP_FRAME_SEND_MULT_N (5)

ZTEST(modem_ppp, ip_frame_send_multiple)
{
	struct net_pkt *pkts[MODEM_PPP_TEST_IP_FRAME_SEND_MULT_N];
	int ret;

	/* Allocate net pkts */
	for (uint8_t i = 0; i < MODEM_PPP_TEST_IP_FRAME_SEND_MULT_N; i++) {
		pkts[i] = net_pkt_alloc_with_buffer(&test_iface, 256, AF_UNSPEC, 0, K_NO_WAIT);

		zassert_true(pkts[i] != NULL, "Failed to allocate network packet");

		net_pkt_cursor_init(pkts[i]);

		ret = net_pkt_write(pkts[i], ip_frame_unwrapped, sizeof(ip_frame_unwrapped));

		zassert_true(ret == 0, "Failed to write data to allocated network packet");

		net_pkt_set_family(pkts[i], AF_INET);
	}

	/* Send net pkts */
	for (uint8_t i = 0; i < MODEM_PPP_TEST_IP_FRAME_SEND_MULT_N; i++) {
		test_net_send(pkts[i]);
	}

	k_msleep(100);

	ret = modem_backend_mock_get(&mock, buffer, TEST_MODEM_PPP_MOCK_PIPE_RX_BUF_SIZE);

	zassert_true(ret == (sizeof(ip_frame_wrapped) * MODEM_PPP_TEST_IP_FRAME_SEND_MULT_N),
		     "Incorrect data amount received");
}

ZTEST_SUITE(modem_ppp, NULL, test_modem_ppp_setup, test_modem_ppp_before, NULL, NULL);
