#include "common.h"
#include "covers.h"
#include "covers_raw.h"
#include "dprintf.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

// Only the JPEG and PNG decoders, reading from memory
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
// No thread-local variables: they're emulated through __emutls_get_address(), which hangs in the launcher
#define STBI_NO_THREAD_LOCALS
#include "../third_party/stb_image.h"

//
// Game covers (games_covers): converts OPL's ART images of the games on MMCE devices into the format
// the patcher draws (see covers_raw.h), when the list is refreshed. OSDSYS can't decode images,
// and the patcher has little memory left, so the covers are scaled and converted here once.
//

#define COVER_SOURCE_MAX_SIZE (4 * 1024 * 1024)

// Returns the source size recorded in the converted cover at path, or -1 if it's missing, invalid or
// converted by another version
static int convertedSourceSize(const char *path) {
  uint32_t header[COVER_RAW_HEADER_SIZE / 4];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int res = read(fd, header, sizeof(header));
  close(fd);
  if ((res != sizeof(header)) || (header[COVER_RAW_WORD_MAGIC] != COVER_RAW_MAGIC) || (header[COVER_RAW_WORD_VERSION] != COVER_RAW_VERSION))
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

// Images with more pixels than this are skipped, since decoding them takes too long and too much memory
#define COVER_SOURCE_MAX_PIXELS (1024 * 1024)

// Milliseconds since the first call, to show how long each step takes
static uint32_t elapsedMs(void) { return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC); }

// Converts the image at source into dest, showing each step so a step that hangs can be seen on screen.
// Returns 0 on success
static int convertCover(const char *source, const char *dest, int ico) {
  uint32_t start = elapsedMs();
  msg("read ");
  int size;
  uint8_t *data = readSource(source, &size);
  if (!data)
    return -1;

  int srcWidth, srcHeight, components;
  if (!stbi_info_from_memory(data, size, &srcWidth, &srcHeight, &components) || ((uint64_t)srcWidth * srcHeight > COVER_SOURCE_MAX_PIXELS)) {
    free(data);
    return -6;
  }
  msg("%dx%d decode ", srcWidth, srcHeight);
  uint8_t *image = stbi_load_from_memory(data, size, &srcWidth, &srcHeight, &components, 4);
  free(data);
  if (!image)
    return -2;

  msg("scale ");
  int width = COVER_RAW_ICO_SIZE;
  int height = COVER_RAW_ICO_SIZE;
  if (!ico) {
    // The cover's own proportions, as large as fits: a PS2 case fills the height, a PS1 one the width
    if ((uint64_t)srcHeight * COVER_RAW_WIDTH <= (uint64_t)srcWidth * COVER_RAW_COV_HEIGHT) {
      width = COVER_RAW_WIDTH;
      height = (srcHeight * COVER_RAW_WIDTH + srcWidth / 2) / srcWidth;
    } else {
      height = COVER_RAW_COV_HEIGHT;
      width = (srcWidth * COVER_RAW_COV_HEIGHT + srcHeight / 2) / srcHeight;
    }
    // The patcher reads two pixels at a time
    width &= ~1;
    if (width < 2)
      width = 2;
    if (height < 1)
      height = 1;
  }
  int rawSize = COVER_RAW_HEADER_SIZE + width * height * 2;
  uint32_t *raw = malloc(rawSize);
  if (!raw) {
    stbi_image_free(image);
    return -3;
  }
  memset(raw, 0, COVER_RAW_HEADER_SIZE);
  raw[COVER_RAW_WORD_MAGIC] = COVER_RAW_MAGIC;
  raw[COVER_RAW_WORD_SIZE] = width | (height << 16);
  raw[COVER_RAW_WORD_SOURCE] = size;
  raw[COVER_RAW_WORD_VERSION] = COVER_RAW_VERSION;
  scaleImage(image, srcWidth, srcHeight, (uint16_t *)&raw[COVER_RAW_HEADER_SIZE / 4], width, height, !ico);
  stbi_image_free(image);

  msg("write ");
  int res = -4;
  int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    res = (write(fd, raw, rawSize) == rawSize) ? 0 : -5;
    close(fd);
  }
  free(raw);
  if (!res)
    msg("ok %lu ms\n", (unsigned long)(elapsedMs() - start));
  return res;
}

// ART images of one device, listed once instead of trying to open every possible name
typedef struct {
  char name[96];
  int size;
} ArtFile;
#define ART_MAX_FILES 1024

// Returns 1 if name contains part, ignoring case
static int containsNoCase(const char *name, const char *part) {
  size_t len = strlen(part);
  for (; *name; name++)
    if (!strncasecmp(name, part, len))
      return 1;
  return 0;
}

// Lists the files in <device>/ART whose name has suffix (like "_COV."), returns their number
static int listArt(const char *device, const char *suffix, ArtFile **files) {
  *files = NULL;
  char dir[32];
  snprintf(dir, sizeof(dir), "%s/ART", device);
  int dfd = fileXioDopen(dir);
  if (dfd < 0)
    return 0;

  int count = 0, max = 0;
  iox_dirent_t dirent;
  while ((fileXioDread(dfd, &dirent) > 0) && (count < ART_MAX_FILES)) {
    if (FIO_S_ISDIR(dirent.stat.mode) || !containsNoCase(dirent.name, suffix) || (strlen(dirent.name) >= sizeof((*files)->name)))
      continue;
    if (count == max) {
      max += 64;
      ArtFile *grown = realloc(*files, max * sizeof(ArtFile));
      if (!grown)
        break;
      *files = grown;
    }
    strcpy((*files)[count].name, dirent.name);
    (*files)[count].size = dirent.stat.size;
    count++;
  }
  fileXioDclose(dfd);
  return count;
}

// Finds the ART image of a game: <title ID><suffix>.jpg/png (OPL's naming), then <artName><suffix>.jpg/png
// (the ISO name, or the PS1 game folder like RiptOPL).
// Returns its size, with its path in path, or -1
static int findSource(const ArtFile *files, int fileCount, const char *device, const char *artName, const char *id, const char *suffix,
                      char *path, size_t pathSize) {
  static const char *extensions[] = {".jpg", ".png"};
  const char *names[] = {id, artName};
  for (int n = 0; n < 2; n++) {
    if (!names[n] || !names[n][0])
      continue;
    for (int e = 0; e < 2; e++) {
      char name[sizeof(files->name)];
      snprintf(name, sizeof(name), "%s%s%s", names[n], suffix, extensions[e]);
      for (int f = 0; f < fileCount; f++) {
        if (!strcasecmp(files[f].name, name) && (files[f].size > 0)) {
          snprintf(path, pathSize, "%s/ART/%s", device, files[f].name);
          return files[f].size;
        }
      }
    }
  }
  return -1;
}

// Device ("mmce0:") of path in device, or 0 if it's not an MMCE path
static int mmceDevice(const char *path, char *device, size_t size) {
  const char *colon = strchr(path, ':');
  if (strncmp(path, "mmce", 4) || !colon || ((size_t)(colon - path + 1) >= size))
    return 0;
  memcpy(device, path, colon - path + 1);
  device[colon - path + 1] = '\0';
  return 1;
}

void convertGameCovers(const CoverGame *games, int count, int ico) {
  const char *suffix = ico ? "_ICO" : "_COV";
  const char *rawSuffix = ico ? COVER_RAW_ICO_SUFFIX : COVER_RAW_COV_SUFFIX;
  int converted = 0, found = 0, failed = 0, done = 0;
  char listedDevice[16] = "";
  ArtFile *files = NULL;
  int fileCount = 0;
  uint32_t start = elapsedMs();

  // Games with a cover
  int total = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < count; i++) {
      // Only MMCE: the patcher reads the covers with gamescan.irx's mmceman
      char device[16];
      if (!mmceDevice(games[i].path, device, sizeof(device)))
        continue;
      if (strcmp(device, listedDevice)) {
        free(files);
        msg("Covers: listing %s/ART... ", device);
        fileCount = listArt(device, ico ? "_ICO." : "_COV.", &files);
        msg("%d images\n", fileCount);
        strcpy(listedDevice, device);
        if (pass && fileCount) {
          char dir[32];
          snprintf(dir, sizeof(dir), "%s/" COVER_RAW_DIR, device);
          msg("Covers: creating %s\n", dir);
          mkdir(dir, 0777);
        }
      }

      char source[256];
      int sourceSize = findSource(files, fileCount, device, games[i].artName, games[i].id, suffix, source, sizeof(source));
      if (sourceSize < 0)
        continue;
      if (!pass) {
        total++;
        continue;
      }
      found++;

      // Named after the title ID (the game name without one), converted again only when the source image changes
      char dest[256];
      snprintf(dest, sizeof(dest), "%s/" COVER_RAW_DIR "/%s%s", device, games[i].id[0] ? games[i].id : games[i].name, rawSuffix);
      msg("[%d/%d] %.40s: ", ++done, total, games[i].name);
      if (convertedSourceSize(dest) == sourceSize) {
        msg("up to date\n");
        continue;
      }
      int res = convertCover(source, dest, ico);
      if (res) {
        msg("failed (%d)\n", res);
        failed++;
      } else
        converted++;
    }
    listedDevice[0] = '\0'; // List again for the second pass
  }
  free(files);
  msg("Covers: %d found, %d converted, %d failed in %lu s\n", found, converted, failed, (unsigned long)((elapsedMs() - start) / 1000));
}
