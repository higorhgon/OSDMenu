// Reads the controller from within OSDSYS for the games submenu.
// OSDSYS passes a DMA buffer to scePadPortOpen() that libpad keeps updated with the pad state,
// so the patcher hooks the calls to scePadPortOpen() to get the buffer and reads the buttons from it,
// the same way Open PS2 Loader's IGR does.
// It also hooks scePadRead() to hide Triangle from OSDSYS while a submenu is shown, since OSDSYS
// would open the Version screen with it.
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
uint32_t padReadAddr = 0; // For games_button_debug

// Buttons are in bytes 2 and 3 of the data (active low), Triangle is bit 4 of byte 3
static int hookPadRead(int port, int slot, unsigned char *rdata) {
  int ret = scePadRead(port, slot, rdata);
  if (hideTriangle && (ret >= 4) && (((uint32_t)rdata & 0x1fffffff) >= 0x100000) && (((uint32_t)rdata & 0x1fffffff) < 0x2000000))
    rdata[3] |= (PADB_TRIANGLE >> 8);
  return ret;
}

void padHideTriangle(int hide) { hideTriangle = hide; }

// Redirects all J/JAL calls to func and function pointers to it to hook
static void redirectCalls(uint8_t *osd, uint32_t func, void *hook) {
  uint32_t call = 0x08000000 | ((func >> 2) & 0x03ffffff);
  uint32_t hookTarget = ((uint32_t)hook >> 2) & 0x03ffffff;
  for (uint32_t *p = (uint32_t *)osd; p < (uint32_t *)(osd + 0x100000); p++) {
    if ((*p & 0xfbffffff) == call)
      *p = (*p & 0xfc000000) | hookTarget;
    else if (*p == func)
      *p = (uint32_t)hook;
  }
}

// Returns the J/JAL target of insn, or 0
static uint32_t jumpTarget(uint32_t *insn) {
  if ((*insn & 0xfc000000) != 0x0c000000)
    return 0;
  return (((uint32_t)insn + 4) & 0xf0000000) | ((*insn & 0x03ffffff) << 2);
}

// Finds scePadRead() among the libpad functions that follow scePadPortOpen():
// libpad links them in the same order (seen in HDD-OSD 1.10: PortOpen, PortClose, PortClose2, GetDmaStr,
// GetFrameCount, Read, GetState...). scePadGetDmaStr() is the function they call the most, and scePadRead()
// is the first function that calls it after using its third argument (a2, the data buffer).
// Returns 0 if not found
#define LIBPAD_SEARCH_SIZE 0x800
static uint32_t findPadRead(uint32_t portOpen) {
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

  // Functions start with "addiu sp, sp, -N"
  for (uint32_t *p = start; p < end; p++) {
    if (((*p & 0xffff8000) != 0x27bd8000) || ((uint32_t)p <= dmaStr))
      continue;
    // Looks for a register instruction using a2 up to the call to scePadGetDmaStr() and its delay slot
    int usesA2 = 0;
    for (int i = 1; (i < 12) && (p + i + 1 < end); i++) {
      uint32_t insn = p[i];
      if ((insn >> 26) == 0x02)
        break; // j: not this function
      if (((insn >> 26) == 0) && ((((insn >> 21) & 0x1f) == 6) || (((insn >> 16) & 0x1f) == 6)))
        usesA2 = 1;
      if (jumpTarget(&p[i]) == dmaStr) {
        uint32_t delay = p[i + 1];
        if (((delay >> 26) == 0) && ((((delay >> 21) & 0x1f) == 6) || (((delay >> 16) & 0x1f) == 6)))
          usesA2 = 1;
        if (usesA2)
          return (uint32_t)p;
        break;
      }
    }
  }
  return 0;
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
  redirectCalls(osd, (uint32_t)func, hookPadPortOpen);

  if ((padReadAddr = findPadRead((uint32_t)func))) {
    scePadRead = (void *)padReadAddr;
    redirectCalls(osd, padReadAddr, hookPadRead);
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
