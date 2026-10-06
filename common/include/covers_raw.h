#ifndef _COVERS_RAW_H_
#define _COVERS_RAW_H_

// Game covers (games_covers): the launcher converts OPL's ART images when the list is refreshed into
// <device>:/ART/OSDHUB/<game name><COVER_RAW_*_SUFFIX>, which gamescan.irx reads for the patcher.
//
// A converted cover is a COVER_RAW_HEADER_SIZE-byte header followed by width x height 16-bit pixels
// in the GS PSMCT16 format (bits 0-4 red, 5-9 green, 10-14 blue, 15 opaque), little endian, row by row
#define COVER_RAW_MAGIC 0x56434f48 // "HOCV"
#define COVER_RAW_HEADER_SIZE 16
#define COVER_RAW_DIR "ART/OSDHUB"
#define COVER_RAW_COV_SUFFIX "_COV.RAW" // From <name>_COV.jpg/png: the case cover
#define COVER_RAW_ICO_SUFFIX "_ICO.RAW" // From <name>_ICO.jpg/png: the disc

// Converted sizes: covers keep the 0.71 aspect ratio of OPL's covers, discs are square
#define COVER_RAW_WIDTH 128
#define COVER_RAW_COV_HEIGHT 180
#define COVER_RAW_ICO_HEIGHT 128
#define COVER_RAW_MAX_SIZE (COVER_RAW_HEADER_SIZE + COVER_RAW_WIDTH * COVER_RAW_COV_HEIGHT * 2)

// Header words: magic, width | height << 16, size of the source image (to convert it again when it changes), reserved
#define COVER_RAW_WORD_MAGIC 0
#define COVER_RAW_WORD_SIZE 1
#define COVER_RAW_WORD_SOURCE 2

#endif
