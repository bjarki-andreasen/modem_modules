/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*************************************************************************************************/
/*                                        Dependencies                                           */
/*************************************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

#include <string.h>
#include <stdlib.h>

static struct net_mgmt_event_callback mgmt_cb;

static void net_mgmt_event_callback_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event,
					    struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		printk("L4 Connected");
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		printk("L4 Disconnected");
	}
}

/*************************************************************************************************/
/*                                           Devices                                             */
/*************************************************************************************************/
const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));

void main(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_event_callback_handler,
				     (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED));

	net_mgmt_add_event_callback(&mgmt_cb);

	while(1) {
		k_msleep(1000);
	}
}
