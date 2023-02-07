#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_MODEM_MODEM_PPP
#define ZEPHYR_MODEM_MODEM_PPP

enum modem_ppp_receive_state {
	/* Searching for start of frame and header */
	MODEM_PPP_RECEIVE_STATE_HDR_SOF = 0,
	MODEM_PPP_RECEIVE_STATE_HDR_FF,
	MODEM_PPP_RECEIVE_STATE_HDR_7D,
	MODEM_PPP_RECEIVE_STATE_HDR_23,
	/* Writing bytes to network packet */
	MODEM_PPP_RECEIVE_STATE_WRITING,
	/* Unescaping next byte before writing to network packet */
	MODEM_PPP_RECEIVE_STATE_UNESCAPING,
};

enum modem_ppp_transmit_state {
	/* Idle */
	MODEM_PPP_TRANSMIT_STATE_IDLE = 0,
	/* Writing header */
	MODEM_PPP_TRANSMIT_STATE_SOF,
	MODEM_PPP_TRANSMIT_STATE_HDR_FF,
	MODEM_PPP_TRANSMIT_STATE_HDR_7D,
	MODEM_PPP_TRANSMIT_STATE_HDR_23,
	/* Writing protocol */
	MODEM_PPP_TRANSMIT_STATE_PROTOCOL_HIGH,
	MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_HIGH,
	MODEM_PPP_TRANSMIT_STATE_PROTOCOL_LOW,
	MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_LOW,
	/* Writing data */
	MODEM_PPP_TRANSMIT_STATE_DATA,
	MODEM_PPP_TRANSMIT_STATE_ESCAPING_DATA,
	/* Writing FCS */
	MODEM_PPP_TRANSMIT_STATE_FCS_LOW,
	MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_LOW,
	MODEM_PPP_TRANSMIT_STATE_FCS_HIGH,
	MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_HIGH,
	/* Writing end of frame */
	MODEM_PPP_TRANSMIT_STATE_EOF,
};

struct modem_ppp;

struct modem_ppp_send_work_item {
	struct k_work work;
	struct modem_ppp *ppp;
	struct net_pkt *pkt;
};

struct modem_ppp_work_item {
	struct k_work work;
	struct modem_ppp *ppp;
};

struct modem_ppp {
	/* Network interface receiving PPP network packets */
	struct net_if *net_iface;

	/* Wrapped PPP frames are sent and received through this pipe */
	struct modem_pipe *pipe;

	/* Buffer used for processing received data */
	uint8_t *rx_buf;
	uint16_t rx_buf_len;
	uint16_t rx_buf_size;

	/* Receive PPP frame state */
	enum modem_ppp_receive_state receive_state;

	/* Allocated network packet being created */
	struct net_pkt *pkt;

	/* Transmit buffer */
	struct ring_buf tx_rb;

	/* Packet being sent */
	enum modem_ppp_transmit_state transmit_state;
	struct net_pkt *tx_pkt;
	uint8_t tx_pkt_escaped;
	uint16_t tx_pkt_protocol;
	uint16_t tx_pkt_fcs;

	/* Send work */
	struct modem_ppp_send_work_item send_submit_work;
	struct modem_ppp_work_item send_work;
	struct modem_ppp_work_item process_work;
};

/**
 * @brief Contains modem PPP instance confuguration data
 * @param rx_buf Buffer for unwrapping PPP frame
 * @param rx_buf_size Size of RX buffer, should be the same size as network buffer
 * @param tx_buf Buffer for wrapping network packet in PPP frame
 * @param tx_buf_size Size of TX buffer, which should be twice the size of network buffer
 * @param tx_pkt_buf Buffer storing network packets to send
 * @param tx_pkt_buf_size Size of buffer storing network packets to send
 */
struct modem_ppp_config {
	uint8_t *rx_buf;
	uint16_t rx_buf_size;
	uint8_t *tx_buf;
	uint16_t tx_buf_size;
};

int modem_ppp_init(struct modem_ppp *ppp, const struct modem_ppp_config *config);

/**
 * @brief Attach pipe to instance and connect
 * @param ppp Modem PPP instance
 * @param pipe Pipe to attach to modem PPP instance
 * @param iface Network interface receiving PPP network packets
 */
int modem_ppp_attach(struct modem_ppp *ppp, struct modem_pipe *pipe, struct net_if *iface);

/**
 * @brief Wrap network packet in PPP frame and send it
 * @param ppp Modem PPP context
 * @param pkt Network packet to be wrapped and sent
 */
int modem_ppp_send(struct modem_ppp *ppp, struct net_pkt *pkt);

/**
 * @brief Release pipe from instance
 * @param ppp Modem PPP instance
 */
int modem_ppp_release(struct modem_ppp *ppp);

#endif /* ZEPHYR_MODEM_MODEM_PPP */
