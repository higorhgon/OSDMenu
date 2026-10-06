// Game covers (games_covers = 1), prototype: a panel and a test texture to the left of the games submenus.
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
#include <kernel.h>
#include <stdint.h>

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

// Cover texture: 128x256 PSMCT32 at the end of the video memory (in words, 64 words per block), holding
// a 128x180 picture
#define COVER_TEX_WIDTH_LOG2 7
#define COVER_TEX_HEIGHT_LOG2 8
#define COVER_TEX_WIDTH (1 << COVER_TEX_WIDTH_LOG2)
#define COVER_TEX_HEIGHT (1 << COVER_TEX_HEIGHT_LOG2)
#define COVER_TEX_ADDR (0x100000 - COVER_TEX_WIDTH * COVER_TEX_HEIGHT)
#define COVER_WIDTH 128
#define COVER_HEIGHT 180

// Screen position: the cover is 120x84 (168 TV lines, about the 2:3 of a PS2 case on a 4:3 TV),
// centered vertically on the menu
#define COVER_X 40
#define COVER_Y (settings.menuY - COVER_SCREEN_HEIGHT / 2)
#define COVER_SCREEN_WIDTH 120
#define COVER_SCREEN_HEIGHT 84
#define COVER_PANEL_BORDER 6

// The texture is uploaded two lines at a time from a buffer on the stack, since the patcher's memory is full
#define UPLOAD_ROWS 2

// Test picture: white border, red and green gradients and a blue checkerboard, to check orientation and scaling
static uint32_t testPixel(int x, int y) {
  if ((x >= COVER_WIDTH) || (y >= COVER_HEIGHT))
    return 0;
  if ((x < 3) || (x >= COVER_WIDTH - 3) || (y < 3) || (y >= COVER_HEIGHT - 3))
    return 0x80ffffff;
  uint32_t r = x * 2;
  uint32_t g = y * 255 / COVER_HEIGHT;
  uint32_t b = (((x >> 4) ^ (y >> 4)) & 1) ? 0xc0 : 0x30;
  return 0x80000000 | (b << 16) | (g << 8) | r; // ABGR, alpha 0x80 is opaque
}

static void uploadTexture(void) {
  uint32_t uploadBuffer[COVER_TEX_WIDTH * UPLOAD_ROWS] __attribute__((aligned(64)));
  uint64_t loadImage[12] __attribute__((aligned(64))); // sceGsLoadImage
  for (int y = 0; y < COVER_TEX_HEIGHT; y += UPLOAD_ROWS) {
    for (int row = 0; row < UPLOAD_ROWS; row++)
      for (int x = 0; x < COVER_TEX_WIDTH; x++)
        uploadBuffer[row * COVER_TEX_WIDTH + x] = testPixel(x, y + row);
    gsSetDefLoadImage(loadImage, COVER_TEX_ADDR / 64, COVER_TEX_WIDTH / 64, 0 /* PSMCT32 */, 0, y, COVER_TEX_WIDTH, UPLOAD_ROWS);
    FlushCache(0);
    gsSyncPath(0, 0);
    gsExecLoadImage(loadImage, uploadBuffer);
    gsSyncPath(0, 0); // The buffer is reused for the next lines
  }
}

static void setRect(OSDSprite *sprite, int x0, int y0, int x1, int y1) {
  sprite->x0 = x0 << 4;
  sprite->y0 = y0 << 4;
  sprite->x1 = x1 << 4;
  sprite->y1 = y1 << 4;
}

void coversDraw(int changed, int alpha) {
  if (!coversReady)
    return;
  if (changed)
    uploadTexture();

  // Dark panel behind the cover
  OSDSprite panel = {0x10, 0x10, 0x18, alpha * 3 / 4};
  setRect(&panel, COVER_X - COVER_PANEL_BORDER, COVER_Y - COVER_PANEL_BORDER / 2, COVER_X + COVER_SCREEN_WIDTH + COVER_PANEL_BORDER,
          COVER_Y + COVER_SCREEN_HEIGHT + COVER_PANEL_BORDER / 2);
  panel.abe = 1;
  spriteSubmit(&panel);

  // The cover, with texture coordinates offset by half a texel like DrawIcon()
  OSDSprite cover = {0x80, 0x80, 0x80, alpha};
  setRect(&cover, COVER_X, COVER_Y, COVER_X + COVER_SCREEN_WIDTH, COVER_Y + COVER_SCREEN_HEIGHT);
  cover.u0 = cover.v0 = 8;
  cover.u1 = (COVER_WIDTH << 4) + 8;
  cover.v1 = (COVER_HEIGHT << 4) + 8;
  cover.abe = 1;
  cover.tme = 1;
  setTexture(COVER_TEX_ADDR, COVER_TEX_WIDTH_LOG2, COVER_TEX_HEIGHT_LOG2, 0, 1, 0);
  spriteSubmit(&cover);
}
