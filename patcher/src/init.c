#include "init.h"
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>

// Resets IOP before loading OSDSYS
void resetModules() {
  while (!SifIopReset("", 0)) {
  };
  while (!SifIopSync()) {
  };

  SifExitIopHeap();
  SifLoadFileExit();
  sceSifExitRpc();
  sceSifExitCmd();

  FlushCache(0);
  FlushCache(2);
  fioInit();
}

#ifndef HOSD
// OSDMenu

// Loads IOP modules
int initModules() {
  sceSifInitRpc(0);
  while (!SifIopReset("", 0)) {
  };
  while (!SifIopSync()) {
  };

  sceSifInitRpc(0);

  // Apply patches required to load executables from memory cards
  sbv_patch_enable_lmb();
  sbv_patch_disable_prefix_check();
  sbv_patch_fileio();

  int ret;
  if ((ret = SifLoadModule("rom0:SIO2MAN", 0, NULL)) < 0)
    return ret;
  if ((ret = SifLoadModule("rom0:MCMAN", 0, NULL)) < 0)
    return ret;
  if ((ret = SifLoadModule("rom0:MCSERV", 0, NULL)) < 0)
    return ret;

  fioInit();
  return 0;
}

#ifdef LIVESCAN
#include "livescan.h"
extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern uint32_t size_iomanX_irx;
extern unsigned char mmceman_irx[] __attribute__((aligned(16)));
extern uint32_t size_mmceman_irx;
extern unsigned char gamescan_irx[] __attribute__((aligned(16)));
extern uint32_t size_gamescan_irx;

// Writes data to path, unless the file already has the same contents,
// so the memory card is only written when the modules change
static int writeFileIfChanged(const char *path, const uint8_t *data, uint32_t size) {
  static uint8_t buf[2048] __attribute__((aligned(64)));
  int fd = fioOpen(path, FIO_O_RDONLY);
  if (fd >= 0) {
    int same = (fioLseek(fd, 0, FIO_SEEK_END) == (int)size);
    fioLseek(fd, 0, FIO_SEEK_SET);
    for (uint32_t offset = 0; same && (offset < size); offset += sizeof(buf)) {
      uint32_t len = ((size - offset) > sizeof(buf)) ? sizeof(buf) : (size - offset);
      same = (fioRead(fd, buf, len) == (int)len) && !memcmp(buf, data + offset, len);
    }
    fioClose(fd);
    if (same)
      return 0;
  }

  if ((fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC)) < 0)
    return fd;
  int ret = fioWrite(fd, (void *)data, size);
  fioClose(fd);
  return (ret == (int)size) ? 0 : -EIO;
}

// Writes the modules used by the experimental live games scan (games_live_scan) to the memory card
// OSDMENU.CNF was loaded from, so they can be loaded after OSDSYS resets the IOP (see livescan.h).
// Returns 0 on success, or -(module number * 1000 + error) where module number is
// 1 for iomanX, 2 for mmceman and 3 for gamescan
int writeLiveScanModules() {
  struct {
    const char *path;
    void *irx;
    uint32_t size;
  } modules[] = {
      {LIVESCAN_IRX_IOMANX, iomanX_irx, size_iomanX_irx},
      {LIVESCAN_IRX_MMCEMAN, mmceman_irx, size_mmceman_irx},
      {LIVESCAN_IRX_GAMESCAN, gamescan_irx, size_gamescan_irx},
  };

  for (int i = 0; i < 3; i++) {
    char path[32];
    strcpy(path, modules[i].path);
    path[2] = (settings.mcSlot == 1) ? '1' : '0';
    int ret = writeFileIfChanged(path, modules[i].irx, modules[i].size);
    if (ret < 0)
      return -((i + 1) * 1000 - ret);
  }
  return 0;
}
#endif
#else
// HOSDMenu

#include <fileXio_rpc.h>

// Number of attempts initModules() will wait for hdd0 before returning
#define DELAY_ATTEMPTS 20

// Macros for loading embedded IOP modules
#define IRX_DEFINE(mod)                                                                                                                              \
  extern unsigned char mod##_irx[] __attribute__((aligned(16)));                                                                                     \
  extern uint32_t size_##mod##_irx

#define IRX_LOAD(mod, argLen, argStr)                                                                                                                \
  ret = SifExecModuleBuffer(mod##_irx, size_##mod##_irx, argLen, argStr, &iopret);                                                                   \
  if (ret < 0)                                                                                                                                       \
    return -1;                                                                                                                                       \
  if (iopret == 1)                                                                                                                                   \
    return -1;

IRX_DEFINE(iomanX);
IRX_DEFINE(fileXio);
IRX_DEFINE(ps2dev9);
IRX_DEFINE(ps2atad);
IRX_DEFINE(ps2hdd_osd);
IRX_DEFINE(ps2fs);

// ps2hdd module arguments. Up to 4 descriptors, 20 buffers
static char ps2hddArguments[] = "-o"
                                "\0"
                                "4"
                                "\0"
                                "-n"
                                "\0"
                                "20";
// ps2fs module arguments. Up to 2 mounts, 10 descriptors, 40 buffers
char ps2fsArguments[] = "-m"
                        "\0"
                        "2"
                        "\0"
                        "-o"
                        "\0"
                        "10"
                        "\0"
                        "-n"
                        "\0"
                        "40";

// Loads IOP modules
int initModules() {
  sceSifInitRpc(0);
  while (!SifIopReset("", 0)) {
  };
  while (!SifIopSync()) {
  };

  sceSifInitRpc(0);

  int ret = 0;
  int iopret = 0;
  // Apply patches required to load executables from EE RAM
  sbv_patch_enable_lmb();
  sbv_patch_disable_prefix_check();
  sbv_patch_fileio();

  IRX_LOAD(iomanX, 0, NULL)
  IRX_LOAD(fileXio, 0, NULL)
  IRX_LOAD(ps2dev9, 0, NULL)
  IRX_LOAD(ps2atad, 0, NULL)
  IRX_LOAD(ps2hdd_osd, sizeof(ps2hddArguments), ps2hddArguments)
  IRX_LOAD(ps2fs, sizeof(ps2fsArguments), ps2fsArguments)

  // Wait for IOP to initialize device drivers
  fileXioInit();
  for (int attempts = 0; attempts < DELAY_ATTEMPTS; attempts++) {
    ret = fileXioOpen("hdd0:", FIO_O_DIROPEN | FIO_O_RDONLY);
    if (ret >= 0) {
      fileXioClose(ret);
      return 0;
    }

    sleep(1);
  }
  return -1;
}

// Inits SIF RPC and fileXio without rebooting the IOP. Assumes all modules are already loaded
void shortInit() {
  sceSifInitRpc(0);
  fileXioInit();
}
#endif
