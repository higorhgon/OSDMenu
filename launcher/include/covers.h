#ifndef _COVERS_H_
#define _COVERS_H_

// A game whose cover is converted: its display name (the name of the converted cover),
// full ISO path and title ID (empty if unknown)
typedef struct {
  const char *name;
  const char *path;
  const char *id;
} CoverGame;

// Converts the ART images of the games on MMCE devices into <device>:/ART/OSDHUB (see covers_raw.h):
// the disc images (<name>_ICO) when ico is set, or the case covers (<name>_COV)
void convertGameCovers(const CoverGame *games, int count, int ico);

#endif
