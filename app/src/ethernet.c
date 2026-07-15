/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ethernet.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/sys/printk.h>

static struct net_mgmt_event_callback net_mgmt_cb;

static void ipv4_addr_added(struct net_mgmt_event_callback *cb,
			    uint64_t event, struct net_if *iface)
{
	char address[NET_IPV4_ADDR_LEN];
	char netmask[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
	const struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	ARG_UNUSED(cb);

	if (event != NET_EVENT_IPV4_ADDR_ADD || ipv4 == NULL) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		const struct net_if_addr_ipv4 *if_addr = &ipv4->unicast[i];

		if (!if_addr->ipv4.is_used ||
		    if_addr->ipv4.addr_type != NET_ADDR_DHCP) {
			continue;
		}

		net_addr_ntop(AF_INET, &if_addr->ipv4.address.in_addr,
			      address, sizeof(address));
		net_addr_ntop(AF_INET, &if_addr->netmask,
			      netmask, sizeof(netmask));
		net_addr_ntop(AF_INET, &ipv4->gw, gateway, sizeof(gateway));

		printk("Ethernet ready: interface %d\n", net_if_get_by_iface(iface));
		printk("  IPv4 address: %s\n", address);
		printk("  Netmask:      %s\n", netmask);
		printk("  Gateway:      %s\n", gateway);
		printk("Test with: net ping -c 4 %s\n", gateway);
		return;
	}
}

static void start_dhcp(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	printk("Starting DHCPv4 on interface %d (%s)\n",
	       net_if_get_by_iface(iface), net_if_get_device(iface)->name);
	net_dhcpv4_start(iface);
}

void ethernet_service_init(void)
{
	net_mgmt_init_event_callback(&net_mgmt_cb, ipv4_addr_added,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&net_mgmt_cb);
	net_if_foreach(start_dhcp, NULL);
}
