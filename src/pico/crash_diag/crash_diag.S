#if PICODOOM_CRASH_DIAG
#include "crash_diag.h"

.syntax unified
.cpu cortex-m33
.thumb

// No stack, calls, locks, allocation, stdio, or flash fetches after entry.
// A broken Core 0 leaves Core 1 free to scan out its frozen frame + report.
// If Core 1 itself faults, its record survives in RAM for SWD, not HDMI.
.section .time_critical.picodoom_crash, "ax", %progbits
.balign 4

.macro record_pointer
    ldr r3, =0xd0000000 // SIO CPUID
    ldr r3, [r3]
    ldr r4, =picodoom_crash
    add r3, r4, r3, lsl #6
.endm

.macro fault_status
    ldr r4, =0xe000ed28 // CFSR, HFSR, DFSR, MMFAR, BFAR
    ldr r0, [r4]
    str r0, [r3, #CR_CFSR]
    ldr r0, [r4, #4]
    str r0, [r3, #CR_HFSR]
    ldr r0, [r4, #12]
    str r0, [r3, #CR_MMFAR]
    ldr r0, [r4, #16]
    str r0, [r3, #CR_BFAR]
    mrs r0, ipsr
    str r0, [r3, #CR_IPSR]
.endm

.global picodoom_crash_stop
.type picodoom_crash_stop, %function
.thumb_func
picodoom_crash_stop:
    cpsid i
    mov r7, r0
    record_pointer
    str r1, [r3, #CR_DETAIL]
    str r2, [r3, #CR_EXTRA]
    str lr, [r3, #CR_PC]
    str lr, [r3, #CR_LR]
    mov r0, sp
    str r0, [r3, #CR_SP]
    cmp r7, #CRASH_ENGINE
    bne 1f
    ldr r4, =hdmi_diag_i_error_count
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
1:
    fault_status
    b publish
.size picodoom_crash_stop, .-picodoom_crash_stop

// Wrappers retain the original caller LR, unlike a C printf/panic chain.
.global __wrap_panic
.type __wrap_panic, %function
.thumb_func
__wrap_panic:
    mov r2, r0 // format-string address, never dereferenced here
    movs r1, #0
    movs r0, #CRASH_PANIC
    b picodoom_crash_stop
.size __wrap_panic, .-__wrap_panic

.global __wrap___assert_func
.type __wrap___assert_func, %function
.thumb_func
__wrap___assert_func:
    mov r2, r0 // file-string address; r1 already contains source line
    movs r0, #CRASH_ASSERT
    b picodoom_crash_stop
.size __wrap___assert_func, .-__wrap___assert_func

.global isr_hardfault
.type isr_hardfault, %function
.thumb_func
isr_hardfault:
    cpsid i
    record_pointer
    movs r7, #CRASH_FAULT
    str lr, [r3, #CR_EXC_RETURN]
    tst lr, #4
    ite eq
    mrseq r5, msp
    mrsne r5, psp
    str r5, [r3, #CR_SP]
    fault_status

    // Only decode this firmware's Secure, default-stacked register frame.
    // Unsupported security transitions keep the raw EXC_RETURN/status but
    // don't risk a second fault by guessing at a different frame layout.
    and r0, lr, #0x60 // S and DCRS
    cmp r0, #0x60
    bne publish
    ldr r0, [r3, #CR_CFSR]
    ldr r1, =0x00101010 // STKOF, STKERR, MSTKERR: frame may be incomplete
    tst r0, r1
    bne publish
    tst r5, #3
    bne publish
    ldr r0, =0x20000000
    cmp r5, r0
    blo publish
    ldr r0, =0x20082000 - 32
    cmp r5, r0
    bhi publish

    // Armv8-M: core frame begins with R0 even when FP context is stacked.
    ldr r0, [r5, #24]
    str r0, [r3, #CR_PC]
    ldr r0, [r5, #20]
    str r0, [r3, #CR_LR]
    ldr r0, [r5, #28]
    str r0, [r3, #CR_XPSR]
    movs r0, #1
    str r0, [r3, #CR_FRAME_VALID]
publish:
    dmb
    str r7, [r3, #CR_KIND]
    dsb
.global picodoom_crash_halted
picodoom_crash_halted:
    wfe
    b picodoom_crash_halted
.size isr_hardfault, .-isr_hardfault

// Don't enable new traps; just catch these too if the SDK enables them.
.global isr_memmanage, isr_busfault, isr_usagefault
.thumb_set isr_memmanage, isr_hardfault
.thumb_set isr_busfault, isr_hardfault
.thumb_set isr_usagefault, isr_hardfault
.ltorg

.section .bss.picodoom_crash, "aw", %nobits
.balign 4
.global picodoom_crash
.type picodoom_crash, %object
picodoom_crash:
    .space CR_SIZE * 2
.size picodoom_crash, .-picodoom_crash
#endif
.section .note.GNU-stack,"",%progbits
