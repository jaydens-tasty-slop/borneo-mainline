/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tests the safety net that hands-off bring-up depends on, without Linux.
 *
 * It looks like an arm64 kernel to the bootloader. It writes a message into the
 * ramoops console zone, arms the APSS watchdog, then hangs. Getting back to rescue
 * with the message in pstore proves three things: registers are reachable this
 * early, a hang resets the phone, and RAM survives a watchdog reset. The MMU is off
 * here, so the write goes straight to memory and no cache flush is needed.
 *
 * The console zone follows the dump zone, whose size is what's left of the 0xbf800
 * region after the 0x40000 console and 0x40000 pmsg zones, so 0xaf03f800. The zone
 * layout is struct persistent_ram_buffer: signature, start, size, then the data.
 */
#define PRZ_CONSOLE	0xaf03f800
#define PRZ_SIG		0x43474244

#define WDT_BASE	0x0f017000
#define WDT_RST		0x04
#define WDT_EN		0x08
#define WDT_BARK	0x10
#define WDT_BITE	0x14

/* The watchdog runs from the 32768 Hz sleep clock. */
#define WDT_TICKS	(10 * 32768)

	.text
	.globl _start
_start:
	b	entry
	.long	0
	.quad	0			/* text_offset */
	.quad	image_end - _start	/* image_size */
	.quad	0x2			/* flags: little endian, 4K pages */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

entry:
	ldr	x0, =PRZ_CONSOLE
	ldr	w1, =PRZ_SIG
	mov	w2, #(msg_end - msg)
	str	w1, [x0]
	str	w2, [x0, #4]
	str	w2, [x0, #8]
	add	x3, x0, #12
	adr	x4, msg
	mov	w5, w2
1:	ldrb	w6, [x4], #1
	strb	w6, [x3], #1
	subs	w5, w5, #1
	b.ne	1b
	dsb	sy

	ldr	x0, =WDT_BASE
	str	wzr, [x0, #WDT_EN]
	mov	w1, #1
	str	w1, [x0, #WDT_RST]
	ldr	w1, =WDT_TICKS
	str	w1, [x0, #WDT_BARK]
	str	w1, [x0, #WDT_BITE]
	mov	w1, #1
	str	w1, [x0, #WDT_EN]
	dsb	sy

	msr	daifset, #0xf
hang:	wfe
	b	hang

msg:	.ascii	"BH-WDT-TEST: alive before the watchdog bite\n"
msg_end:
	.ltorg
	.balign	4096
image_end:
