#ifndef _COVERS_RAW_H_
#define _COVERS_RAW_H_

// Game covers (games_covers): the launcher converts OPL's ART images (RiptOPL's for PS1 games run by Ember)
// when the list is refreshed into
// <device>:/ART/OSDHUB/<title ID><COVER_RAW_*_SUFFIX> (or <game name><COVER_RAW_*_SUFFIX> for games without an ID),
// which gamescan.irx reads for the patcher.
//
// A converted cover is a COVER_RAW_HEADER_SIZE-byte header followed by width x height 16-bit pixels
// in the GS PSMCT16 format (bits 0-4 red, 5-9 green, 10-14 blue, 15 opaque), little endian, row by row
#define COVER_RAW_MAGIC 0x56434f48 // "HOCV"
#define COVER_RAW_HEADER_SIZE 16
#define COVER_RAW_DIR "ART/OSDHUB"
#define COVER_RAW_COV_SUFFIX "_COV.RAW" // From <name>_COV.jpg/png: the case cover
#define COVER_RAW_ICO_SUFFIX "_ICO.RAW" // From <name>_ICO.jpg/png: the disc

// Converted sizes: covers keep their own proportions, as large as fits in COVER_RAW_WIDTH x COVER_RAW_COV_HEIGHT
// (128x180 for a PS2 case, about 128x128 for a PS1 one), with an even width, and discs are as big as
// OSD-XMB's selected game icon (72x72 on a 640x480 screen), drawn without scaling
#define COVER_RAW_WIDTH 128
#define COVER_RAW_COV_HEIGHT 180
#define COVER_RAW_ICO_SIZE 72
#define COVER_RAW_MAX_SIZE (COVER_RAW_HEADER_SIZE + COVER_RAW_WIDTH * COVER_RAW_COV_HEIGHT * 2)

// Header words: magic, width | height << 16, size of the source image (to convert it again when it changes),
// version of the conversion (to convert the covers again when it changes; 0 stretched every cover to 128x180)
#define COVER_RAW_WORD_MAGIC 0
#define COVER_RAW_WORD_SIZE 1
#define COVER_RAW_WORD_SOURCE 2
#define COVER_RAW_WORD_VERSION 3
#define COVER_RAW_VERSION 1

#endif
