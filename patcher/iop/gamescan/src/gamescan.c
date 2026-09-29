// gamescan.irx: experimental live games scan for the OSDMenu games submenu (games_live_scan)
//
// Loaded by the patcher before OSDSYS starts. Polls a LiveScanShared structure
// that the patcher writes directly into IOP RAM, scans the CD/DVD folders of
// MMCE devices when requested and writes GAMES_CACHE_PATH in the same format
// as the launcher's games handler, so launching games keeps working unchanged.
#include "irx_imports.h"
#include "livescan.h"
#include <iox_stat.h>

#define MODNAME "gamescan"
IRX_ID(MODNAME, 1, 0);

#define GAME_PATH_LEN 160
#define GAME_ID_LEN 12 // "SLUS_202.12" + NUL

static LiveScanShared shared __attribute__((aligned(64)));
static char gamePaths[LIVESCAN_MAX_GAMES][GAME_PATH_LEN];
static char gameIDs[LIVESCAN_MAX_GAMES][GAME_ID_LEN];
static unsigned int gameCount;

static iox_dirent_t dirent __attribute__((aligned(64)));
static iox_dirent_t subDirent __attribute__((aligned(64)));
static unsigned char buf[4096];
static char line[LIVESCAN_NAME_LEN + GAME_PATH_LEN + 64];

static int isUpper(char c) { return (c >= 'A') && (c <= 'Z'); }
static int isDigit(char c) { return (c >= '0') && (c <= '9'); }

static int strCaseCmp(const char *a, const char *b) {
  while (*a && (tolower(*a) == tolower(*b))) {
    a++;
    b++;
  }
  return tolower(*a) - tolower(*b);
}

static int hasISOExtension(const char *name) {
  const char *ext = strrchr(name, '.');
  return ext && !strCaseCmp(ext, ".iso");
}

static unsigned int readLE32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24); }

// Looks for a title ID at s in the "SLUS_202.12"/"SLUS-202.12" or "SLUS-20212" form
static int parseGameID(const char *s, char *out) {
  for (int i = 0; i < 4; i++)
    if (!isUpper(s[i]))
      return 0;

  if (((s[4] == '_') || (s[4] == '-')) && isDigit(s[5]) && isDigit(s[6]) && isDigit(s[7]) && (s[8] == '.') && isDigit(s[9]) && isDigit(s[10])) {
    memcpy(out, s, 11);
    out[4] = '_';
    out[11] = '\0';
    return 1;
  }

  if ((s[4] == '-') && isDigit(s[5]) && isDigit(s[6]) && isDigit(s[7]) && isDigit(s[8]) && isDigit(s[9])) {
    memcpy(out, s, 4);
    out[4] = '_';
    memcpy(&out[5], &s[5], 3);
    out[8] = '.';
    memcpy(&out[9], &s[8], 2);
    out[11] = '\0';
    return 1;
  }
  return 0;
}

static int findGameID(const char *s, char *out) {
  for (; strlen(s) >= 10; s++)
    if (parseGameID(s, out))
      return 1;
  return 0;
}

// Reads the title ID from the BOOT2 line of SYSTEM.CNF inside the ISO,
// or from a title ID-like file name in the root directory
static int getISOGameID(const char *isoPath, char *out) {
  int found = 0;
  char fallback[GAME_ID_LEN] = {0};
  unsigned int cnfLBA = 0, cnfSize = 0;
  char name[64];

  int fd = iomanX_open(isoPath, FIO_O_RDONLY);
  if (fd < 0)
    return 0;

  if ((iomanX_lseek(fd, 16 * 2048, FIO_SEEK_SET) < 0) || (iomanX_read(fd, buf, 2048) != 2048) || (buf[0] != 1) ||
      strncmp((char *)&buf[1], "CD001", 5))
    goto out;

  unsigned int rootLBA = readLE32(&buf[156 + 2]);
  unsigned int rootSize = readLE32(&buf[156 + 10]);
  if (rootSize > sizeof(buf))
    rootSize = sizeof(buf);
  if ((iomanX_lseek(fd, rootLBA * 2048, FIO_SEEK_SET) < 0) || (iomanX_read(fd, buf, rootSize) != (int)rootSize))
    goto out;

  for (unsigned int off = 0; off < rootSize;) {
    unsigned char len = buf[off];
    if (len == 0) { // Records don't cross sector boundaries
      off = (off / 2048 + 1) * 2048;
      continue;
    }
    if ((len < 34) || (off + len > rootSize))
      break;

    unsigned int nameLen = buf[off + 32];
    if (nameLen >= sizeof(name))
      nameLen = sizeof(name) - 1;
    memcpy(name, &buf[off + 33], nameLen);
    name[nameLen] = '\0';

    if (!strncmp(name, "SYSTEM.CNF", 10)) {
      cnfLBA = readLE32(&buf[off + 2]);
      cnfSize = readLE32(&buf[off + 10]);
    } else if (!fallback[0])
      findGameID(name, fallback);
    off += len;
  }

  if (cnfLBA) {
    if (cnfSize > 1023)
      cnfSize = 1023;
    if ((iomanX_lseek(fd, cnfLBA * 2048, FIO_SEEK_SET) >= 0) && (iomanX_read(fd, buf, cnfSize) == (int)cnfSize)) {
      buf[cnfSize] = '\0';
      char *boot = strstr((char *)buf, "BOOT2");
      if (boot && (boot = strstr(boot, "cdrom0:"))) {
        boot += 7;
        while ((*boot == '\\') || (*boot == '/'))
          boot++;
        found = parseGameID(boot, out);
      }
    }
  }

  if (!found && fallback[0]) {
    strcpy(out, fallback);
    found = 1;
  }

out:
  iomanX_close(fd);
  return found;
}

// Copies src into out, dropping a leading OPL-style title ID prefix and a trailing ".iso"
static void makeDisplayName(const char *src, char *out) {
  if ((strlen(src) > 12) && isUpper(src[0]) && isUpper(src[1]) && isUpper(src[2]) && isUpper(src[3]) && (src[4] == '_') && isDigit(src[5]) &&
      isDigit(src[6]) && isDigit(src[7]) && (src[8] == '.') && isDigit(src[9]) && isDigit(src[10]) && (src[11] == '.'))
    src += 12;

  strncpy(out, src, LIVESCAN_NAME_LEN - 1);
  out[LIVESCAN_NAME_LEN - 1] = '\0';

  char *ext = strrchr(out, '.');
  if (ext && (ext != out) && !strCaseCmp(ext, ".iso"))
    *ext = '\0';
}

static void addGame(const char *dirPath, const char *isoName, const char *displayName) {
  if (gameCount >= LIVESCAN_MAX_GAMES)
    return;

  if ((strlen(dirPath) + strlen(isoName) + 2) > GAME_PATH_LEN)
    return;

  unsigned int i = gameCount;
  makeDisplayName(displayName, shared.names[i]);
  strcpy(gamePaths[i], dirPath);
  strcpy(&gamePaths[i][strlen(gamePaths[i])], "/");
  strcpy(&gamePaths[i][strlen(gamePaths[i])], isoName);

  gameIDs[i][0] = '\0';
  if (!findGameID(isoName, gameIDs[i]) && !findGameID(displayName, gameIDs[i]))
    getISOGameID(gamePaths[i], gameIDs[i]);
  gameCount++;
}

// Returns 1 if dirPath contains exactly one ISO, copying its name to isoName
static int hasExactlyOneISO(const char *dirPath, char *isoName) {
  int dfd = iomanX_dopen(dirPath);
  if (dfd < 0)
    return 0;

  int count = 0;
  while (iomanX_dread(dfd, &subDirent) > 0) {
    if (FIO_S_ISDIR(subDirent.stat.mode) || !hasISOExtension(subDirent.name))
      continue;
    if (++count > 1)
      break;
    strncpy(isoName, subDirent.name, 127);
    isoName[127] = '\0';
  }
  iomanX_dclose(dfd);
  return (count == 1);
}

static void scanFolder(const char *mountpoint, const char *folder) {
  char dirPath[GAME_PATH_LEN];
  char subDirPath[GAME_PATH_LEN];
  char isoName[128];
  if ((strlen(mountpoint) + strlen(folder) + 2) > sizeof(dirPath))
    return;
  strcpy(dirPath, mountpoint);
  strcpy(&dirPath[strlen(dirPath)], "/");
  strcpy(&dirPath[strlen(dirPath)], folder);

  int dfd = iomanX_dopen(dirPath);
  if (dfd < 0)
    return;

  while (iomanX_dread(dfd, &dirent) > 0) {
    if (dirent.name[0] == '.')
      continue;

    if (FIO_S_ISDIR(dirent.stat.mode)) {
      if ((strlen(dirPath) + strlen(dirent.name) + 2) > sizeof(subDirPath))
        continue;
      strcpy(subDirPath, dirPath);
      strcpy(&subDirPath[strlen(subDirPath)], "/");
      strcpy(&subDirPath[strlen(subDirPath)], dirent.name);
      if (hasExactlyOneISO(subDirPath, isoName))
        addGame(subDirPath, isoName, dirent.name);
      continue;
    }

    if (hasISOExtension(dirent.name))
      addGame(dirPath, dirent.name, dirent.name);
  }
  iomanX_dclose(dfd);
}

static void swapGames(unsigned int a, unsigned int b) {
  memcpy(line, shared.names[a], LIVESCAN_NAME_LEN);
  memcpy(shared.names[a], shared.names[b], LIVESCAN_NAME_LEN);
  memcpy(shared.names[b], line, LIVESCAN_NAME_LEN);

  memcpy(line, gamePaths[a], GAME_PATH_LEN);
  memcpy(gamePaths[a], gamePaths[b], GAME_PATH_LEN);
  memcpy(gamePaths[b], line, GAME_PATH_LEN);

  memcpy(line, gameIDs[a], GAME_ID_LEN);
  memcpy(gameIDs[a], gameIDs[b], GAME_ID_LEN);
  memcpy(gameIDs[b], line, GAME_ID_LEN);
}

static void appendStr(const char *str) { strcpy(&line[strlen(line)], str); }

// Writes the list in the launcher's GAMES_CACHE_PATH format
static int writeCache(void) {
  int fd = iomanX_open(shared.cachePath, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
  if (fd < 0)
    return fd;

  int res = 0;
  for (unsigned int i = 0; (i < gameCount) && !res; i++) {
    // Each part is NUL-terminated within its array, so line can't overflow
    line[0] = '\0';
    appendStr("game = ");
    appendStr(shared.names[i]);
    appendStr("\nbsd = mmce\ndvd = ");
    appendStr(gamePaths[i]);
    appendStr("\n");
    if (gameIDs[i][0]) {
      appendStr("id = ");
      appendStr(gameIDs[i]);
      appendStr("\n");
    }
    int len = strlen(line);
    if (iomanX_write(fd, line, len) != len)
      res = -5; // EIO
  }
  iomanX_close(fd);
  return res;
}

static void doScan(void) {
  char mountpoint[8];
  gameCount = 0;

  if (shared.devices & LIVESCAN_DEV_MMCE) {
    for (int slot = 0; slot < 2; slot++) {
      sprintf(mountpoint, "mmce%d:", slot);
      scanFolder(mountpoint, shared.cdFolder);
      scanFolder(mountpoint, shared.dvdFolder);
    }
  }

  // Insertion sort by name
  for (unsigned int i = 1; i < gameCount; i++)
    for (unsigned int j = i; (j > 0) && (strCaseCmp(shared.names[j - 1], shared.names[j]) > 0); j--)
      swapGames(j - 1, j);

  shared.count = gameCount;
  shared.result = writeCache();
}

static void scanThread(void *arg) {
  while (1) {
    shared.heartbeat++;
    if (shared.request) {
      shared.request = 0;
      shared.status = LIVESCAN_STATUS_BUSY;
      doScan();
      shared.status = LIVESCAN_STATUS_DONE;
    }
    DelayThread(50 * 1000);
  }
}

int _start(int argc, char *argv[]) {
  memset(&shared, 0, sizeof(shared));

  iop_thread_t thread;
  memset(&thread, 0, sizeof(thread));
  thread.attr = TH_C;
  thread.thread = scanThread;
  thread.priority = 100;
  thread.stacksize = 0x1000;

  int tid = CreateThread(&thread);
  if (tid < 0)
    return MODULE_NO_RESIDENT_END;
  StartThread(tid, NULL);

  // Write the magic last so the EE never finds a half-initialized structure
  shared.magic[1] = LIVESCAN_MAGIC1;
  shared.magic[2] = LIVESCAN_MAGIC2;
  shared.magic[3] = LIVESCAN_MAGIC3;
  shared.magic[0] = LIVESCAN_MAGIC0;
  return MODULE_RESIDENT_END;
}
