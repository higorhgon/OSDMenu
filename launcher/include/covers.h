#ifndef _COVERS_H_
#define _COVERS_H_

// A game whose cover is converted: its display name, full path (on an MMCE device), title ID (empty if unknown)
// and the name its ART images may have besides the title ID (the ISO name without the extension, or the PS1 game folder)
typedef struct {
  const char *name;
  const char *path;
  const char *id;
  const char *artName;
} CoverGame;

// Converts the ART images of the games on MMCE devices into <device>:/ART/OSDHUB (see covers_raw.h):
// the disc images (_ICO) when ico is set, or the case covers (_COV)
void convertGameCovers(const CoverGame *games, int count, int ico);

#endif
