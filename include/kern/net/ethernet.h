/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_NET_ETHERNET_H
#define ZEDBSD_KERN_NET_ETHERNET_H

#include <stddef.h>
#include <stdint.h>

#define ETHERNET_ADDRESS_LENGTH 6U
#define ETHERNET_HEADER_LENGTH  14U
#define ETHERNET_TYPE_ALL       0x0003U
#define ETHERNET_TYPE_IPV4      0x0800U
#define ETHERNET_TYPE_ARP       0x0806U

struct net_device;
struct packet_buf;

typedef int (*ethernet_input_fn)(struct packet_buf *packet);

void ethernet_init(void);
int ethernet_protocol_register(uint16_t type, ethernet_input_fn input);
int ethernet_input(struct packet_buf *packet);
int ethernet_output(struct net_device *device, const uint8_t destination[6],
		    uint16_t type, struct packet_buf *packet);

#endif
