/* SPDX-License-Identifier: LGPL-2.1+ */

#include <net/if.h>

#include "alloc-util.h"
#include "conf-parser.h"
#include "firewall-util.h"
#include "memory-util.h"
#include "missing_network.h"
#include "netlink-util.h"
#include "networkd-address.h"
#include "networkd-manager.h"
#include "networkd-ndisc.h"
#include "parse-util.h"
#include "set.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"
#include "utf8.h"

#define ADDRESSES_PER_LINK_MAX 2048U
#define STATIC_ADDRESSES_PER_NETWORK_MAX 1024U

int generate_ipv6_eui_64_address(Link *link, struct in6_addr *ret) {
        assert(link);
        assert(ret);

        /* see RFC4291 section 2.5.1 */
        ret->s6_addr[8]  = link->mac.ether_addr_octet[0];
        ret->s6_addr[8] ^= 1 << 1;
        ret->s6_addr[9]  = link->mac.ether_addr_octet[1];
        ret->s6_addr[10] = link->mac.ether_addr_octet[2];
        ret->s6_addr[11] = 0xff;
        ret->s6_addr[12] = 0xfe;
        ret->s6_addr[13] = link->mac.ether_addr_octet[3];
        ret->s6_addr[14] = link->mac.ether_addr_octet[4];
        ret->s6_addr[15] = link->mac.ether_addr_octet[5];

        return 0;
}

int address_new(Address **ret) {
        _cleanup_(address_freep) Address *address = NULL;

        address = new(Address, 1);
        if (!address)
                return -ENOMEM;

        *address = (Address) {
                .family = AF_UNSPEC,
                .scope = RT_SCOPE_UNIVERSE,
                .cinfo.ifa_prefered = CACHE_INFO_INFINITY_LIFE_TIME,
                .cinfo.ifa_valid = CACHE_INFO_INFINITY_LIFE_TIME,
                .duplicate_address_detection = ADDRESS_FAMILY_IPV6,
                .prefix_route = true,
        };

        *ret = TAKE_PTR(address);

        return 0;
}

static int address_new_static(Network *network, const char *filename, unsigned section_line, Address **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(!!filename == (section_line > 0));

        if (filename) {
                r = network_config_section_new(filename, section_line, &n);
                if (r < 0)
                        return r;

                address = hashmap_get(network->addresses_by_section, n);
                if (address) {
                        *ret = TAKE_PTR(address);

                        return 0;
                }
        }

        if (network->n_static_addresses >= STATIC_ADDRESSES_PER_NETWORK_MAX)
                return -E2BIG;

        r = address_new(&address);
        if (r < 0)
                return r;

        address->network = network;
        LIST_APPEND(addresses, network->static_addresses, address);
        network->n_static_addresses++;

        if (filename) {
                address->section = TAKE_PTR(n);

                r = hashmap_ensure_allocated(&network->addresses_by_section, &network_config_hash_ops);
                if (r < 0)
                        return r;

                r = hashmap_put(network->addresses_by_section, address->section, address);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(address);

        return 0;
}

void address_free(Address *address) {
        if (!address)
                return;

        if (address->network) {
                LIST_REMOVE(addresses, address->network->static_addresses, address);
                assert(address->network->n_static_addresses > 0);
                address->network->n_static_addresses--;

                if (address->section)
                        hashmap_remove(address->network->addresses_by_section, address->section);
        }

        if (address->link && !address->acd) {
                NDiscAddress *n;

                set_remove(address->link->addresses, address);
                set_remove(address->link->addresses_foreign, address);
                set_remove(address->link->static_addresses, address);
                if (address->link->dhcp_address == address)
                        address->link->dhcp_address = NULL;
                if (address->link->dhcp_address_old == address)
                        address->link->dhcp_address_old = NULL;
                set_remove(address->link->dhcp6_addresses, address);
                set_remove(address->link->dhcp6_addresses_old, address);
                set_remove(address->link->dhcp6_pd_addresses, address);
                set_remove(address->link->dhcp6_pd_addresses_old, address);
                SET_FOREACH(n, address->link->ndisc_addresses)
                        if (n->address == address)
                                free(set_remove(address->link->ndisc_addresses, n));

                if (in_addr_equal(AF_INET6, &address->in_addr, (const union in_addr_union *) &address->link->ipv6ll_address))
                        memzero(&address->link->ipv6ll_address, sizeof(struct in6_addr));
        }

        sd_ipv4acd_unref(address->acd);

        network_config_section_free(address->section);
        free(address->label);
        free(address);
}

static uint32_t address_prefix(const Address *a) {
        assert(a);

        /* make sure we don't try to shift by 32.
         * See ISO/IEC 9899:TC3 § 6.5.7.3. */
        if (a->prefixlen == 0)
                return 0;

        if (a->in_addr_peer.in.s_addr != 0)
                return be32toh(a->in_addr_peer.in.s_addr) >> (32 - a->prefixlen);
        else
                return be32toh(a->in_addr.in.s_addr) >> (32 - a->prefixlen);
}

void address_hash_func(const Address *a, struct siphash *state) {
        assert(a);

        siphash24_compress(&a->family, sizeof(a->family), state);

        switch (a->family) {
        case AF_INET:
                siphash24_compress(&a->prefixlen, sizeof(a->prefixlen), state);

                /* peer prefix */
                uint32_t prefix = address_prefix(a);
                siphash24_compress(&prefix, sizeof(prefix), state);

                _fallthrough_;
        case AF_INET6:
                /* local address */
                siphash24_compress(&a->in_addr, FAMILY_ADDRESS_SIZE(a->family), state);

                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

int address_compare_func(const Address *a1, const Address *a2) {
        int r;

        r = CMP(a1->family, a2->family);
        if (r != 0)
                return r;

        switch (a1->family) {
        /* use the same notion of equality as the kernel does */
        case AF_INET:
                r = CMP(a1->prefixlen, a2->prefixlen);
                if (r != 0)
                        return r;

                uint32_t prefix1 = address_prefix(a1);
                uint32_t prefix2 = address_prefix(a2);
                r = CMP(prefix1, prefix2);
                if (r != 0)
                        return r;

                _fallthrough_;
        case AF_INET6:
                return memcmp(&a1->in_addr, &a2->in_addr, FAMILY_ADDRESS_SIZE(a1->family));
        default:
                /* treat any other address family as AF_UNSPEC */
                return 0;
        }
}

DEFINE_HASH_OPS_WITH_KEY_DESTRUCTOR(address_hash_ops, Address, address_hash_func, address_compare_func, address_free);

bool address_equal(Address *a1, Address *a2) {
        if (a1 == a2)
                return true;

        if (!a1 || !a2)
                return false;

        return address_compare_func(a1, a2) == 0;
}

static int address_establish(Address *address, Link *link) {
        bool masq;
        int r;

        assert(address);
        assert(link);

        masq = link->network &&
               link->network->ip_masquerade &&
               address->family == AF_INET &&
               address->scope < RT_SCOPE_LINK;

        /* Add firewall entry if this is requested */
        if (address->ip_masquerade_done != masq) {
                union in_addr_union masked = address->in_addr;
                in_addr_mask(address->family, &masked, address->prefixlen);

                r = fw_add_masquerade(masq, AF_INET, 0, &masked, address->prefixlen, NULL, NULL, 0);
                if (r < 0)
                        return r;

                address->ip_masquerade_done = masq;
        }

        return 0;
}

static int address_add_internal(Link *link, Set **addresses,
                                int family,
                                const union in_addr_union *in_addr,
                                unsigned char prefixlen,
                                Address **ret) {
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(link);
        assert(addresses);
        assert(in_addr);

        r = address_new(&address);
        if (r < 0)
                return r;

        address->family = family;
        address->in_addr = *in_addr;
        address->prefixlen = prefixlen;
        /* Consider address tentative until we get the real flags from the kernel */
        address->flags = IFA_F_TENTATIVE;

        r = set_ensure_put(addresses, &address_hash_ops, address);
        if (r < 0)
                return r;
        if (r == 0)
                return -EEXIST;

        address->link = link;

        if (ret)
                *ret = address;
        TAKE_PTR(address);
        return 0;
}

int address_add_foreign(Link *link, int family, const union in_addr_union *in_addr, unsigned char prefixlen, Address **ret) {
        return address_add_internal(link, &link->addresses_foreign, family, in_addr, prefixlen, ret);
}

int address_add(Link *link, int family, const union in_addr_union *in_addr, unsigned char prefixlen, Address **ret) {
        Address *address;
        int r;

        r = address_get(link, family, in_addr, prefixlen, &address);
        if (r == -ENOENT) {
                /* Address does not exist, create a new one */
                r = address_add_internal(link, &link->addresses, family, in_addr, prefixlen, &address);
                if (r < 0)
                        return r;
        } else if (r == 0) {
                /* Take over a foreign address */
                r = set_ensure_put(&link->addresses, &address_hash_ops, address);
                if (r < 0)
                        return r;

                set_remove(link->addresses_foreign, address);
        } else if (r == 1) {
                /* Already exists, do nothing */
                ;
        } else
                return r;

        if (ret)
                *ret = address;

        return 0;
}

static int address_release(Address *address) {
        int r;

        assert(address);
        assert(address->link);

        /* Remove masquerading firewall entry if it was added */
        if (address->ip_masquerade_done) {
                union in_addr_union masked = address->in_addr;
                in_addr_mask(address->family, &masked, address->prefixlen);

                r = fw_add_masquerade(false, AF_INET, 0, &masked, address->prefixlen, NULL, NULL, 0);
                if (r < 0)
                        return r;

                address->ip_masquerade_done = false;
        }

        return 0;
}

int address_update(
                Address *address,
                unsigned char flags,
                unsigned char scope,
                const struct ifa_cacheinfo *cinfo) {

        bool ready;
        int r;

        assert(address);
        assert(address->link);
        assert(cinfo);

        ready = address_is_ready(address);

        address->flags = flags;
        address->scope = scope;
        address->cinfo = *cinfo;

        if (IN_SET(address->link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 0;

        link_update_operstate(address->link, true);
        link_check_ready(address->link);

        if (!ready && address_is_ready(address)) {
                if (address->callback) {
                        r = address->callback(address);
                        if (r < 0)
                                return r;
                }

                if (address->family == AF_INET6 &&
                    in_addr_is_link_local(AF_INET6, &address->in_addr) > 0 &&
                    IN6_IS_ADDR_UNSPECIFIED(&address->link->ipv6ll_address) > 0) {

                        r = link_ipv6ll_gained(address->link, &address->in_addr.in6);
                        if (r < 0)
                                return r;
                }
        }

        return 0;
}

int address_drop(Address *address) {
        Link *link;
        bool ready;
        int r;

        assert(address);

        ready = address_is_ready(address);
        link = address->link;

        r = address_release(address);
        if (r < 0)
                log_link_warning_errno(link, r, "Failed to disable IP masquerading, ignoring: %m");

        address_free(address);

        link_update_operstate(link, true);

        if (link && !ready)
                link_check_ready(link);

        return 0;
}

int address_get(Link *link,
                int family,
                const union in_addr_union *in_addr,
                unsigned char prefixlen,
                Address **ret) {

        Address address, *existing;

        assert(link);
        assert(in_addr);

        address = (Address) {
                .family = family,
                .in_addr = *in_addr,
                .prefixlen = prefixlen,
        };

        existing = set_get(link->addresses, &address);
        if (existing) {
                if (ret)
                        *ret = existing;
                return 1;
        }

        existing = set_get(link->addresses_foreign, &address);
        if (existing) {
                if (ret)
                        *ret = existing;
                return 0;
        }

        return -ENOENT;
}

static bool address_exists_internal(Set *addresses, int family, const union in_addr_union *in_addr) {
        Address *address;

        SET_FOREACH(address, addresses) {
                if (address->family != family)
                        continue;
                if (in_addr_equal(address->family, &address->in_addr, in_addr))
                        return true;
        }

        return false;
}

bool address_exists(Link *link, int family, const union in_addr_union *in_addr) {
        assert(link);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(in_addr);

        if (address_exists_internal(link->addresses, family, in_addr))
                return true;
        if (address_exists_internal(link->addresses_foreign, family, in_addr))
                return true;
        return false;
}

static int address_remove_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EADDRNOTAVAIL)
                log_link_message_warning_errno(link, m, r, "Could not drop address");
        else if (r >= 0)
                (void) manager_rtnl_process_address(rtnl, m, link->manager);

        return 1;
}

int address_remove(
                Address *address,
                Link *link,
                link_netlink_message_handler_t callback) {

        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(address);
        assert(IN_SET(address->family, AF_INET, AF_INET6));
        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->manager->rtnl);

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *b = NULL;

                (void) in_addr_to_string(address->family, &address->in_addr, &b);
                log_link_debug(link, "Removing address %s", strna(b));
        }

        r = sd_rtnl_message_new_addr(link->manager->rtnl, &req, RTM_DELADDR,
                                     link->ifindex, address->family);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not allocate RTM_DELADDR message: %m");

        r = sd_rtnl_message_addr_set_prefixlen(req, address->prefixlen);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set prefixlen: %m");

        r = netlink_message_append_in_addr_union(req, IFA_LOCAL, address->family, &address->in_addr);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append IFA_LOCAL attribute: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, req,
                               callback ?: address_remove_handler,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not send rtnetlink message: %m");

        link_ref(link);

        return 0;
}

static bool link_is_static_address_configured(Link *link, Address *address) {
        Address *net_address;

        assert(link);
        assert(address);

        if (!link->network)
                return false;

        LIST_FOREACH(addresses, net_address, link->network->static_addresses)
                if (address_equal(net_address, address))
                        return true;
                else if (address->family == AF_INET6 && net_address->family == AF_INET6 &&
                         in_addr_equal(AF_INET6, &address->in_addr, &net_address->in_addr_peer) > 0)
                        return true;

        return false;
}

static bool link_address_is_dynamic(Link *link, Address *address) {
        Route *route;

        assert(link);
        assert(address);

        if (address->cinfo.ifa_prefered != CACHE_INFO_INFINITY_LIFE_TIME)
                return true;

        /* Even when the address is leased from a DHCP server, networkd assign the address
         * without lifetime when KeepConfiguration=dhcp. So, let's check that we have
         * corresponding routes with RTPROT_DHCP. */
        SET_FOREACH(route, link->routes_foreign) {
                if (route->protocol != RTPROT_DHCP)
                        continue;

                if (address->family != route->family)
                        continue;

                if (in_addr_equal(address->family, &address->in_addr, &route->prefsrc))
                        return true;
        }

        return false;
}

int link_drop_foreign_addresses(Link *link) {
        Address *address;
        int k, r = 0;

        assert(link);

        SET_FOREACH(address, link->addresses_foreign) {
                /* we consider IPv6LL addresses to be managed by the kernel */
                if (address->family == AF_INET6 && in_addr_is_link_local(AF_INET6, &address->in_addr) == 1 && link_ipv6ll_enabled(link))
                        continue;

                if (link_address_is_dynamic(link, address)) {
                        if (link->network && FLAGS_SET(link->network->keep_configuration, KEEP_CONFIGURATION_DHCP))
                                continue;
                } else if (link->network && FLAGS_SET(link->network->keep_configuration, KEEP_CONFIGURATION_STATIC))
                        continue;

                if (link_is_static_address_configured(link, address)) {
                        k = address_add(link, address->family, &address->in_addr, address->prefixlen, NULL);
                        if (k < 0) {
                                log_link_error_errno(link, k, "Failed to add address: %m");
                                if (r >= 0)
                                        r = k;
                        }
                } else {
                        k = address_remove(address, link, NULL);
                        if (k < 0 && r >= 0)
                                r = k;
                }
        }

        return r;
}

static int remove_static_address_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);
        assert(link->address_remove_messages > 0);

        link->address_remove_messages--;

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EADDRNOTAVAIL)
                log_link_message_warning_errno(link, m, r, "Could not drop address");
        else if (r >= 0)
                (void) manager_rtnl_process_address(rtnl, m, link->manager);

        if (link->address_remove_messages == 0 && link->request_static_addresses) {
                link_set_state(link, LINK_STATE_CONFIGURING);
                r = link_set_addresses(link);
                if (r < 0)
                        link_enter_failed(link);
        }

        return 1;
}

int link_drop_addresses(Link *link) {
        Address *address, *pool_address;
        int k, r = 0;

        assert(link);

        SET_FOREACH(address, link->addresses) {
                /* we consider IPv6LL addresses to be managed by the kernel */
                if (address->family == AF_INET6 && in_addr_is_link_local(AF_INET6, &address->in_addr) == 1 && link_ipv6ll_enabled(link))
                        continue;

                k = address_remove(address, link, remove_static_address_handler);
                if (k < 0 && r >= 0) {
                        r = k;
                        continue;
                }

                link->address_remove_messages++;

                /* If this address came from an address pool, clean up the pool */
                LIST_FOREACH(addresses, pool_address, link->pool_addresses)
                        if (address_equal(address, pool_address)) {
                                LIST_REMOVE(addresses, link->pool_addresses, pool_address);
                                address_free(pool_address);
                                break;
                        }
        }

        return r;
}

static int address_acquire(Link *link, Address *original, Address **ret) {
        union in_addr_union in_addr = IN_ADDR_NULL;
        struct in_addr broadcast = {};
        _cleanup_(address_freep) Address *na = NULL;
        int r;

        assert(link);
        assert(original);
        assert(ret);

        /* Something useful was configured? just use it */
        r = in_addr_is_null(original->family, &original->in_addr);
        if (r <= 0)
                return r;

        /* The address is configured to be 0.0.0.0 or [::] by the user?
         * Then let's acquire something more useful from the pool. */
        r = manager_address_pool_acquire(link->manager, original->family, original->prefixlen, &in_addr);
        if (r < 0)
                return r;
        if (r == 0)
                return -EBUSY;

        if (original->family == AF_INET) {
                /* Pick first address in range for ourselves ... */
                in_addr.in.s_addr = in_addr.in.s_addr | htobe32(1);

                /* .. and use last as broadcast address */
                if (original->prefixlen > 30)
                        broadcast.s_addr = 0;
                else
                        broadcast.s_addr = in_addr.in.s_addr | htobe32(0xFFFFFFFFUL >> original->prefixlen);
        } else if (original->family == AF_INET6)
                in_addr.in6.s6_addr[15] |= 1;

        r = address_new(&na);
        if (r < 0)
                return r;

        na->family = original->family;
        na->prefixlen = original->prefixlen;
        na->scope = original->scope;
        na->cinfo = original->cinfo;

        if (original->label) {
                na->label = strdup(original->label);
                if (!na->label)
                        return -ENOMEM;
        }

        na->broadcast = broadcast;
        na->in_addr = in_addr;

        LIST_PREPEND(addresses, link->pool_addresses, na);

        *ret = TAKE_PTR(na);

        return 0;
}

int address_configure(
                Address *address,
                Link *link,
                link_netlink_message_handler_t callback,
                bool update,
                Address **ret) {

        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        Address *a;
        int r;

        assert(address);
        assert(IN_SET(address->family, AF_INET, AF_INET6));
        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(callback);

        /* If this is a new address, then refuse adding more than the limit */
        if (address_get(link, address->family, &address->in_addr, address->prefixlen, NULL) <= 0 &&
            set_size(link->addresses) >= ADDRESSES_PER_LINK_MAX)
                return log_link_error_errno(link, SYNTHETIC_ERRNO(E2BIG),
                                            "Too many addresses are configured, refusing: %m");

        r = address_acquire(link, address, &address);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to acquire an address from pool: %m");

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *str = NULL;

                (void) in_addr_to_string(address->family, &address->in_addr, &str);
                log_link_debug(link, "%s address: %s", update ? "Updating" : "Configuring", strna(str));
        }

        if (update)
                r = sd_rtnl_message_new_addr_update(link->manager->rtnl, &req,
                                                    link->ifindex, address->family);
        else
                r = sd_rtnl_message_new_addr(link->manager->rtnl, &req, RTM_NEWADDR,
                                             link->ifindex, address->family);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not allocate RTM_NEWADDR message: %m");

        r = sd_rtnl_message_addr_set_prefixlen(req, address->prefixlen);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set prefixlen: %m");

        address->flags |= IFA_F_PERMANENT;

        if (address->home_address)
                address->flags |= IFA_F_HOMEADDRESS;

        if (!FLAGS_SET(address->duplicate_address_detection, ADDRESS_FAMILY_IPV6))
                address->flags |= IFA_F_NODAD;

        if (address->manage_temporary_address)
                address->flags |= IFA_F_MANAGETEMPADDR;

        if (!address->prefix_route)
                address->flags |= IFA_F_NOPREFIXROUTE;

        if (address->autojoin)
                address->flags |= IFA_F_MCAUTOJOIN;

        r = sd_rtnl_message_addr_set_flags(req, (address->flags & 0xff));
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set flags: %m");

        if (address->flags & ~0xff) {
                r = sd_netlink_message_append_u32(req, IFA_FLAGS, address->flags);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not set extended flags: %m");
        }

        r = sd_rtnl_message_addr_set_scope(req, address->scope);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set scope: %m");

        r = netlink_message_append_in_addr_union(req, IFA_LOCAL, address->family, &address->in_addr);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append IFA_LOCAL attribute: %m");

        if (in_addr_is_null(address->family, &address->in_addr_peer) == 0) {
                r = netlink_message_append_in_addr_union(req, IFA_ADDRESS, address->family, &address->in_addr_peer);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFA_ADDRESS attribute: %m");
        } else if (address->family == AF_INET && address->prefixlen <= 30) {
                r = sd_netlink_message_append_in_addr(req, IFA_BROADCAST, &address->broadcast);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFA_BROADCAST attribute: %m");
        }

        if (address->label) {
                r = sd_netlink_message_append_string(req, IFA_LABEL, address->label);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFA_LABEL attribute: %m");
        }

        r = sd_netlink_message_append_cache_info(req, IFA_CACHEINFO, &address->cinfo);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append IFA_CACHEINFO attribute: %m");

        r = address_establish(address, link);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not enable IP masquerading, ignoring: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, req, callback, link_netlink_destroy_callback, link);
        if (r < 0) {
                address_release(address);
                return log_link_error_errno(link, r, "Could not send rtnetlink message: %m");
        }

        link_ref(link);

        if (address->family == AF_INET6 && !in_addr_is_null(address->family, &address->in_addr_peer))
                r = address_add(link, address->family, &address->in_addr_peer, address->prefixlen, &a);
        else
                r = address_add(link, address->family, &address->in_addr, address->prefixlen, &a);
        if (r < 0) {
                address_release(address);
                return log_link_error_errno(link, r, "Could not add address: %m");
        }

        if (address->acd) {
                assert(address->family == AF_INET);
                if (DEBUG_LOGGING) {
                        _cleanup_free_ char *pretty = NULL;

                        (void) in_addr_to_string(address->family, &address->in_addr, &pretty);
                        log_link_debug(link, "Starting IPv4ACD client. Probing address %s", strna(pretty));
                }

                r = sd_ipv4acd_start(address->acd, true);
                if (r < 0)
                        log_link_warning_errno(link, r, "Failed to start IPv4ACD client, ignoring: %m");
        }

        if (ret)
                *ret = a;

        return 1;
}

static int static_address_ready_callback(Address *address) {
        Address *a;
        Link *link;

        assert(address);
        assert(address->link);

        link = address->link;

        if (!link->addresses_configured)
                return 0;

        SET_FOREACH(a, link->static_addresses)
                if (!address_is_ready(a)) {
                        _cleanup_free_ char *str = NULL;

                        (void) in_addr_to_string(a->family, &a->in_addr, &str);
                        log_link_debug(link, "an address %s/%u is not ready", strnull(str), a->prefixlen);
                        return 0;
                }

        /* This should not be called again */
        SET_FOREACH(a, link->static_addresses)
                a->callback = NULL;

        link->addresses_ready = true;

        return link_set_routes(link);
}

static int address_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(rtnl);
        assert(m);
        assert(link);
        assert(link->ifname);
        assert(link->address_messages > 0);
        assert(IN_SET(link->state, LINK_STATE_CONFIGURING,
               LINK_STATE_FAILED, LINK_STATE_LINGER));

        link->address_messages--;

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_warning_errno(link, m, r, "Could not set address");
                link_enter_failed(link);
                return 1;
        } else if (r >= 0)
                (void) manager_rtnl_process_address(rtnl, m, link->manager);

        if (link->address_messages == 0) {
                Address *a;

                log_link_debug(link, "Addresses set");
                link->addresses_configured = true;

                /* When all static addresses are already ready, then static_address_ready_callback()
                 * will not be called automatically. So, call it here. */
                a = set_first(link->static_addresses);
                if (!a) {
                        log_link_warning(link, "No static address is stored.");
                        link_enter_failed(link);
                        return 1;
                }
                if (!a->callback) {
                        log_link_warning(link, "Address ready callback is not set.");
                        link_enter_failed(link);
                        return 1;
                }
                r = a->callback(a);
                if (r < 0)
                        link_enter_failed(link);
        }

        return 1;
}

static int static_address_configure(Address *address, Link *link, bool update) {
        Address *ret;
        int r;

        assert(address);
        assert(link);

        r = address_configure(address, link, address_handler, update, &ret);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not configure static address: %m");

        link->address_messages++;

        r = set_ensure_put(&link->static_addresses, &address_hash_ops, ret);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to store static address: %m");

        ret->callback = static_address_ready_callback;

        return 0;
}

int link_set_addresses(Link *link) {
        Address *ad;
        int r;

        assert(link);
        assert(link->network);

        if (link->address_remove_messages != 0) {
                log_link_debug(link, "Removing old addresses, new addresses will be configured later.");
                link->request_static_addresses = true;
                return 0;
        }

        LIST_FOREACH(addresses, ad, link->network->static_addresses) {
                bool update;

                if (ad->family == AF_INET6 && !in_addr_is_null(ad->family, &ad->in_addr_peer))
                        update = address_get(link, ad->family, &ad->in_addr_peer, ad->prefixlen, NULL) > 0;
                else
                        update = address_get(link, ad->family, &ad->in_addr, ad->prefixlen, NULL) > 0;

                r = static_address_configure(ad, link, update);
                if (r < 0)
                        return r;
        }

        if (link->network->router_prefix_delegation & RADV_PREFIX_DELEGATION_STATIC) {
                Prefix *p;

                HASHMAP_FOREACH(p, link->network->prefixes_by_section) {
                        _cleanup_(address_freep) Address *address = NULL;

                        if (!p->assign)
                                continue;

                        r = address_new(&address);
                        if (r < 0)
                                return log_oom();

                        r = sd_radv_prefix_get_prefix(p->radv_prefix, &address->in_addr.in6, &address->prefixlen);
                        if (r < 0)
                                return log_link_warning_errno(link, r, "Could not get RA prefix: %m");

                        r = generate_ipv6_eui_64_address(link, &address->in_addr.in6);
                        if (r < 0)
                                return log_link_warning_errno(link, r, "Could not generate EUI64 address: %m");

                        address->family = AF_INET6;
                        r = static_address_configure(address, link, true);
                        if (r < 0)
                                return r;
                }
        }

        if (link->address_messages == 0) {
                link->addresses_configured = true;
                link->addresses_ready = true;
                r = link_set_routes(link);
                if (r < 0)
                        return r;
        } else {
                log_link_debug(link, "Setting addresses");
                link_set_state(link, LINK_STATE_CONFIGURING);
        }

        return 0;
}

int manager_rtnl_process_address(sd_netlink *rtnl, sd_netlink_message *message, Manager *m) {
        _cleanup_free_ char *buf = NULL;
        Link *link = NULL;
        uint16_t type;
        unsigned char flags, prefixlen, scope;
        union in_addr_union in_addr = IN_ADDR_NULL;
        struct ifa_cacheinfo cinfo;
        Address *address = NULL;
        char valid_buf[FORMAT_TIMESPAN_MAX];
        const char *valid_str = NULL;
        int ifindex, family, r;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "rtnl: failed to receive address message, ignoring");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWADDR, RTM_DELADDR)) {
                log_warning("rtnl: received unexpected message type %u when processing address, ignoring.", type);
                return 0;
        }

        r = sd_rtnl_message_addr_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get ifindex from message, ignoring: %m");
                return 0;
        } else if (ifindex <= 0) {
                log_warning("rtnl: received address message with invalid ifindex %d, ignoring.", ifindex);
                return 0;
        }

        r = link_get(m, ifindex, &link);
        if (r < 0 || !link) {
                /* when enumerating we might be out of sync, but we will get the address again, so just
                 * ignore it */
                if (!m->enumerating)
                        log_warning("rtnl: received address for link '%d' we don't know about, ignoring.", ifindex);
                return 0;
        }

        r = sd_rtnl_message_addr_get_family(message, &family);
        if (r < 0) {
                log_link_warning(link, "rtnl: received address message without family, ignoring.");
                return 0;
        } else if (!IN_SET(family, AF_INET, AF_INET6)) {
                log_link_debug(link, "rtnl: received address message with invalid family '%i', ignoring.", family);
                return 0;
        }

        r = sd_rtnl_message_addr_get_prefixlen(message, &prefixlen);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_addr_get_scope(message, &scope);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without scope, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_addr_get_flags(message, &flags);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without flags, ignoring: %m");
                return 0;
        }

        switch (family) {
        case AF_INET:
                r = sd_netlink_message_read_in_addr(message, IFA_LOCAL, &in_addr.in);
                if (r < 0) {
                        log_link_warning_errno(link, r, "rtnl: received address message without valid address, ignoring: %m");
                        return 0;
                }

                break;

        case AF_INET6:
                r = sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &in_addr.in6);
                if (r < 0) {
                        log_link_warning_errno(link, r, "rtnl: received address message without valid address, ignoring: %m");
                        return 0;
                }

                break;

        default:
                assert_not_reached("Received unsupported address family");
        }

        (void) in_addr_to_string(family, &in_addr, &buf);

        r = sd_netlink_message_read_cache_info(message, IFA_CACHEINFO, &cinfo);
        if (r < 0 && r != -ENODATA) {
                log_link_warning_errno(link, r, "rtnl: cannot get IFA_CACHEINFO attribute, ignoring: %m");
                return 0;
        } else if (r >= 0 && cinfo.ifa_valid != CACHE_INFO_INFINITY_LIFE_TIME)
                valid_str = format_timespan(valid_buf, FORMAT_TIMESPAN_MAX,
                                            cinfo.ifa_valid * USEC_PER_SEC,
                                            USEC_PER_SEC);

        (void) address_get(link, family, &in_addr, prefixlen, &address);

        switch (type) {
        case RTM_NEWADDR:
                if (address)
                        log_link_debug(link, "Remembering updated address: %s/%u (valid %s%s)",
                                       strnull(buf), prefixlen,
                                       valid_str ? "for " : "forever", strempty(valid_str));
                else {
                        /* An address appeared that we did not request */
                        r = address_add_foreign(link, family, &in_addr, prefixlen, &address);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to remember foreign address %s/%u, ignoring: %m",
                                                       strnull(buf), prefixlen);
                                return 0;
                        } else
                                log_link_debug(link, "Remembering foreign address: %s/%u (valid %s%s)",
                                               strnull(buf), prefixlen,
                                               valid_str ? "for " : "forever", strempty(valid_str));
                }

                /* address_update() logs internally, so we don't need to here. */
                r = address_update(address, flags, scope, &cinfo);
                if (r < 0)
                        link_enter_failed(link);

                break;

        case RTM_DELADDR:
                if (address) {
                        log_link_debug(link, "Forgetting address: %s/%u (valid %s%s)",
                                       strnull(buf), prefixlen,
                                       valid_str ? "for " : "forever", strempty(valid_str));
                        (void) address_drop(address);
                } else
                        log_link_debug(link, "Kernel removed an address we don't remember: %s/%u (valid %s%s), ignoring.",
                                       strnull(buf), prefixlen,
                                       valid_str ? "for " : "forever", strempty(valid_str));

                break;

        default:
                assert_not_reached("Received invalid RTNL message type");
        }

        return 1;
}

int link_serialize_addresses(Link *link, FILE *f) {
        bool space = false;
        Address *a;

        assert(link);

        fputs("ADDRESSES=", f);
        SET_FOREACH(a, link->addresses) {
                _cleanup_free_ char *address_str = NULL;

                if (in_addr_to_string(a->family, &a->in_addr, &address_str) < 0)
                        continue;

                fprintf(f, "%s%s/%u", space ? " " : "", address_str, a->prefixlen);
                space = true;
        }
        fputc('\n', f);

        return 0;
}

int link_deserialize_addresses(Link *link, const char *addresses) {
        int r;

        assert(link);

        for (const char *p = addresses;; ) {
                _cleanup_free_ char *address_str = NULL;
                union in_addr_union address;
                unsigned char prefixlen;
                char *prefixlen_str;
                int family;

                r = extract_first_word(&p, &address_str, NULL, 0);
                if (r < 0)
                        return log_link_debug_errno(link, r, "Failed to parse ADDRESSES=: %m");
                if (r == 0)
                        return 0;

                prefixlen_str = strchr(address_str, '/');
                if (!prefixlen_str) {
                        log_link_debug(link, "Failed to parse address and prefix length, ignoring: %s", address_str);
                        continue;
                }
                *prefixlen_str++ = '\0';

                r = sscanf(prefixlen_str, "%hhu", &prefixlen);
                if (r != 1) {
                        log_link_debug(link, "Failed to parse prefixlen: %s", prefixlen_str);
                        continue;
                }

                r = in_addr_from_string_auto(address_str, &family, &address);
                if (r < 0) {
                        log_link_debug_errno(link, r, "Failed to parse address: %s", address_str);
                        continue;
                }

                r = address_add(link, family, &address, prefixlen, NULL);
                if (r < 0)
                        log_link_debug_errno(link, r, "Failed to add address: %m");
        }

        return 0;
}

static void static_address_on_acd(sd_ipv4acd *acd, int event, void *userdata) {
        _cleanup_free_ char *pretty = NULL;
        Address *address;
        Link *link;
        int r;

        assert(acd);
        assert(userdata);

        address = (Address *) userdata;
        link = address->link;

        (void) in_addr_to_string(address->family, &address->in_addr, &pretty);
        switch (event) {
        case SD_IPV4ACD_EVENT_STOP:
                log_link_debug(link, "Stopping ACD client...");
                return;

        case SD_IPV4ACD_EVENT_BIND:
                log_link_debug(link, "Successfully claimed address %s", strna(pretty));
                link_check_ready(link);
                break;

        case SD_IPV4ACD_EVENT_CONFLICT:
                log_link_warning(link, "DAD conflict. Dropping address %s", strna(pretty));
                r = address_remove(address, link, NULL);
                if (r < 0)
                        log_link_error_errno(link, r, "Failed to drop DAD conflicted address %s", strna(pretty));;

                link_check_ready(link);
                break;

        default:
                assert_not_reached("Invalid IPv4ACD event.");
        }

        sd_ipv4acd_stop(acd);

        return;
}

static int ipv4_dad_configure(Link *link, Address *address) {
        int r;

        assert(link);
        assert(address);
        assert(address->family == AF_INET);
        assert(!address->link && address->network);

        address->link = link;

        r = sd_ipv4acd_new(&address->acd);
        if (r < 0)
                return r;

        r = sd_ipv4acd_attach_event(address->acd, NULL, 0);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_ifindex(address->acd, link->ifindex);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_mac(address->acd, &link->mac);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_address(address->acd, &address->in_addr.in);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_callback(address->acd, static_address_on_acd, address);
        if (r < 0)
                return r;

        return 0;
}

int link_configure_ipv4_dad(Link *link) {
        Address *address;
        int r;

        assert(link);
        assert(link->network);

        LIST_FOREACH(addresses, address, link->network->static_addresses)
                if (address->family == AF_INET &&
                    FLAGS_SET(address->duplicate_address_detection, ADDRESS_FAMILY_IPV4)) {
                        r = ipv4_dad_configure(link, address);
                        if (r < 0)
                                return log_link_error_errno(link, r, "Failed to configure IPv4ACD: %m");
                }

        return 0;
}

int config_parse_broadcast(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        if (n->family == AF_INET6) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Broadcast is not valid for IPv6 addresses, ignoring assignment: %s", rvalue);
                return 0;
        }

        r = in_addr_from_string(AF_INET, rvalue, (union in_addr_union*) &n->broadcast);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Broadcast is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->family = AF_INET;
        n = NULL;

        return 0;
}

int config_parse_address(const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        union in_addr_union buffer;
        unsigned char prefixlen;
        int r, f;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (streq(section, "Network")) {
                /* we are not in an Address section, so treat
                 * this as the special '0' section */
                r = address_new_static(network, NULL, 0, &n);
        } else
                r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        /* Address=address/prefixlen */
        r = in_addr_prefix_from_string_auto_internal(rvalue, PREFIXLEN_REFUSE, &f, &buffer, &prefixlen);
        if (r == -ENOANO) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "An address '%s' is specified without prefix length. "
                           "The behavior of parsing addresses without prefix length will be changed in the future release. "
                           "Please specify prefix length explicitly.", rvalue);

                r = in_addr_prefix_from_string_auto_internal(rvalue, PREFIXLEN_LEGACY, &f, &buffer, &prefixlen);
        }
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Invalid address '%s', ignoring assignment: %m", rvalue);
                return 0;
        }

        if (n->family != AF_UNSPEC && f != n->family) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Address is incompatible, ignoring assignment: %s", rvalue);
                return 0;
        }

        if (in_addr_is_null(f, &buffer)) {
                /* Will use address from address pool. Note that for ipv6 case, prefix of the address
                 * pool is 8, but 40 bit is used by the global ID and 16 bit by the subnet ID. So,
                 * let's limit the prefix length to 64 or larger. See RFC4193. */
                if ((f == AF_INET && prefixlen < 8) ||
                    (f == AF_INET6 && prefixlen < 64)) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Null address with invalid prefixlen='%u', ignoring assignment: %s",
                                   prefixlen, rvalue);
                        return 0;
                }
        }

        n->family = f;
        n->prefixlen = prefixlen;

        if (streq(lvalue, "Address"))
                n->in_addr = buffer;
        else
                n->in_addr_peer = buffer;

        if (n->family == AF_INET && n->broadcast.s_addr == 0 && n->prefixlen <= 30)
                n->broadcast.s_addr = n->in_addr.in.s_addr | htobe32(0xfffffffflu >> n->prefixlen);

        n = NULL;

        return 0;
}

int config_parse_label(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        Network *network = userdata;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        if (!address_label_valid(rvalue)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Interface label is too long or invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        r = free_and_strdup(&n->label, rvalue);
        if (r < 0)
                return log_oom();

        n = NULL;
        return 0;
}

int config_parse_lifetime(const char *unit,
                          const char *filename,
                          unsigned line,
                          const char *section,
                          unsigned section_line,
                          const char *lvalue,
                          int ltype,
                          const char *rvalue,
                          void *data,
                          void *userdata) {
        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        uint32_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        /* We accept only "forever", "infinity", empty, or "0". */
        if (STR_IN_SET(rvalue, "forever", "infinity", ""))
                k = CACHE_INFO_INFINITY_LIFE_TIME;
        else if (streq(rvalue, "0"))
                k = 0;
        else {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid PreferredLifetime= value, ignoring: %s", rvalue);
                return 0;
        }

        n->cinfo.ifa_prefered = k;
        TAKE_PTR(n);

        return 0;
}

int config_parse_address_flags(const char *unit,
                               const char *filename,
                               unsigned line,
                               const char *section,
                               unsigned section_line,
                               const char *lvalue,
                               int ltype,
                               const char *rvalue,
                               void *data,
                               void *userdata) {
        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring: %s", lvalue, rvalue);
                return 0;
        }

        if (streq(lvalue, "HomeAddress"))
                n->home_address = r;
        else if (streq(lvalue, "ManageTemporaryAddress"))
                n->manage_temporary_address = r;
        else if (streq(lvalue, "PrefixRoute"))
                n->prefix_route = !r;
        else if (streq(lvalue, "AddPrefixRoute"))
                n->prefix_route = r;
        else if (streq(lvalue, "AutoJoin"))
                n->autojoin = r;
        else
                assert_not_reached("Invalid address flag type.");

        n = NULL;
        return 0;
}

int config_parse_address_scope(const char *unit,
                               const char *filename,
                               unsigned line,
                               const char *section,
                               unsigned section_line,
                               const char *lvalue,
                               int ltype,
                               const char *rvalue,
                               void *data,
                               void *userdata) {
        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        if (streq(rvalue, "host"))
                n->scope = RT_SCOPE_HOST;
        else if (streq(rvalue, "link"))
                n->scope = RT_SCOPE_LINK;
        else if (streq(rvalue, "global"))
                n->scope = RT_SCOPE_UNIVERSE;
        else {
                r = safe_atou8(rvalue , &n->scope);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Could not parse address scope \"%s\", ignoring assignment: %m", rvalue);
                        return 0;
                }
        }

        n->scope_set = true;
        n = NULL;
        return 0;
}

int config_parse_duplicate_address_detection(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        AddressFamily a;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r >= 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "For historical reasons, %s=%s means %s=%s. "
                           "Please use 'both', 'ipv4', 'ipv6' or 'none' instead.",
                           lvalue, rvalue, lvalue, r ? "none" : "both");
                n->duplicate_address_detection = r ? ADDRESS_FAMILY_NO : ADDRESS_FAMILY_YES;
                n = NULL;
                return 0;
        }

        a = duplicate_address_detection_address_family_from_string(rvalue);
        if (a < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, SYNTHETIC_ERRNO(EINVAL),
                           "Failed to parse %s=, ignoring: %s", lvalue, rvalue);
                return 0;
        }

        n->duplicate_address_detection = a;
        n = NULL;
        return 0;
}

bool address_is_ready(const Address *a) {
        assert(a);

        return !(a->flags & IFA_F_TENTATIVE);
}

static int address_section_verify(Address *address) {
        if (section_is_invalid(address->section))
                return -EINVAL;

        if (address->family == AF_UNSPEC) {
                assert(address->section);

                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Address section without Address= field configured. "
                                         "Ignoring [Address] section from line %u.",
                                         address->section->filename, address->section->line);
        }

        if (!address->scope_set && in_addr_is_localhost(address->family, &address->in_addr) > 0)
                address->scope = RT_SCOPE_HOST;

        return 0;
}

void network_verify_addresses(Network *network) {
        Address *address, *address_next;

        assert(network);

        LIST_FOREACH_SAFE(addresses, address, address_next, network->static_addresses)
                if (address_section_verify(address) < 0)
                        address_free(address);
}
