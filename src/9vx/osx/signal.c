/*
 * Truly awful code to simulate Unix signal handler dispatch
 * using Mach signal handler dispatch.  The BSD support routines
 * can't deal with our SIGSEGVs properly.  Among other things,
 * they keep waking up other threads and they cause a popup 
 * about the application quitting when it hasn't.
 *
 * This code is inspired by similar code in SBCL.
 * See also http://paste.lisp.org/display/19593.
 * See also http://lists.apple.com/archives/darwin-dev/2006/Oct/msg00122.html
 */

#define __DARWIN_UNIX03 0

#include <mach/mach.h>
#include <sys/ucontext.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signal.h>

#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

extern void sigsegv(int, siginfo_t*, void*);	/* provided by Plan 9 VX */
extern int invx32;

static x86_thread_state32_t normal;	/* normal segment registers */
static void *altstack;

/*
 * Manipulate stack in regs.
 */
static void
push(x86_thread_state32_t *regs, ulong data)
{
	uint *sp;
	
	sp = (uint*)regs->esp;
	*--sp = data;
	regs->esp = (uint)sp;
}

static void
align(x86_thread_state32_t *regs)
{
	uint *sp;
	
	sp = (uint*)regs->esp;
	while((ulong)sp & 15)
		sp--;
	regs->esp = (uint)sp;
}

static void*
alloc(x86_thread_state32_t *regs, int n)
{
	n = (n+15) & ~15;
	regs->esp -= n;
	return (void*)regs->esp;
}

/*
 * Signal handler wrapper.  Calls handler and then
 * causes an illegal instruction exception to jump
 * back to us.
 */
static void
wrapper(siginfo_t *siginfo,
	mcontext_t mcontext,
	void (*handler)(int, siginfo_t*, void*))
{
	ucontext_t ucontext;

	memset(&ucontext, 0, sizeof ucontext);
	ucontext.uc_mcontext = mcontext;
	handler(siginfo->si_signo, siginfo, &ucontext);

	/* Cause EXC_BAD_INSTRUCTION to "exit" signal handler */
	asm volatile(
		"movl %0, %%eax\n"
		"movl $0xdeadbeef, %%esp\n"
		".long 0xffff0b0f\n"
		: : "r" (mcontext));
}

void
dumpmcontext(mcontext_t m)
{
	x86_thread_state32_t *ureg;
	
	ureg = &m->ss;
	iprint("FLAGS=%luX TRAP=%luX ECODE=%luX PC=%luX CR2=%luX\n",
		ureg->eflags, m->es.trapno, m->es.err, ureg->eip, m->es.faultvaddr);
	iprint("  AX %8.8luX  BX %8.8luX  CX %8.8luX  DX %8.8luX\n",
		ureg->eax, ureg->ebx, ureg->ecx, ureg->edx);
	iprint("  SI %8.8luX  DI %8.8luX  BP %8.8luX  SP %8.8luX\n",
		ureg->esi, ureg->edi, ureg->ebp, ureg->esp);
}

void
dumpregs1(x86_thread_state32_t *ureg)
{
	iprint("FLAGS=%luX PC=%luX\n",
		ureg->eflags, ureg->eip);
	iprint("  AX %8.8luX  BX %8.8luX  CX %8.8luX  DX %8.8luX\n",
		ureg->eax, ureg->ebx, ureg->ecx, ureg->edx);
	iprint("  SI %8.8luX  DI %8.8luX  BP %8.8luX  SP %8.8luX\n",
		ureg->esi, ureg->edi, ureg->ebp, ureg->esp);
}


/*
 * Called by mach loop in exception handling thread.
 */
kern_return_t
catch_exception_raise(mach_port_t exception_port,
                      mach_port_t thread,
                      mach_port_t task,
                      exception_type_t exception,
                      exception_data_t code_vector,
                      mach_msg_type_number_t code_count)
{
	mach_msg_type_number_t n;
	x86_thread_state32_t regs, save_regs;
	siginfo_t *stk_siginfo;
	kern_return_t ret;
	uint *sp;
	mcontext_t stk_mcontext;

	n = x86_THREAD_STATE32_COUNT;
	ret = thread_get_state(thread, x86_THREAD_STATE32, (void*)&regs, &n);
	if(ret != KERN_SUCCESS)
		panic("mach get regs failed: %d", ret);

	switch(exception){
	case EXC_BAD_ACCESS:
		save_regs = regs;
		if(invx32)
			regs.esp = (uint)altstack;
		else if(regs.ss != normal.ss)
			panic("not in vx32 but bogus ss");
		align(&regs);
		regs.cs = normal.cs;
		regs.ds = normal.ds;
		regs.es = normal.es;
		regs.ss = normal.ss;

		stk_siginfo = alloc(&regs, sizeof *stk_siginfo);
		stk_mcontext = alloc(&regs, sizeof *stk_mcontext);

		memset(stk_siginfo, 0, sizeof *stk_siginfo);
		stk_siginfo->si_signo = SIGBUS;
		stk_siginfo->si_addr = (void*)code_vector[1];

		stk_mcontext->ss = save_regs;
		n = x86_FLOAT_STATE32_COUNT;
		ret = thread_get_state(thread, x86_FLOAT_STATE32, (void*)&stk_mcontext->fs, &n);
		if(ret != KERN_SUCCESS)
			panic("mach get fpregs failed: %d", ret);
		n = x86_EXCEPTION_STATE32_COUNT;
		ret = thread_get_state(thread, x86_EXCEPTION_STATE32, (void*)&stk_mcontext->es, &n);
		if(ret != KERN_SUCCESS)
			panic("mach get eregs: %d", ret);

		sp = alloc(&regs, 3*4);
		sp[0] = (uint)stk_siginfo;
		sp[1] = (uint)stk_mcontext;
		sp[2] = (uint)sigsegv;
		
		push(&regs, regs.eip);	/* for debugger; wrapper won't return */
		regs.eip = (uint)wrapper;

		ret = thread_set_state(thread, x86_THREAD_STATE32,
			(void*)&regs, x86_THREAD_STATE32_COUNT);
		if(ret != KERN_SUCCESS)
			panic("mach set regs failed: %d", ret);
		return KERN_SUCCESS;
	
	case EXC_BAD_INSTRUCTION:
		/* Thread signalling that it's done with sigsegv. */
		if(regs.esp != 0xdeadbeef){
			dumpregs1(&regs);
			return KERN_INVALID_RIGHT;
			panic("bad instruction eip=%p", regs.eip);
		}
		stk_mcontext = (mcontext_t)regs.eax;
		ret = thread_set_state(thread, x86_THREAD_STATE32,
			(void*)&stk_mcontext->ss, x86_THREAD_STATE32_COUNT);
		if(ret != KERN_SUCCESS)
			panic("mach set regs1 failed: %d", ret);
		ret = thread_set_state(thread, x86_FLOAT_STATE32,
			(void*)&stk_mcontext->fs, x86_FLOAT_STATE32_COUNT);
		if(ret != KERN_SUCCESS)
			panic("mach set fpregs failed: %d", ret);
		return KERN_SUCCESS;
	}
	return KERN_INVALID_RIGHT;
}

static void*
handler(void *v)
{
	extern boolean_t exc_server();
	mach_port_t port;
	
	setmach(machp[0]);
	port = (mach_port_t)v;
	mach_msg_server(exc_server, 2048, port, 0);
	return 0;	/* not reached */
}

void
machsiginit(void)
{
	mach_port_t port;
	pthread_t pid;
	stack_t ss;
	int ret;
	
	extern int vx32_getcontext(x86_thread_state32_t*);
	vx32_getcontext(&normal);

	if(sigaltstack(nil, &ss) < 0 || (ss.ss_flags & SS_DISABLE))
		panic("machsiginit: no alt stack");
	altstack = ss.ss_sp + ss.ss_size;

	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	ret = thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS|EXC_MASK_BAD_INSTRUCTION, port,
		EXCEPTION_DEFAULT, MACHINE_THREAD_STATE);
	pthread_create(&pid, nil, handler, (void*)port);
}

