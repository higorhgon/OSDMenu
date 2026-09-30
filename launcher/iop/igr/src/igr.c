// igr.irx: button combos for the PS1 games launched via Ember, which has no in-game reset.
//
// Ember doesn't reset the IOP and keeps the modules the launcher loaded, so this module stays loaded
// while the game runs. It hooks sio2man's transfer function (export 25), like mmceman does, and reads
// the buttons from the controller replies on ports 0 and 1, whatever library the game reads them with.
//
// Held for a few frames:
// - L1 + L2 + R1 + R2 + START + SELECT returns to OSDMenu: writes EIGR_FLAG_MAGIC to EIGR_FLAG_ADDR in EE RAM
//   with SIF DMA, which eIGR, installed on the EE by the loader, checks in a few syscalls (see eigr.h)
// - L1 + L2 + R1 + R2 + L3 + R3 turns the console off
#include "eigr.h"
#include "irx_imports.h"
#include "ioplib.h"

#define MODNAME "osdmigr"
IRX_ID(MODNAME, 1, 0);

// Buttons, active high
#define BTN_SELECT 0x0001
#define BTN_START 0x0008
#define BTN_L3 0x0002
#define BTN_R3 0x0004
#define BTN_L2 0x0100
#define BTN_R2 0x0200
#define BTN_L1 0x0400
#define BTN_R1 0x0800

#define COMBO_HOLD (BTN_L1 | BTN_L2 | BTN_R1 | BTN_R2)
#define COMBO_POWEROFF (COMBO_HOLD | BTN_L3 | BTN_R3)
#define COMBO_RESET (COMBO_HOLD | BTN_START | BTN_SELECT)
#define COMBO_READS 8 // Consecutive controller reads with the combo held (the game reads the controller every frame)

#define ACTION_POWEROFF 1
#define ACTION_RESET 2

typedef int (*transfer_t)(sio2_transfer_data_t *td);
static transfer_t origTransfer[2]; // sio2man can register its 1.x and 2.x interfaces
static int comboReads[2];          // By port
static int actionThreadId = -1;
static volatile int action = 0;

static int isIopRAM(const void *ptr) { return ((u32)ptr & 0x1fffffff) < 0x200000; }

// Checks the reply to the "read data" command (0x01 0x42): 0xff, the mode (0x4? digital, 0x7? analog),
// 0x5a and the two button bytes, active low
static void checkPadReply(int port, const u8 *tx, const u8 *rx) {
  if ((tx[0] != 0x01) || (tx[1] != 0x42) || (rx[2] != 0x5a))
    return;
  if (((rx[1] >> 4) != 0x4) && ((rx[1] >> 4) != 0x7))
    return;

  u16 buttons = ~(rx[3] | (rx[4] << 8));
  int combo = 0;
  if ((buttons & COMBO_POWEROFF) == COMBO_POWEROFF)
    combo = ACTION_POWEROFF;
  else if ((buttons & COMBO_RESET) == COMBO_RESET)
    combo = ACTION_RESET;
  if (!combo) {
    comboReads[port] = 0;
    return;
  }
  if ((++comboReads[port] == COMBO_READS) && !action) {
    action = combo;
    WakeupThread(actionThreadId);
  }
}

// Goes through the transfers of td: each one sends and receives the number of bytes in its regdata word,
// one after the other in the in and out buffers
static void checkTransfer(sio2_transfer_data_t *td) {
  const u8 *tx = td->in ? td->in : (const u8 *)td->in_dma.addr;
  const u8 *rx = td->out ? td->out : (const u8 *)td->out_dma.addr;
  if (!tx || !rx || !isIopRAM(tx) || !isIopRAM(rx))
    return;

  u32 txOffset = 0, rxOffset = 0;
  for (int i = 0; i < 16; i++) {
    u32 reg = td->regdata[i];
    if (!reg)
      break;
    u32 port = reg & 3;
    u32 txSize = (reg >> 8) & 0x1ff;
    u32 rxSize = (reg >> 18) & 0x1ff;
    if ((port < 2) && (txSize >= 2) && (rxSize >= 5))
      checkPadReply(port, &tx[txOffset], &rx[rxOffset]);
    txOffset += txSize;
    rxOffset += rxSize;
  }
}

static int hookTransfer0(sio2_transfer_data_t *td) {
  int res = origTransfer[0](td);
  checkTransfer(td);
  return res;
}

static int hookTransfer1(sio2_transfer_data_t *td) {
  int res = origTransfer[1](td);
  checkTransfer(td);
  return res;
}

static int hooked = 0;
static int hookSio2man(iop_library_t *lib, void *userdata) {
  if (hooked >= 2)
    return 1;
  void *orig = ioplib_hookSameExportEntries(lib, 25, hooked ? (void *)hookTransfer1 : (void *)hookTransfer0);
  if (!orig)
    return 0;
  origTransfer[hooked++] = orig;
  ioplib_relinkExports(lib);
  return 0;
}

// Stops the DEV9 devices (network adapter, HDD) before turning the console off, like PS2SDK's poweroff does
static int stopDev9(iop_library_t *lib, void *userdata) {
  if (ioplib_getTableSize(lib) > 6)
    ((void (*)(void))lib->exports[6])(); // Dev9CardStop
  return 1;
}

// Written to EE RAM with SIF DMA, which transfers 16-byte blocks
static u32 flagData[4] __attribute__((aligned(16))) = {EIGR_FLAG_MAGIC, 0, 0, 0};

static void actionThread(void *arg) {
  while (!action)
    SleepThread();

  if (action == ACTION_RESET) {
    // eIGR takes it from here on the EE, the next time the game calls one of the hooked syscalls
    SifDmaTransfer_t dma;
    dma.src = flagData;
    dma.dest = (void *)EIGR_FLAG_ADDR;
    dma.size = sizeof(flagData);
    dma.attr = 0;
    int state, id;
    CpuSuspendIntr(&state);
    id = sceSifSetDma(&dma, 1);
    CpuResumeIntr(state);
    while (id && (sceSifDmaStat(id) >= 0))
      DelayThread(1000);
  }
  if (action == ACTION_POWEROFF) {
    u32 stat;
    ioplib_iterateByName("dev9\0\0\0\0", stopDev9, NULL);
    sceCdPowerOff(&stat);
  }
  while (1)
    SleepThread();
}

int _start(int argc, char *argv[]) {
  iop_thread_t thread = {0};
  thread.attr = TH_C;
  thread.thread = actionThread;
  thread.priority = 20;
  thread.stacksize = 0x800;
  if ((actionThreadId = CreateThread(&thread)) < 0)
    return MODULE_NO_RESIDENT_END;
  StartThread(actionThreadId, NULL);

  ioplib_iterateByName("sio2man\0", hookSio2man, NULL);
  if (!hooked)
    return MODULE_NO_RESIDENT_END;
  return MODULE_RESIDENT_END;
}
