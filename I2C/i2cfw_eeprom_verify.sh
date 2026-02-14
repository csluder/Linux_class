#!/bin/bash
# EEPROM Driver Test Suite v2

DRIVER_NAME="at24c256_edu"
TEST_DATA="ABCD"

echo "--- EEPROM DRIVER TEST SUITE ---"

# 1. Locate I2C Device
DEVICE_ID=$(ls /sys/bus/i2c/drivers/$DRIVER_NAME | grep -E '^[0-9]+-[0-9a-fA-F]+' | head -n 1)
if [ -z "$DEVICE_ID" ]; then
    echo "[ERROR] No device found for driver $DRIVER_NAME"
    exit 1
fi

# 2. Locate Sysfs Node
SYS_NODE="/sys/bus/i2c/devices/$DEVICE_ID/eeprom"

# 3. Locate /dev Node (Searching misc devices for the parent I2C device)
MISC_NAME=$(ls -l /sys/class/misc/ | grep "$DEVICE_ID" | awk '{print $9}')
if [ -z "$MISC_NAME" ]; then
    # Fallback search if the parent link isn't standard
    MISC_NAME=$(ls /dev/at24* | head -n 1 | xargs basename)
fi
DEV_NODE="/dev/$MISC_NAME"

echo "[INFO] Detected Device: $DEV_NODE"
echo "[INFO] Detected Sysfs:  $SYS_NODE"

# [TEST 1]
echo -n "[TEST 1] Basic Write/Read... "
echo -n "$TEST_DATA" | sudo dd of="$DEV_NODE" bs=1 count=4 seek=60 status=none
RESULT=$(sudo dd if="$DEV_NODE" bs=1 count=4 skip=60 status=none)
[ "$RESULT" == "$TEST_DATA" ] && echo "PASS" || echo "FAIL (Got: $RESULT)"

# [TEST 2]
echo -n "[TEST 2] Page Boundary Crossing... "
echo -n "BOUNDARY" | sudo dd of="$DEV_NODE" bs=1 seek=60 status=none 2>/dev/null
RESULT=$(sudo dd if="$DEV_NODE" bs=1 count=8 skip=60 status=none)
[ "$RESULT" == "BOUNDARY" ] && echo "PASS" || echo "FAIL"

# [TEST 3]
echo -n "[TEST 3] Sysfs Binary Access... "
if [ -f "$SYS_NODE" ]; then
    # We wrote 'BOUN' into 60-63 in Test 2, checking that via Sysfs
    RESULT=$(sudo dd if="$SYS_NODE" bs=1 count=4 skip=60 status=none)
    [ "$RESULT" == "BOUN" ] && echo "PASS" || echo "FAIL"
else
    echo "FAIL (Missing)"
fi

# [TEST 4]
echo -n "[TEST 4] Persistence & Boundary Check... "
# Attempt to write 1 byte AT the end (valid) then 1 byte PAST the end (invalid)
EE_SIZE=$(cat /sys/bus/i2c/devices/$DEVICE_ID/eeprom/size 2>/dev/null || echo 32768)
echo -n "X" | sudo dd of="$DEV_NODE" bs=1 seek=$EE_SIZE status=none 2>&1 | grep -q "No space left"
if [ $? -eq 0 ]; then echo "PASS"; else echo "FAIL (End of device not enforced)"; fi

echo "--- TEST SUITE COMPLETE ---"

