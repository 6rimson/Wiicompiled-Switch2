// AArch64 ELF build of the cooperative context switch in ../macos/co_switch.S (same frame
// layout and contract). Used on AArch64 Linux instead of libco's AArch64 backend, which is a
// machine-code array that needs mprotect to become executable and has no BTI landing pad, so it
// faults wherever code pages are BTI-guarded (HBLauncher on Switch 2, BTI-enabled distributions).
// Both entry points start with `bti c` (a NOP on cores without BTI); the trampoline is reached
// by `ret`, which BTI does not check, and its `blr x19` lands on a compiler-emitted `bti c`.

    .text
    .p2align 2

// Frame (240 bytes): x18-x30, then v8-v15. x18 is a temporary on Linux; saving it is harmless.
// x0 = address holding the target frame pointer; x1 = address to receive the current one.
    .globl mkw_co_switch
    .type mkw_co_switch, %function
mkw_co_switch:
    bti c
    sub sp, sp, #240
    str x18, [sp, #0]
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp x29, x30, [sp, #96]
    stp q8, q9, [sp, #112]
    stp q10, q11, [sp, #144]
    stp q12, q13, [sp, #176]
    stp q14, q15, [sp, #208]
    mov x2, sp
    str x2, [x1]

    ldr x2, [x0]
    mov sp, x2
    ldr x18, [sp, #0]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp, #96]
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    add sp, sp, #240
    ret
    .size mkw_co_switch, .-mkw_co_switch

// Creates a frame compatible with mkw_co_switch and returns its saved SP.
// x0 = one-past-end stack pointer, x1 = entry(void*), x2 = entry argument.
    .globl mkw_co_init
    .type mkw_co_init, %function
mkw_co_init:
    bti c
    bic x0, x0, #0xf
    sub x0, x0, #240
    str x18, [x0, #0]
    str x1, [x0, #16]      // x19: entry
    str x2, [x0, #24]      // x20: argument
    str xzr, [x0, #96]     // x29
    adrp x3, mkw_co_entry_trampoline
    add x3, x3, :lo12:mkw_co_entry_trampoline
    str x3, [x0, #104]     // x30
    ret
    .size mkw_co_init, .-mkw_co_init

    .type mkw_co_entry_trampoline, %function
mkw_co_entry_trampoline:
    mov x0, x20
    blr x19
    brk #0
    .size mkw_co_entry_trampoline, .-mkw_co_entry_trampoline

    .section .note.GNU-stack, "", %progbits

// GNU property note: this object supports BTI, so linking it does not clear the output's BTI
// marking (GNU_PROPERTY_AARCH64_FEATURE_1_AND = 0xc0000000, BTI = bit 0).
    .section .note.gnu.property, "a"
    .p2align 3
    .word 4
    .word 16
    .word 5
    .asciz "GNU"
    .word 0xc0000000
    .word 4
    .word 1
    .word 0
