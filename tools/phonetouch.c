// SPDX-License-Identifier: GPL-2.0-only
/*
 * phonetouch: a touchscreen made of nothing, to drive a phone's UI with nobody at the phone.
 *
 *   phonetouch tap X Y
 *   phonetouch swipe X1 Y1 X2 Y2 MS
 *
 * In panel pixels. Goes through uinput, so it tests everything above the input layer and says
 * nothing about the real touch panel. Built by tools/mkphonetouch, in the kernel's nolibc.
 */
#include <linux/uinput.h>

#define W 720
#define H 1600
/* The panel's size in millimetres sets how far libinput thinks a swipe went. */
#define RES_X (W / 68)
#define RES_Y (H / 152)

static int fd;

static void ev(int type, int code, int value)
{
	struct input_event e;

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.value = value;
	write(fd, &e, sizeof(e));
}

static void position(int x, int y)
{
	ev(EV_ABS, ABS_MT_POSITION_X, x);
	ev(EV_ABS, ABS_MT_POSITION_Y, y);
	ev(EV_ABS, ABS_X, x);
	ev(EV_ABS, ABS_Y, y);
}

static void down(int x, int y)
{
	ev(EV_ABS, ABS_MT_SLOT, 0);
	ev(EV_ABS, ABS_MT_TRACKING_ID, 1);
	position(x, y);
	ev(EV_KEY, BTN_TOUCH, 1);
	ev(EV_SYN, SYN_REPORT, 0);
}

static void up(void)
{
	ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
	ev(EV_KEY, BTN_TOUCH, 0);
	ev(EV_SYN, SYN_REPORT, 0);
}

static void axis(int code, int max, int res)
{
	struct uinput_abs_setup s;

	memset(&s, 0, sizeof(s));
	s.code = code;
	s.absinfo.maximum = max;
	s.absinfo.resolution = res;
	ioctl(fd, UI_ABS_SETUP, &s);
}

int main(int argc, char **argv)
{
	struct uinput_setup setup;

	if (argc < 4) {
		write(2, "usage: phonetouch tap X Y | swipe X1 Y1 X2 Y2 MS\n", 49);
		return 2;
	}
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK, 0);
	if (fd < 0) {
		write(2, "can't open /dev/uinput\n", 23);
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	axis(ABS_X, W - 1, RES_X);
	axis(ABS_Y, H - 1, RES_Y);
	axis(ABS_MT_SLOT, 1, 0);
	axis(ABS_MT_TRACKING_ID, 65535, 0);
	axis(ABS_MT_POSITION_X, W - 1, RES_X);
	axis(ABS_MT_POSITION_Y, H - 1, RES_Y);

	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1209;
	setup.id.product = 0x0001;
	strcpy(setup.name, "phonetouch");
	ioctl(fd, UI_DEV_SETUP, &setup);
	ioctl(fd, UI_DEV_CREATE, 0);
	/* The compositor has to notice the new device first. */
	msleep(1200);

	if (!strcmp(argv[1], "tap")) {
		down(atoi(argv[2]), atoi(argv[3]));
		msleep(60);
		up();
	} else if (!strcmp(argv[1], "swipe") && argc >= 7) {
		int x1 = atoi(argv[2]), y1 = atoi(argv[3]);
		int x2 = atoi(argv[4]), y2 = atoi(argv[5]);
		int steps = atoi(argv[6]) / 10;

		if (steps < 1)
			steps = 1;
		down(x1, y1);
		for (int i = 1; i <= steps; i++) {
			msleep(10);
			position(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
			ev(EV_SYN, SYN_REPORT, 0);
		}
		msleep(30);
		up();
	}
	msleep(100);
	ioctl(fd, UI_DEV_DESTROY, 0);
	close(fd);
	return 0;
}
