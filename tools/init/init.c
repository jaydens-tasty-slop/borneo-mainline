// SPDX-License-Identifier: GPL-2.0-only
/*
 * The /init for bring-up images, built against the kernel's nolibc so it needs no
 * toolchain libc. It writes what we would otherwise have to ask for into the kernel log,
 * so that it lands in pstore, then offers adb over USB.
 *
 * It must never leave the phone alive but out of reach. If no host talks to adbd in
 * time, or one stops talking for long enough, it resets through the watchdog, and rescue
 * then collects the log.
 *
 * With bh.root=<file> on the command line it boots a real distribution instead, from an
 * ext4 image in that file on the microSD card. If that can't be mounted it carries on as
 * above, so that the phone stays reachable. The same binary, when it isn't PID 1, is the
 * distribution's adbd: see tools/pmos/.
 */
#include <linux/loop.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>

#define FIRST_CONTACT_S	120
#define IDLE_S		600
#define SERVICE_FIRST_CONTACT_S	300

static int kmsg = -1;

static void say(const char *s)
{
	if (kmsg >= 0)
		write(kmsg, s, strlen(s));
}

#include "adbd.c"

/* Each write to /dev/kmsg is one log record, so send the file a line at a time. */
static void dump(const char *path)
{
	static char buf[8192];
	static char line[512];
	int fd, n, used = 0;

	strcpy(line, "BH-INIT: --- ");
	strncat(line, path, sizeof(line) - 16);
	strncat(line, "\n", 1);
	say(line);

	fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		say("BH-INIT: (could not open)\n");
		return;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (int i = 0; i < n; i++) {
			if (buf[i] != '\n' && used < (int)sizeof(line) - 2) {
				line[used++] = buf[i];
				continue;
			}
			if (buf[i] != '\n')
				continue;
			line[used++] = '\n';
			line[used] = 0;
			say(line);
			used = 0;
		}
	}
	if (used) {
		line[used++] = '\n';
		line[used] = 0;
		say(line);
	}
	close(fd);
}

/*
 * A watchdog reset is the one path proven to keep RAM, and with it pstore. Opening the
 * device stops the kernel feeding it, so this just shortens the timeout and waits.
 *
 * Never fall back on reboot(LINUX_REBOOT_CMD_HALT) here. The PMIC power key driver's
 * reboot notifier sets the PS_HOLD reset type to shutdown on a halt, so the bite that
 * follows powers the phone off. If the driver isn't there, the stub's watchdog is still
 * armed and will fire.
 */
static void bite(void)
{
	int timeout = 1;
	int fd = open("/dev/watchdog0", O_WRONLY, 0);

	if (fd >= 0)
		ioctl(fd, WDIOC_SETTIMEOUT, &timeout);
	else
		say("BH-INIT: no watchdog device, waiting for the stub's timer\n");
	for (;;)
		sleep(60);
}

/* Copies the value of a key on the kernel command line. Returns 0 if it isn't there. */
static int cmdline_value(const char *key, char *value, size_t size)
{
	static char cmdline[4096];
	int fd = open("/proc/cmdline", O_RDONLY, 0);
	int n = fd < 0 ? 0 : read(fd, cmdline, sizeof(cmdline) - 1);
	char *p;
	size_t i = 0;

	if (fd >= 0)
		close(fd);
	cmdline[n > 0 ? n : 0] = 0;
	p = strstr(cmdline, key);
	if (p) {
		p += strlen(key);
		while (p[i] && p[i] != ' ' && p[i] != '\n' && i < size - 1) {
			value[i] = p[i];
			i++;
		}
	}
	value[i] = 0;
	return p != NULL;
}

/* The bootloader passes the serial on the command line, and adb -s needs it to match. */
static void read_serial(char *serial, size_t size)
{
	cmdline_value("androidboot.serialno=", serial, size);
}

/*
 * The eMMC driver probes asynchronously, so block devices may not exist yet. Wait for it
 * briefly, then write-protect the whole disk and both boot areas at the block layer.
 * Partition devices inherit this from the whole disk. Reading still works.
 *
 * Go by the disk's type, not its name. The DTS's aliases fix the numbering, but a kernel without
 * them numbers the disks in the order they probe, which puts the microSD card at mmcblk0 on some
 * boots. Protecting a name would then leave the eMMC writable.
 */
static int find_disk(const char *wanted)
{
	char path[] = "/sys/block/mmcblkN/device/type";
	char *n = strchr(path, 'N');
	size_t want = strlen(wanted);

	for (int i = 0; i < 8; i++) {
		char type[8];
		int fd, len;

		*n = '0' + i;
		fd = open(path, O_RDONLY, 0);
		if (fd < 0)
			continue;
		len = read(fd, type, sizeof(type));
		close(fd);
		if (len >= (int)want && !memcmp(type, wanted, want) && type[want] == '\n')
			return i;
	}
	return -1;
}

static void protect_emmc(void)
{
	char ro[][40] = {
		"/sys/block/mmcblkN/force_ro",
		"/sys/block/mmcblkNboot0/force_ro",
		"/sys/block/mmcblkNboot1/force_ro",
	};
	int emmc = -1;

	for (int tries = 0; tries < 50 && emmc < 0; tries++) {
		emmc = find_disk("MMC");
		if (emmc < 0)
			msleep(100);
	}
	if (emmc < 0) {
		say("BH-INIT: NO eMMC FOUND, IT IS NOT WRITE-PROTECTED\n");
		return;
	}
	for (unsigned int i = 0; i < 3; i++) {
		int fd;

		*strchr(ro[i], 'N') = '0' + emmc;
		fd = open(ro[i], O_WRONLY, 0);
		if (fd < 0)
			continue;
		if (write(fd, "1", 1) == 1)
			say("BH-INIT: eMMC write-protected\n");
		close(fd);
	}
}

static pid_t start_adbd(int activity)
{
	pid_t pid = fork();

	if (pid == 0) {
		adbd_run(activity);
		exit(1);
	}
	return pid;
}

static void install_busybox(void)
{
	/* It wants an absolute argv[0] if it can't read /proc/self/exe yet. */
	char *const argv[] = { "/bin/busybox", "--install", "-s", "/bin", NULL };
	int status = 0;
	pid_t pid = fork();

	if (pid == 0) {
		dup2(kmsg, 1);
		dup2(kmsg, 2);
		execve("/bin/busybox", argv, shell_env);
		exit(127);
	}
	if (pid > 0)
		waitpid(pid, &status, 0);
	if (status)
		say("BH-INIT: busybox --install failed\n");
}

static int usb_configured(void)
{
	static char udc[128];
	char path[192], state[16];
	int fd, n;

	if (find_udc(udc, sizeof(udc)))
		return 0;
	strcpy(path, "/sys/class/udc/");
	strcpy(path + strlen(path), udc);
	strcpy(path + strlen(path), "/state");
	fd = open(path, O_RDONLY, 0);
	if (fd < 0)
		return 0;
	n = read(fd, state, sizeof(state));
	close(fd);
	return n >= 10 && !memcmp(state, "configured", 10);
}

/*
 * modprobe fails on a module that's built in unless the kernel's index says it's built in, and
 * pmOS's zram setup, for one, gives up there. The index belongs to the kernel that booted, which
 * changes with every build and isn't on the card, so it comes from the initramfs and stays in RAM.
 */
static void install_module_index(void)
{
	static const char *const names[] = {
		"modules.builtin", "modules.builtin.bin", "modules.builtin.alias.bin",
		"modules.dep", "modules.dep.bin", "modules.alias", "modules.alias.bin",
		"modules.symbols", "modules.symbols.bin", "modules.softdep", "modules.devname",
		"modules.order",
	};
	static struct utsname uts;
	static char dir[128], path[192], buf[4096];
	size_t len;

	if (uname(&uts) ||
	    (mkdir("/newroot/lib/modules", 0755) && errno != EEXIST) ||
	    mount("tmpfs", "/newroot/lib/modules", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755")) {
		say("BH-INIT: no module index\n");
		return;
	}
	strcpy(dir, "/newroot/lib/modules/");
	strcpy(dir + strlen(dir), uts.release);
	mkdir(dir, 0755);
	len = strlen(dir);
	dir[len] = '/';
	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		int in, out;
		ssize_t n;

		strcpy(path, "/modules/");
		strcpy(path + strlen(path), names[i]);
		in = open(path, O_RDONLY, 0);
		if (in < 0)
			continue;
		strcpy(dir + len + 1, names[i]);
		out = open(dir, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		while (out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
			write(out, buf, n);
		if (out >= 0)
			close(out);
		close(in);
	}
}

/* Only returns if the root can't be mounted, having undone what it did. */
static void boot_from_card(const char *file)
{
	char *const argv[] = { "/sbin/init", NULL };
	char *const env[] = { "TERM=linux", NULL };
	static const char *const moved[][2] = {
		{ "/dev", "/newroot/dev" },
		{ "/proc", "/newroot/proc" },
		{ "/sys", "/newroot/sys" },
		{ "/sd", "/newroot/sd" },
	};
	static char path[128];
	static char part[] = "/dev/mmcblkNp1";
	static char loop[32];
	int card = -1, ctl, img, dev, num;

	/* The card's host probes asynchronously too. Never go by its number. */
	for (int tries = 0; tries < 100 && card < 0; tries++) {
		card = find_disk("SD");
		if (card < 0)
			msleep(100);
	}
	if (card < 0) {
		say("BH-INIT: no microSD card to boot from\n");
		return;
	}
	*strchr(part, 'N') = '0' + card;
	mkdir("/sd", 0755);
	mkdir("/newroot", 0755);
	for (int tries = 0; tries < 50; tries++) {
		if (!mount(part, "/sd", "exfat", 0, ""))
			break;
		if (tries == 49) {
			say("BH-INIT: couldn't mount the microSD card\n");
			return;
		}
		msleep(100);
	}
	strcpy(path, "/sd/");
	strcpy(path + strlen(path), file);

	img = open(path, O_RDWR, 0);
	ctl = open("/dev/loop-control", O_RDWR, 0);
	num = ctl < 0 ? -1 : ioctl(ctl, LOOP_CTL_GET_FREE, 0);
	if (img < 0 || num < 0) {
		say("BH-INIT: couldn't open the root image\n");
		goto out;
	}
	strcpy(loop, "/dev/loop");
	itoa_r(num, loop + strlen(loop));
	dev = open(loop, O_RDWR, 0);
	if (dev < 0 || ioctl(dev, LOOP_SET_FD, img) < 0) {
		say("BH-INIT: couldn't attach the root image\n");
		goto out;
	}
	close(dev);
	close(img);
	close(ctl);
	/*
	 * Lazy inode table zeroing does nothing useful here. exFAT can't punch holes, so every
	 * attempt fails and logs an error, for hours.
	 */
	if (mount(loop, "/newroot", "ext4", 0, "noinit_itable") ||
	    access("/newroot/sbin/init", X_OK)) {
		say("BH-INIT: couldn't mount the root image\n");
		return;
	}

	install_module_index();
	say("BH-INIT: switching to the root on the card\n");
	for (unsigned int i = 0; i < sizeof(moved) / sizeof(moved[0]); i++)
		mount(moved[i][0], moved[i][1], NULL, MS_MOVE, "");
	chdir("/newroot");
	mount(".", "/", NULL, MS_MOVE, "");
	chroot(".");
	chdir("/");
	if (kmsg >= 0)
		close(kmsg);
	execve("/sbin/init", argv, env);
	/* Nothing to go back to now. Panicking reaches the watchdog, and then rescue. */
	exit(127);

out:
	umount2("/sd", 0);
}

/*
 * The distribution's adbd. Without bh.nowatch, a phone on a host that never opens adb resets
 * after a while, as it would under PID 1. Once a host has been seen it stays up however long
 * it's left, since someone may be using it.
 */
static int run_adbd_service(void)
{
	static char serial[64];
	static char flag[8];
	static char drain[256];
	int activity[2];
	int ep0, idle = 0, seen = cmdline_value("bh.nowatch", flag, sizeof(flag));
	pid_t adbd;

	kmsg = open("/dev/kmsg", O_WRONLY, 0);
	read_serial(serial, sizeof(serial));
	if (pipe2(activity, O_NONBLOCK))
		return 1;
	ep0 = gadget_start(serial);
	if (ep0 < 0) {
		say("BH-ADBD: couldn't start the gadget\n");
		return 1;
	}
	say("BH-ADBD: gadget bound\n");
	adbd = start_adbd(activity[1]);

	for (;;) {
		struct pollfd pfd = { activity[0], POLLIN, 0 };
		int n = poll(&pfd, 1, 1000);
		pid_t done;

		while ((done = waitpid(-1, NULL, WNOHANG)) > 0) {
			if (done == adbd)
				adbd = start_adbd(activity[1]);
		}
		if (n > 0 && (pfd.revents & POLLIN)) {
			while (read(activity[0], drain, sizeof(drain)) > 0)
				;
			seen = 1;
		} else if (!seen && ++idle >= SERVICE_FIRST_CONTACT_S && usb_configured()) {
			say("BH-ADBD: no host traffic, rebooting\n");
			__nolibc_syscall0(__NR_sync);
			reboot(LINUX_REBOOT_CMD_RESTART);
		}
	}
}

int main(void)
{
	static char serial[64];
	static char code[32];
	static char drain[256];
	static char root[64];
	int activity[2];
	int ep0, idle = 0, limit = FIRST_CONTACT_S;
	pid_t adbd = -1;

	if (getpid() != 1)
		return run_adbd_service();

	mkdir("/dev", 0755);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mount("devtmpfs", "/dev", "devtmpfs", 0, "");
	mount("proc", "/proc", "proc", 0, "");
	mount("sysfs", "/sys", "sysfs", 0, "");
	mount("debugfs", "/sys/kernel/debug", "debugfs", 0, "");
	mount("pstore", "/sys/fs/pstore", "pstore", 0, "");
	mkdir("/tmp", 01777);

	kmsg = open("/dev/kmsg", O_WRONLY, 0);
	say("BH-INIT: reached userspace\n");

	/* Bring-up images have no business writing to the phone's own storage. */
	protect_emmc();

	if (cmdline_value("bh.root=", root, sizeof(root)))
		boot_from_card(root);

	install_busybox();

	dump("/proc/cmdline");
	dump("/proc/meminfo");
	dump("/proc/iomem");
	dump("/proc/interrupts");
	dump("/sys/kernel/debug/devices_deferred");
	dump("/sys/class/watchdog/watchdog0/state");
	dump("/sys/class/watchdog/watchdog0/timeout");

	read_serial(serial, sizeof(serial));
	if (pipe2(activity, O_NONBLOCK)) {
		say("BH-INIT: no pipe, waiting for the watchdog\n");
		bite();
	}

	ep0 = gadget_start(serial);
	if (ep0 < 0) {
		strcpy(code, "BH-INIT: gadget failed: -");
		code[25] = '0' - ep0;
		code[26] = '\n';
		code[27] = 0;
		say(code);
	} else {
		say("BH-INIT: gadget bound\n");
		adbd = start_adbd(activity[1]);
	}

	for (;;) {
		struct pollfd pfd = { activity[0], POLLIN, 0 };
		int n = poll(&pfd, 1, 1000);
		pid_t done;

		while ((done = waitpid(-1, NULL, WNOHANG)) > 0) {
			if (done == adbd)
				adbd = start_adbd(activity[1]);
		}
		if (n > 0 && (pfd.revents & POLLIN)) {
			while (read(activity[0], drain, sizeof(drain)) > 0)
				;
			if (limit != IDLE_S)
				say("BH-INIT: host connected\n");
			idle = 0;
			limit = IDLE_S;
		} else if (++idle >= limit) {
			say("BH-INIT: no host traffic, waiting for the watchdog\n");
			bite();
		}
	}
	return 0;
}
