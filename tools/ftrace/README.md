# Function trace for hangs that leave nothing on the console

For a phone that stops dead (bootreason=watchdog, no panic). Function records go into persistent RAM and
are read in stock recovery after the reset. Dev only, never for upstream.

1. In linux/: `git apply --recount ../tools/ftrace/ram-dt.patch`, then `BK_VARIANT=ftrace ../tools/bk --dtbs`,
   then `git apply --recount -R ../tools/ftrace/ram-dt.patch`. Image: build/ftrace/borneo-pmos.img.
2. On the phone: `sh filter.sh`, save `grep -E ' [tT] ' /proc/kallsyms` to the SD card, then
   `echo 1 > /sys/kernel/debug/pstore/record_ftrace` just before the trigger. `crash-example.sh` does this.
3. After the reset, in stock recovery: `adb exec-out cat /sys/fs/pstore/pmsg-ramoops-0 > raw.bin`.
   Read it there. Booting mainline afterwards finds pstore empty.
4. Boot mainline only to fetch the saved symbol table, then `ftbin.py raw.bin kallsyms.txt`.

What the hack does: pmsg gives up its zone, at the same address stock calls pmsg. Stock only shows that zone
if the signature is pmsg's, 0, hence the ram.c line. Per-CPU zones (flags = 1) can't be read this way.

Things that bit me:
- Tracing changes timing: unfiltered, the hang being chased went away in 3 of 3 runs. Filtered, it took
  5 tries in a boot. So a run with no hang says little.
- Records are ip, parent ip, stamp << 8 | cpu. Stamp counts events, not time. Addresses were 0x80000 below
  /proc/kallsyms on this build, which ftbin.py adds. Check that the names match your filter before
  believing anything; a wrong offset gives plausible looking nonsense.
