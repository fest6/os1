#include "trap.h"

#include "console.h"
#include "debug.h"
#include "defs.h"
#include "plic.h"
#include "timer.h"

#include "proc.h"

static int64 kp_print_lock = 0;
extern volatile int panicked;

void plic_handle() {
    int irq = plic_claim();
    if (irq == UART0_IRQ) {
        uart_intr();
        // printf("intr %d: UART0\n", r_tp());
    }

    if (irq)
        plic_complete(irq);
}

void kernel_trap(struct ktrapframe *ktf) {
    assert(!intr_get());

    if ((r_sstatus() & SSTATUS_SPP) == 0) {
        errorf("kerneltrap: not from supervisor mode");
        goto kernel_panic;
    }

    mycpu()->inkernel_trap++;

    uint64 cause          = r_scause();
    uint64 exception_code = cause & SCAUSE_EXCEPTION_CODE_MASK;
    if (cause & SCAUSE_INTERRUPT) {
        // correctness checking:
        if (mycpu()->inkernel_trap > 1) {
            // should never have nested interrupt
            print_sysregs(true);
            print_ktrapframe(ktf);
            errorf("nested kerneltrap");
            goto kernel_panic;
        }
        if (panicked) {
            panic("other CPU has panicked");
        }
        // handle interrupt
        switch (exception_code) {
            case SupervisorTimer:
                tracef("s-timer interrupt, cycle: %d", r_time());
                set_next_timer();
                // we never preempt kernel threads.
                struct proc *p = curr_proc();
                if (p != NULL) {

                    int inkernel_trap = mycpu()->inkernel_trap;
                    uint64 sstatus = r_sstatus();
                    uint64 scause  = r_scause();
                    uint64 sie     = r_sie();
                    uint64 sepc    = r_sepc();
                    uint64 stval   = r_stval();
                    uint64 sip     = r_sip();
                    uint64 satp    = r_satp();
                    mycpu()->inkernel_trap = 0;
                    yield();
                    mycpu()->inkernel_trap = inkernel_trap;
                    w_sstatus(sstatus);
                    w_sie(sie);
                    w_sepc(sepc);
                    w_sip(sip);
                    w_satp(satp);
                }
                break;
            case SupervisorExternal:
                tracef("s-external interrupt.");
                plic_handle();
                break;
            default:
                errorf("unhandled interrupt: %d", cause);
                goto kernel_panic;
        }
    } else {
        // kernel exception, unexpected.
        goto kernel_panic;
    }

    assert(!intr_get());
    assert(mycpu()->inkernel_trap == 1);

    mycpu()->inkernel_trap--;

    return;

kernel_panic:
    // lock against other cpu, to show a complete panic message.

    while (__sync_lock_test_and_set(&kp_print_lock, 1) != 0);

    errorf("=========== Kernel Panic ===========");
    print_sysregs(true);
    print_ktrapframe(ktf);

    __sync_lock_release(&kp_print_lock);

    panic("kernel panic");
}

void set_kerneltrap() {
    assert(IS_ALIGNED((uint64)kernel_trap_entry, 4));
    w_stvec((uint64)kernel_trap_entry);  // DIRECT
}

// set up to take exceptions and traps while in the kernel.
void trap_init() {
    set_kerneltrap();
}
