@ The guest program's entry (#385, ADR 0213): crt1.o's start for iOS armv7, the same eleven ARM
@ instructions the game's stub has (k_crt1_start in runtime/src/boot/image.c), so the boot finds
@ main the way it does for the game. The kernel left argc at [sp].
    .text
    .arm
    .align 2
    .globl start
start:
    ldr  r0, [sp]              @ argc
    add  r1, sp, #4            @ argv
    add  r4, r0, #1
    add  r2, r1, r4, lsl #2    @ envp
    bic  sp, sp, #7
    mov  r3, r2
1:  ldr  r4, [r3], #4          @ skip envp ...
    cmp  r4, #0
    bne  1b                    @ ... to apple
    blx  _main
    b    _exit
