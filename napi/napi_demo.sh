#!/bin/bash

# --- 1. Cleanup old state to ensure a fresh demo ---
echo "Cleaning up old namespaces and bridge..."
ip netns del client_ns 2>/dev/null
ip netns del listener_ns 2>/dev/null
ip link del br0 2>/dev/null

# --- 2. Create the Namespaces ---
ip netns add client_ns
ip netns add listener_ns

# --- 3. Create the VETH pairs (Short names to avoid 15-char limit) ---
# v-cli/v-lis are the 'cables' inside the namespaces
# v-cbr/v-lbr are the 'plugs' that go into the bridge
ip link add v-cli type veth peer name v-cbr
ip link add v-lis type veth peer name v-lbr

# --- 4. Move VETH ends into the namespaces ---
ip link set v-cli netns client_ns
ip link set v-lis netns listener_ns

# --- 5. Create the Bridge ---
ip link add br0 type bridge
ip link set br0 up

# --- 6. Attach everything to the Bridge ---
# v-cbr: Client connection
# v-lbr: Listener connection
# l3loop0: YOUR NAPI DRIVER (The 'loopback wire')
ip link set v-cbr master br0
ip link set v-lbr master br0
ip link set l3loop0 master br0

# --- 7. CRITICAL: Enable Hairpin Mode on the Driver ---
# This tells the bridge: "Yes, send packets back out the same port they arrived on"
# Without this, the bridge drops the looped-back packets as a 'loop' error.
ip link set l3loop0 type bridge_slave hairpin on
bridge link set dev l3loop0 learning off # Prevents FDB pollution

# --- 8. Bring all interfaces UP ---
ip link set v-cbr up
ip link set v-lbr up
ip link set l3loop0 up

# --- 9. Configure IPs inside Namespaces ---
# Client: 10.0.0.1
ip netns exec client_ns ip link set lo up
ip netns exec client_ns ip link set v-cli up
ip netns exec client_ns ip addr add 10.0.0.1/24 dev v-cli

# Listener: 10.0.0.2
ip netns exec listener_ns ip link set lo up
ip netns exec listener_ns ip link set v-lis up
ip netns exec listener_ns ip addr add 10.0.0.2/24 dev v-lis

echo "--------------------------------------------------------"
echo "L3 NAPI LAB READY"
echo "--------------------------------------------------------"
echo "Path: [Client: 10.0.0.1] -> br0 -> l3loop0 (NAPI Driver) -> br0 -> [Listener: 10.0.0.2]"
echo ""
echo "To Test (Listener): sudo ip netns exec listener_ns nc -l -u -p 8080"
echo "To Test (Client):   sudo ip netns exec client_ns nc -u 10.0.0.2 8080"
echo "--------------------------------------------------------"

