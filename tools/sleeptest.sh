#!/bin/sh
# One real s2idle cycle with the RTC armed to wake it, logged to the SD card. Runs on the phone,
# outside the chroot and in the background, since USB drops while it sleeps:
#   adb push -q tools/sleeptest.sh /r/run/sleeptest.sh
#   adb shell "nohup sh /r/run/sleeptest.sh 10 name >/dev/null 2>&1 &"
# Then read /sd/name.log and /sd/name.dmesg.
#
# Wait about 40 s after boot, or Bluetooth's setup aborts suspend with -110.
#
# Keep it under 30 s. tools/stub's watchdog keeps running in s2idle, and its limit is 32 s. That
# also means a hang during the sleep resets the phone by itself.
#
# pm_test stages "processors" and "core" don't apply to s2idle: enter_state() rejects them
# with -EAGAIN, so "platform" is the deepest test stage there is.
SECS=${1:-10}
NAME=${2:-sleep}
LOG=/sd/$NAME.log
STATS=/sys/kernel/debug/qcom_stats
say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $LOG; sync; }
stats() {
	for f in "$STATS"/*; do
		[ -f "$f" ] && echo "$(basename "$f"): $(tr -s ' \n' ' ' < "$f" | cut -c1-90)" >> $LOG
	done
	for f in /sys/kernel/debug/qcom_rpm_master_stats/*; do
		[ -f "$f" ] && echo "$(tr -s '\t\n' ' ' < "$f" | sed -E 's/Last (shutdown|bringup req|bringup ack|wakeup idx)[^L]*//g; s/transition duration/trans/g' | cut -c1-200)" >> $LOG
	done
	sync
}
idle() {
	for s in /sys/devices/system/cpu/cpu0/cpuidle/state* /sys/devices/system/cpu/cpu4/cpuidle/state*; do
		[ -d "$s" ] && echo "$s: $(cat "$s/name") usage $(cat "$s/usage") s2idle $(cat "$s/s2idle/usage" 2>/dev/null) rej $(cat "$s/rejected")" >> $LOG
	done
	sync
}
genpd() {
	for p in power-domain-cpu-cluster0 power-domain-cpu-cluster1; do
		echo "$p $(tail -n +2 /sys/kernel/debug/pm_genpd/$p/idle_states | tr -s ' ' | tr '\n' ';' | cut -c1-90)" >> $LOG
	done
	sync
}
echo none > /sys/power/pm_test
echo 0 > /sys/power/pm_debug_messages
say "--- sleep for $SECS s, rtc $(cat /sys/class/rtc/rtc0/since_epoch)"
stats; idle; genpd
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo "+$SECS" > /sys/class/rtc/rtc0/wakealarm || { say "couldn't arm the alarm, not sleeping"; exit 1; }
say "alarm at $(cat /sys/class/rtc/rtc0/wakealarm)"
echo freeze > /sys/power/state 2>> $LOG
rc=$?
say "--- returned rc=$rc, rtc $(cat /sys/class/rtc/rtc0/since_epoch), wake irq: $(cat /sys/power/pm_wakeup_irq 2>&1)"
stats; idle; genpd
say "suspend success $(cat /sys/power/suspend_stats/success) fail $(cat /sys/power/suspend_stats/fail) last_hw_sleep $(cat /sys/power/suspend_stats/last_hw_sleep 2>&1) total $(cat /sys/power/suspend_stats/total_hw_sleep 2>&1)"
dmesg > /sd/$NAME.dmesg
sync
