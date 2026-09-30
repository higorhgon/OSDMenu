// gamescan.irx: experimental live games scan for the OSDMenu games submenu (games_live_scan)
//
// Loaded through OSDSYS by the patcher (see livescan.h). Polls a LiveScanShared structure
// that the patcher writes directly into IOP RAM, scans the CD/DVD folders of
// MMCE devices when requested and writes GAMES_CACHE_PATH in the same format
// as the launcher's games handler, so launching games keeps working unchanged.
// Also writes the diagnostics logs the patcher sends (games_live_scan = 2).
//
// OSDSYS's rom0:SIO2MAN is the older sio2man 1.2, where every SIO2 transfer goes through
// sio2man's thread, while mmceman drives the SIO2 directly (swapping sio2man's interrupt handler).
// So while mmceman runs, gamescan.irx holds sio2man's transfer lock, and mmceman is read into
// IOP RAM before, since the memory card can't be read while the lock is held.
#include "irx_imports.h"
#include "defaults.h"
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

static void *mmceBuffer = NULL; // mmceman read from the memory card, until it's loaded
static int mmceSize = 0;

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

// Returns 1 if dirPath contains a *.cue file
static int hasCueFile(const char *dirPath) {
  int dfd = iomanX_dopen(dirPath);
  if (dfd < 0)
    return 0;

  int found = 0;
  while (!found && (iomanX_dread(dfd, &subDirent) > 0)) {
    if (FIO_S_ISDIR(subDirent.stat.mode))
      continue;
    const char *ext = strrchr(subDirent.name, '.');
    found = ext && !strCaseCmp(ext, ".cue");
  }
  iomanX_dclose(dfd);
  return found;
}

// Scans <mountpoint>/EMBER/games for PS1 games like the launcher: subfolders with a *.cue file inside,
// only when <mountpoint>/EMBER/ember.elf exists. The folder is the game's name and path
static void scanEmberFolder(const char *mountpoint) {
  char path[GAME_PATH_LEN];
  sprintf(path, "%s/" PSX_EMBER_FOLDER "/" PSX_EMBER_ELF, mountpoint);
  int fd = iomanX_open(path, FIO_O_RDONLY);
  if (fd < 0)
    return;
  iomanX_close(fd);

  char gamesPath[GAME_PATH_LEN];
  sprintf(gamesPath, "%s/" PSX_EMBER_FOLDER "/games", mountpoint);
  int dfd = iomanX_dopen(gamesPath);
  if (dfd < 0)
    return;

  while ((gameCount < LIVESCAN_MAX_GAMES) && (iomanX_dread(dfd, &dirent) > 0)) {
    if (!FIO_S_ISDIR(dirent.stat.mode) || (dirent.name[0] == '.'))
      continue;
    if ((strlen(gamesPath) + strlen(dirent.name) + 2) > GAME_PATH_LEN)
      continue; // A truncated folder name wouldn't launch
    strcpy(path, gamesPath);
    strcpy(&path[strlen(path)], "/");
    strcpy(&path[strlen(path)], dirent.name);
    if (!hasCueFile(path))
      continue;

    strncpy(shared.names[gameCount], dirent.name, LIVESCAN_NAME_LEN - 1);
    shared.names[gameCount][LIVESCAN_NAME_LEN - 1] = '\0';
    strcpy(gamePaths[gameCount], path);
    gameIDs[gameCount][0] = '\0';
    gameCount++;
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

//
// Games cache
//
// "key = value" lines: an optional "sort" line, then for each game its "game" (name) line,
// "bsd"/"dvd"/"id" (PS2) or "psx" (PS1) lines and optional "played" and "fav" lines

// Reads the file at path into a new NUL-terminated buffer, freed with FreeSysMemory(), or returns NULL
static char *readFile(const char *path) {
  int fd = iomanX_open(path, FIO_O_RDONLY);
  if (fd < 0)
    return NULL;

  char *data = NULL;
  int size = iomanX_lseek(fd, 0, FIO_SEEK_END);
  if ((size >= 0) && (size <= 128 * 1024) && (iomanX_lseek(fd, 0, FIO_SEEK_SET) == 0) && (data = AllocSysMemory(ALLOC_FIRST, size + 1, NULL))) {
    if (iomanX_read(fd, data, size) == size)
      data[size] = '\0';
    else {
      FreeSysMemory(data);
      data = NULL;
    }
  }
  iomanX_close(fd);
  return data;
}

static int isSpace(char c) { return (c == ' ') || (c == '\t') || (c == '\r'); }

// Splits the next "key = value" line of the text at *pos in place. Returns 0 at the end of the text
static int nextCacheLine(char **pos, char **key, char **value) {
  while (**pos) {
    char *start = *pos;
    char *end = start;
    while (*end && (*end != '\n'))
      end++;
    *pos = *end ? end + 1 : end;
    *end = '\0';

    char *eq = strchr(start, '=');
    if (!eq)
      continue;
    char *k = eq;
    while ((k > start) && isSpace(k[-1]))
      k--;
    *k = '\0';
    char *v = eq + 1;
    while (isSpace(*v))
      v++;
    for (char *e = v + strlen(v); (e > v) && isSpace(e[-1]); e--)
      e[-1] = '\0';
    while (isSpace(*start))
      start++;
    *key = start;
    *value = v;
    return 1;
  }
  return 0;
}

static unsigned int parseUInt(const char *s) {
  unsigned int value = 0;
  for (; isDigit(*s); s++)
    value = value * 10 + (*s - '0');
  return value;
}

static const char *cachePathKey(void) { return (shared.kind == LIVESCAN_KIND_PSX) ? "psx" : "dvd"; }

static int isFavorite(unsigned int idx) { return (idx < LIVESCAN_MAX_GAMES) && (shared.fav[idx / 32] & (1u << (idx % 32))); }

// Buffered cache writer, since every write to the memory card is slow
static char outBuf[2048];
static int outLen;
static int outFd;
static int outRes;

static void outFlush(void) {
  if (outLen && !outRes && (iomanX_write(outFd, outBuf, outLen) != outLen))
    outRes = -5; // EIO
  outLen = 0;
}

static void outLine(const char *key, const char *value) {
  int len = strlen(key) + strlen(value) + 4;
  if (len > (int)sizeof(outBuf))
    return;
  if (outLen + len > (int)sizeof(outBuf))
    outFlush();
  sprintf(&outBuf[outLen], "%s = %s\n", key, value);
  outLen += len;
}

static int outOpen(void) {
  outLen = 0;
  outRes = 0;
  outFd = iomanX_open(shared.cachePath, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
  return outFd;
}

static int outClose(void) {
  outFlush();
  iomanX_close(outFd);
  return outRes;
}

// Writes the list in the launcher's cache format, keeping the sort order of the previous cache and
// the "played" counters and favorites of the games that are still there, which are also returned
// in shared.played and shared.fav
static int writeCache(void) {
  char sort[8] = "name";
  // Favorites toggled in the submenu and not saved yet replace the "fav" lines of the previous cache
  unsigned int favInput[LIVESCAN_MAX_GAMES / 32];
  int useFavInput = shared.favInput;
  memcpy(favInput, shared.fav, sizeof(favInput));
  memset(shared.played, 0, sizeof(shared.played));
  memset(shared.fav, 0, sizeof(shared.fav));

  char *old = readFile(shared.cachePath);
  if (old) {
    char *pos = old, *key, *value;
    int current = -1; // Game of the lines being read, by index in the new list
    int oldIdx = -1;  // Its index in the previous cache
    while (nextCacheLine(&pos, &key, &value)) {
      if (!strcmp(key, "sort")) {
        strncpy(sort, value, sizeof(sort) - 1);
        sort[sizeof(sort) - 1] = '\0';
      } else if (!strcmp(key, "game")) {
        current = -1;
        oldIdx++;
      } else if (!strcmp(key, cachePathKey())) {
        for (unsigned int i = 0; i < gameCount; i++)
          if (!strcmp(gamePaths[i], value))
            current = i;
        if ((current >= 0) && useFavInput && (oldIdx >= 0) && (oldIdx < LIVESCAN_MAX_GAMES) && (favInput[oldIdx / 32] & (1u << (oldIdx % 32))))
          shared.fav[current / 32] |= 1u << (current % 32);
      } else if ((current >= 0) && !strcmp(key, "played"))
        shared.played[current] = parseUInt(value);
      else if ((current >= 0) && !useFavInput && !strcmp(key, "fav") && parseUInt(value))
        shared.fav[current / 32] |= 1u << (current % 32);
    }
    FreeSysMemory(old);
  }

  if (outOpen() < 0)
    return outFd;
  outLine("sort", sort);
  char number[12];
  for (unsigned int i = 0; i < gameCount; i++) {
    outLine("game", shared.names[i]);
    if (shared.kind == LIVESCAN_KIND_PSX)
      outLine("psx", gamePaths[i]);
    else {
      outLine("bsd", "mmce");
      outLine("dvd", gamePaths[i]);
      if (gameIDs[i][0])
        outLine("id", gameIDs[i]);
    }
    if (shared.played[i]) {
      sprintf(number, "%u", shared.played[i]);
      outLine("played", number);
    }
    if (isFavorite(i))
      outLine("fav", "1");
  }
  return outClose();
}

// Rewrites the "fav" lines of the cache from shared.fav, by index of the games in the cache
static int saveFavorites(void) {
  char *cache = readFile(shared.cachePath);
  if (!cache)
    return -2; // ENOENT
  if (outOpen() < 0) {
    FreeSysMemory(cache);
    return outFd;
  }

  char *pos = cache, *key, *value;
  int current = -1;
  while (nextCacheLine(&pos, &key, &value)) {
    if (!strcmp(key, "fav"))
      continue;
    if (!strcmp(key, "game")) {
      if ((current >= 0) && isFavorite(current))
        outLine("fav", "1");
      current++;
    }
    outLine(key, value);
  }
  if ((current >= 0) && isFavorite(current))
    outLine("fav", "1");
  FreeSysMemory(cache);
  return outClose();
}

//
// SIO2 lock
//
// OSDSYS's sio2man 1.2 runs every transfer from its own thread: a driver asks for the SIO2 by setting
// its "transfer init" event flag bit (export 23 for the controller, 24 for the memory card),
// waits until sio2man's thread grants it, sends its transfers (export 25) and hands the SIO2 back with
// "transfer reset" (export 26). While gamescan.irx holds it, padman, mcserv, multitap_manager and rmman2
// wait for their turn in sio2man, so mmceman can drive the SIO2 alone.
// The IOP threadman doesn't implement SuspendThread() (it returns KE_ERROR), so the threads can't be paused.
// The memory card lock is used, since padman asks for the controller one every frame and two threads
// waiting for the same grant would both get it.
static void (*sio2Lock)(void) = NULL;
static void (*sio2Unlock)(void) = NULL;
static volatile int sio2Locked = 0;
static volatile unsigned int lockedTicks = 0;

// Finds sio2man's memory card transfer init (24) and transfer reset (26) in loadcore's library list
static int findSIO2Lock(void) {
  if (sio2Lock)
    return 0;
  lc_internals_t *lc = GetLoadcoreInternalData();
  iop_library_t *lib = lc ? lc->let_next : NULL;
  for (int n = 0; lib && (n < 128); n++, lib = lib->prev) {
    int match = 1;
    for (int i = 0; i < 8; i++)
      if (lib->name[i] != "sio2man"[i])
        match = 0;
    if (!match)
      continue;
    // Both interfaces have the lock functions at 24 and 26, but only 1.2 is expected under OSDSYS
    int count = 0;
    while ((count < 32) && lib->exports[count])
      count++;
    if (count <= 26)
      continue;
    shared.sio2Version = lib->version;
    sio2Lock = lib->exports[24];
    sio2Unlock = lib->exports[26];
    shared.sio2Lock = (unsigned int)sio2Lock;
    shared.sio2Unlock = (unsigned int)sio2Unlock;
    return 0;
  }
  return LIVESCAN_ERR_NO_LOCK;
}

static void lockSIO2(void) {
  sio2Lock();
  lockedTicks = 0;
  sio2Locked = 1;
}

// Hands the SIO2 back once: a second transfer reset would end the next driver's turn
static void unlockSIO2(void) {
  int state;
  CpuSuspendIntr(&state);
  int locked = sio2Locked;
  sio2Locked = 0;
  CpuResumeIntr(state);
  if (locked)
    sio2Unlock();
}

// Hands the SIO2 back if the scan holds it for more than 20 seconds, so a stuck scan doesn't leave
// the controller and the memory cards stopped
static void watchdogThread(void *arg) {
  while (1) {
    DelayThread(500 * 1000);
    if (sio2Locked && (++lockedTicks >= 40)) {
      shared.watchdogResumed = 1;
      unlockSIO2();
    }
  }
}

//
// Threads, for the diagnostics log
//
// Threads are found by scanning IOP RAM for the thread control block tag (0x7f01 in the first halfword,
// the ID in the second), turning each address into a thread ID the same way threadman does
// and checking it with ReferThreadStatus().
// In the ROM threadman (checked on ROM 2.30), the tag is 8 bytes into the control block, after the
// queue links, and thread IDs point to the start of the block. PS2SDK's threadman puts the tag first,
// so that's tried when the ROM layout doesn't match.
#define TAG_THREAD 0x7f01
#define THREAD_HANDLE(addr, id) ((int)(((addr) << 5) | (((id) & 0x3f) << 1) | 1))

// Name prefixes of the modules whose threads use the SIO2: controller, memory card, multitap and remote
static const char *sio2Modules[] = {"pad", "xpad", "mc", "xmc", "multitap", "mtap", "xmtap", "rmman", "dbcman"};
#define SIO2_MODULES (sizeof(sio2Modules) / sizeof(sio2Modules[0]))

#define MAX_SIO2_RANGES 16
static unsigned int sio2Start[MAX_SIO2_RANGES], sio2End[MAX_SIO2_RANGES];
static int sio2Ranges;

// Finds the code of the loaded modules that use the SIO2
static void findSIO2Modules(void) {
  sio2Ranges = 0;
  lc_internals_t *lc = GetLoadcoreInternalData();
  ModuleInfo_t *module = lc ? lc->image_info : NULL;
  for (int n = 0; module && (n < 128) && (sio2Ranges < MAX_SIO2_RANGES); n++, module = module->next) {
    if (!module->name)
      continue;
    for (unsigned int i = 0; i < SIO2_MODULES; i++) {
      if (!strncmp(module->name, sio2Modules[i], strlen(sio2Modules[i]))) {
        sio2Start[sio2Ranges] = module->text_start & 0x1fffff;
        sio2End[sio2Ranges] = sio2Start[sio2Ranges] + module->text_size;
        sio2Ranges++;
        break;
      }
    }
  }
}

static int isSIO2Code(unsigned int addr) {
  addr &= 0x1fffff;
  for (int i = 0; i < sio2Ranges; i++)
    if ((addr >= sio2Start[i]) && (addr < sio2End[i]))
      return 1;
  return 0;
}

// Lists the threads in shared.threads, along with the SIO2 lock functions and interrupt handler
static void listThreads(void) {
  iop_thread_info_t info;
  findSIO2Modules();
  findSIO2Lock();
  intrman_internals_t *intr = GetIntrmanInternalData();
  if (intr && intr->interrupt_handler_table) {
    shared.sio2Intr = (unsigned int)intr->interrupt_handler_table[IOP_IRQ_SIO2].handler;
    shared.sio2IntrArg = (unsigned int)intr->interrupt_handler_table[IOP_IRQ_SIO2].userdata;
  }
  shared.threadCount = 0;

  for (unsigned int addr = 0x808; addr < LIVESCAN_IOP_RAM_SIZE; addr += 4) {
    // Thread IDs stored here could look like a tag
    if ((addr >= (unsigned int)&shared) && (addr < (unsigned int)&shared + sizeof(shared)))
      continue;
    unsigned int word = *(volatile unsigned int *)addr;
    if ((word & 0xffff) != TAG_THREAD)
      continue;

    int handle = THREAD_HANDLE(addr - 8, word >> 16);
    memset(&info, 0, sizeof(info));
    int result = ReferThreadStatus(handle, &info);
    if (result != 0) {
      handle = THREAD_HANDLE(addr, word >> 16);
      memset(&info, 0, sizeof(info));
      result = ReferThreadStatus(handle, &info);
    }
    // Skip anything that doesn't look like a thread
    unsigned int status = info.status;
    if ((result != 0) ||
        ((status != THS_RUN) && (status != THS_READY) && (status != THS_WAIT) && (status != THS_SUSPEND) && (status != THS_WAITSUSPEND) &&
         (status != THS_DORMANT)) ||
        (info.initPriority < 1) || (info.initPriority > 127) || (((unsigned int)info.entry & 0x1fffff) < 0x800))
      continue;

    int duplicate = 0;
    for (unsigned int i = 0; i < shared.threadCount; i++)
      if (shared.threads[i].handle == handle)
        duplicate = 1;
    if (duplicate || (shared.threadCount >= LIVESCAN_MAX_THREADS))
      continue;

    LiveScanThread *thread = &shared.threads[shared.threadCount++];
    thread->handle = handle;
    thread->entry = (unsigned int)info.entry;
    thread->status = status;
    thread->priority = info.currentPriority;
    thread->waitType = info.waitType;
    thread->sio2 = isSIO2Code(thread->entry);
  }
}

//
// mmceman
//

// Reads mmceman from the memory card into IOP RAM
static int readMMCE(void) {
  if (mmceBuffer)
    return 0;

  int fd = iomanX_open(shared.mmcePath, FIO_O_RDONLY);
  if (fd < 0)
    return LIVESCAN_ERR_MMCE_READ + fd;

  int res = 0;
  int size = iomanX_lseek(fd, 0, FIO_SEEK_END);
  if ((size <= 0) || (size > 256 * 1024) || (iomanX_lseek(fd, 0, FIO_SEEK_SET) != 0)) {
    res = LIVESCAN_ERR_MMCE_READ - 22; // EINVAL
    goto out;
  }
  if (!(mmceBuffer = AllocSysMemory(ALLOC_FIRST, size, NULL))) {
    res = LIVESCAN_ERR_MMCE_READ - 12; // ENOMEM
    goto out;
  }
  if (iomanX_read(fd, mmceBuffer, size) != size) {
    FreeSysMemory(mmceBuffer);
    mmceBuffer = NULL;
    res = LIVESCAN_ERR_MMCE_READ - 5; // EIO
    goto out;
  }
  mmceSize = size;

out:
  iomanX_close(fd);
  return res;
}

// Loads and starts mmceman from IOP RAM. Must be called with the SIO2 locked,
// since mmceman looks for the MMCE devices when it starts
static int startMMCE(void) {
  int id = LoadModuleBuffer(mmceBuffer);
  FreeSysMemory(mmceBuffer); // LoadModuleBuffer() copies the module
  mmceBuffer = NULL;
  if (id < 0)
    return LIVESCAN_ERR_MMCE_LOAD + id;

  int ret = 1;
  int res = StartModule(id, "mmceman", 0, NULL, &ret);
  if ((res < 0) || ((ret & 3) == MODULE_NO_RESIDENT_END))
    return LIVESCAN_ERR_MMCE_START;
  shared.mmceLoaded = id;
  return 0;
}

static void doScan(void) {
  char mountpoint[8];
  int res = 0;
  gameCount = 0;
  shared.watchdogResumed = 0;

  if ((res = findSIO2Lock()))
    goto out;

  if (!shared.mmceLoaded) {
    shared.stage = LIVESCAN_STAGE_READ_MMCE;
    if ((res = readMMCE()))
      goto out;
  }

  shared.stage = LIVESCAN_STAGE_PAUSE;
  lockSIO2();

  if (!shared.mmceLoaded) {
    shared.stage = LIVESCAN_STAGE_LOAD_MMCE;
    if ((res = startMMCE()))
      goto out;
  }

  shared.stage = LIVESCAN_STAGE_SCAN;
  if (shared.devices & LIVESCAN_DEV_MMCE) {
    for (int slot = 0; slot < 2; slot++) {
      sprintf(mountpoint, "mmce%d:", slot);
      if (shared.kind == LIVESCAN_KIND_PSX)
        scanEmberFolder(mountpoint);
      else {
        scanFolder(mountpoint, shared.cdFolder);
        scanFolder(mountpoint, shared.dvdFolder);
      }
    }
  }

  // The cache is on the memory card, so it's written after handing the SIO2 back
  shared.stage = LIVESCAN_STAGE_RESUME;
  unlockSIO2();

  // Insertion sort by name
  for (unsigned int i = 1; i < gameCount; i++)
    for (unsigned int j = i; (j > 0) && (strCaseCmp(shared.names[j - 1], shared.names[j]) > 0); j--)
      swapGames(j - 1, j);

  shared.stage = LIVESCAN_STAGE_WRITE;
  res = writeCache();

out:
  unlockSIO2();
  listThreads(); // For the log: the thread states and the SIO2 interrupt handler after the scan
  shared.count = gameCount;
  shared.result = res;
  shared.stage = LIVESCAN_STAGE_IDLE;
}

static unsigned int bcdToInt(unsigned char bcd) { return ((bcd >> 4) * 10 + (bcd & 0xf)) % 100; }

// Writes the log chunk in logBuffer, creating the file named after the console clock with the first one
static int logFd = -1;
static void writeLogChunk(unsigned int request) {
  if (request & LIVESCAN_LOG_FIRST) {
    if (logFd >= 0)
      iomanX_close(logFd);

    char dir[LIVESCAN_LOG_NAME_LEN];
    strncpy(dir, shared.logName, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    sceCdCLOCK clock;
    memset(&clock, 0, sizeof(clock));
    static unsigned int counter = 0;
    if (sceCdReadClock(&clock) && !clock.stat)
      sprintf(shared.logName, "%.20sOSDMLIVE-%02u%02u%02u-%02u%02u%02u.LOG", dir, bcdToInt(clock.year), bcdToInt(clock.month & 0x1f),
              bcdToInt(clock.day), bcdToInt(clock.hour), bcdToInt(clock.minute), bcdToInt(clock.second));
    else
      sprintf(shared.logName, "%.20sOSDMLIVE-%u.LOG", dir, counter++);

    if ((logFd = iomanX_open(shared.logName, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC)) < 0) {
      shared.logResult = logFd;
      return;
    }
  }
  if (logFd < 0) {
    shared.logResult = -9; // EBADF
    return;
  }

  unsigned int length = (shared.logLength > LIVESCAN_LOG_CHUNK) ? LIVESCAN_LOG_CHUNK : shared.logLength;
  shared.logResult = iomanX_write(logFd, shared.logBuffer, length);
  if (request & LIVESCAN_LOG_LAST) {
    iomanX_close(logFd);
    logFd = -1;
  }
}

static void scanThread(void *arg) {
  while (1) {
    shared.heartbeat++;
    unsigned int logRequest = shared.logRequest;
    if (logRequest) {
      writeLogChunk(logRequest);
      shared.logRequest = 0;
    }
    unsigned int request = shared.request;
    if (request) {
      shared.request = 0;
      shared.status = LIVESCAN_STATUS_BUSY;
      if (request == LIVESCAN_LIST)
        listThreads();
      else if (request == LIVESCAN_SAVE_FAV)
        shared.result = saveFavorites();
      else
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

  // The watchdog runs before anything else, so it gets to resume the threads whatever the scan does
  thread.thread = watchdogThread;
  thread.priority = 8;
  thread.stacksize = 0x400;
  int watchdog = CreateThread(&thread);
  if (watchdog < 0)
    return MODULE_NO_RESIDENT_END;

  StartThread(watchdog, NULL);
  StartThread(tid, NULL);

  // Write the magic last so the EE never finds a half-initialized structure
  shared.magic[1] = LIVESCAN_MAGIC1;
  shared.magic[2] = LIVESCAN_MAGIC2;
  shared.magic[3] = LIVESCAN_MAGIC3;
  shared.magic[0] = LIVESCAN_MAGIC0;
  return MODULE_RESIDENT_END;
}
