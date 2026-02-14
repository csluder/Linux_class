#!/bin/bash

# Configuration
MASTER_KEY_HEX="0123456789abcdeffedcba9876543210"
SESSION_KEY_RAW="SixteenByteS_Key" 
ZERO_IV="00000000000000000000000000000000"

SYSFS_LADDER="/sys/class/l3harris_secure/l3harris_secure/key_blob"
CHAR_DEV="/dev/l3harris_secure"

# Helper function to convert raw string to hex using hexdump
to_hex() {
    printf "%s" "$1" | hexdump -ve '1/1 "%.2x"'
}

# 1. Create a simulated 512-byte LUKS key file
cat pass_phrase | head -c 512 > luks_raw.key
truncate -s 512 luks_raw.key

# 2. Wrap Session Key with Master Key
echo "[+] Wrapping Session Key..."
S_BLOB=$(printf "%s" "$SESSION_KEY_RAW" | openssl enc -aes-128-cbc -e -nopad -K "$MASTER_KEY_HEX" -iv "$ZERO_IV")

# Injecting Session Blob into sysfs
# Binary blobs from openssl can be piped directly to dd
echo -n "$S_BLOB" | sudo dd of="$SYSFS_LADDER" bs=16 count=1 2>/dev/null

# 3. Wrap 512-byte LUKS Key with Session Key
echo "[+] Wrapping 512-byte LUKS Key..."
S_KEY_HEX=$(to_hex "$SESSION_KEY_RAW")
openssl enc -aes-128-cbc -e -nopad -K "$S_KEY_HEX" -iv "$ZERO_IV" -in luks_raw.key -out luks_encrypted.blob

# 4. Inject and Retrieve
echo "[+] Injecting 512-byte blob into driver..."
sudo dd if=luks_encrypted.blob of="$CHAR_DEV" bs=512 count=1 2>/dev/null

echo "[+] Retrieving plaintext via cat..."
RESULT=$(sudo cat "$CHAR_DEV")

echo "------------------------------------------"
echo "Retrieved Result:\n $RESULT"
echo "------------------------------------------"

