#!/bin/bash
#
# xdp_demo.sh - XDP - based Software Router Demo Setup
#
# ARCHITECTURE:
# This script sets up a complete XDP - based routing environment :
#
#   [Client NS]          [Host]              [Listener NS]
#   10.0.0.1             l3loop0             10.0.0.2
#      |                    |                    |
#   v - cli ?? v - cbr ??[XDP Router] ?? v - lbr ?? v - lis
#   [dummy]  [redirect]  [routing]  [redirect]  [dummy]
#
# PACKET FLOW :
# 1. Client sends packet ? v - cli(dummy XDP)
# 2. Packet crosses veth ? v - cbr(redirect XDP) ? redirects to l3loop0
# 3. l3loop0 runs routing XDP ? determines destination ? redirects to v - lbr
# 4. v - lbr checks source MAC ? sees it's from l3loop0 ? passes to v-lis
# 5. v - lis(dummy XDP) ? delivers to listener namespace
#
# Return path works the same way in reverse.

set - e  # Exit on any error
trap 'echo "ERROR at line $LINENO"' ERR  # Show line number on error

# --- Configuration ---
DRIVER_IF = "l3loop0"      # Custom kernel driver interface
V_CLI_BR = "v-cbr"         # Client veth(host side)
V_LIS_BR = "v-lbr"         # Listener veth(host side)
BPF_INC = "-I/usr/include/$(uname -m)-linux-gnu -I/usr/include"
BPF_PIN_DIR = "/sys/fs/bpf"  # BPF filesystem for pinning programs / maps
MODULE_NAME = "napi_xdp"

log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

# Helper function : Convert MAC address to hex bytes for bpftool
# Example : "aa:bb:cc:dd:ee:ff" ? "0xaa 0xbb 0xcc 0xdd 0xee 0xff"
mac_to_hex() {
    local mac = $1
        echo $mac | sed 's/://g' | sed 's/../0x& /g'
}

# --- 1. Cleanup ---
# Remove any existing configuration to start fresh
echo "Cleaning up old state..."

# Remove XDP programs from interfaces
ip link set dev $DRIVER_IF xdp off 2 > / dev / null || true
ip link set dev $V_CLI_BR xdp off 2 > / dev / null || true
ip link set dev $V_LIS_BR xdp off 2 > / dev / null || true

# Remove XDP from namespace interfaces(if they exist)
ip netns exec client_ns ip link set dev v - cli xdp off 2 > / dev / null || true
ip netns exec listener_ns ip link set dev v - lis xdp off 2 > / dev / null || true

# Delete network namespaces
ip netns del client_ns 2 > / dev / null || true
ip netns del listener_ns 2 > / dev / null || true

# Delete veth pairs
ip link del $V_CLI_BR 2 > / dev / null || true
ip link del $V_LIS_BR 2 > / dev / null || true

# Remove pinned BPF programs and maps
rm - rf $BPF_PIN_DIR / xdp_cli $BPF_PIN_DIR / xdp_drv $BPF_PIN_DIR / xdp_lis \
$BPF_PIN_DIR / veth_maps_cli $BPF_PIN_DIR / veth_maps_lis \
$BPF_PIN_DIR / drv_maps $BPF_PIN_DIR / xdp_dummy 2 > / dev / null || true

# Unload kernel module if loaded
if lsmod | grep - q $MODULE_NAME; then
rmmod $MODULE_NAME 2 > / dev / null || true
fi

# --- 2. Load Kernel Module ---
# The kernel module creates the l3loop0 device
log "Loading kernel module..."
insmod ${ MODULE_NAME }.ko || {
    echo "ERROR: Failed to load ${MODULE_NAME}.ko"
        exit 1
}

# Wait for device to appear(kernel module creates it asynchronously)
for i in{ 1..10 }; do
if ip link show $DRIVER_IF& > / dev / null; then
log "Device $DRIVER_IF ready"
break
fi
sleep 0.5
done

# --- 3. Create Network Namespaces ---
# Namespaces provide isolated network stacks for clientand listener
log "Creating namespaces..."
ip netns add client_ns
ip netns add listener_ns

# Bring up loopback interfaces in each namespace
ip netns exec client_ns ip link set lo up
ip netns exec listener_ns ip link set lo up

# --- 4. Create VETH Pairs ---
# Veth pairs are virtual Ethernet cables connecting namespaces to host
log "Creating veth pairs..."

# Create client veth pair : v - cli(in namespace) ? v - cbr(in host)
ip link add v - cli type veth peer name $V_CLI_BR
ip link set v - cli netns client_ns

# Create listener veth pair : v - lis(in namespace) ? v - lbr(in host)
ip link add v - lis type veth peer name $V_LIS_BR
ip link set v - lis netns listener_ns

# Disable hardware offloads(important for XDP to see all packets)
for dev in $V_CLI_BR $V_LIS_BR; do
ethtool - K $dev rx off tx off tso off gso off gro off lro off 2 > / dev / null || true
done

# --- 5. Bring Up Interfaces ---
log "Bringing up interfaces..."
ip link set $V_CLI_BR up
ip link set $V_LIS_BR up
ip link set $DRIVER_IF up

# --- 6. Configure IP Addresses ---
log "Configuring IPs..."

# Client namespace : 10.0.0.1 / 24
ip netns exec client_ns ip addr add 10.0.0.1 / 24 dev v - cli
ip netns exec client_ns ip link set v - cli up

# Listener namespace : 10.0.0.2 / 24
ip netns exec listener_ns ip addr add 10.0.0.2 / 24 dev v - lis
ip netns exec listener_ns ip link set v - lis up

# --- 7. Compile BPF Programs ---
log "Compiling BPF programs..."

# Compile veth redirect program(runs on v - cbr and v - lbr)
clang - O2 - g - Wall - target bpf $BPF_INC \
- c xdp_redirect.c - o xdp_redirect.o || {
    echo "ERROR: Failed to compile xdp_redirect.c"
        exit 1
}

# Compile driver routing program(runs on l3loop0)
clang - O2 - g - Wall - target bpf $BPF_INC \
- c xdp_driver_wire.c - o xdp_driver_wire.o || {
    echo "ERROR: Failed to compile xdp_driver_wire.c"
        exit 1
}

# Compile dummy passthrough program(runs on v - cli and v - lis)
clang - O2 - g - Wall - target bpf $BPF_INC \
- c xdp_dummy.c - o xdp_dummy.o || {
    echo "ERROR: Failed to compile xdp_dummy.c"
        exit 1
}

# --- 8. Load XDP Programs ---
# Programs are loaded into the kerneland pinned to the BPF filesystem
log "Loading XDP programs..."

# Create directories for pinned maps(each program instance needs its own)
mkdir - p $BPF_PIN_DIR / veth_maps_cli   # Maps for v - cbr
mkdir - p $BPF_PIN_DIR / veth_maps_lis   # Maps for v - lbr
mkdir - p $BPF_PIN_DIR / drv_maps        # Maps for l3loop0

# Load driver routing program
bpftool prog load xdp_driver_wire.o $BPF_PIN_DIR / xdp_drv \
pinmaps $BPF_PIN_DIR / drv_maps/ || {
    echo "ERROR: Failed to load driver XDP program"
        exit 1
}

# Attach driver program to l3loop0 in native XDP mode
ip link set dev $DRIVER_IF xdp pinned $BPF_PIN_DIR / xdp_drv

# Load veth redirect program for client side
# Note: We load the same program twice but with different map instances
bpftool prog load xdp_redirect.o $BPF_PIN_DIR / xdp_cli \
pinmaps $BPF_PIN_DIR / veth_maps_cli/ || {
    echo "ERROR: Failed to load client veth XDP program"
        exit 1
}

# Load veth redirect program for listener side
bpftool prog load xdp_redirect.o $BPF_PIN_DIR / xdp_lis \
pinmaps $BPF_PIN_DIR / veth_maps_lis/ || {
    echo "ERROR: Failed to load listener veth XDP program"
        exit 1
}

# Load dummy program(shared by both namespace - side veths)
bpftool prog load xdp_dummy.o $BPF_PIN_DIR / xdp_dummy || {
    echo "ERROR: Failed to load dummy XDP program"
        exit 1
}

# Get the program ID for attaching in namespaces
DUMMY_PROG_ID = $(bpftool prog show pinned $BPF_PIN_DIR / xdp_dummy | head - 1 | awk '{print $1}' | sed 's/://')
log "Dummy XDP program ID: $DUMMY_PROG_ID"

# --- 9. Attach XDP Programs to Interfaces ---
log "Attaching XDP to veth interfaces in NATIVE mode..."

# Attach redirect programs to host - side veth interfaces
ip link set dev $V_CLI_BR xdp pinned $BPF_PIN_DIR / xdp_cli || {
    echo "ERROR: Failed to attach XDP to $V_CLI_BR"
        exit 1
}

ip link set dev $V_LIS_BR xdp pinned $BPF_PIN_DIR / xdp_lis || {
    echo "ERROR: Failed to attach XDP to $V_LIS_BR"
        exit 1
}

log "Attaching dummy XDP to namespace-side veth interfaces..."

# Attach dummy program to namespace - side interfaces using program ID
# (Program IDs are global, so they work across namespaces)
ip netns exec client_ns bpftool net attach xdp id $DUMMY_PROG_ID dev v - cli || {
    echo "ERROR: Failed to attach dummy XDP to v-cli"
        exit 1
}

ip netns exec listener_ns bpftool net attach xdp id $DUMMY_PROG_ID dev v - lis || {
    echo "ERROR: Failed to attach dummy XDP to v-lis"
        exit 1
}

# --- 10. Configure BPF Maps ---
# Maps store configuration data that XDP programs use at runtime
log "Configuring DEVMAPs and l3loop0 MAC address..."

# Get interface indices(used for redirect targets)
CLI_IDX = $(cat / sys / class / net / $V_CLI_BR / ifindex)
LIS_IDX = $(cat / sys / class / net / $V_LIS_BR / ifindex)
DRV_IDX = $(cat / sys / class / net / $DRIVER_IF / ifindex)

# Get MAC addresses
CLI_MAC = $(cat / sys / class / net / $V_CLI_BR / address)
LIS_MAC = $(cat / sys / class / net / $V_LIS_BR / address)
DRV_MAC = $(cat / sys / class / net / $DRIVER_IF / address)

log "Interface indices: Client=$CLI_IDX, Listener=$LIS_IDX, Driver=$DRV_IDX"
log "MAC addresses: Client=$CLI_MAC, Listener=$LIS_MAC, Driver=$DRV_MAC"

# Convert l3loop0 MAC to hex format for bpftool
DRV_MAC_HEX = $(mac_to_hex $DRV_MAC)

# Configure client veth maps
# tx_port map : Where to redirect packets(to l3loop0)
bpftool map update pinned $BPF_PIN_DIR / veth_maps_cli / tx_port \
key 0 0 0 0 value $DRV_IDX 0 0 0

# l3loop_mac map : l3loop0's MAC for identifying return traffic
bpftool map update pinned $BPF_PIN_DIR / veth_maps_cli / l3loop_mac \
key 0 0 0 0 value $DRV_MAC_HEX 0x00 0x00

# Configure listener veth maps(same as client)
bpftool map update pinned $BPF_PIN_DIR / veth_maps_lis / tx_port \
key 0 0 0 0 value $DRV_IDX 0 0 0

bpftool map update pinned $BPF_PIN_DIR / veth_maps_lis / l3loop_mac \
key 0 0 0 0 value $DRV_MAC_HEX 0x00 0x00

# Configure driver routing map
# Key 0 ? Client veth(10.0.0.1)
bpftool map update pinned $BPF_PIN_DIR / drv_maps / route_map \
key 0 0 0 0 value $CLI_IDX 0 0 0

# Key 1 ? Listener veth(10.0.0.2)
bpftool map update pinned $BPF_PIN_DIR / drv_maps / route_map \
key 1 0 0 0 value $LIS_IDX 0 0 0

# --- 11. Verification and Summary ---
echo ""
echo "========================================="
echo "XDP LAB READY"
echo "========================================="
echo "Architecture:"
echo "  Client (10.0.0.1) ? v-cbr [redirect] ? l3loop0 [router] ? v-lbr [redirect] ? Listener (10.0.0.2)"
echo ""
echo "Devices:"
echo "  Driver:   $DRIVER_IF (ifindex: $DRV_IDX, MAC: $DRV_MAC)"
echo "  Client:   $V_CLI_BR (ifindex: $CLI_IDX, MAC: $CLI_MAC)"
echo "  Listener: $V_LIS_BR (ifindex: $LIS_IDX, MAC: $LIS_MAC)"
echo ""
echo "XDP Programs Attached:"
bpftool net list
echo ""
echo "Routing Configuration:"
echo "  Client veth tx_port (redirect target):"
bpftool map dump pinned $BPF_PIN_DIR / veth_maps_cli / tx_port | head - 2
echo "  Client veth l3loop_mac (return traffic filter):"
bpftool map dump pinned $BPF_PIN_DIR / veth_maps_cli / l3loop_mac | head - 2
echo "  Listener veth tx_port:"
bpftool map dump pinned $BPF_PIN_DIR / veth_maps_lis / tx_port | head - 2
echo "  Listener veth l3loop_mac:"
bpftool map dump pinned $BPF_PIN_DIR / veth_maps_lis / l3loop_mac | head - 2
echo "  Driver route_map (IP routing table):"
bpftool map dump pinned $BPF_PIN_DIR / drv_maps / route_map | head - 3
echo ""
echo "========================================="
echo "TEST COMMANDS:"
echo "  Ping:     sudo ip netns exec client_ns ping -c 3 10.0.0.2"
echo "  Reverse:  sudo ip netns exec listener_ns ping -c 3 10.0.0.1"
echo "  Monitor:  sudo ip netns exec listener_ns tcpdump -i v-lis -n"
echo "  Flush ARP: sudo ip netns exec client_ns ip neigh flush all"
echo "========================================="

log "Setup complete! XDP-based software router is ready."