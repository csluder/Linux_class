/*
 * xdp_dummy.c - Minimal XDP program for namespace-side veth interfaces
 *
 * PURPOSE:
 * This program runs on v-cli and v-lis (inside the namespaces).
 * It simply passes all packets through to the network stack.
 *
 * WHY IS THIS NEEDED?
 * Veth devices require XDP programs on BOTH ends for ndo_xdp_xmit() to work.
 * This dummy program satisfies that requirement without modifying packets.
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

SEC("xdp")
int xdp_dummy_prog(struct xdp_md* ctx) {
    /*
     * XDP_PASS: Send packet to normal network stack
     * No packet inspection or modification - just pass everything through
     */
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";