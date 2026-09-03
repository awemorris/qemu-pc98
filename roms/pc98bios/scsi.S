/*
 * Free PC-9801-92 compatible SCSI option ROM
 * Copyright (C) 2026 Awe Morris
 *
 * This ROM implements the PC-98 common fixed-disk INT 1Bh interface and
 * direct IPL boot using the PC-9801-92 board's WD33C93 register interface.
 * It is an independent implementation based on the public board interface
 * and the option-ROM ABI used by the free PC-98 BIOS in this tree.  No code
 * from NEC's SCSIBIOS.ROM is included.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

.code16
.text

	.equ	SCSI_ASR,		0x0cc0
	.equ	SCSI_INDIRECT,		0x0cc2
	.equ	SCSI_DATA,		0x0cc6

	.equ	SBIC_CDB,		0x03
	.equ	SBIC_TARGET_LUN,	0x0f
	.equ	SBIC_COUNT,		0x12
	.equ	SBIC_DEST_ID,		0x15
	.equ	SBIC_STATUS,		0x17
	.equ	SBIC_COMMAND,		0x18

	.equ	ASR_INT,		0x80
	.equ	ASR_DBR,		0x01
	.equ	CMD_RESET,		0x00
	.equ	CMD_SELECT_XFER,	0x09
	.equ	CSR_RESET_DONE,		0x01
	.equ	CSR_XFER_DONE,		0x16

	.equ	WA_BUSY,		0x0456
	.equ	WA_TARGETS,		0x0482
	.equ	WA_SCSI_STATUS,		0x0483
	.equ	WA_SCSI_TARGET,		0x0484
	.equ	WA_DISK_ROM_SCSI0,	0x04b2
	.equ	WA_DISK_ROM_SCSI1,	0x04ba
	.equ	WA_DISK_ROM_SCSI2,	0x04bc
	.equ	WA_BOOT_OFF,		0x04ac
	.equ	WA_BOOT_SEG,		0x04ae
	.equ	WA_BOOT_DEVICE,		0x0584

	.equ	MAIN_INT1B_REAL_OFF,	0x17c0
	.equ	MAIN_INT1B_FREE_OFF,	0x17c4
	.equ	MAIN_BIOS_SEG,		0xfd80
	.equ	BOOT_SEGMENT,		0x1fc0
	/* PC-9801-92 compatible BIOS logical geometry (8 GiB limit). */
	.equ	SCSI_HEADS,		8
	.equ	SCSI_SECTORS,		32

	/*
	 * PC-98 C-Bus disk ROM entry table.  The boot-control property patches
	 * the complete three-byte entry at 15h, so every entry is independent.
	 */
	.globl	_start
_start:
	jmp	rom_init

	.org	0x0003
	lret
	nop
	nop
	lret
	nop
	nop

	.org	0x0009
	.byte	0x55, 0xaa, 0x02
	.byte	0xe9
	.word	install_entry - . - 2
	.byte	0xe9
	.word	controller_init_entry - . - 2
	lret
	nop
	nop
	.byte	0xe9
	.word	boot_entry - . - 2
	.byte	0xe9
	.word	int1b_entry - . - 2
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret

	.org	0x0040

/* Install the SCSI dispatcher, initialise the controller and probe targets. */
rom_init:
	pushw	%ds
	pushw	%es
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	xorw	%ax, %ax
	movw	%ax, %ds

	/* Publish the three fixed-disk aliases used by the common SCSI BIOS. */
	movw	%cs, %ax
	movb	%ah, WA_DISK_ROM_SCSI0
	movb	%ah, WA_DISK_ROM_SCSI1
	movb	%ah, WA_DISK_ROM_SCSI2

	/*
	 * The SCSI ROM is initialised before the internal IDE ROM.  Interpose
	 * INT 1Bh now and chain every non-SCSI request to the main BIOS.  The
	 * free firmware publishes a stable, signed entry at FD80:17C4h; the
	 * original Xa7-family convention uses FD80:17C0h.
	 */
	cli
	movw	$int1b_hook, 0x006c
	movw	%cs, 0x006e
	sti

	call	controller_reset
	call	probe_targets

	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	popw	%es
	popw	%ds
	lret

/* Compatibility entry used by firmware that installs disk-ROM templates. */
install_entry:
	movb	$0xc2, 0(%bx)
	movb	$0x82, 1(%bx)
	lret

controller_init_entry:
	pushw	%ds
	pushw	%ax
	xorw	%ax, %ax
	movw	%ax, %ds
	call	controller_reset
	call	probe_targets
	popw	%ax
	popw	%ds
	lret

/* Reset the WD33C93 and consume its reset-complete status. */
controller_reset:
	pushw	%ax
	pushw	%cx
	pushw	%dx
	movw	$SCSI_ASR, %dx
	movb	$SBIC_COMMAND, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	movb	$CMD_RESET, %al
	outb	%al, %dx
	movw	$0xffff, %cx
1:
	movw	$SCSI_ASR, %dx
	inb	%dx, %al
	testb	$ASR_INT, %al
	jnz	2f
	loop	1b
	jmp	3f
2:
	movb	$SBIC_STATUS, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	inb	%dx, %al
3:
	popw	%dx
	popw	%cx
	popw	%ax
	ret

/*
 * Probe targets 0..6 with INQUIRY.  Only non-removable direct-access disks
 * enter the BIOS fixed-disk bitmap; CD-ROMs remain available to native SCSI
 * drivers without being mistaken for 512-byte boot disks.
 */
probe_targets:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%es
	movb	$0, WA_TARGETS
	xorw	%bx, %bx
1:
	movb	%bl, WA_SCSI_TARGET
	subw	$42, %sp		/* six-byte CDB plus 36-byte reply */
	movw	%sp, %si
	movw	$0x0012, %ss:0(%si)
	movw	$0, %ss:2(%si)
	movw	$0x0024, %ss:4(%si)
	movw	%ss, %ax
	movw	%ax, %es
	leaw	6(%si), %di
	movw	$36, %ax
	pushw	%bx			/* retain target loop index */
	movw	$36, %bx
	movw	$6, %cx
	call	scsi_exec_in
	xorw	%dx, %dx
	jc	2f
	movb	%ss:6(%si), %al
	andb	$0x1f, %al
	jnz	2f			/* not a direct-access device */
	testb	$0x80, %ss:7(%si)
	jnz	2f			/* removable disks are not BIOS HDDs */
	movb	$1, %dl
2:
	popw	%bx
	addw	$42, %sp
	testb	%dl, %dl
	jz	3f
	/* Clear power-on Unit Attention before the first boot-sector READ. */
	subw	$6, %sp
	movw	%sp, %si
	movw	$0, %ss:0(%si)
	movw	$0, %ss:2(%si)
	movw	$0, %ss:4(%si)
	pushw	%bx
	movw	$0x0106, %cx
	call	scsi_exec_nodata
	popw	%bx
	popw	%ax
	popw	%ax
	popw	%ax
	jc	3f
	movb	$1, %al
	movb	%bl, %cl
	shlb	%cl, %al
	orb	%al, WA_TARGETS
3:
	incw	%bx
	cmpw	$7, %bx
	jb	1b
	popw	%es
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

/*
 * INT 1Bh vector interposer.  20h/A0h/C0h are the common SCSI fixed-disk
 * aliases.  Preserve the caller's untouched frame when chaining elsewhere.
 */
int1b_hook:
	sti
	cld
	pushw	%ax
	andb	$0xf0, %al
	cmpb	$0x20, %al
	je	1f
	cmpb	$0xa0, %al
	je	1f
	cmpb	$0xc0, %al
	je	1f
	popw	%ax
	/*
	 * The free BIOS carries "Q98" after its fixed chain trampoline.  Test
	 * that marker without consuming any caller register.  A firmware that
	 * lacks it is treated as an original Xa7-family BIOS.
	 */
	pushw	%ds
	pushw	%ax
	movw	$MAIN_BIOS_SEG, %ax
	movw	%ax, %ds
	cmpw	$0x3951, MAIN_INT1B_FREE_OFF + 3
	jne	2f
	cmpb	$0x38, MAIN_INT1B_FREE_OFF + 5
	jne	2f
	popw	%ax
	popw	%ds
	ljmp	$MAIN_BIOS_SEG, $MAIN_INT1B_FREE_OFF
2:
	popw	%ax
	popw	%ds
	ljmp	$MAIN_BIOS_SEG, $MAIN_INT1B_REAL_OFF
1:
	popw	%ax
	pushw	%ds
	pushw	%si
	pushw	%di
	pushw	%es
	pushw	%bp
	pushw	%dx
	pushw	%cx
	pushw	%bx
	pushw	%ax
	movw	%sp, %bp
	jmp	int1b_entry

/*
 * Direct disk-ROM entry.  BP addresses the standard saved-register frame:
 * AX +00, BX +02, CX +04, DX +06, BP +08, ES +0A, DI +0C, SI +0E,
 * DS +10, IP +12, CS +14, FLAGS +16.
 */
int1b_entry:
	pushl	%eax
	pushl	%ecx
	pushl	%edx
	xorw	%ax, %ax
	movw	%ax, %ds
	incb	WA_BUSY
	movb	0(%bp), %al
	andb	$0x07, %al
	movb	%al, WA_SCSI_TARGET
	movb	$1, %dl
	movb	%al, %cl
	shlb	%cl, %dl
	testb	%dl, WA_TARGETS
	jz	int1b_not_ready

	movb	1(%bp), %al
	andb	$0x0f, %al
	cmpb	$0x04, %al
	je	int1b_sense
	cmpb	$0x05, %al
	je	int1b_write
	cmpb	$0x06, %al
	je	int1b_read
	/* INITIALIZE, RECALIBRATE and legacy controls are harmless no-ops. */
	xorw	%ax, %ax
	jmp	int1b_finish

int1b_sense:
	movw	$512, 2(%bp)
	call	scsi_capacity_cylinders
	jnc	1f
	movw	$0xffff, %ax
1:
	movw	%ax, 4(%bp)
	movb	$SCSI_HEADS, %ah
	movb	$SCSI_SECTORS, %al
	movw	%ax, 6(%bp)
	movb	$0x0f, %ah
	jmp	int1b_finish

int1b_read:
	call	request_lba
	jc	int1b_finish
	call	request_buffer_count
	call	scsi_read10
	jmp	int1b_finish

int1b_write:
	call	request_lba
	jc	int1b_finish
	call	request_buffer_count
	/* Partial-sector writes cannot be represented by WRITE(10). */
	testw	$0x01ff, %bx
	jnz	int1b_bad_parameter
	call	scsi_write10
	jmp	int1b_finish

int1b_not_ready:
	movb	$0x60, %ah
	jmp	int1b_finish

int1b_bad_parameter:
	movb	$0x40, %ah

int1b_finish:
	movb	%ah, 1(%bp)
	andb	$0xfe, 0x16(%bp)
	cmpb	$0x20, %ah
	jb	1f
	orb	$0x01, 0x16(%bp)
1:
	xorw	%ax, %ax
	movw	%ax, %ds
	decb	WA_BUSY
	popl	%edx
	popl	%ecx
	popl	%eax
	popw	%ax
	popw	%bx
	popw	%cx
	popw	%dx
	popw	%bp
	popw	%es
	popw	%di
	popw	%si
	popw	%ds
	iret

/* Convert the BIOS request address to a 32-bit LBA in EAX. */
request_lba:
	testb	$0x80, 0(%bp)
	jz	2f
	movzwl	4(%bp), %eax
	imull	$(SCSI_HEADS * SCSI_SECTORS), %eax
	movzbl	7(%bp), %edx
	imull	$SCSI_SECTORS, %edx
	addl	%edx, %eax
	movzbl	6(%bp), %edx
	addl	%edx, %eax
	clc
	ret
2:
	movzwl	4(%bp), %eax
	movzbl	6(%bp), %edx
	shll	$16, %edx
	orl	%edx, %eax
	clc
	ret

/* Return ES:DI buffer, BX byte count and CX rounded 512-byte block count. */
request_buffer_count:
	movw	2(%bp), %bx
	movzwl	%bx, %ecx
	testw	%bx, %bx
	jnz	1f
	movl	$0x00010000, %ecx
1:
	addl	$511, %ecx
	shrl	$9, %ecx
	movw	8(%bp), %di
	movw	10(%bp), %dx
	movw	%dx, %es
	ret

/* Return READ CAPACITY converted to the BIOS 8/32 cylinder count in AX. */
scsi_capacity_cylinders:
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%es
	subw	$18, %sp		/* ten-byte CDB plus eight-byte reply */
	movw	%sp, %si
	movw	$0x0025, %ss:0(%si)
	movw	$0, %ss:2(%si)
	movw	$0, %ss:4(%si)
	movw	$0, %ss:6(%si)
	movw	$0, %ss:8(%si)
	movw	%ss, %ax
	movw	%ax, %es
	leaw	10(%si), %di
	movw	$8, %ax
	movw	$8, %bx
	movw	$10, %cx
	call	scsi_exec_in
	jc	2f

	movzbl	%ss:10(%si), %eax
	shll	$24, %eax
	movzbl	%ss:11(%si), %edx
	shll	$16, %edx
	orl	%edx, %eax
	movzbl	%ss:12(%si), %edx
	shll	$8, %edx
	orl	%edx, %eax
	movzbl	%ss:13(%si), %edx
	orl	%edx, %eax
	incl	%eax			/* READ CAPACITY returns the last LBA */
	xorl	%edx, %edx
	movl	$(SCSI_HEADS * SCSI_SECTORS), %ecx
	divl	%ecx
	testl	%edx, %edx
	jz	1f
	incl	%eax			/* include a partially populated cylinder */
1:
	testl	%eax, %eax
	jnz	3f
	incl	%eax
3:
	cmpl	$0x0000ffff, %eax
	jbe	4f
	movl	$0x0000ffff, %eax
4:
	addw	$18, %sp
	popw	%es
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	clc
	ret
2:
	addw	$18, %sp
	popw	%es
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	stc
	ret

/* EAX=LBA, CX=blocks, BX=requested bytes, ES:DI=buffer. */
scsi_read10:
	pushl	%eax
	pushw	%bx
	pushw	%cx
	pushw	%si
	pushw	%bp
	subw	$10, %sp
	movw	%sp, %si
	movb	$0x28, %ss:0(%si)
	movb	$0, %ss:1(%si)
	movl	%eax, %edx
	shrl	$24, %edx
	movb	%dl, %ss:2(%si)
	movl	%eax, %edx
	shrl	$16, %edx
	movb	%dl, %ss:3(%si)
	movl	%eax, %edx
	shrl	$8, %edx
	movb	%dl, %ss:4(%si)
	movb	%al, %ss:5(%si)
	movb	$0, %ss:6(%si)
	movb	%ch, %ss:7(%si)
	movb	%cl, %ss:8(%si)
	movb	$0, %ss:9(%si)
	movw	%cx, %ax
	shlw	$9, %ax
	movw	$10, %cx
	call	scsi_exec_in
	pushfw
	popw	%ax
	popw	%dx			/* discard CDB without changing FLAGS */
	popw	%dx
	popw	%dx
	popw	%dx
	popw	%dx
	popw	%bp
	popw	%si
	popw	%cx
	popw	%bx
	testw	$1, %ax
	jnz	1f
	popl	%eax
	xorb	%ah, %ah
	clc
	ret
1:
	popl	%eax
	movb	$0x80, %ah
	stc
	ret

/* EAX=LBA, CX=blocks, BX=requested bytes, ES:DI=buffer. */
scsi_write10:
	pushl	%eax
	pushw	%bx
	pushw	%cx
	pushw	%si
	pushw	%bp
	subw	$10, %sp
	movw	%sp, %si
	movb	$0x2a, %ss:0(%si)
	movb	$0, %ss:1(%si)
	movl	%eax, %edx
	shrl	$24, %edx
	movb	%dl, %ss:2(%si)
	movl	%eax, %edx
	shrl	$16, %edx
	movb	%dl, %ss:3(%si)
	movl	%eax, %edx
	shrl	$8, %edx
	movb	%dl, %ss:4(%si)
	movb	%al, %ss:5(%si)
	movb	$0, %ss:6(%si)
	movb	%ch, %ss:7(%si)
	movb	%cl, %ss:8(%si)
	movb	$0, %ss:9(%si)
	movw	%cx, %ax
	shlw	$9, %ax
	movw	$10, %cx
	call	scsi_exec_out
	pushfw
	popw	%ax
	popw	%dx			/* discard CDB without changing FLAGS */
	popw	%dx
	popw	%dx
	popw	%dx
	popw	%dx
	popw	%bp
	popw	%si
	popw	%cx
	popw	%bx
	testw	$1, %ax
	jnz	1f
	popl	%eax
	xorb	%ah, %ah
	clc
	ret
1:
	popl	%eax
	movb	$0x80, %ah
	stc
	ret

/* AX=wire byte count (zero means 64 KiB), BX=stored bytes, SS:SI CDB. */
scsi_exec_in:
	pushw	%ax
	pushw	%bx
	call	scsi_start
	popw	%bx
	popw	%cx
	jc	9f
	pushl	%ebp
	movzwl	%bx, %ebp
	testl	%ebp, %ebp
	jnz	0f
	movl	$0x00010000, %ebp
0:
	/* CX=0 intentionally executes 65536 iterations with LOOP. */
1:
	call	scsi_wait_data_or_status
	jc	8f
	testb	$ASR_DBR, %al
	jz	7f
	movw	$SCSI_DATA, %dx
	inb	%dx, %al
	testl	%ebp, %ebp
	je	2f
	stosb
	decl	%ebp
2:
	loop	1b
	call	scsi_finish
	popl	%ebp
	ret
7:
	call	scsi_ack_interrupt
	movb	$0x80, %ah
	stc
8:
	popl	%ebp
9:
	ret

/* AX=wire byte count, SS:SI CDB, ES:DI source. */
scsi_exec_out:
	pushw	%ax
	call	scsi_start
	popw	%cx
	jc	9f
1:
	call	scsi_wait_data_or_status
	jc	9f
	testb	$ASR_DBR, %al
	jz	8f
	movb	%es:(%di), %al
	incw	%di
	movw	$SCSI_DATA, %dx
	outb	%al, %dx
	loop	1b
	jmp	scsi_finish
8:
	call	scsi_ack_interrupt
	movb	$0x80, %ah
	stc
	ret
9:
	ret

/* No-data command: AX=0, CL=CDB length, SS:SI CDB. */
scsi_exec_nodata:
	xorw	%ax, %ax
	call	scsi_start
	jc	1f
	jmp	scsi_finish
1:
	ret

/*
 * Start SELECT-AND-TRANSFER.  AX is the transfer count, CL the CDB length,
 * and SS:SI points at the CDB.  The transfer count is kept for the caller.
 */
scsi_start:
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%bp
	movw	%ax, %bp
	movw	%cx, %bx

	movw	$SCSI_ASR, %dx
	movb	$SBIC_DEST_ID, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	movb	WA_SCSI_TARGET, %al
	outb	%al, %dx

	movw	$SCSI_ASR, %dx
	movb	$SBIC_TARGET_LUN, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	xorb	%al, %al
	outb	%al, %dx

	movw	$SCSI_ASR, %dx
	movb	$SBIC_COUNT, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	movw	%bp, %ax
	testw	%bp, %bp
	jnz	1f
	testb	%bh, %bh
	jne	4f
	movb	$1, %al
	outb	%al, %dx
	xorb	%al, %al
	outb	%al, %dx
	outb	%al, %dx
	jmp	2f
4:
	xorb	%al, %al
	outb	%al, %dx
	outb	%al, %dx
	outb	%al, %dx
	jmp	2f
1:
	xorb	%al, %al
	outb	%al, %dx
	movw	%bp, %ax
	movb	%ah, %al
	outb	%al, %dx
	movw	%bp, %ax
	outb	%al, %dx
2:
	movw	$SCSI_ASR, %dx
	movb	$SBIC_CDB, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	/* BL retained the CDB length before the setup sequence. */
	xorb	%bh, %bh
3:
	movb	%ss:(%si), %al
	outb	%al, %dx
	incw	%si
	decw	%bx
	jnz	3b

	movw	$SCSI_ASR, %dx
	movb	$SBIC_COMMAND, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	movb	$CMD_SELECT_XFER, %al
	outb	%al, %dx

	popw	%bp
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	clc
	ret

/* Poll until data buffer ready or command completion; CF on timeout. */
scsi_wait_data_or_status:
	pushw	%cx
	pushw	%dx
	movw	$0xffff, %cx
1:
	movw	$SCSI_ASR, %dx
	inb	%dx, %al
	testb	$(ASR_DBR | ASR_INT), %al
	jnz	2f
	loop	1b
	popw	%dx
	popw	%cx
	movb	$0x80, %ah
	stc
	ret

/* Consume a timeout or premature completion status so IRQ5 cannot stick. */
scsi_ack_interrupt:
	pushw	%ax
	pushw	%dx
	movw	$SCSI_ASR, %dx
	inb	%dx, %al
	testb	$ASR_INT, %al
	jz	1f
	movb	$SBIC_STATUS, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	inb	%dx, %al
1:
	popw	%dx
	popw	%ax
	ret
2:
	popw	%dx
	popw	%cx
	clc
	ret

/* Wait for final interrupt and validate CSR 16h plus target status 00h. */
scsi_finish:
	pushw	%cx
	pushw	%dx
	movw	$0xffff, %cx
1:
	movw	$SCSI_ASR, %dx
	inb	%dx, %al
	testb	$ASR_INT, %al
	jnz	2f
	loop	1b
	movb	$0x80, %ah
	stc
	jmp	5f
2:
	movb	$SBIC_STATUS, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	inb	%dx, %al
	cmpb	$CSR_XFER_DONE, %al
	jne	4f
	movw	$SCSI_ASR, %dx
	movb	$SBIC_TARGET_LUN, %al
	outb	%al, %dx
	movw	$SCSI_INDIRECT, %dx
	inb	%dx, %al
	movb	%al, WA_SCSI_STATUS
	testb	%al, %al
	jnz	4f
	xorw	%ax, %ax
	clc
	jmp	5f
4:
	movb	$0x80, %ah
	stc
5:
	popw	%dx
	popw	%cx
	ret

/*
 * Try every detected target, read its first two sectors, and start a PC-98
 * IPL carrying the standard IPL1 marker at offset four.
 */
boot_entry:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%bp
	pushw	%ds
	pushw	%es
	xorw	%ax, %ax
	movw	%ax, %ds
	xorw	%bx, %bx
1:
	movb	$1, %al
	movb	%bl, %cl
	shlb	%cl, %al
	testb	%al, WA_TARGETS
	jz	3f
	movb	%bl, WA_SCSI_TARGET
	movw	$BOOT_SEGMENT, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorl	%eax, %eax
	movw	$2, %cx
	pushw	%bx
	movw	$1024, %bx
	call	scsi_read10
	popw	%bx
	jc	3f
	cmpw	$0x5049, %es:4
	jne	3f
	cmpw	$0x314c, %es:6
	jne	3f
	movb	WA_SCSI_TARGET, %al
	orb	$0x20, %al
	movb	%al, WA_BOOT_DEVICE
	movw	$0, WA_BOOT_OFF
	movw	$BOOT_SEGMENT, WA_BOOT_SEG
	xorb	%ah, %ah
	xorw	%bp, %bp
	lcall	*WA_BOOT_OFF
3:
	incw	%bx
	cmpw	$7, %bx
	jb	1b
	popw	%es
	popw	%ds
	popw	%bp
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	lret

	/* Keep the image exactly within the board's 8 KiB ROM aperture. */
	.org	0x1fff
	.byte	0xff
