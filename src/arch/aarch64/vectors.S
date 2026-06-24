.global vector_table_el1

.macro save_all
    sub sp, sp, #272
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #16 * 15]

    // Save hardware ELR and SPSR directly into the trap frame
    mrs x21, elr_el1
    mrs x22, spsr_el1
    stp x21, x22, [sp, #16 * 16] // Offset 256
.endm

.macro restore_all
    // The stack pointer (sp) MUST already point to the 272-byte trap frame!
    ldp x21, x22, [sp, #16 * 16] // Offset 256
    msr elr_el1, x21
    msr spsr_el1, x22

    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30, [sp, #16 * 15]

    add sp, sp, #272
.endm

.align 11 // VBAR_EL1 requires 2048-byte alignment
vector_table_el1:
    // Current EL with SP0
    .align 7
    b trap_sync_sp0
    .align 7
    b trap_irq_sp0
    .align 7
    b trap_fiq_sp0
    .align 7
    b trap_serror_sp0

    // Current EL with SPx
    .align 7
    b sync_entry_current
    .align 7
    b irq_entry
    .align 7
    b trap_fiq_spx
    .align 7
    b trap_serror_spx

    // Lower EL using AArch64
    .align 7
    b sync_entry_lower64
    .align 7
    b irq_entry
    .align 7
    b trap_fiq_lower64
    .align 7
    b trap_serror_lower64

    // Lower EL using AArch32
    .align 7
    b trap_sync_lower32
    .align 7
    b irq_entry
    .align 7
    b trap_fiq_lower32
    .align 7
    b trap_serror_lower32

// Trap stubs (for fatal unhandled traps)
trap_sync_sp0:      b handle_sync
trap_irq_sp0:       b handle_irq
trap_fiq_sp0:       b handle_fiq
trap_serror_sp0:    b handle_serror

trap_fiq_spx:       b handle_fiq
trap_serror_spx:    b handle_serror

trap_fiq_lower64:   b handle_fiq
trap_serror_lower64:b handle_serror

trap_sync_lower32:  b handle_sync
trap_fiq_lower32:   b handle_fiq
trap_serror_lower32:b handle_serror

sync_entry_current:
    save_all
    mov x0, sp
    mov x1, #1
    bl handle_sync
    restore_all
    eret

sync_entry_lower64:
    save_all
    mov x0, sp
    mov x1, #2
    bl handle_sync
    restore_all
    eret

irq_entry:
    save_all
    mov x0, sp
    bl handle_irq
    restore_all
    eret

.global cpu_switch_to
cpu_switch_to:
    // x0 = prev_tcb, x1 = next_tcb
    // At offset 248 is uint64_t sp;
    // At offset 0 is uint64_t registers[31];
    
    // Save Callee-Saved Registers for PREV
    stp x19, x20, [x0, #19*8]
    stp x21, x22, [x0, #21*8]
    stp x23, x24, [x0, #23*8]
    stp x25, x26, [x0, #25*8]
    stp x27, x28, [x0, #27*8]
    stp x29, x30, [x0, #29*8]
    
    // Save Kernel Stack Pointer for PREV
    mov x9, sp
    str x9, [x0, #248]

    // Load Kernel Stack Pointer for NEXT
    ldr x9, [x1, #248]
    mov sp, x9

    // Restore Callee-Saved Registers for NEXT
    ldp x19, x20, [x1, #19*8]
    ldp x21, x22, [x1, #21*8]
    ldp x23, x24, [x1, #23*8]
    ldp x25, x26, [x1, #25*8]
    ldp x27, x28, [x1, #27*8]
    ldp x29, x30, [x1, #29*8]

    ret // Returns on the new stack natively!

.global cpu_switch_to_dead
cpu_switch_to_dead:
    // x0 = next_tcb
    // The previous task has already been destroyed, so do not save into it.

    // Load Kernel Stack Pointer for NEXT
    ldr x9, [x0, #248]
    mov sp, x9

    // Restore Callee-Saved Registers for NEXT
    ldp x19, x20, [x0, #19*8]
    ldp x21, x22, [x0, #21*8]
    ldp x23, x24, [x0, #23*8]
    ldp x25, x26, [x0, #25*8]
    ldp x27, x28, [x0, #27*8]
    ldp x29, x30, [x0, #29*8]

    ret // Returns on the next task's stack.

.global ret_from_fork
ret_from_fork:
    // We arrive here from cpu_switch_to.
    // cpu_switch_to restored our stack pointer to `tcb->sp`.
    // For a newly created task, `tcb->sp` points exactly to the 272-byte Trap Frame.
    // So we just fall through to `restore_all` and `eret`!
    
    restore_all
    eret

.global smp_secondary_entry
.extern smp_secondary_stacks
.extern smp_secondary_main
smp_secondary_entry:
    mov x19, x0
    adrp x1, smp_secondary_stacks
    add x1, x1, :lo12:smp_secondary_stacks
    mov x2, #4096
    madd x1, x19, x2, x1
    add x1, x1, x2
    mov sp, x1
    mov x0, x19
    bl smp_secondary_main
1:
    wfi
    b 1b
