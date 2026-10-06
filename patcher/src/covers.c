// Game covers (games_covers = 1): the cover of the selected game to the left of the Games submenu.
//
// OSDSYS draws its button icons with DrawIcon(), which fills a sprite structure (color, position, texture
// coordinates, alpha blending and texturing flags), selects the icons texture by its ID and submits the sprite.
// The covers use the same functions, found from DrawIcon() so no addresses are hardcoded:
//   - the sprite submit function, the jump at the end of DrawIcon()
//   - the function that selects a texture by ID, the first call in DrawIcon(), which jumps to the function
//     that selects a texture by its video memory address and size
//   - the function that loads a texture by ID, right before that one, which uploads the texture with
//     the libgraph functions sceGsSetDefLoadImage(), FlushCache(), sceGsSyncPath() and sceGsExecLoadImage()
//
// OSDSYS screen coordinates are 640x224 (640x256 on PAL), each line being two TV lines.
// OSDSYS puts the frame buffers at the start of the 4 MB video memory and allocates its textures after them,
// using a fraction of it, so the cover texture is placed at the end.
#include "covers.h"
#include "settings.h"
#include "covers_raw.h"
#include <kernel.h>
#include <stdint.h>
#include <string.h>

// OSDSYS sprite, as filled by DrawIcon(). Positions and texture coordinates are 12.4 fixed point
typedef struct {
  int32_t r, g, b, a; // 0x80 is 1.0
  int32_t x0, y0, u0, v0;
  int32_t x1, y1, u1, v1;
  int32_t z;
  int32_t abe; // Alpha blending
  int32_t tme; // Texturing
} OSDSprite;

static void (*spriteSubmit)(OSDSprite *sprite);
static void (*setTexture)(int tbp, int log2Width, int log2Height, int a3, int t0, int t1);
static void (*gsSetDefLoadImage)(void *loadImage, short dbp, short dbw, short dpsm, short x, short y, short w, short h);
static void (*gsSyncPath)(int mode, int timeout);
static int (*gsExecLoadImage)(void *loadImage, void *data);

uint32_t coverSpriteAddr, coverTextureAddr, coverLoadImageAddr, coverSyncPathAddr;
int coversReady = 0;

#define OP_J 0x02
#define OP_JAL 0x03
#define JUMP_TARGET(insn, pc) (((uint32_t)(pc) & 0xf0000000) | (((insn) & 0x03ffffff) << 2))

// Returns the target of the first instruction with the given opcode in size bytes from start, or 0
static uint32_t findJump(uint32_t start, int size, int op) {
  for (uint32_t pc = start; pc < start + size; pc += 4) {
    uint32_t insn = *(uint32_t *)pc;
    if ((insn >> 26) == op)
      return JUMP_TARGET(insn, pc);
  }
  return 0;
}

// Returns 1 if the function at start calls target in its first size bytes
static int callsFunction(uint32_t start, int size, uint32_t target) {
  for (uint32_t pc = start; pc < start + size; pc += 4) {
    uint32_t insn = *(uint32_t *)pc;
    if (((insn >> 26) == OP_JAL) && (JUMP_TARGET(insn, pc) == target))
      return 1;
  }
  return 0;
}

static int validCode(uint32_t addr) { return (addr >= 0x200000) && (addr < 0x300000) && !(addr & 3); }

void coversInit(uint32_t drawIcon) {
  coversReady = 0;
  if (!validCode(drawIcon))
    return;

  uint32_t textureByID = findJump(drawIcon, 0x200, OP_JAL);
  coverSpriteAddr = findJump(drawIcon, 0x200, OP_J);
  if (!validCode(textureByID) || !validCode(coverSpriteAddr))
    return;
  coverTextureAddr = findJump(textureByID, 0x100, OP_J);
  if (!validCode(coverTextureAddr))
    return;

  // The texture loading function ends with the four libgraph calls, the last calls before the function above
  uint32_t calls[4];
  int found = 0;
  for (uint32_t pc = coverTextureAddr - 4; (pc > coverTextureAddr - 0x100) && (found < 4); pc -= 4) {
    uint32_t insn = *(uint32_t *)pc;
    if ((insn >> 26) == OP_JAL)
      calls[found++] = JUMP_TARGET(insn, pc);
  }
  if (found < 4)
    return;
  // calls[0]: sceGsExecLoadImage(), calls[1]: sceGsSyncPath(), calls[2]: FlushCache(), calls[3]: sceGsSetDefLoadImage()
  coverLoadImageAddr = calls[0];
  coverSyncPathAddr = calls[1];
  // The texture selection function also waits with sceGsSyncPath()
  if (!validCode(calls[0]) || !validCode(calls[1]) || !validCode(calls[3]) || !callsFunction(coverTextureAddr, 0x120, coverSyncPathAddr))
    return;

  spriteSubmit = (void *)coverSpriteAddr;
  setTexture = (void *)coverTextureAddr;
  gsExecLoadImage = (void *)calls[0];
  gsSyncPath = (void *)calls[1];
  gsSetDefLoadImage = (void *)calls[3];
  coversReady = 1;
}

// Cover texture: 128x256 PSMCT32 at the end of the video memory (in words, 64 words per block),
// holding a cover of up to 128x180 (covers_raw.h)
#define COVER_TEX_WIDTH_LOG2 7
#define COVER_TEX_HEIGHT_LOG2 8
#define COVER_TEX_WIDTH (1 << COVER_TEX_WIDTH_LOG2)
#define COVER_TEX_HEIGHT (1 << COVER_TEX_HEIGHT_LOG2)
#define COVER_TEX_ADDR (0x100000 - COVER_TEX_WIDTH * COVER_TEX_HEIGHT)

// Screen position: COVER_SCREEN_WIDTH wide, with the height that keeps the aspect ratio on a 4:3 TV
// (an OSDSYS line is two TV lines, and 640 pixels are about 7/15 as wide as 224 lines are tall),
// centered vertically on the menu in a panel as tall as a case cover
#define COVER_X 40
#define COVER_SCREEN_WIDTH 120
#define COVER_SCREEN_HEIGHT(width, height) (COVER_SCREEN_WIDTH * (height) * 7 / ((width) * 15))
#define COVER_PANEL_HEIGHT COVER_SCREEN_HEIGHT(COVER_RAW_WIDTH, COVER_RAW_COV_HEIGHT)
#define COVER_PANEL_BORDER 6

static int coverWidth = 0; // Size of the cover in the texture, 0 when there's none
static int coverHeight = 0;
static int coverVisible = 0;

// The cover is uploaded two lines at a time from a buffer on the stack, since the patcher's memory is full
#define UPLOAD_ROWS 2

void coversSetImage(int width, int height, void (*readRow)(int y, uint16_t *row)) {
  coverWidth = coverVisible = 0;
  if (!coversReady || (width > COVER_TEX_WIDTH) || (height > COVER_TEX_HEIGHT))
    return;

  uint32_t uploadBuffer[COVER_TEX_WIDTH * UPLOAD_ROWS] __attribute__((aligned(64)));
  uint16_t row[COVER_TEX_WIDTH];
  uint64_t loadImage[12] __attribute__((aligned(64))); // sceGsLoadImage
  for (int y = 0; y < height; y += UPLOAD_ROWS) {
    int rows = (height - y < UPLOAD_ROWS) ? (height - y) : UPLOAD_ROWS;
    for (int r = 0; r < rows; r++) {
      // PSMCT16 to PSMCT32, where 0x80 is opaque
      readRow(y + r, row);
      for (int x = 0; x < width; x++) {
        uint32_t p = row[x];
        uint32_t red = (p & 0x1f) << 3, green = ((p >> 5) & 0x1f) << 3, blue = ((p >> 10) & 0x1f) << 3;
        uploadBuffer[r * width + x] =
            ((p & 0x8000) ? 0x80000000 : 0) | ((blue | (blue >> 5)) << 16) | ((green | (green >> 5)) << 8) | red | (red >> 5);
      }
    }
    gsSetDefLoadImage(loadImage, COVER_TEX_ADDR / 64, COVER_TEX_WIDTH / 64, 0 /* PSMCT32 */, 0, y, width, rows);
    FlushCache(0);
    gsSyncPath(0, 0);
    gsExecLoadImage(loadImage, uploadBuffer);
    gsSyncPath(0, 0); // The buffer is reused for the next lines
  }
  coverWidth = width;
  coverHeight = height;
  coverVisible = 1;
}

void coversClear(void) { coverVisible = 0; }

void coversShow(void) { coverVisible = (coverWidth != 0); }

static void setRect(OSDSprite *sprite, int x0, int y0, int x1, int y1) {
  sprite->x0 = x0 << 4;
  sprite->y0 = y0 << 4;
  sprite->x1 = x1 << 4;
  sprite->y1 = y1 << 4;
}

void coversDraw(int alpha) {
  if (!coversReady || !coverVisible)
    return;

  // Dark panel behind the cover, only shown with a cover
  int panelY = settings.menuY - COVER_PANEL_HEIGHT / 2;
  OSDSprite panel = {0x10, 0x10, 0x18, alpha * 3 / 4};
  setRect(&panel, COVER_X - COVER_PANEL_BORDER, panelY - COVER_PANEL_BORDER / 2, COVER_X + COVER_SCREEN_WIDTH + COVER_PANEL_BORDER,
          panelY + COVER_PANEL_HEIGHT + COVER_PANEL_BORDER / 2);
  panel.abe = 1;
  spriteSubmit(&panel);

  // The cover, with texture coordinates offset by half a texel like DrawIcon()
  int height = COVER_SCREEN_HEIGHT(coverWidth, coverHeight);
  int y = settings.menuY - height / 2;
  OSDSprite cover = {0x80, 0x80, 0x80, alpha};
  setRect(&cover, COVER_X, y, COVER_X + COVER_SCREEN_WIDTH, y + height);
  cover.u0 = cover.v0 = 8;
  cover.u1 = (coverWidth << 4) + 8;
  cover.v1 = (coverHeight << 4) + 8;
  cover.abe = 1;
  cover.tme = 1;
  setTexture(COVER_TEX_ADDR, COVER_TEX_WIDTH_LOG2, COVER_TEX_HEIGHT_LOG2, 0, 1, 0);
  spriteSubmit(&cover);
}

// Text width function, the first call in DrawMenuItem(), which centers the text with it
static int (*textWidth)(const char *string) = NULL;

void coversInitText(uint32_t drawMenuItem) {
  uint32_t func = validCode(drawMenuItem) ? findJump(drawMenuItem, 0x40, OP_JAL) : 0;
  textWidth = validCode(func) ? (void *)func : NULL;
}

const char *coversFitText(const char *string) {
  static char fitted[NAME_LEN];
  // The menu is centered on menuX, between the panel and the right edge of the screen
  int room = settings.menuX - (COVER_X + COVER_SCREEN_WIDTH + COVER_PANEL_BORDER + 8);
  if (640 - 8 - settings.menuX < room)
    room = 640 - 8 - settings.menuX;
  room *= 2;
  if (!textWidth || (room <= 0) || (textWidth(string) <= room))
    return string;

  int len = strlen(string);
  if (len > NAME_LEN - 4)
    len = NAME_LEN - 4;
  memcpy(fitted, string, len);
  while (len > 1) {
    len--;
    while ((len > 1) && (fitted[len - 1] == ' '))
      len--;
    strcpy(&fitted[len], "...");
    if (textWidth(fitted) <= room)
      break;
  }
  return fitted;
}
