#!/bin/sh
# Runs on the phone, in the chroot, from tools/wifiup. Nothing here may print the
# credentials or anything derived from them.
umask 077
for i in $(seq 40); do [ -e /sys/class/net/wlan0 ] && break; sleep 1; done
if [ ! -e /sys/class/net/wlan0 ]; then
	echo "no wlan0"
	rm -f /run/wifi/creds
	exit 1
fi
# ath10k makes up a new address each boot. The bootloader passes the phone's real one, which
# also keeps the DHCP lease stable.
mac=$(sed -n 's/.*wifimacaddr=\([0-9A-Fa-f:]\{17\}\).*/\1/p' /proc/cmdline)
if [ -n "$mac" ]; then ip link set wlan0 address "$mac"; fi
ip link set wlan0 up
cr=$(printf '\r')
{
	read -r ssid
	read -r pass
} </run/wifi/creds
rm -f /run/wifi/creds
ssid=${ssid%"$cr"}
pass=${pass%"$cr"}
# The SSID goes in as hex, and wpa_supplicant ends a quoted passphrase at its last quote,
# so neither can break the config whatever characters they hold.
{
	echo 'ctrl_interface=/run/wpa_supplicant'
	echo 'update_config=0'
	echo 'network={'
	echo "	ssid=$(printf %s "$ssid" | od -An -tx1 | tr -d ' \n')"
	echo "	psk=\"$pass\""
	echo '	key_mgmt=WPA-PSK'
	echo '}'
} >/run/wifi/wpa.conf
unset ssid pass cr
iw reg reload 2>/dev/null
pidof wpa_supplicant >/dev/null || wpa_supplicant -B -q -s -i wlan0 -c /run/wifi/wpa.conf >/dev/null 2>&1
for i in $(seq 40); do
	[ "$(wpa_cli -p /run/wpa_supplicant -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')" = COMPLETED ] && break
	sleep 1
done
echo "wpa_state=$(wpa_cli -p /run/wpa_supplicant -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')"
udhcpc -q -n -t 10 -i wlan0 >/dev/null 2>&1 || echo "no DHCP lease"
ip -4 -o addr show wlan0 | awk '{print "address=" $4}'
mkdir -p /etc/dropbear
pidof dropbear >/dev/null || dropbear -R -s -g -p 22
