/*
 * Copyright (C) 2011 IITiS PAN Gliwice <www.iitis.pl>
 * Author: Paweł Foremski <pjf@iitis.pl>
 * Licensed under GNU GPL v. 3
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <linux/net.h>
#include <netinet/in.h>
#include <stdarg.h>

#include "tracedump.h"

int32_t inject_socketcall(struct tracedump *td, pid_t pid, uint32_t sc_code, ...)
{
	/* int 0x80, int3 */
	unsigned char code[4] = { 0xcd, 0x80, 0xcc, 0 };
	char backup[4];
	struct user_regs_struct regs, regs2;
	int ss_vals, ss_mem, ss;
	va_list vl;
	enum arg_type type;
	uint32_t sv;
	void *ptr;
	uint8_t *stack, *stack_mem;
	uint32_t *stack32;
	int i, j;

	/*
	 * get the required amount of stack space
	 */
	ss_vals = 0;
	ss_mem = 0;
	va_start(vl, sc_code);
	do {
		type = va_arg(vl, enum arg_type);
		if (type == AT_LAST) break;
		sv  = va_arg(vl, uint32_t);

		/* each socketcall argument takes 4 bytes */
		ss_vals += 4;

		/* if its memory, it takes additional sv bytes */
		if (type == AT_MEM_IN || type == AT_MEM_INOUT) {
			ss_mem += sv;
			ptr = va_arg(vl, void *);
		}
	} while (true);
	va_end(vl);
	ss = ss_vals + ss_mem;

	/*
	 * backup
	 */
	ptrace_getregs(pid, &regs);
	memcpy(&regs2, &regs, sizeof regs);
	ptrace_read(pid, regs.eip, backup, sizeof backup);

	/*
	 * write the stack
	 */
	stack = mmatic_zalloc(td->mm, ss);
	stack32 = (uint32_t *) stack;
	stack_mem = stack + ss_vals;

	va_start(vl, sc_code);
	i = 0; j = 0;
	do {
		type = va_arg(vl, enum arg_type);
		if (type == AT_LAST) break;

		sv  = va_arg(vl, uint32_t);

		if (type == AT_VALUE) {
			stack32[i++] = sv;
		} else { /* i.e. its a memory arg */
			stack32[i++] = regs.esp - ss_mem + j;

			/* copy the memory */
			ptr = va_arg(vl, void *);
			memcpy(stack_mem + j, ptr, sv);
			j += sv;
		}
	} while (true);
	va_end(vl);

	ptrace_write(pid, regs.esp - ss, stack, ss);

	/*
	 * write the code and run
	 */
	regs2.eax = 102; // socketcall
	regs2.ebx = sc_code;
	regs2.ecx = regs.esp - ss;

	ptrace_write(pid, regs.eip, code, sizeof code);
	ptrace_setregs(pid, &regs2);
	ptrace_cont(pid, true);

	/*
	 * read back
	 */
	ptrace_getregs(pid, &regs2);
	ptrace_read(pid, regs.esp - ss_mem, stack_mem, ss_mem);

	va_start(vl, sc_code);
	do {
		type = va_arg(vl, enum arg_type);
		if (type == AT_LAST) break;

		sv = va_arg(vl, uint32_t);
		if (type == AT_VALUE) continue;

		ptr = va_arg(vl, void *);
		if (type == AT_MEM_IN) continue;

		memcpy(ptr, stack_mem, sv);
		stack_mem += sv;
	} while (true);
	va_end(vl);

	/* restore */
	ptrace_write(pid, regs.eip, backup, sizeof backup);
	ptrace_setregs(pid, &regs);

	mmatic_free(stack);

	return regs2.eax;
}

/* TODO: check the ~/tmp version */
void inject_escape_socketcall(struct tracedump *td, pid_t pid)
{
	struct user_regs_struct regs;
	uint32_t orig_ebx;

	/* update the registers */
	ptrace_getregs(pid, &regs);
	orig_ebx = regs.ebx;
	regs.ebx = 0;
	ptrace_setregs(pid, &regs);

	/* run the invalid socketcall and wait */
	ptrace_cont_syscall(pid, true);

	/* restore */
	regs.eax = regs.orig_eax;
	regs.ebx = orig_ebx;
	ptrace_setregs(pid, &regs);

	/* now the process is in user mode */
}

void inject_restore_socketcall(struct tracedump *td, pid_t pid)
{
	/* int 0x80, int3 */
	unsigned char code[4] = { 0xcd, 0x80, 0xcc, 0 };
	char backup[4];
	struct user_regs_struct regs, regs2;

	/* backup */
	ptrace_getregs(pid, &regs);
	ptrace_read(pid, regs.eip, backup, 4);

	/* exec */
	ptrace_write(pid, regs.eip, code, 4);
	ptrace_cont(pid, true);

	/* copy the return code */
	ptrace_getregs(pid, &regs2);
	regs.eax = regs2.eax;

	/* restore */
	ptrace_write(pid, regs.eip, backup, 4);
	ptrace_setregs(pid, &regs);
}
