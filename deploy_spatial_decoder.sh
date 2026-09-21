#!/bin/bash
# deploy_spatial_decoder.sh
# Deployment script for Spatial Decoder library on MT5862 Android TV
# Usage: sudo ./deploy_spatial_decoder.sh [target_device_ip]

set -e

# Configuration
TARGET_DEVICE="${1:-192.168.1.3:5555}"  # Default MT5862 ADB address
LIB_DIR="/data/local/tmp"
VENDOR_LIB_DIR="/vendor/lib"
VENDOR_LIB64_DIR="/vendor/lib64"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Spatial Decoder MT5862 Deployment Script ===${NC}"
echo "Target device: $TARGET_DEVICE"

# Check ADB connection
echo -e "${YELLOW}Checking ADB connection...${NC}"
adb connect $TARGET_DEVICE 2>/dev/null || true
adb wait-for-device
DEVICE_SERIAL=$(adb devices | grep -v "List" | awk '{print $1}')
if [ -z "$DEVICE_SERIAL" ]; then
    echo -e "${RED}Error: No device found at $TARGET_DEVICE${NC}"
    exit 1
fi
echo -e "${GREEN}Connected to device: $DEVICE_SERIAL${NC}"

# Verify device is rooted (required for /vendor partition)
echo -e "${YELLOW}Checking root access...${NC}"
if ! adb shell "su -c 'id' 2>/dev/null | grep -q 'uid=0'"; then
    echo -e "${RED}Error: Device must be rooted for /vendor partition access${NC}"
    exit 1
fi
echo -e "${GREEN}Root access confirmed${NC}"

# Remount vendor partition as read-write
echo -e "${YELLOW}Remounting /vendor as read-write...${NC}"
adb shell "su -c 'mount -o rw,remount /vendor'"

# Create vendor lib directories if they don't exist
adb shell "su -c 'mkdir -p /vendor/lib /vendor/lib64'"

# Deploy spatial decoder library
echo -e "${YELLOW}Deploying libspatialdecoder.so...${NC}"
adb push ./build_android/lib/libspatialdecoder.so /data/local/tmp/libspatialdecoder.so
adb shell "su -c 'cp /data/local/tmp/libspatialdecoder.so /vendor/lib/libspatialdecoder.so && chmod 644 /vendor/lib/libspatialdecoder.so && chown root:root /vendor/lib/libspatialdecoder.so'"
adb shell "su -c 'cp /data/local/tmp/libspatialdecoder.so /vendor/lib64/libspatialdecoder.so && chmod 644 /vendor/lib64/libspatialdecoder.so && chown root:root /vendor/lib64/libspatialdecoder.so'"

# Deploy FFmpeg libraries
echo -e "${YELLOW}Deploying FFmpeg libraries...${NC}"
for lib in libavcodec.so libavutil.so libavformat.so libswresample.so; do
    if [ -f "./build_android/ffmpeg/$lib" ]; then
        adb push ./build_android/ffmpeg/$lib /data/local/tmp/
        adb shell "su -c 'cp /data/local/tmp/$lib /vendor/lib/$lib && chmod 644 /vendor/lib/$lib && chown root:root /vendor/lib/$lib'"
        adb shell "su -c 'cp /data/local/tmp/$lib /vendor/lib64/$lib && chmod 644 /vendor/lib64/$lib && chown root:root /vendor/lib64/$lib'"
    else
        echo "Warning: $lib not found in build_android/ffmpeg/"
    fi
done

# Deploy test binary for verification
echo -e "${YELLOW}Deploying decoder_test...${NC}"
adb push ./build_android/bin/decoder_test /data/local/tmp/decoder_test
adb shell "chmod 755 /data/local/tmp/decoder_test"

# Deploy test audio files
echo -e "${YELLOW}Deploying test audio files...${NC}"
adb shell "mkdir -p /data/local/tmp/test_audio"
for file in test_ac3_51_chid.ac3 test_eac3_51.ec3 test_eac3_71_chid.ec3 test_dts_51_chid.dts test_truehd_51_chid.thd; do
    if [ -f "test_audio/$file" ]; then
        adb push "test_audio/$file" /data/local/tmp/test_audio/
    fi
done

# Verify deployment
echo -e "${YELLOW}Verifying deployment...${NC}"
adb shell "ls -lh /vendor/lib/libspatialdecoder.so /vendor/lib64/libspatialdecoder.so"
adb shell "ls -lh /vendor/lib/libavcodec.so /vendor/lib/libavutil.so /vendor/lib/libavformat.so /vendor/lib/libswresample.so"
adb shell "ls -lh /data/local/tmp/decoder_test /data/local/tmp/test_audio/"

# Run quick verification test
echo -e "${YELLOW}Running quick verification test...${NC}"
adb shell "cd /data/local/tmp && export LD_LIBRARY_PATH=/vendor/lib:/vendor/lib64:\$LD_LIBRARY_PATH && ./decoder_test /data/local/tmp/test_audio/ 2>&1 | head -50"

echo -e "${GREEN}=== Deployment Complete ===${NC}"
echo "To run full test suite:"
echo "  adb shell \"cd /data/local/tmp && export LD_LIBRARY_PATH=/vendor/lib:/vendor/lib64:\$LD_LIBRARY_PATH && ./decoder_test /data/local/tmp/test_audio/\""
echo ""
echo "To integrate with Android HAL:"
echo "  1. Copy libspatialdecoder.so to /vendor/lib/ and /vendor/lib64/"
echo "  2. Copy FFmpeg libraries to /vendor/lib/ and /vendor/lib64/"
echo "  3. Update audio HAL to link against libspatialdecoder.so"
echo "  4. Update audio_policy_configuration.xml for passthrough support"
echo "  5. Reboot device"

echo -e "${GREEN}=== Deployment Complete ===${NC}"