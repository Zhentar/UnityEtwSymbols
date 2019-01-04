#ifndef __MONO_SGENARCHDEP_H__
#define __MONO_SGENARCHDEP_H__

#include <mono/utils/mono-sigcontext.h>

#ifdef __i386__

#define REDZONE_SIZE	0

#define ARCH_NUM_REGS 7		/* we're never storing ESP */
#define ARCH_STORE_REGS(ptr)	\
	__asm__ __volatile__(	\
		"mov %%ecx, 0x00(%0)\n"	\
		"mov %%edx, 0x04(%0)\n"	\
		"mov %%ebx, 0x08(%0)\n"	\
		"mov %%edi, 0x0c(%0)\n"	\
		"mov %%esi, 0x10(%0)\n"	\
		"mov %%ebp, 0x14(%0)\n"	\
		: "=&a" (ptr)	\
		: "0" (cur_thread_regs)	\
		: "memory"	\
	)
#define ARCH_SIGCTX_SP(ctx)	(UCONTEXT_REG_ESP ((ctx)))
#define ARCH_SIGCTX_IP(ctx)	(UCONTEXT_REG_EIP ((ctx)))
#define ARCH_COPY_SIGCTX_REGS(a,ctx) do {			\
	(a)[0] = (gpointer) UCONTEXT_REG_EAX ((ctx));		\
	(a)[1] = (gpointer) UCONTEXT_REG_EBX ((ctx));		\
	(a)[2] = (gpointer) UCONTEXT_REG_ECX ((ctx));		\
	(a)[3] = (gpointer) UCONTEXT_REG_EDX ((ctx));		\
	(a)[4] = (gpointer) UCONTEXT_REG_ESI ((ctx));		\
	(a)[5] = (gpointer) UCONTEXT_REG_EDI ((ctx));		\
	(a)[6] = (gpointer) UCONTEXT_REG_EBP ((ctx));		\
	} while (0)

#elif defined(__x86_64__)

#define REDZONE_SIZE	128

#define ARCH_NUM_REGS 15	/* we're never storing RSP */
#define ARCH_STORE_REGS(ptr)	\
	__asm__ __volatile__(	\
		"movq %%rcx, 0x00(%0)\n"	\
		"movq %%rdx, 0x08(%0)\n"	\
		"movq %%rbx, 0x10(%0)\n"	\
		"movq %%rdi, 0x18(%0)\n"	\
		"movq %%rsi, 0x20(%0)\n"	\
		"movq %%rbp, 0x28(%0)\n"	\
		"movq %%r8, 0x30(%0)\n"	\
		"movq %%r9, 0x38(%0)\n"	\
		"movq %%r10, 0x40(%0)\n"	\
		"movq %%r11, 0x48(%0)\n"	\
		"movq %%r12, 0x50(%0)\n"	\
		"movq %%r13, 0x58(%0)\n"	\
		"movq %%r14, 0x60(%0)\n"	\
		"movq %%r15, 0x68(%0)\n"	\
		: "=&a" (ptr)	\
		: "0" (cur_thread_regs)	\
		: "memory"	\
	)
#define ARCH_SIGCTX_SP(ctx)    (UCONTEXT_REG_RSP (ctx))
#define ARCH_SIGCTX_IP(ctx)    (UCONTEXT_REG_RIP (ctx))
#define ARCH_COPY_SIGCTX_REGS(a,ctx) do {	\
	((a)[0] = (UCONTEXT_REG_RAX (ctx)));	\
	((a)[1] = (UCONTEXT_REG_RBX (ctx)));	\
	((a)[2] = (UCONTEXT_REG_RCX (ctx)));	\
	((a)[3] = (UCONTEXT_REG_RDX (ctx)));	\
	((a)[4] = (UCONTEXT_REG_RSI (ctx)));	\
	((a)[5] = (UCONTEXT_REG_RDI (ctx)));	\
	((a)[6] = (UCONTEXT_REG_RBP (ctx)));	\
	((a)[7] = (UCONTEXT_REG_R8 (ctx)));	\
	((a)[8] = (UCONTEXT_REG_R9 (ctx)));	\
	((a)[9] = (UCONTEXT_REG_R10 (ctx)));	\
	((a)[10] = (UCONTEXT_REG_R11 (ctx)));	\
	((a)[11] = (UCONTEXT_REG_R12 (ctx)));	\
	((a)[12] = (UCONTEXT_REG_R13 (ctx)));	\
	((a)[13] = (UCONTEXT_REG_R14 (ctx)));	\
	((a)[14] = (UCONTEXT_REG_R15 (ctx)));	\
	} while (0)

#elif defined(__ppc__)

#define REDZONE_SIZE	224

#define ARCH_NUM_REGS 32
#define ARCH_STORE_REGS(ptr)	\
	__asm__ __volatile__(	\
		"stmw r0, 0(%0)\n"	\
		:			\
		: "b" (ptr)		\
	)
#define ARCH_SIGCTX_SP(ctx)	(UCONTEXT_REG_Rn((ctx), 1))
#define ARCH_SIGCTX_IP(ctx)	(UCONTEXT_REG_NIP((ctx)))
#define ARCH_COPY_SIGCTX_REGS(a,ctx) do {	\
	int __i;	\
	for (__i = 0; __i < 32; ++__i)	\
		((a)[__i]) = UCONTEXT_REG_Rn((ctx), __i);	\
	} while (0)

#endif

#endif /* __MONO_SGENARCHDEP_H__ */
