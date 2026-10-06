#ifndef _COVERS_H_
#define _COVERS_H_
#include <stdint.h>

// Game covers (games_covers = 1, prototype): draws a panel and a test texture to the left of the games submenus
// with OSDSYS's own sprite and texture functions, found from DrawIcon()

// Finds the OSDSYS functions used to draw the covers, from the address of DrawIcon()
void coversInit(uint32_t drawIcon);

// Draws the cover panel; called from the button panel, once per frame while a games submenu is shown.
// changed is set when the submenu was opened since the last call, so the texture is uploaded again,
// and alpha is the button panel's (0-0x80, it fades in)
void coversDraw(int changed, int alpha);

// Resolved addresses for games_button_debug: sprite submit, set texture, load image, sync path,
// and 1 when everything was found
extern uint32_t coverSpriteAddr, coverTextureAddr, coverLoadImageAddr, coverSyncPathAddr;
extern int coversReady;

#endif
