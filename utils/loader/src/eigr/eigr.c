// eIGR: EE side of the in-game reset for games that don't have one (Ember), see eigr.h.
// Everything here is placed at EIGR_BASE. eigrExit must not call anything outside of it,
// since it overwrites the rest of the loader
#include "eigr.h"
#include <elf.h>
#include <kernel.h>
#include <stdint.h>
#include <syscallnr.h>

#define EIGR_HOOKS 4

// Syscalls the game calls often from threads: waiting for the next frame, sending data to the IOP
// and flushing the caches after writing code (Ember's recompiler)
static const int hookedSyscalls[EIGR_HOOKS] = {__NR_WaitSema, __NR_SignalSema, __NR_sceSifSetDma, __NR_FlushCache};

void eigrHook0(void);
void eigrHook1(void);
void eigrHook2(void);
void eigrHook3(void);
int eigrSetSyscall(int number, void *handler);
void eigrFlushCache(int mode);
int eigrExecPS2(void *entry, void *gp, int argc, char *argv[]);

// Written by eigrInstall, so nothing depends on the section being zeroed
void *eigrOrigHandlers[EIGR_HOOKS];
static char returnPath[EIGR_RETURN_PATH_MAX];
static char reopenArg[8];
static char loaderArg[8];
static char *exitArgv[3];

// The hooked syscall returns here when the reset combo is held: puts the original syscalls back,
// copies the loader from its stash and runs it to load the return ELF after resetting the IOP
void eigrExit(void) {
  for (int i = 0; i < EIGR_HOOKS; i++)
    eigrSetSyscall(hookedSyscalls[i], eigrOrigHandlers[i]);

  const uint8_t *elf = (const uint8_t *)EIGR_STASH_ADDR;
  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)elf;
  const Elf32_Phdr *eph = (const Elf32_Phdr *)(elf + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; i++) {
    // eIGR's own section is running
    if ((eph[i].p_type != PT_LOAD) || (eph[i].p_vaddr >= EIGR_BASE))
      continue;
    volatile uint8_t *dst = (volatile uint8_t *)eph[i].p_vaddr;
    const uint8_t *src = elf + eph[i].p_offset;
    for (uint32_t b = 0; b < eph[i].p_filesz; b++)
      dst[b] = src[b];
  }
  eigrFlushCache(0);
  eigrFlushCache(2);

  exitArgv[0] = returnPath;
  exitArgv[1] = reopenArg;
  exitArgv[2] = loaderArg;
  eigrExecPS2((void *)eh->e_entry, 0, 3, exitArgv);
  while (1)
    ;
}

// Called by the loader right before starting the game: saves the return ELF path and hooks the syscalls.
// The launcher must have copied the loader ELF to EIGR_STASH_ADDR
void eigrInstall(const char *path, const char *reopen) {
  static void (*const hooks[EIGR_HOOKS])(void) = {eigrHook0, eigrHook1, eigrHook2, eigrHook3};

  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)EIGR_STASH_ADDR;
  if ((eh->e_ident[0] != ELFMAG0) || (eh->e_ident[1] != ELFMAG1) || (eh->e_ident[2] != ELFMAG2) || (eh->e_ident[3] != ELFMAG3))
    return;

  int i = 0;
  for (; path[i] && (i < EIGR_RETURN_PATH_MAX - 1); i++)
    returnPath[i] = path[i];
  returnPath[i] = '\0';
  for (i = 0; reopen[i] && (i < (int)sizeof(reopenArg) - 1); i++)
    reopenArg[i] = reopen[i];
  reopenArg[i] = '\0';
  // Reset the IOP and load its memory card drivers before loading the return ELF ('M'),
  // then reset it again before starting it ('R')
  const char la[] = "-la=MR";
  for (i = 0; la[i]; i++)
    loaderArg[i] = la[i];
  loaderArg[i] = '\0';

  // The flag may have been set before the game started
  *(volatile uint32_t *)(0xA0000000 | EIGR_FLAG_ADDR) = 0;

  for (i = 0; i < EIGR_HOOKS; i++) {
    eigrOrigHandlers[i] = GetSyscallHandler(hookedSyscalls[i]);
    SetSyscall(hookedSyscalls[i], (void *)(((uint32_t)hooks[i] & ~0xE0000000) | 0x80000000));
  }
  FlushCache(0);
  FlushCache(2);
}
