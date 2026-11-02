/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A bring-up shim that runs before Linux, so that no debug code has to live in the
 * kernel tree.
 *
 * It looks like an arm64 kernel to the bootloader. It leaves a marker in the ramoops
 * console zone, arms the APSS watchdog, then jumps to the real Image, which sits 2MiB
 * further on. Everything the kernel expects is left alone: x0 still holds the DTB and
 * the MMU is still off. The bootloader doesn't load us on a 2MiB boundary, but neither
 * would it a real kernel, which copes when built with CONFIG_RELOCATABLE.
 *
 * What rescue finds in pstore tells us how far the boot got:
 * - this marker: Linux was entered but died before ramoops took over the zone
 * - a kernel log: Linux got at least as far as registering the pstore console
 * - the other marker: the load address isn't aligned enough for the kernel's segments
 *
 * The kernel's qcom_wdt driver sees the running watchdog at probe and keeps feeding
 * it, so a kernel that gets that far is only reset if it really hangs. See
 * tools/wdtest for the test that proved the reset path and the zone address.
 */
#define PRZ_CONSOLE	0xaf03f800
#define PRZ_SIG		0x43474244

#define WDT_BASE	0x0f017000
#define WDT_RST		0x04
#define WDT_EN		0x08
#define WDT_BARK	0x10
#define WDT_BITE	0x14

/* The watchdog runs from the 32768 Hz sleep clock and its limit is 0xfffff ticks. */
#define WDT_TICKS	(30 * 32768)

#define SZ_2M		0x200000
#define SZ_64K		0x10000

	.text
	.globl _start
_start:
	b	entry
	.long	0
	.quad	0			/* text_offset */
	.quad	SZ_2M + KERNEL_SIZE	/* image_size, so the bootloader leaves room */
	.quad	0x2			/* flags: little endian, 4K pages */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

entry:
	/*
	 * x0-x3 belong to the kernel, so only x4 and up are used here. The kernel can
	 * start anywhere its 64KiB segments stay aligned, relative to the load address.
	 */
	adr	x11, _start
	adr	x8, msg
	mov	w6, #(msg_end - msg)
	tst	x11, #(SZ_64K - 1)
	b.eq	1f
	adr	x8, msg_bad
	mov	w6, #(msg_bad_end - msg_bad)
1:
	ldr	x4, =PRZ_CONSOLE
	ldr	w5, =PRZ_SIG
	str	w5, [x4]
	str	w6, [x4, #4]
	str	w6, [x4, #8]
	add	x7, x4, #12
	mov	w9, w6
2:	ldrb	w10, [x8], #1
	strb	w10, [x7], #1
	subs	w9, w9, #1
	b.ne	2b
	dsb	sy

	ldr	x4, =WDT_BASE
	str	wzr, [x4, #WDT_EN]
	mov	w5, #1
	str	w5, [x4, #WDT_RST]
	ldr	w5, =WDT_TICKS
	str	w5, [x4, #WDT_BARK]
	str	w5, [x4, #WDT_BITE]
	mov	w5, #1
	str	w5, [x4, #WDT_EN]
	dsb	sy

	/* If the load address was wrong, hanging lets the watchdog reset us. */
	tst	x11, #(SZ_64K - 1)
	b.ne	hang
	add	x4, x11, #SZ_2M
	isb
	br	x4

hang:	wfe
	b	hang

msg:	.ascii	"BH-STUB: entered Linux, no kernel log after this\n"
msg_end:
msg_bad:	.ascii	"BH-STUB: not loaded on a 64KiB boundary, did not enter Linux\n"
msg_bad_end:
	.ltorg
