/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <linux/batman_adv.h>

#include "conf-parser.h"
#include "netdev.h"


#define BATADV_GENL_NAME "batadv"
#define BATADV_GENL_VERSION 1


typedef enum BatadvGatewayModes {
        BATADV_GATEWAY_MODE_OFF = BATADV_GW_MODE_OFF,
        BATADV_GATEWAY_MODE_CLIENT = BATADV_GW_MODE_CLIENT,
        BATADV_GATEWAY_MODE_SERVER = BATADV_GW_MODE_SERVER,
        _BATADV_GATEWAY_MODE_MAX,
        _BATADV_GATEWAY_MODE_INVALID = -1,
} BatadvGatewayModes;

typedef struct Batadv {
        NetDev meta;

        BatadvGatewayModes gateway_mode;
        uint32_t gateway_bandwidth_down;
        uint32_t gateway_bandwidth_up;
        uint8_t hop_penalty;
        const char* routing_algo;
        usec_t originator_interval;
        bool aggregation;
        bool bridge_loop_avoidance;
        bool distributed_arp_table;
        bool fragmentation;
} BatmanAdvanced;

DEFINE_NETDEV_CAST(BATADV, BatmanAdvanced);
extern const NetDevVTable batadv_vtable;

const char *batadv_gateway_mode_to_string(BatadvGatewayModes d) _const_;
BatadvGatewayModes batadv_gateway_mode_from_string(const char *d) _pure_;

CONFIG_PARSER_PROTOTYPE(config_parse_batadv_gateway_mode);
CONFIG_PARSER_PROTOTYPE(config_parse_badadv_bandwidth);
