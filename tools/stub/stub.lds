/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { *(.text) }
	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
