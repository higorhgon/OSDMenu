// Reads the controller from within OSDSYS for the games submenu.
// OSDSYS passes a DMA buffer to scePadPortOpen() that libpad keeps updated with the pad state,
// so the patcher hooks the calls to scePadPortOpen() to get the buffer and reads the buttons from it,
// the same way Open PS2 Loader's IGR does.
#include "patches_pad.h"
#include "patches_common.h"
#include "patterns_pad.h"
#include "settings.h"
#include <kernel.h>

#define PAD_STATE_STABLE 6

// Button bits in the second button byte (active low)
#define PAD_TRIANGLE 0x10
#define PAD_CIRCLE 0x20
#define PAD_CROSS 0x40

static int (*scePadPortOpen)(int port, int slot, void *addr) = NULL;
static volatile uint8_t *padBuf = NULL;
static int posState = 0;
static int posButtons = 0; // Second button byte
static uint8_t backMask = 0;
static uint8_t prevButtons = 0xff;

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
    posButtons = 3;
  } else {
    posState = 4;
    posButtons = 11;
  }
  scePadPortOpen = (void *)func;

  // Redirect all J/JAL calls to scePadPortOpen() and function pointers to it
  uint32_t call = 0x08000000 | (((uint32_t)func >> 2) & 0x03ffffff);
  uint32_t hookTarget = ((uint32_t)hookPadPortOpen >> 2) & 0x03ffffff;
  for (uint32_t *p = (uint32_t *)osd; p < (uint32_t *)(osd + 0x100000); p++) {
    if ((*p & 0xfbffffff) == call)
      *p = (*p & 0xfc000000) | hookTarget;
    else if (*p == (uint32_t)func)
      *p = (uint32_t)hookPadPortOpen;
  }

  // Circle confirms on Japanese consoles, so Cross goes back there
  int japanese = (settings.region == OSD_REGION_JAP) || ((settings.region == OSD_REGION_DEFAULT) && (settings.romver[4] == 'J'));
  backMask = PAD_TRIANGLE | (japanese ? PAD_CROSS : PAD_CIRCLE);
}

int padBackPressed(void) {
  if (!padBuf || (padBuf[posState] != PAD_STATE_STABLE))
    return 0;

  uint8_t buttons = padBuf[posButtons];
  // Buttons are active low: a bit going from 1 to 0 is a new press
  int pressed = (prevButtons & ~buttons) & backMask;
  prevButtons = buttons;
  return pressed != 0;
}
