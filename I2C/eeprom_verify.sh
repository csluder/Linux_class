#!/bin/bash
# EEPROM Driver Verification Script
# Designed for use with the at24c256 driver

DEV_NODE=$(ls /dev/at24c256-* 2>/dev/null | head -n 1)
SYS_NODE=$(find /sys/bus/i2c/devices/ -name "eeprom" 2>/dev/null | head -n 1)

echo "--- EEPROM DRIVER TEST SUITE ---"

# Check if driver is loaded
if [ -z "$DEV_NODE" ]; then
    echo "[FAIL] Device node /dev/at24c256-* not found. Is the driver loaded?"
    exit 1
fi

echo "[INFO] Testing Device: $DEV_NODE"
echo "[INFO] Testing Sysfs:  $SYS_NODE"

# Test 1: Basic Write/Read
echo -n "[TEST 1] Basic Write/Read... "
echo "Kernel-Lab" | sudo dd of=$DEV_NODE bs=1 seek=0 status=none
RESULT=$(sudo dd if=$DEV_NODE bs=1 count=10 status=none)
if [ "$RESULT" == "Kernel-Lab" ]; then
    echo "PASS"
else
    echo "FAIL (Got: $RESULT)"
fi

# Test 2: Page Boundary Crossing (The most critical hardware test)
# We write 10 bytes at offset 60. The boundary is at 64.
echo -n "[TEST 2] Page Boundary Crossing (Offset 60)... "
STR="ABCDEFGHIJ"
echo -n "$STR" | sudo dd of=$DEV_NODE bs=1 seek=60 status=none
# Read back 10 bytes starting at 60
RESULT=$(sudo dd if=$DEV_NODE bs=1 skip=60 count=10 status=none)
if [ "$RESULT" == "$STR" ]; then
    echo "PASS"
else
    echo "FAIL (Rollover detected or bad write)"
fi

# Test 3: Sysfs Interface Verification
echo -n "[TEST 3] Sysfs Binary Access... "
if [ -f "$SYS_NODE" ]; then
    sudo dd if=$SYS_NODE bs=1 count=4 skip=60 status=none | grep -q "ABCD"
    if [ $? -eq 0 ]; then
        echo "PASS"
    else
        echo "FAIL (Data mismatch via sysfs)"
    fi
else
    echo "FAIL (Sysfs node missing)"
fi

# Test 4: Persistence Test
echo -n "[TEST 4] Persistence & Offset Seek... "
# Write to the end of the address space
echo "END" | sudo dd of=$DEV_NODE bs=1 seek=32765 status=none
RESULT=$(sudo dd if=$DEV_NODE bs=1 skip=32765 count=3 status=none)
if [ "$RESULT" == "END" ]; then
    echo "PASS"
else
    echo "FAIL (End of memory access failed)"
fi

echo "--- TEST SUITE COMPLETE ---"
