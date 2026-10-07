#ifndef _COVERS_H_
#define _COVERS_H_
#include <stdint.h>

// Game covers (games_covers = 1): draws the cover of the selected game to the left of the Games submenu
// with OSDSYS's own sprite and texture functions, found from DrawIcon()

// Finds the OSDSYS functions used to draw the covers, from the address of DrawIcon()
void coversInit(uint32_t drawIcon);

// Finds the text width function, from the address of DrawMenuItem()
void coversInitText(uint32_t drawMenuItem);

// Uploads a width x height PSMCT16 cover (covers_raw.h) to the video memory, reading it a row at a time
// with readRow(), and shows it
void coversSetImage(int width, int height, void (*readRow)(int y, uint16_t *row));

// Hides the cover, keeping it in the video memory
void coversClear(void);

// Shows the cover in the video memory again
void coversShow(void);

// Draws the cover; called from the button panel, once per frame while the Games submenu is shown,
// with the button panel's alpha (0-0x80, it fades in)
void coversDraw(int alpha);

// Returns string, or a copy shortened with "..." so that it fits between the cover and the right edge of the screen
const char *coversFitText(const char *string);

// Resolved addresses for games_button_debug: sprite submit, set texture, load image, sync path,
// and 1 when everything was found
extern uint32_t coverSpriteAddr, coverTextureAddr, coverLoadImageAddr, coverSyncPathAddr;
extern int coversReady;

#endif
