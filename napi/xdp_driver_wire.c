/*
 * xdp_driver_wire.c - XDP routing program for l3loop0 driver
 *
 * PURPOSE:
 * This program runs on the l3loop0 device and makes Layer 3 routing decisions.
 * It examines the destination IP address in ARP and IP packets and determines
 * which veth interface to redirect the packet to.
 *
 * ROUTING TABLE:
 * 10.0.0.1 ? v-cbr (client veth, map key 0)
 * 10.0.0.2 ? v-lbr (listener veth, map key 1)
 *
 * This implements a simple software router using XDP for line-rate performance.
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>

 /*
  * DEVMAP: Routes packets to destination interfaces
  * Key 0 ? v-cbr ifindex (client at 10.0.0.1)
  * Key 1 ? v-lbr ifindex (listener at 10.0.0.2)
  */
struct {
    __uint(type, BPF_MAP_TYPE_DEVMAP);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u32);
} route_map SEC(".maps");

/*
 * ARP packet structure
 * Defines the layout of an ARP packet for parsing
 */
struct arp_hdr {
    __u16 hrd;              /* Hardware type (Ethernet = 1) */
    __u16 pro;              /* Protocol type (IPv4 = 0x0800) */
    __u8  hln;              /* Hardware address length (6 for MAC) */
    __u8  pln;              /* Protocol address length (4 for IPv4) */
    __u16 op;               /* Operation (1=request, 2=reply) */
    __u8  sha[ETH_ALEN];    /* Sender hardware address (MAC) */
    __u32 spa;              /* Sender protocol address (IP) */
    __u8  tha[ETH_ALEN];    /* Target hardware address (MAC) */
    __u32 tpa;              /* Target protocol address (IP) */
} __attribute__((packed));

/* IP addresses in network byte order (big-endian) */
#define CLIENT_IP    0x0100000A  /* 10.0.0.1 */
#define LISTENER_IP  0x0200000A  /* 10.0.0.2 */

/*
 * Main XDP routing program
 * Examines packet headers and makes forwarding decisions
 */
SEC("xdp")
int xdp_driver_prog(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;
    struct ethhdr* eth = data;

    /* Validate Ethernet header */
    if ((void*)(eth + 1) > data_end) {
        return XDP_PASS;  /* Malformed packet, pass to stack */
    }

    /*
     * ARP PACKET HANDLING
     * ARP is used to resolve IP addresses to MAC addresses
     * We route based on the Target Protocol Address (TPA)
     */
    if (eth->h_proto == bpf_htons(ETH_P_ARP)) {
        struct arp_hdr* arp = data + sizeof(*eth);

        /* Validate ARP header */
        if ((void*)(arp + 1) > data_end) {
            return XDP_PASS;
        }

        /* Route to client (10.0.0.1) */
        if (arp->tpa == CLIENT_IP) {
            return bpf_redirect_map(&route_map, 0, 0);
        }

        /* Route to listener (10.0.0.2) */
        if (arp->tpa == LISTENER_IP) {
            return bpf_redirect_map(&route_map, 1, 0);
        }
    }

    /*
     * IP PACKET HANDLING
     * Route based on destination IP address
     */
    if (eth->h_proto == bpf_htons(ETH_P_IP)) {
        struct iphdr* iph = data + sizeof(*eth);

        /* Validate IP header */
        if ((void*)(iph + 1) > data_end) {
            return XDP_PASS;
        }

        /* Route to client (10.0.0.1) */
        if (iph->daddr == CLIENT_IP) {
            return bpf_redirect_map(&route_map, 0, 0);
        }

        /* Route to listener (10.0.0.2) */
        if (iph->daddr == LISTENER_IP) {
            return bpf_redirect_map(&route_map, 1, 0);
        }
    }

    /*
     * Unknown packet type or destination
     * Pass to normal network stack for handling
     */
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";