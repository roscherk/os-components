#!/bin/bash
set -e

DEVICE=/dev/ramdisk
MOUNT=/mnt/ramdisk_test
MODULE=ramdisk
SIZE_MB=${1:-16}
PASS=0
FAIL=0

die() { echo "FAIL: $*" >&2; FAIL=$((FAIL+1)); cleanup; exit 1; }
pass() { echo "PASS: $*"; PASS=$((PASS+1)); }

cleanup() {
	mountpoint -q "$MOUNT" && umount "$MOUNT" 2>/dev/null || true
	lsmod | grep -q "^$MODULE " && rmmod "$MODULE" 2>/dev/null || true
	rm -rf "$MOUNT"
}

trap cleanup EXIT

mkdir -p "$MOUNT"

echo "=== Loading module: disk_size_mb=$SIZE_MB ==="
insmod ${MODULE}.ko disk_size_mb="$SIZE_MB"
[ -b "$DEVICE" ] || die "device $DEVICE not created"
pass "module loaded, $DEVICE exists"

echo "=== mkfs.ext4 ==="
mkfs.ext4 -F "$DEVICE" >/dev/null 2>&1
pass "ext4 filesystem created"

echo "=== mount ==="
mount "$DEVICE" "$MOUNT"
pass "mounted at $MOUNT"

echo "=== write and verify file ==="
TESTFILE="$MOUNT/testfile"
echo "hello ramdisk $(date)" > "$TESTFILE"
sync
CONTENT=$(cat "$TESTFILE")
echo "$CONTENT" | grep -q "hello ramdisk" || die "file content mismatch after write"
pass "file write/read verified"

echo "=== checksum: direct I/O + unmount/remount round-trip ==="
SRC=/tmp/ramdisk_src.bin
dd if=/dev/urandom of="$SRC" bs=1M count=4 2>/dev/null
SUM_SRC=$(md5sum "$SRC" | awk '{print $1}')
dd if="$SRC" of="$MOUNT/random.bin" bs=1M oflag=direct 2>/dev/null
sync
umount "$MOUNT"
mount "$DEVICE" "$MOUNT"
SUM_BACK=$(dd if="$MOUNT/random.bin" bs=1M iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
[ "$SUM_SRC" = "$SUM_BACK" ] || die "checksum mismatch after remount: src=$SUM_SRC back=$SUM_BACK"
pass "data survived unmount/remount with direct I/O: $SUM_SRC"
rm -f "$SRC"

echo "=== raw device dd direct I/O round-trip ==="
umount "$MOUNT"
RAW=/tmp/ramdisk_raw.bin
dd if=/dev/urandom of="$RAW" bs=4096 count=256 2>/dev/null
SUM_RAW=$(md5sum "$RAW" | awk '{print $1}')
dd if="$RAW" of="$DEVICE" bs=4096 count=256 oflag=direct,sync 2>/dev/null
SUM_DEV=$(dd if="$DEVICE" bs=4096 count=256 iflag=direct 2>/dev/null | md5sum | awk '{print $1}')
[ "$SUM_RAW" = "$SUM_DEV" ] || die "raw direct I/O mismatch: wrote=$SUM_RAW read=$SUM_DEV"
pass "raw direct I/O round-trip verified: $SUM_RAW"
rm -f "$RAW"

echo "=== raw dd with sub-sector block size (256 B, < 512) ==="
SMALL=/tmp/ramdisk_small.bin
dd if=/dev/urandom of="$SMALL" bs=256 count=64 2>/dev/null
SUM_SMALL=$(md5sum "$SMALL" | awk '{print $1}')
dd if="$SMALL" of="$DEVICE" bs=256 count=64 2>/dev/null
sync
echo 3 > /proc/sys/vm/drop_caches
SUM_SMALL_BACK=$(dd if="$DEVICE" bs=256 count=64 2>/dev/null | md5sum | awk '{print $1}')
[ "$SUM_SMALL" = "$SUM_SMALL_BACK" ] || die "sub-sector round-trip mismatch: wrote=$SUM_SMALL read=$SUM_SMALL_BACK"
pass "sub-sector (256 B) round-trip verified: $SUM_SMALL"
rm -f "$SMALL"

echo "=== raw dd with non-aligned block size (523 B, indivisible by 512) ==="
ODD=/tmp/ramdisk_odd.bin
dd if=/dev/urandom of="$ODD" bs=523 count=32 2>/dev/null
SUM_ODD=$(md5sum "$ODD" | awk '{print $1}')
dd if="$ODD" of="$DEVICE" bs=523 count=32 2>/dev/null
sync
echo 3 > /proc/sys/vm/drop_caches
SUM_ODD_BACK=$(dd if="$DEVICE" bs=523 count=32 2>/dev/null | md5sum | awk '{print $1}')
[ "$SUM_ODD" = "$SUM_ODD_BACK" ] || die "non-aligned round-trip mismatch: wrote=$SUM_ODD read=$SUM_ODD_BACK"
pass "non-aligned (523 B) round-trip verified: $SUM_ODD"
rm -f "$ODD"

echo "=== badblocks read-write test ==="
BB_OUT=$(badblocks -wsv -b 4096 "$DEVICE" 2>&1)
echo "$BB_OUT" | tail -5
echo "$BB_OUT" | grep -q "0 bad blocks found" || die "badblocks found errors"
pass "badblocks: no bad blocks"

echo ""
echo "=== Results: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
