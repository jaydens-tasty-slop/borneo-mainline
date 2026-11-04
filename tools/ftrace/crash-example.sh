#!/bin/sh
# One simulated ADSP crash during a mic capture, with every kernel function call recorded in persistent
# RAM from just before the crash. Keeps this boot's symbol table on the SD card. Silent.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
D=/sys/kernel/debug/remoteproc/remoteproc1
grep -E ' [tT] ' /proc/kallsyms > /var/tmp/dbg/kallsyms-ft.txt
echo "symbols saved: $(wc -l < /var/tmp/dbg/kallsyms-ft.txt)"
sync
sh /run/u.sh "timeout 8 pw-record --format=s16 --rate=48000 --channels=1 --raw - >/dev/null 2>/tmp/live.err" &
sleep 4
echo "live crash at $(cut -d' ' -f1 /proc/uptime)"; sync
echo 1 > /sys/kernel/debug/pstore/record_ftrace
echo 2 > "$D/crash"
sleep 25
echo 0 > /sys/kernel/debug/pstore/record_ftrace
echo "survived: state=$(cat /sys/class/remoteproc/remoteproc1/state)"
