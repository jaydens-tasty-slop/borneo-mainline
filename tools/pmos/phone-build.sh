#!/bin/sh
# Runs on the phone, outside the Alpine chroot, started by tools/mkpmos. Builds the postmarketOS
# root in an ext4 image on the microSD card, using apk from the Alpine chroot, so that nothing
# downloaded ever runs on the host. Install scripts run here on the phone, natively.
#
# The eMMC isn't involved: init has already write-protected it at the block layer.
set -eu

IMG=/sd/pmos.ext4
SIZE=20G
FILES=/sd/pmos-files
T=/r/mnt/pmos

say() { echo "=== $(date -u +%T) $*"; }

for d in /sys/block/mmcblk*; do
	if [ "$(cat "$d/device/type" 2>/dev/null)" = MMC ] && [ "$(cat "$d/ro")" != 1 ]; then
		echo "the eMMC is writable, stopping"
		exit 1
	fi
done

mkdir -p /r/sd "$T"
grep -q " /r/sd " /proc/mounts || mount --bind /sd /r/sd

if [ ! -e "$IMG" ]; then
	say "making $SIZE image"
	truncate -s "$SIZE" "$IMG"
	chroot /r /sbin/mke2fs -F -q -t ext4 -L pmOS_root -m 0 /sd/pmos.ext4
fi
# noinit_itable: exFAT can't punch holes, so ext4's lazy zeroing only fails and logs errors.
grep -q " $T " /proc/mounts || mount -o loop,noinit_itable "$IMG" "$T"

mkdir -p "$T/etc/apk/keys" "$T/proc" "$T/sys" "$T/dev" "$T/run" "$T/tmp" "$T/sd"
cp "$FILES"/keys/* "$T/etc/apk/keys/"
cat > "$T/etc/apk/repositories" <<'EOF'
https://mirror.postmarketos.org/postmarketos/extra-repos/systemd/master
https://mirror.postmarketos.org/postmarketos/master
https://dl-cdn.alpinelinux.org/alpine/edge/main
https://dl-cdn.alpinelinux.org/alpine/edge/community
https://dl-cdn.alpinelinux.org/alpine/edge/testing
EOF
for d in proc sys dev; do
	grep -q " $T/$d " /proc/mounts || mount -o rbind "/$d" "$T/$d"
done
grep -q " $T/run " /proc/mounts || mount -t tmpfs tmpfs "$T/run"
grep -q " $T/tmp " /proc/mounts || mount -t tmpfs tmpfs "$T/tmp"
# apk needs to resolve names itself, inside the target when scripts run.
cp /r/etc/resolv.conf "$T/etc/resolv.conf" 2>/dev/null || true

PACKAGES="
postmarketos-base postmarketos-base-systemd
postmarketos-base-ui-audio-backend-pipewire
postmarketos-ui-plasma-mobile plasma-mobile plasma-settings spectacle
angelfish qmlkonsole plasma-dialer spacebar kclock kweather kalk elisa koko
mesa-dri-gallium mesa-vulkan-freedreno mesa-gles mesa-egl kmscube
alsa-utils alsa-ucm-conf pipewire-alsa
networkmanager networkmanager-wifi networkmanager-wwan iw modemmanager qmi-utils qrtr rmtfs tqftpserv
linux-firmware-ath10k linux-firmware-qca
bluez bluez-btmgmt bootmac e2fsprogs e2fsprogs-extra
hexagonrpcd hexagonrpcd-systemd hexagonrpcd-udev libssc iio-sensor-proxy
evtest i2c-tools strace htop tcpdump font-noto
"

say "installing packages"
# shellcheck disable=SC2086
chroot /r /sbin/apk --root /mnt/pmos --initdb --arch aarch64 --no-interactive --update-cache \
	--no-progress add $PACKAGES || say "apk returned $? (triggers may fail in a chroot; checking below)"

chroot "$T" /sbin/apk --no-interactive --no-progress fix 2>&1 | tail -n 5 || true

say "firmware"
tar -xf "$FILES/firmware.tar" -C "$T/lib/"
# What hexagonrpcd serves to the ADSP.
mkdir -p "$T/usr/share/qcom"
tar -xf "$FILES/qcom.tar" -C "$T/usr/share/qcom/"

# pmbootstrap would have done this. Without it nothing works under systemd: its PATH has no /bin,
# where busybox's applets are, and units name paths such as /usr/sbin/agetty.
say "merging /usr"
[ -L "$T/bin" ] || chroot "$T" /usr/bin/merge-usr 2>&1 | grep -v "^INFO"

say "configuring"
mkdir -p "$T/usr/local/sbin"
cp "$FILES/bh-adbd" "$T/usr/local/sbin/bh-adbd"
chmod 755 "$T/usr/local/sbin/bh-adbd"
tar -xf "$FILES/config.tar" -C "$T/"
cp "$FILES/phone-config.sh" "$T/tmp/phone-config.sh"
# Put there by tools/mkpmos. The config script reads them and removes this copy.
mkdir -p "$T/tmp/secrets"
chmod 700 "$T/tmp/secrets"
cp "$FILES/secrets/"* "$T/tmp/secrets/"
chroot "$T" /bin/sh /tmp/phone-config.sh
rm -rf "$T/tmp/secrets" "$FILES/secrets"

say "used space"
df -h "$T" | tail -n 1

sync
for d in tmp run dev sys proc; do
	umount -l "$T/$d"
done
umount "$T"
sync
say "done"
