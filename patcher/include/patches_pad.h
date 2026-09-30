#ifndef _PATCHES_PAD_H_
#define _PATCHES_PAD_H_
#include <stdint.h>

// Button bits returned by padNewPresses()
#define PADB_SELECT 0x0001
#define PADB_START 0x0008
#define PADB_UP 0x0010
#define PADB_RIGHT 0x0020
#define PADB_DOWN 0x0040
#define PADB_LEFT 0x0080
#define PADB_TRIANGLE 0x1000
#define PADB_CIRCLE 0x2000
#define PADB_CROSS 0x4000
#define PADB_SQUARE 0x8000

// Hooks OSDSYS calls to scePadPortOpen() to get the pad buffer of the first controller
void patchPadPortOpen(uint8_t *osd);

// Returns the buttons of the first controller pressed since the last call
uint16_t padNewPresses(void);

// Returns the button that goes back: Circle (Cross on Japanese consoles)
uint16_t padBackButtons(void);

// Hides Triangle from OSDSYS, which would open the Version screen, while hide is set
void padHideTriangle(int hide);

// Addresses of the libpad functions found, 0 if not found, the number of calls to scePadRead() that were
// redirected and the number of calls to the hook (shown by games_button_debug and the games_live_scan = 2 boot report)
extern uint32_t padReadAddr;
extern uint32_t padPortOpenAddr;
extern uint32_t padDmaStrAddr;
extern int padReadRedirects;
extern volatile uint32_t padReadCalls;

#endif
