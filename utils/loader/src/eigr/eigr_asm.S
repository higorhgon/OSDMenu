# eIGR syscall hooks and syscall stubs, resident at EIGR_BASE (see eigr.h)
#
# Each hook checks the flag igr.irx writes with SIF DMA, reading it uncached. When it's set, the hook
# clears it and replaces the address the syscall returns to (saved by the kernel at 8($sp), like
# Open PS2 Loader's Hook_Exit does) with eigrExit, without running the syscall.
# Otherwise it jumps to the original handler. Only $v1 is used, like OPL's hooks.

#include "eigr.h"

.set noreorder
.set noat

.section .text

.macro EIGR_HOOK name, orig
.globl \name
.ent \name
\name:
  lui   $v1, (0xA0000000 | EIGR_FLAG_ADDR) >> 16
  lw    $v1, 0($v1)
  bne   $v1, $zero, 1f
  nop
  lui   $v1, %hi(\orig)
  lw    $v1, %lo(\orig)($v1)
  jr    $v1
  nop
1:
  lui   $v1, (0xA0000000 | EIGR_FLAG_ADDR) >> 16
  sw    $zero, 0($v1)
  lui   $v1, %hi(eigrExit)
  addiu $v1, $v1, %lo(eigrExit)
  jr    $ra
  sw    $v1, 8($sp)
.end \name
.endm

EIGR_HOOK eigrHook0, eigrOrigHandlers
EIGR_HOOK eigrHook1, eigrOrigHandlers + 4
EIGR_HOOK eigrHook2, eigrOrigHandlers + 8
EIGR_HOOK eigrHook3, eigrOrigHandlers + 12

# Syscall stubs, since the loader's libkernel is overwritten by eigrExit
.macro EIGR_SYSCALL name, number
.globl \name
.ent \name
\name:
  addiu $v1, $zero, \number
  syscall
  jr    $ra
  nop
.end \name
.endm

EIGR_SYSCALL eigrSetSyscall, 0x74
EIGR_SYSCALL eigrFlushCache, 0x64
EIGR_SYSCALL eigrExecPS2, 7
