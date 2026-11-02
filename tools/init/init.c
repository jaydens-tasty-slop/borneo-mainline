// SPDX-License-Identifier: GPL-2.0-only
/*
 * The /init for bring-up images, built against the kernel's nolibc so it needs no
 * toolchain libc. It writes what we would otherwise have to ask for into the kernel log,
 * so that it lands in pstore, then offers adb over USB.
 *
 * It must never leave the phone alive but out of reach. If no host talks to adbd in
 * time, or one stops talking for long enough, it resets through the watchdog, and rescue
 * then collects the log.
 */
#include <linux/watchdog.h>

#define FIRST_CONTACT_S	120
#define IDLE_S		600

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

/* The bootloader passes the serial on the command line, and adb -s needs it to match. */
static void read_serial(char *serial, size_t size)
{
	static char cmdline[4096];
	static const char key[] = "androidboot.serialno=";
	int fd = open("/proc/cmdline", O_RDONLY, 0);
	int n = fd < 0 ? 0 : read(fd, cmdline, sizeof(cmdline) - 1);
	char *p;
	size_t i = 0;

	if (fd >= 0)
		close(fd);
	cmdline[n > 0 ? n : 0] = 0;
	p = strstr(cmdline, key);
	if (p) {
		p += sizeof(key) - 1;
		while (p[i] && p[i] != ' ' && p[i] != '\n' && i < size - 1) {
			serial[i] = p[i];
			i++;
		}
	}
	serial[i] = 0;
}

/*
 * The eMMC driver probes asynchronously, so block devices may not exist yet. Wait for it
 * briefly, then write-protect the whole disk and both boot areas at the block layer.
 * Partition devices inherit this from the whole disk. Reading still works.
 */
static void protect_emmc(void)
{
	static const char *const ro[] = {
		"/sys/block/mmcblk0/force_ro",
		"/sys/block/mmcblk0boot0/force_ro",
		"/sys/block/mmcblk0boot1/force_ro",
	};

	for (int tries = 0; tries < 50; tries++) {
		int fd = open(ro[0], O_RDONLY, 0);

		if (fd >= 0) {
			close(fd);
			break;
		}
		msleep(100);
	}
	for (unsigned int i = 0; i < 3; i++) {
		int fd = open(ro[i], O_WRONLY, 0);

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

int main(void)
{
	static char serial[64];
	static char code[32];
	static char drain[256];
	int activity[2];
	int ep0, idle = 0, limit = FIRST_CONTACT_S;
	pid_t adbd = -1;

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
