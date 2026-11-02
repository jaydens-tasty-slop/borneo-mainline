#!/bin/sh
# Runs inside the new root on the phone, once the packages are in. Does what pmbootstrap's install
# step would have: the user, the hostname and the enabled services. Prints nothing derived from
# the WiFi credentials.
set -u

echo borneo > /etc/hostname
cat > /etc/hosts <<'EOF'
127.0.0.1 localhost borneo
::1 localhost borneo
EOF

# Some packages' install scripts failed to make their users and hid the error: avahi and pulse, in
# the first build. These scripts are safe to run again, which is what apk's reinstall would do.
mkdir -p /tmp/apk-scripts
tar -xzf /lib/apk/db/scripts.tar.gz -C /tmp/apk-scripts
for s in /tmp/apk-scripts/*-install; do
	if grep -qE 'adduser|addgroup' "$s"; then
		sh "$s" >/dev/null 2>&1 || echo "failed again: ${s##*/}"
	fi
done
rm -rf /tmp/apk-scripts
systemd-sysusers >/dev/null 2>&1

if ! id user >/dev/null 2>&1; then
	adduser -D -u 10000 -s /bin/ash user
	echo "user:147147" | chpasswd
fi
for g in audio input netdev plugdev video wheel feedbackd seat render dialout bluetooth; do
	addgroup -S "$g" 2>/dev/null
	addgroup user "$g" 2>/dev/null
done
echo user > /etc/default_user

mkdir -p /home/user/.ssh /root/.ssh
cp /tmp/secrets/authorized_keys /home/user/.ssh/authorized_keys
cp /tmp/secrets/authorized_keys /root/.ssh/authorized_keys
chown -R user:user /home/user/.ssh
chmod 700 /home/user/.ssh /root/.ssh

# The WiFi network, as NetworkManager wants it. build/wifi.txt's first line is the name and the
# second is the password.
if [ -s /tmp/secrets/wifi ]; then
	{
		ssid=$(sed -n 1p /tmp/secrets/wifi)
		psk=$(sed -n 2p /tmp/secrets/wifi)
		umask 077
		mkdir -p /etc/NetworkManager/system-connections
		cat > /etc/NetworkManager/system-connections/home.nmconnection <<EOF
[connection]
id=home
type=wifi
autoconnect=true

[wifi]
mode=infrastructure
ssid=$ssid

[wifi-security]
key-mgmt=wpa-psk
psk=$psk

[ipv4]
method=auto

[ipv6]
# Disabled for now: on this network the phone gets an IPv6 route, but replies never reach it,
# and everything that prefers IPv6 then hangs until it times out. See the bring-up notes.
method=disabled
EOF
		unset ssid psk
	}
fi

# Our schema override, which tools/pmos/root put there.
glib-compile-schemas /usr/share/glib-2.0/schemas >/dev/null 2>&1

# Enable what the packages' presets say, as pmbootstrap does, then ours.
systemctl preset-all >/dev/null 2>&1
systemctl enable bh-adbd.service bh-modem-files.service bh-rmtfs.service bh-tqftpserv.service \
	bh-remoteprocs.service bh-hexagonrpcd-resume.service >/dev/null 2>&1
systemctl enable plasma-mobile.service NetworkManager.service ModemManager.service \
	bluetooth.service sshd.service 2>&1 | grep -v '^Created symlink'
systemctl set-default graphical.target >/dev/null 2>&1

# It would wait at the console for someone to answer its questions.
systemctl mask systemd-firstboot.service >/dev/null 2>&1

# These make a USB network gadget of their own, and take the USB controller away from adb.
systemctl mask usb-moded.service usb-signaller.service usb-signaller-developer-mode.service \
	>/dev/null 2>&1
# And this one then tells the user, on every boot, that it can't tell which mode USB is in.
systemctl --global mask usb-moded-notify.service >/dev/null 2>&1

# Packaged units for these would use the real partitions.
systemctl mask rmtfs.service tqftpserv.service >/dev/null 2>&1

echo "avahi: $(id avahi 2>&1 | cut -c1-60)"
echo "pulse: $(id pulse 2>&1 | cut -c1-60)"
echo "/bin: $(readlink /bin)"
echo "user: $(id user 2>&1 | cut -c1-120)"
echo "default target: $(systemctl get-default 2>&1)"
echo "plasma-mobile: $(systemctl is-enabled plasma-mobile.service 2>&1)"
echo "display-manager: $(systemctl is-enabled display-manager.service 2>&1)"
echo "bh-adbd: $(systemctl is-enabled bh-adbd.service 2>&1)"
