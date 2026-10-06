#!/usr/bin/env bash
# Build json2dir.ko, assemble a minimal initramfs and boot it under QEMU
# with the target kernel to verify the module.
set -euo pipefail

cd "$(dirname "$0")"

: "${KDIR:?run inside the nix-shell (see shell.nix)}"
: "${BZIMAGE:?}"
: "${BUSYBOX:?}"

make

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/initrd/bin"

cp "$BUSYBOX" "$WORK/initrd/bin/busybox"
cp json2dir.ko "$WORK/initrd/json2dir.ko"
cp initramfs/init "$WORK/initrd/init"
chmod +x "$WORK/initrd/init"

# Payload: base path (.) NUL json.  Produced with printf so the NUL byte
# lands in the file literally.
printf '.\0{"greeting":"Hello, world!","dir":{"subfile":"Content","subdir":{}},"symlink":["link","target path"],"script":["script","echo Howdy!"]}' \
	> "$WORK/initrd/payload"

(
	cd "$WORK/initrd"
	find . -print0 | cpio --null -ov --format=newc
) | gzip -9 > "$WORK/initramfs.cpio.gz"

echo "== booting =="
timeout 120 qemu-system-x86_64 \
	-m 512M \
	-nographic \
	-kernel "$BZIMAGE" \
	-initrd "$WORK/initramfs.cpio.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1" \
	-no-reboot
