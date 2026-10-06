#include "common.h"
#include "covers.h"
#include "covers_raw.h"
#include "dprintf.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Only the JPEG and PNG decoders, reading from memory
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "../third_party/stb_image.h"

//
// Game covers (games_covers): converts OPL's ART images of the games on MMCE devices into the format
// the patcher draws (see covers_raw.h), when the list is refreshed. OSDSYS can't decode images,
// and the patcher has little memory left, so the covers are scaled and converted here once.
//

#define COVER_SOURCE_MAX_SIZE (4 * 1024 * 1024)

// Returns the size of the file at path, or -1
static int fileSize(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int size = lseek(fd, 0, SEEK_END);
  close(fd);
  return size;
}

// Returns the source size recorded in the converted cover at path, or -1 if it's missing or invalid
static int convertedSourceSize(const char *path) {
  uint32_t header[COVER_RAW_HEADER_SIZE / 4];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int res = read(fd, header, sizeof(header));
  close(fd);
  if ((res != sizeof(header)) || (header[COVER_RAW_WORD_MAGIC] != COVER_RAW_MAGIC))
    return -1;
  return header[COVER_RAW_WORD_SOURCE];
}

// Reads the file at path into a new buffer
static uint8_t *readSource(const char *path, int *size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  uint8_t *data = NULL;
  *size = lseek(fd, 0, SEEK_END);
  if ((*size > 0) && (*size <= COVER_SOURCE_MAX_SIZE) && (lseek(fd, 0, SEEK_SET) == 0) && (data = malloc(*size))) {
    if (read(fd, data, *size) != *size) {
      free(data);
      data = NULL;
    }
  }
  close(fd);
  return data;
}

// Scales the RGBA image to width x height by averaging the source pixels each pixel covers,
// and packs it as PSMCT16. Pixels less than half opaque are transparent; covers are always opaque
static void scaleImage(const uint8_t *src, int srcWidth, int srcHeight, uint16_t *out, int width, int height, int opaque) {
  for (int y = 0; y < height; y++) {
    int y0 = y * srcHeight / height;
    int y1 = (y + 1) * srcHeight / height;
    if (y1 <= y0)
      y1 = y0 + 1;
    for (int x = 0; x < width; x++) {
      int x0 = x * srcWidth / width;
      int x1 = (x + 1) * srcWidth / width;
      if (x1 <= x0)
        x1 = x0 + 1;

      uint32_t sum[4] = {0, 0, 0, 0};
      for (int sy = y0; sy < y1; sy++) {
        const uint8_t *p = &src[(sy * srcWidth + x0) * 4];
        for (int sx = x0; sx < x1; sx++, p += 4) {
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          sum[3] += p[3];
        }
      }
      uint32_t count = (x1 - x0) * (y1 - y0);
      uint16_t pixel = ((sum[0] / count) >> 3) | (((sum[1] / count) >> 3) << 5) | (((sum[2] / count) >> 3) << 10);
      if (opaque || ((sum[3] / count) >= 0x80))
        pixel |= 0x8000;
      out[y * width + x] = pixel;
    }
  }
}

// Converts the image at source into dest. Returns 0 on success
static int convertCover(const char *source, const char *dest, int ico) {
  int size;
  uint8_t *data = readSource(source, &size);
  if (!data)
    return -1;

  int srcWidth, srcHeight, components;
  uint8_t *image = stbi_load_from_memory(data, size, &srcWidth, &srcHeight, &components, 4);
  free(data);
  if (!image)
    return -2;

  int height = ico ? COVER_RAW_ICO_HEIGHT : COVER_RAW_COV_HEIGHT;
  int rawSize = COVER_RAW_HEADER_SIZE + COVER_RAW_WIDTH * height * 2;
  uint32_t *raw = malloc(rawSize);
  if (!raw) {
    stbi_image_free(image);
    return -3;
  }
  memset(raw, 0, COVER_RAW_HEADER_SIZE);
  raw[COVER_RAW_WORD_MAGIC] = COVER_RAW_MAGIC;
  raw[COVER_RAW_WORD_SIZE] = COVER_RAW_WIDTH | (height << 16);
  raw[COVER_RAW_WORD_SOURCE] = size;
  scaleImage(image, srcWidth, srcHeight, (uint16_t *)&raw[COVER_RAW_HEADER_SIZE / 4], COVER_RAW_WIDTH, height, !ico);
  stbi_image_free(image);

  int res = -4;
  int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    res = (write(fd, raw, rawSize) == rawSize) ? 0 : -5;
    close(fd);
  }
  free(raw);
  return res;
}

// Finds the ART image of a game: <ART>/<ISO name><suffix>.jpg/png, then <ART>/<title ID><suffix>.jpg/png.
// Returns its size, with its path in path, or -1
static int findSource(const char *device, const char *isoName, const char *id, const char *suffix, char *path, size_t pathSize) {
  static const char *extensions[] = {".jpg", ".png"};
  const char *names[] = {isoName, id};
  for (int n = 0; n < 2; n++) {
    if (!names[n] || !names[n][0])
      continue;
    for (int e = 0; e < 2; e++) {
      snprintf(path, pathSize, "%s/ART/%s%s%s", device, names[n], suffix, extensions[e]);
      int size = fileSize(path);
      if (size > 0)
        return size;
    }
  }
  return -1;
}

void convertGameCovers(const CoverGame *games, int count, int ico) {
  const char *suffix = ico ? "_ICO" : "_COV";
  const char *rawSuffix = ico ? COVER_RAW_ICO_SUFFIX : COVER_RAW_COV_SUFFIX;
  int converted = 0, found = 0;
  char lastDevice[16] = "";

  for (int i = 0; i < count; i++) {
    // Only MMCE: the patcher reads the covers with gamescan.irx's mmceman
    const char *path = games[i].path;
    const char *colon = strchr(path, ':');
    if (strncmp(path, "mmce", 4) || !colon || ((colon - path + 1) >= (int)sizeof(lastDevice)))
      continue;
    char device[16];
    memcpy(device, path, colon - path + 1);
    device[colon - path + 1] = '\0';

    // ISO name without the extension
    char isoName[128];
    const char *slash = strrchr(path, '/');
    snprintf(isoName, sizeof(isoName), "%s", slash ? slash + 1 : colon + 1);
    char *ext = strrchr(isoName, '.');
    if (ext)
      *ext = '\0';

    char source[256];
    int sourceSize = findSource(device, isoName, games[i].id, suffix, source, sizeof(source));
    if (sourceSize < 0)
      continue;
    found++;

    if (strcmp(device, lastDevice)) {
      if (!lastDevice[0])
        msg("Converting covers...\n");
      char dir[32];
      snprintf(dir, sizeof(dir), "%s/ART", device);
      mkdir(dir, 0777);
      snprintf(dir, sizeof(dir), "%s/" COVER_RAW_DIR, device);
      mkdir(dir, 0777);
      strcpy(lastDevice, device);
    }

    // Converted again only when the source image changes
    char dest[256];
    snprintf(dest, sizeof(dest), "%s/" COVER_RAW_DIR "/%s%s", device, games[i].name, rawSuffix);
    if (convertedSourceSize(dest) == sourceSize)
      continue;
    int res = convertCover(source, dest, ico);
    if (res)
      DPRINTF("Covers: failed to convert %s: %d\n", source, res);
    else
      converted++;
  }
  if (found)
    msg("Covers: %d found, %d converted\n", found, converted);
}
