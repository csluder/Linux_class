/*
 * xdp_redirect.c - XDP program for veth interfaces
 *
 * PURPOSE:
 * This program runs on the host-side veth interfaces (v-cbr and v-lbr).
 * It redirects packets FROM the namespaces TO the l3loop0 driver for routing,
 * and passes packets FROM l3loop0 THROUGH to the namespaces.
 *
 * PACKET FLOW:
 * 1. Packet from namespace ? Check source MAC ? Not l3loop0 ? REDIRECT to l3loop0
 * 2. Packet from l3loop0 ? Check source MAC ? Is l3loop0 ? PASS to namespace
 *
 * This prevents redirect loops while allowing bidirectional traffic.
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>

 /*
  * DEVMAP: Maps a key to a network device interface index
  * Used to specify the redirect target (l3loop0)
  * Key 0 ? l3loop0's ifindex (populated by userspace script)
  */
struct {
    __uint(type, BPF_MAP_TYPE_DEVMAP);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} tx_port SEC(".maps");

/*
 * ARRAY MAP: Stores l3loop0's MAC address
 * Used to identify packets coming FROM l3loop0 (return traffic)
 * Key 0 ? l3loop0's MAC address as 64-bit value (6 bytes + 2 padding)
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} l3loop_mac SEC(".maps");

/*
 * Helper function: Compare Ethernet MAC addresses
 *
 * @mac1: First MAC address (from packet header)
 * @mac2_val: Second MAC address stored as 64-bit value
 * @return: 1 if equal, 0 if different
 *
 * Note: #pragma unroll tells the compiler to unroll the loop for performance
 */
static inline int mac_equals(unsigned char* mac1, __u64 mac2_val) {
    unsigned char* mac2 = (unsigned char*)&mac2_val;
#pragma unroll
    for (int i = 0; i < ETH_ALEN; i++) {
        if (mac1[i] != mac2[i])
            return 0;
    }
    return 1;
}

/*
 * Main XDP program - runs for every packet on the veth interface
 *
 * @ctx: XDP context containing packet data pointers
 * @return: XDP action (XDP_PASS, XDP_REDIRECT, or XDP_DROP)
 */
SEC("xdp")
int xdp_redirect_prog(struct xdp_md* ctx) {
    /* Get packet data boundaries from XDP context */
    void* data_end = (void*)(long)ctx->data_end;
    void* data = (void*)(long)ctx->data;

    /* Parse Ethernet header */
    struct ethhdr* eth = data;

    /* Bounds check: Ensure we have at least a full Ethernet header
     * This is REQUIRED in XDP to pass the verifier */
    if ((void*)(eth + 1) > data_end) {
        return XDP_DROP;  /* Packet too small, drop it */
    }

    /*
     * DIRECTION DETECTION:
     * Check if this packet is FROM l3loop0 (return traffic to namespace)
     * or FROM namespace (outgoing traffic to be routed)
     */
    __u32 key = 0;
    __u64* l3loop_mac_val = bpf_map_lookup_elem(&l3loop_mac, &key);

    if (l3loop_mac_val && mac_equals(eth->h_source, *l3loop_mac_val)) {
        /*
         * Source MAC matches l3loop0 ? This is RETURN traffic
         * Pass it through to the namespace (don't redirect back to l3loop0)
         * This prevents redirect loops
         */
        return XDP_PASS;
    }

    /*
     * Source MAC does NOT match l3loop0 ? This is OUTGOING traffic from namespace
     * Redirect it to l3loop0 for routing
     */

     /* Check if redirect target is configured */
    void* target = bpf_map_lookup_elem(&tx_port, &key);
    if (!target) {
        /* No redirect target configured, pass to normal stack */
        return XDP_PASS;
    }

    /*
     * Perform the redirect using DEVMAP
     * This is a zero-copy operation - the packet buffer is transferred
     * to the target device without copying
     */
    return bpf_redirect_map(&tx_port, key, 0);
}

/* License declaration - required for BPF programs */
char _license[] SEC("license") = "GPL";