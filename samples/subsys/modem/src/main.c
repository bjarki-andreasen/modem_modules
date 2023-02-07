/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/modem/modem_chat.h>
#include <zephyr/modem/modem_cmux.h>
#include <zephyr/modem/modem_pipe.h>
#include <zephyr/modem/modem_ppp.h>

void main(void) {

  const struct modem_ppp_config ppp_config = {
      .rx_buf = ppp_rx_buf,
      .rx_buf_size = sizeof(ppp_rx_buf),
      .tx_buf = ppp_tx_buf,
      .tx_buf_size = sizeof(ppp_tx_buf),
  };
}
