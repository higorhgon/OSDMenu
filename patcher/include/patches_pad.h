#ifndef _PATCHES_PAD_H_
#define _PATCHES_PAD_H_
#include <stdint.h>

// Hooks OSDSYS calls to scePadPortOpen() to get the pad buffer of the first controller
void patchPadPortOpen(uint8_t *osd);

// Returns 1 once when the "back" button (Circle, or Cross on Japanese consoles) or Triangle is pressed
int padBackPressed(void);

#endif
