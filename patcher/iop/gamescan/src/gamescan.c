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
// So while mmceman runs, the threads of the modules that use the SIO2 are suspended,
// and mmceman is read into IOP RAM before, since the memory card can't be read without them.
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

//
// SIO2 threads
//
// Threads are found by scanning IOP RAM for the thread control block tag (0x7f01 in the first halfword,
// the ID in the second), turning each address into a thread ID the same way threadman does
// and checking it with ReferThreadStatus(). Only threads whose entry point is in the code of a module
// that uses the SIO2 are suspended, so a false match can't be suspended.
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

static int pausedHandles[LIVESCAN_MAX_THREADS];
static volatile int pausedCount = 0;
static volatile unsigned int pausedTicks = 0;

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

// Lists the threads in shared.threads, suspending the SIO2 ones if suspend is set
static void listThreads(int suspend) {
  iop_thread_info_t info;
  findSIO2Modules();
  shared.threadCount = 0;
  shared.tagCount = 0;

  // Words around the address gamescan.irx's own thread ID points to, if it's made like THREAD_HANDLE()
  unsigned int own = (((unsigned int)shared.ownThreads[0] >> 7) << 2) & 0x1ffffc;
  for (int i = 0; i < 16; i++)
    shared.ownTcb[i] = ((own >= 8) && (own + 56 < LIVESCAN_IOP_RAM_SIZE)) ? *(volatile unsigned int *)(own - 8 + i * 4) : 0;

  for (unsigned int addr = 0x800; addr < LIVESCAN_IOP_RAM_SIZE; addr += 4) {
    // Thread IDs stored here could look like a tag
    if (((addr >= (unsigned int)&shared) && (addr < (unsigned int)&shared + sizeof(shared))) ||
        ((addr >= (unsigned int)pausedHandles) && (addr < (unsigned int)pausedHandles + sizeof(pausedHandles))))
      continue;
    unsigned int word = *(volatile unsigned int *)addr;
    if ((word & 0xffff) != TAG_THREAD)
      continue;
    shared.tagCount++;

    int handle = THREAD_HANDLE(addr - 8, word >> 16);
    memset(&info, 0, sizeof(info));
    int result = ReferThreadStatus(handle, &info);
    if (result != 0) {
      handle = THREAD_HANDLE(addr, word >> 16);
      memset(&info, 0, sizeof(info));
      result = ReferThreadStatus(handle, &info);
    }
    unsigned int status = info.status;
    if (shared.tagCount <= LIVESCAN_MAX_TAGS) {
      LiveScanTag *tag = &shared.tags[shared.tagCount - 1];
      tag->addr = addr;
      tag->word = word;
      tag->handle = handle;
      tag->result = result;
      tag->status = status;
      tag->initPriority = info.initPriority;
      tag->entry = (unsigned int)info.entry;
    }
    if (result != 0)
      continue;
    // Skip anything that doesn't look like a thread
    if (((status != THS_RUN) && (status != THS_READY) && (status != THS_WAIT) && (status != THS_SUSPEND) && (status != THS_WAITSUSPEND) &&
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
    thread->sio2 = isSIO2Code(thread->entry);
    thread->suspended = 1;

    // Running, ready or waiting threads are suspended. The current thread is never in a SIO2 module
    if (suspend && thread->sio2 && ((status == THS_READY) || (status == THS_WAIT) || (status == THS_RUN))) {
      thread->suspended = SuspendThread(handle);
      if (!thread->suspended)
        pausedHandles[pausedCount++] = handle;
    }
  }
}

static void resumeThreads(void) {
  int count = pausedCount;
  pausedCount = 0;
  for (int i = 0; i < count; i++)
    ResumeThread(pausedHandles[i]);
}

// Resumes the SIO2 threads if the scan takes more than 20 seconds, so a stuck scan doesn't leave
// the controller and the memory cards stopped
static void watchdogThread(void *arg) {
  while (1) {
    DelayThread(500 * 1000);
    if (!pausedCount) {
      pausedTicks = 0;
      continue;
    }
    if (++pausedTicks >= 40) {
      shared.watchdogResumed = 1;
      resumeThreads();
    }
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

// Loads and starts mmceman from IOP RAM. Must be called with the SIO2 threads suspended,
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
  shared.suspendedCount = 0;

  if (!shared.mmceLoaded) {
    shared.stage = LIVESCAN_STAGE_READ_MMCE;
    if ((res = readMMCE()))
      goto out;
  }

  shared.stage = LIVESCAN_STAGE_PAUSE;
  listThreads(1);
  shared.suspendedCount = pausedCount;
  if (!pausedCount) {
    res = LIVESCAN_ERR_NO_THREADS;
    goto out;
  }
  DelayThread(20 * 1000); // Lets a transfer that was running finish

  if (!shared.mmceLoaded) {
    shared.stage = LIVESCAN_STAGE_LOAD_MMCE;
    if ((res = startMMCE()))
      goto out;
  }

  shared.stage = LIVESCAN_STAGE_SCAN;
  if (shared.devices & LIVESCAN_DEV_MMCE) {
    for (int slot = 0; slot < 2; slot++) {
      sprintf(mountpoint, "mmce%d:", slot);
      scanFolder(mountpoint, shared.cdFolder);
      scanFolder(mountpoint, shared.dvdFolder);
    }
  }

  // The cache is on the memory card, so it's written after resuming the threads
  shared.stage = LIVESCAN_STAGE_RESUME;
  resumeThreads();

  // Insertion sort by name
  for (unsigned int i = 1; i < gameCount; i++)
    for (unsigned int j = i; (j > 0) && (strCaseCmp(shared.names[j - 1], shared.names[j]) > 0); j--)
      swapGames(j - 1, j);

  shared.stage = LIVESCAN_STAGE_WRITE;
  res = writeCache();

out:
  resumeThreads();
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
        listThreads(0);
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

  shared.ownThreads[0] = tid;
  shared.ownThreads[1] = watchdog;
  StartThread(watchdog, NULL);
  StartThread(tid, NULL);

  // Write the magic last so the EE never finds a half-initialized structure
  shared.magic[1] = LIVESCAN_MAGIC1;
  shared.magic[2] = LIVESCAN_MAGIC2;
  shared.magic[3] = LIVESCAN_MAGIC3;
  shared.magic[0] = LIVESCAN_MAGIC0;
  return MODULE_RESIDENT_END;
}
