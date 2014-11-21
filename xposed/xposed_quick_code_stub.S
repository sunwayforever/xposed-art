#include "arch/arm/asm_support_arm.S"
ENTRY xposed_quick_invoke_stub
    push   {r0, r4, r5, r9, r11, lr}       @ spill regs
    .save  {r0, r4, r5, r9, r11, lr}
    .pad #24
    .cfi_adjust_cfa_offset 24
    .cfi_rel_offset r0, 0
    .cfi_rel_offset r4, 4
    .cfi_rel_offset r5, 8
    .cfi_rel_offset r9, 12
    .cfi_rel_offset r11, 16
    .cfi_rel_offset lr, 20
    mov    r11, sp                         @ save the stack pointer
    .cfi_def_cfa_register r11
    mov    r9, r3                          @ move managed thread pointer into r9
    add    r5, r2, #4                      @ create space for method pointer in frame

    sub    r5, sp, r5                      @ reserve & align *stack* to 16 bytes: native calling
    and    r5, #0xFFFFFFF0                 @ convention only aligns to 8B, so we have to ensure ART
    mov    sp, r5                          @ 16B alignment ourselves.

    add    r0, sp, #4                      @ pass stack pointer + method ptr as dest for memcpy
    bl     memcpy                          @ memcpy (dest, src, bytes)
    ldr    r0, [r11]                       @ restore method*
    ldr    r1, [sp, #4]                    @ copy arg value for r1
    ldr    r2, [sp, #8]                    @ copy arg value for r2
    ldr    r3, [sp, #12]                   @ copy arg value for r3
    mov    ip, #0                          @ set ip to 0
    str    ip, [sp]                        @ store NULL for method* at bottom of frame
    ldr    ip, [r11, #24]  @ get pointer to the code
    blx    ip                              @ call the method
    mov    sp, r11                         @ restore the stack pointer
    ldr    ip, [sp, #28]                   @ load the result pointer
    strd   r0, [ip]                        @ store r0/r1 into result pointer
    pop    {r0, r4, r5, r9, r11, lr}       @ restore spill regs
    .cfi_restore r0
    .cfi_restore r4
    .cfi_restore r5
    .cfi_restore r9
    .cfi_restore lr
    .cfi_adjust_cfa_offset -24
    bx     lr
END xposed_quick_invoke_stub
