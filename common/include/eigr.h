#ifndef _EIGR_H_
#define _EIGR_H_

// In-game reset for games that don't have one (Ember), in two parts:
// - igr.irx, loaded by the launcher on the IOP, reads the controller and, when the reset combo is held,
//   writes EIGR_FLAG_MAGIC to EIGR_FLAG_ADDR in EE RAM with SIF DMA
// - eIGR, resident on the EE in the loader's EIGR_BASE section, hooks a few syscalls the game calls often.
//   When the flag is set, the hooked syscall returns to eIGR's exit routine instead of the game, which
//   runs the loader again from the copy the launcher left at EIGR_STASH_ADDR, loading the return ELF
//   after resetting the IOP (loader flag 'M')
//
// EIGR_BASE is in the memory below 0x100000 that ELFs don't use, after the loader (0x84000-0xA0000)
// and below its stack

#define EIGR_BASE 0x000C0000
#define EIGR_FLAG_ADDR EIGR_BASE     // 16 bytes, the SIF DMA unit
#define EIGR_SIZE 0x1000             // eIGR code and data
#define EIGR_STASH_ADDR (EIGR_BASE + EIGR_SIZE)
#define EIGR_STASH_MAX 0x20000       // Largest loader ELF that can be kept
#define EIGR_FLAG_MAGIC 0x52474945   // "EIGR"
#define EIGR_RETURN_PATH_MAX 128

#endif
