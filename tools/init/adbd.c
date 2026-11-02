// SPDX-License-Identifier: GPL-2.0-only
/*
 * A small adbd on FunctionFS, so bring-up can use the adb the host already has
 * permission for, with no Android userspace on the phone.
 *
 * One process owns writes to the host. Bulk OUT reads block and FunctionFS endpoints
 * can't be polled, so a second process does nothing but copy them into a pipe that can
 * be. Each stream is a child process joined by two pipes, which keeps the main loop
 * identical for a shell and for file transfer.
 *
 * It answers CNXN without asking for AUTH and advertises no features, so the host falls
 * back to the plain v1 shell and sync protocols. It is for a phone on the bench only.
 */
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

#define A_CNXN		0x4e584e43
#define A_OPEN		0x4e45504f
#define A_OKAY		0x59414b4f
#define A_CLSE		0x45534c43
#define A_WRTE		0x45545257
#define A_VERSION	0x01000001
#define MAX_PAYLOAD	65536
#define MAX_STREAMS	16

#define FFS_DIR		"/dev/usb-ffs/adb"
#define BANNER		"device::ro.product.name=mainline;ro.product.model=borneo;" \
			"ro.product.device=borneo;features="

struct amessage {
	uint32_t command;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t data_length;
	uint32_t data_check;
	uint32_t magic;
};

struct stream {
	int used;
	int can_send;
	uint32_t local;
	uint32_t remote;
	int in_fd;
	int out_fd;
	pid_t pid;
};

static struct stream streams[MAX_STREAMS];
static uint32_t next_local = 1;
static uint32_t max_payload = 4096;
static int ep_in = -1;
static int from_reader = -1;
static int activity_fd = -1;
static pid_t reader_pid;

static char *const shell_env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=dumb", NULL };

static void ignore_sigpipe(int ignore)
{
	struct { void *handler; unsigned long flags; unsigned long mask; } sa = {
		(void *)(unsigned long)(ignore ? 1 : 0), 0, 0,
	};

	__nolibc_syscall4(__NR_rt_sigaction, SIGPIPE, &sa, NULL, 8);
}

static int write_all(int fd, const void *data, size_t len)
{
	const char *p = data;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static int read_all(int fd, void *data, size_t len)
{
	char *p = data;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static void adbd_exit(void)
{
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (streams[i].used && streams[i].pid > 0)
			kill(streams[i].pid, SIGKILL);
	}
	if (reader_pid > 0)
		kill(reader_pid, SIGKILL);
	exit(1);
}

static void send_packet(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data,
			uint32_t len)
{
	const unsigned char *p = data;
	struct amessage m = {
		.command = cmd, .arg0 = arg0, .arg1 = arg1, .data_length = len,
		.magic = cmd ^ 0xffffffff,
	};

	for (uint32_t i = 0; i < len; i++)
		m.data_check += p[i];
	if (write_all(ep_in, &m, sizeof(m)))
		adbd_exit();
	if (len && write_all(ep_in, data, len))
		adbd_exit();
}

/* --- file transfer, run in its own child with stdin and stdout as the stream --- */

struct sync_msg {
	char id[4];
	uint32_t len;
};

static void sync_reply(const char *id, const void *data, uint32_t len)
{
	struct sync_msg m;

	memcpy(m.id, id, 4);
	m.len = len;
	write_all(1, &m, sizeof(m));
	if (len)
		write_all(1, data, len);
}

static void sync_fail(const char *why)
{
	sync_reply("FAIL", why, strlen(why));
}

static void sync_stat(const char *path)
{
	struct stat st;
	struct { char id[4]; uint32_t mode, size, time; } r;

	memset(&r, 0, sizeof(r));
	memcpy(r.id, "STAT", 4);
	if (!stat(path, &st)) {
		r.mode = st.st_mode;
		r.size = st.st_size;
		r.time = st.st_mtime;
	}
	write_all(1, &r, sizeof(r));
}

static void sync_list(const char *path)
{
	static char dents[8192];
	static char full[1024];
	struct { char id[4]; uint32_t mode, size, time, namelen; } r;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
	int n;

	while (fd >= 0 && (n = getdents64(fd, (void *)dents, sizeof(dents))) > 0) {
		for (int off = 0; off < n;) {
			struct linux_dirent64 *d = (void *)(dents + off);
			struct stat st;
			size_t len = strlen(d->d_name);

			off += d->d_reclen;
			if (strlen(path) + len + 2 > sizeof(full))
				continue;
			strcpy(full, path);
			strncat(full, "/", 1);
			strncat(full, d->d_name, len);
			memset(&r, 0, sizeof(r));
			memcpy(r.id, "DENT", 4);
			if (!stat(full, &st)) {
				r.mode = st.st_mode;
				r.size = st.st_size;
				r.time = st.st_mtime;
			}
			r.namelen = len;
			write_all(1, &r, sizeof(r));
			write_all(1, d->d_name, len);
		}
	}
	if (fd >= 0)
		close(fd);
	memset(&r, 0, sizeof(r));
	memcpy(r.id, "DONE", 4);
	write_all(1, &r, sizeof(r));
}

static void sync_recv(const char *path)
{
	static char data[MAX_PAYLOAD];
	int fd = open(path, O_RDONLY, 0);
	ssize_t n;

	if (fd < 0) {
		sync_fail("open failed");
		return;
	}
	while ((n = read(fd, data, sizeof(data))) > 0)
		sync_reply("DATA", data, n);
	close(fd);
	if (n < 0)
		sync_fail("read failed");
	else
		sync_reply("DONE", NULL, 0);
}

static void make_parents(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		mkdir(path, 0755);
		*p = '/';
	}
}

static void sync_send(char *spec)
{
	static char data[MAX_PAYLOAD];
	char *comma = strrchr(spec, ',');
	int mode = 0644;
	struct sync_msg m;
	int fd;

	if (comma) {
		*comma = 0;
		mode = 0;
		for (char *p = comma + 1; *p >= '0' && *p <= '9'; p++)
			mode = mode * 10 + (*p - '0');
		mode &= 0777;
	}
	make_parents(spec);
	fd = open(spec, O_WRONLY | O_CREAT | O_TRUNC, mode);

	while (!read_all(0, &m, sizeof(m))) {
		if (!memcmp(m.id, "DONE", 4)) {
			if (fd < 0) {
				sync_fail("create failed");
				return;
			}
			close(fd);
			sync_reply("OKAY", NULL, 0);
			return;
		}
		if (memcmp(m.id, "DATA", 4) || m.len > sizeof(data)) {
			sync_fail("bad transfer message");
			return;
		}
		if (read_all(0, data, m.len))
			return;
		if (fd >= 0 && write_all(fd, data, m.len)) {
			close(fd);
			fd = -1;
		}
	}
}

static void sync_service(void)
{
	static char path[1024];
	struct sync_msg m;

	while (!read_all(0, &m, sizeof(m))) {
		if (!memcmp(m.id, "QUIT", 4))
			return;
		if (m.len >= sizeof(path) || read_all(0, path, m.len))
			return;
		path[m.len] = 0;
		if (!memcmp(m.id, "STAT", 4))
			sync_stat(path);
		else if (!memcmp(m.id, "LIST", 4))
			sync_list(path);
		else if (!memcmp(m.id, "RECV", 4))
			sync_recv(path);
		else if (!memcmp(m.id, "SEND", 4))
			sync_send(path);
		else
			return;
	}
}

/* --- services --- */

static void run_service(const char *svc)
{
	if (!strncmp(svc, "shell:", 6)) {
		const char *cmd = svc + 6;
		char *const with_cmd[] = { "sh", "-c", (char *)cmd, NULL };
		char *const plain[] = { "sh", NULL };

		/* Named by argv[0], so this doesn't rely on the applet links existing. */
		ignore_sigpipe(0);
		execve("/bin/busybox", *cmd ? with_cmd : plain, shell_env);
		write_all(1, "no /bin/busybox\n", 16);
	} else if (!strcmp(svc, "sync:")) {
		sync_service();
	} else if (!strncmp(svc, "reboot:", 7)) {
		/* An ordinary reboot, or a named mode for the reboot-mode driver. */
		if (svc[7])
			__nolibc_syscall4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
				    LINUX_REBOOT_CMD_RESTART2, svc + 7);
		else
			reboot(LINUX_REBOOT_CMD_RESTART);
	} else if (!strncmp(svc, "root:", 5)) {
		write_all(1, "adbd is already running as root\n", 32);
	}
}

static struct stream *find_stream(uint32_t local)
{
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (streams[i].used && streams[i].local == local)
			return &streams[i];
	}
	return NULL;
}

static void close_stream(struct stream *s)
{
	if (s->in_fd >= 0)
		close(s->in_fd);
	if (s->out_fd >= 0)
		close(s->out_fd);
	s->used = 0;
}

static void open_stream(uint32_t remote, const char *svc)
{
	struct stream *s = NULL;
	int in[2], out[2];
	pid_t pid;

	for (int i = 0; i < MAX_STREAMS; i++) {
		if (!streams[i].used) {
			s = &streams[i];
			break;
		}
	}
	if (!s || pipe(in) || pipe(out)) {
		send_packet(A_CLSE, 0, remote, NULL, 0);
		return;
	}
	pid = fork();
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(out[1], 2);
		for (int fd = 3; fd < 64; fd++)
			close(fd);
		setsid();
		run_service(svc);
		exit(0);
	}
	close(in[0]);
	close(out[1]);

	s->used = 1;
	s->can_send = 1;
	s->local = next_local++;
	s->remote = remote;
	s->in_fd = in[1];
	s->out_fd = out[0];
	s->pid = pid;
	send_packet(A_OKAY, s->local, remote, NULL, 0);
}

static void handle_packet(const struct amessage *m, char *data)
{
	struct stream *s;

	switch (m->command) {
	case A_CNXN:
		for (int i = 0; i < MAX_STREAMS; i++) {
			if (streams[i].used) {
				kill(streams[i].pid, SIGKILL);
				close_stream(&streams[i]);
			}
		}
		max_payload = m->arg1 < MAX_PAYLOAD ? m->arg1 : MAX_PAYLOAD;
		send_packet(A_CNXN, A_VERSION, MAX_PAYLOAD, BANNER, sizeof(BANNER));
		break;
	case A_OPEN:
		if (m->data_length) {
			data[m->data_length - 1] = 0;
			open_stream(m->arg0, data);
		}
		break;
	case A_OKAY:
		s = find_stream(m->arg1);
		if (s)
			s->can_send = 1;
		break;
	case A_WRTE:
		s = find_stream(m->arg1);
		if (!s)
			break;
		if (s->in_fd >= 0 && write_all(s->in_fd, data, m->data_length)) {
			close(s->in_fd);
			s->in_fd = -1;
		}
		send_packet(A_OKAY, s->local, s->remote, NULL, 0);
		break;
	case A_CLSE:
		s = find_stream(m->arg1);
		if (s) {
			kill(s->pid, SIGHUP);
			send_packet(A_CLSE, s->local, s->remote, NULL, 0);
			close_stream(s);
		}
		break;
	}
}

static void read_packet(void)
{
	static char data[MAX_PAYLOAD + 1];
	struct amessage m;

	if (read_all(from_reader, &m, sizeof(m)))
		adbd_exit();
	if (m.magic != (m.command ^ 0xffffffff) || m.data_length > MAX_PAYLOAD)
		adbd_exit();
	if (m.data_length && read_all(from_reader, data, m.data_length))
		adbd_exit();
	write(activity_fd, "", 1);
	handle_packet(&m, data);
}

static void pump_stream(struct stream *s)
{
	static char data[MAX_PAYLOAD];
	ssize_t n = read(s->out_fd, data, max_payload);

	if (n < 0 && errno == EINTR)
		return;
	if (n <= 0) {
		send_packet(A_CLSE, s->local, s->remote, NULL, 0);
		close_stream(s);
		return;
	}
	s->can_send = 0;
	send_packet(A_WRTE, s->local, s->remote, data, n);
}

static void bulk_reader(int ep_out, int to_main)
{
	static char buf[16384];

	for (;;) {
		ssize_t n = read(ep_out, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			exit(0);
		if (n && write_all(to_main, buf, n))
			exit(0);
	}
}

/* Runs until the USB link breaks. PID 1 starts another one when it does. */
static void adbd_run(int activity)
{
	int ep_out, fds[2];

	activity_fd = activity;
	ignore_sigpipe(1);

	ep_in = open(FFS_DIR "/ep1", O_RDWR, 0);
	ep_out = open(FFS_DIR "/ep2", O_RDWR, 0);
	if (ep_in < 0 || ep_out < 0 || pipe(fds))
		exit(1);

	reader_pid = fork();
	if (reader_pid == 0) {
		close(fds[0]);
		bulk_reader(ep_out, fds[1]);
	}
	close(fds[1]);
	close(ep_out);
	from_reader = fds[0];

	for (;;) {
		struct pollfd pfds[MAX_STREAMS + 1];
		struct stream *owners[MAX_STREAMS + 1];
		int nfds = 1;

		pfds[0].fd = from_reader;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		for (int i = 0; i < MAX_STREAMS; i++) {
			if (!streams[i].used || !streams[i].can_send)
				continue;
			pfds[nfds].fd = streams[i].out_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			owners[nfds++] = &streams[i];
		}

		if (poll(pfds, nfds, 1000) < 0 && errno != EINTR)
			adbd_exit();
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;

		for (int i = 1; i < nfds; i++) {
			if (pfds[i].revents && owners[i]->used)
				pump_stream(owners[i]);
		}
		if (pfds[0].revents)
			read_packet();
	}
}

/* --- gadget setup --- */

#define GADGET "/sys/kernel/config/usb_gadget/g1"

static const struct {
	struct usb_functionfs_descs_head_v2 header;
	__le32 fs_count;
	__le32 hs_count;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio source;
		struct usb_endpoint_descriptor_no_audio sink;
	} __attribute__((packed)) fs, hs;
} __attribute__((packed)) ffs_descriptors = {
	.header = {
		.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
		.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
		.length = sizeof(ffs_descriptors),
	},
	.fs_count = 3,
	.hs_count = 3,
#define ADB_SPEED(pkt)								\
	{									\
		.intf = {							\
			.bLength = sizeof(struct usb_interface_descriptor),	\
			.bDescriptorType = USB_DT_INTERFACE,			\
			.bNumEndpoints = 2,					\
			.bInterfaceClass = 0xff,				\
			.bInterfaceSubClass = 0x42,				\
			.bInterfaceProtocol = 1,				\
			.iInterface = 1,					\
		},								\
		.source = {							\
			.bLength = USB_DT_ENDPOINT_SIZE,			\
			.bDescriptorType = USB_DT_ENDPOINT,			\
			.bEndpointAddress = 1 | USB_DIR_IN,			\
			.bmAttributes = USB_ENDPOINT_XFER_BULK,			\
			.wMaxPacketSize = pkt,					\
		},								\
		.sink = {							\
			.bLength = USB_DT_ENDPOINT_SIZE,			\
			.bDescriptorType = USB_DT_ENDPOINT,			\
			.bEndpointAddress = 2 | USB_DIR_OUT,			\
			.bmAttributes = USB_ENDPOINT_XFER_BULK,			\
			.wMaxPacketSize = pkt,					\
		},								\
	}
	.fs = ADB_SPEED(64),
	.hs = ADB_SPEED(512),
};

static const struct {
	struct usb_functionfs_strings_head header;
	struct {
		__le16 code;
		char str1[sizeof("ADB Interface")];
	} __attribute__((packed)) lang0;
} __attribute__((packed)) ffs_strings = {
	.header = {
		.magic = FUNCTIONFS_STRINGS_MAGIC,
		.length = sizeof(ffs_strings),
		.str_count = 1,
		.lang_count = 1,
	},
	.lang0 = { 0x0409, "ADB Interface" },
};

static int put(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY, 0);
	int ret;

	if (fd < 0)
		return -1;
	ret = write_all(fd, val, strlen(val));
	close(fd);
	return ret;
}

/* The first entry in /sys/class/udc, once the controller has registered. */
static int find_udc(char *name, size_t size)
{
	static char dents[1024];

	for (int tries = 0; tries < 300; tries++) {
		int fd = open("/sys/class/udc", O_RDONLY | O_DIRECTORY, 0);
		int n = fd < 0 ? 0 : getdents64(fd, (void *)dents, sizeof(dents));

		if (fd >= 0)
			close(fd);
		for (int off = 0; off < n;) {
			struct linux_dirent64 *d = (void *)(dents + off);

			off += d->d_reclen;
			if (d->d_name[0] == '.')
				continue;
			strncpy(name, d->d_name, size - 1);
			name[size - 1] = 0;
			return 0;
		}
		msleep(100);
	}
	return -1;
}

/* Returns the ep0 descriptor, which must stay open for the gadget's whole life. */
static int gadget_start(const char *serial)
{
	static char udc[128];
	int ep0;

	/* Under systemd it's already mounted, which leaves EBUSY. */
	if (mount("configfs", "/sys/kernel/config", "configfs", 0, "") && errno != EBUSY)
		return -1;
	mkdir(GADGET, 0755);
	put(GADGET "/idVendor", "0x18d1");
	put(GADGET "/idProduct", "0x4ee7");
	put(GADGET "/bcdDevice", "0x0100");
	put(GADGET "/bcdUSB", "0x0200");
	mkdir(GADGET "/strings/0x409", 0755);
	put(GADGET "/strings/0x409/serialnumber", serial);
	put(GADGET "/strings/0x409/manufacturer", "Motorola");
	put(GADGET "/strings/0x409/product", "borneo mainline");
	mkdir(GADGET "/configs/c.1", 0755);
	mkdir(GADGET "/configs/c.1/strings/0x409", 0755);
	put(GADGET "/configs/c.1/strings/0x409/configuration", "adb");
	put(GADGET "/configs/c.1/MaxPower", "500");
	mkdir(GADGET "/functions/ffs.adb", 0755);
	if (symlink(GADGET "/functions/ffs.adb", GADGET "/configs/c.1/ffs.adb"))
		return -2;

	mkdir("/dev/usb-ffs", 0755);
	mkdir(FFS_DIR, 0755);
	if (mount("adb", FFS_DIR, "functionfs", 0, ""))
		return -3;
	ep0 = open(FFS_DIR "/ep0", O_RDWR, 0);
	if (ep0 < 0)
		return -4;
	if (write_all(ep0, &ffs_descriptors, sizeof(ffs_descriptors)) ||
	    write_all(ep0, &ffs_strings, sizeof(ffs_strings)))
		return -5;

	if (find_udc(udc, sizeof(udc)))
		return -6;
	if (put(GADGET "/UDC", udc))
		return -7;
	return ep0;
}
