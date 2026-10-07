// Reads the controller from within OSDSYS for the games submenu.
// OSDSYS passes a DMA buffer to scePadPortOpen() that libpad keeps updated with the pad state,
// so the patcher hooks the calls to scePadPortOpen() to get the buffer and reads the buttons from it,
// the same way Open PS2 Loader's IGR does.
// It also hooks scePadRead() to hide Triangle from OSDSYS while a submenu is shown, since OSDSYS
// would open the Version screen with it, and to hide every button while the controller is paused.
#include "patches_pad.h"
#include "patches_common.h"
#include "patterns_pad.h"
#include "settings.h"
#include <kernel.h>

#define PAD_STATE_STABLE 6

static int (*scePadPortOpen)(int port, int slot, void *addr) = NULL;
static volatile uint8_t *padBuf = NULL;
static int posState = 0;
static int posButtons = 0; // First of the two button bytes
static uint16_t backMask = 0;
static uint16_t prevButtons = 0;

static int (*scePadRead)(int port, int slot, unsigned char *rdata) = NULL;
static int hideTriangle = 0;
static volatile int muteReads = 0;
// For games_button_debug and the games_live_scan = 2 boot report
uint32_t padReadAddr = 0;
uint32_t padPortOpenAddr = 0;
uint32_t padDmaStrAddr = 0;
int padReadRedirects = 0;
volatile uint32_t padReadCalls = 0;

// Buttons are in bytes 2 and 3 of the data (active low), Triangle is bit 4 of byte 3, and the analog sticks
// in bytes 4 to 7 (0x80 at rest)
static int hookPadRead(int port, int slot, unsigned char *rdata) {
  int ret = scePadRead(port, slot, rdata);
  padReadCalls++;
  if ((ret < 4) || (((uint32_t)rdata & 0x1fffffff) < 0x100000) || (((uint32_t)rdata & 0x1fffffff) >= 0x2000000))
    return ret;
  if (muteReads > 0) {
    // The pad state isn't updated while the controller is paused, so the last one read (like Down held)
    // would go on repeating: nothing is pressed instead
    muteReads--;
    rdata[2] = rdata[3] = 0xff;
    for (int i = 4; (i < 8) && (i < ret); i++)
      rdata[i] = 0x80;
  } else if (hideTriangle)
    rdata[3] |= (PADB_TRIANGLE >> 8);
  return ret;
}

void padHideTriangle(int hide) { hideTriangle = hide; }

void padMute(int reads) {
  if (reads > muteReads)
    muteReads = reads;
}

// Redirects all J/JAL calls to func and function pointers to it to hook. Returns the number of redirects
static int redirectCalls(uint8_t *osd, uint32_t func, void *hook) {
  uint32_t call = 0x08000000 | ((func >> 2) & 0x03ffffff);
  uint32_t hookTarget = ((uint32_t)hook >> 2) & 0x03ffffff;
  int count = 0;
  for (uint32_t *p = (uint32_t *)osd; p < (uint32_t *)(osd + 0x100000); p++) {
    if ((*p & 0xfbffffff) == call) {
      *p = (*p & 0xfc000000) | hookTarget;
      count++;
    } else if (*p == func) {
      *p = (uint32_t)hook;
      count++;
    }
  }
  return count;
}

// Returns the J/JAL target of insn, or 0
static uint32_t jumpTarget(uint32_t *insn) {
  if ((*insn & 0xfc000000) != 0x0c000000)
    return 0;
  return (((uint32_t)insn + 4) & 0xf0000000) | ((*insn & 0x03ffffff) << 2);
}

// Whether the instruction reads or writes the general purpose register r (approximately: enough for libpad's code)
static int readsReg(uint32_t insn, int r) {
  int op = insn >> 26, rs = (insn >> 21) & 0x1f, rt = (insn >> 16) & 0x1f;
  switch (op) {
  case 0x02: // j
  case 0x03: // jal
  case 0x0f: // lui
    return 0;
  case 0x00: // SPECIAL
  case 0x1c: // MMI
  case 0x04: // beq
  case 0x05: // bne
  case 0x14: // beql
  case 0x15: // bnel
    return (rs == r) || (rt == r);
  }
  if (((op >= 0x28) && (op <= 0x2f)) || (op == 0x1f) || (op == 0x3f)) // Stores (sq, sd)
    return (rs == r) || (rt == r);
  return rs == r; // Loads, immediates, REGIMM
}

static int writesReg(uint32_t insn, int r) {
  int op = insn >> 26, rt = (insn >> 16) & 0x1f, rd = (insn >> 11) & 0x1f;
  if ((op == 0x00) || (op == 0x1c))
    return rd == r;
  if (((op >= 0x08) && (op <= 0x0f)) || (op == 0x18) || (op == 0x19) || (op == 0x1a) || (op == 0x1b) || (op == 0x1e) ||
      ((op >= 0x20) && (op <= 0x27)) || (op == 0x37)) // Immediates, daddi(u), ldl/ldr, lq, loads, ld
    return rt == r;
  return 0;
}

// Finds scePadRead() among the libpad functions that follow scePadPortOpen() (seen in HDD-OSD 1.10 and
// ROM 2.30: PortOpen, PortClose, PortClose2, GetDmaStr, GetFrameCount, Read, GetState...).
// scePadGetDmaStr() is the function they call the most. scePadRead() is the first function OSDSYS calls in
// the range that calls scePadGetDmaStr() and uses its third argument (a2, the data buffer): reads a2
// before writing it. scePadGetFrameCount() uses a2 too, but to keep its first argument.
// Returns 0 if not found
#define LIBPAD_SEARCH_SIZE 0x800
#define LIBPAD_FUNC_MAX_INSNS 96
static uint32_t findPadRead(uint8_t *osd, uint32_t portOpen) {
  uint32_t *start = (uint32_t *)(portOpen + 4);
  uint32_t *end = (uint32_t *)(portOpen + LIBPAD_SEARCH_SIZE);

  // The most called function in the range
  uint32_t dmaStr = 0;
  int best = 0;
  for (uint32_t *p = start; p < end; p++) {
    uint32_t target = jumpTarget(p);
    if (!target || (target < portOpen) || (target >= (uint32_t)end))
      continue;
    int calls = 0;
    for (uint32_t *q = start; q < end; q++)
      if (jumpTarget(q) == target)
        calls++;
    if (calls > best) {
      best = calls;
      dmaStr = target;
    }
  }
  if (best < 3)
    return 0;
  padDmaStrAddr = dmaStr;

  // The lowest function after scePadGetDmaStr() that OSDSYS calls and that matches
  uint32_t found = 0;
  for (uint32_t *p = (uint32_t *)osd; p < (uint32_t *)(osd + 0x100000); p++) {
    uint32_t target = jumpTarget(p);
    if ((target <= dmaStr) || (target >= (uint32_t)end) || (target & 3) || (found && (target >= found)))
      continue;

    uint32_t *code = (uint32_t *)target;
    int a2Read = 0, a2Written = 0, callsDmaStr = 0;
    for (int i = 0; (i < LIBPAD_FUNC_MAX_INSNS) && (&code[i] < end); i++) {
      uint32_t insn = code[i];
      if (!a2Written && readsReg(insn, 6))
        a2Read = 1;
      if (writesReg(insn, 6))
        a2Written = 1;
      if (jumpTarget(&code[i]) == dmaStr)
        callsDmaStr = 1;
      if ((insn == 0x03e00008) || ((insn >> 26) == 0x02)) // jr ra or j: end of the function
        break;
    }
    if (a2Read && callsDmaStr)
      found = target;
  }
  return found;
}

static int hookPadPortOpen(int port, int slot, void *addr) {
  if ((port == 0) && (slot == 0))
    padBuf = (uint8_t *)((uint32_t)addr | 0x20000000); // Uncached
  return scePadPortOpen(port, slot, addr);
}

void patchPadPortOpen(uint8_t *osd) {
  static const struct {
    uint32_t *pattern;
    uint32_t *mask;
    uint32_t size;
    int newLayout; // libpad >= 1.6
  } patterns[] = {
      {patternPadPortOpen0, patternPadPortOpen0_mask, sizeof(patternPadPortOpen0), 1},
      {patternPadPortOpen1, patternPadPortOpen1_mask, sizeof(patternPadPortOpen1), 1},
      {patternPadPortOpen2, patternPadPortOpen2_mask, sizeof(patternPadPortOpen2), 1},
      {patternPadPortOpen3, patternPadPortOpen3_mask, sizeof(patternPadPortOpen3), 0},
  };

  uint8_t *func = NULL;
  int i;
  for (i = 0; i < (int)(sizeof(patterns) / sizeof(patterns[0])); i++) {
    func = findPatternWithMask(osd, 0x100000, (uint8_t *)patterns[i].pattern, (uint8_t *)patterns[i].mask, patterns[i].size);
    if (func)
      break;
  }
  if (!func)
    return;

  if (patterns[i].newLayout) {
    posState = 112;
    posButtons = 2;
  } else {
    posState = 4;
    posButtons = 10;
  }
  scePadPortOpen = (void *)func;
  padPortOpenAddr = (uint32_t)func;
  redirectCalls(osd, (uint32_t)func, hookPadPortOpen);

  if ((padReadAddr = findPadRead(osd, (uint32_t)func))) {
    scePadRead = (void *)padReadAddr;
    padReadRedirects = redirectCalls(osd, padReadAddr, hookPadRead);
  }

  // Circle confirms on Japanese consoles, so Cross goes back there
  int japanese = (settings.region == OSD_REGION_JAP) || ((settings.region == OSD_REGION_DEFAULT) && (settings.romver[4] == 'J'));
  backMask = japanese ? PADB_CROSS : PADB_CIRCLE;
}

uint16_t padNewPresses(void) {
  if (!padBuf || (padBuf[posState] != PAD_STATE_STABLE))
    return 0;

  // Buttons are active low in the buffer
  uint16_t buttons = ~(padBuf[posButtons] | (padBuf[posButtons + 1] << 8));
  uint16_t pressed = buttons & ~prevButtons;
  prevButtons = buttons;
  return pressed;
}

uint16_t padBackButtons(void) { return backMask; }
