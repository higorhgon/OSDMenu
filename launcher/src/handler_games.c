#include "common.h"
#include "defaults.h"
#include "dprintf.h"
#include "handler_games.h"
#include "handlers.h"
#include "init.h"
#include <ctype.h>
#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

//
// Games menu
//
// Scans CD/ and DVD/ folders (both hold PS2 titles, CD/ for titles released
// on CD media, DVD/ for titles released on DVD media) across the enabled
// devices, shows a text list drawn with libdebug's console (the same one
// already used by msg()/fail() in common.c — reused here instead of writing
// a new font/graphics renderer, since it's already proven to work at this
// exact point in the boot flow), and launches the selection via the
// user-installed standalone neutrino.elf.
//

#define GAMES_MAX_ENTRIES 500
#define GAMES_NAME_LEN 128
#define GAMES_REL_PATH_LEN 256
#define GAMES_MAX_EXTRA_ARGS 8
#define GAMES_ID_LEN 12 // "SLUS_202.12" + NUL

// Short settle window per mountpoint probe — this scans up to 5 mountpoints
// (USB x2, MX4SIO x1, MMCE x2), so it deliberately does not reuse
// handler_bdm.c's DELAY_ATTEMPTS (20s), which is meant for a single,
// specifically-targeted device at item-launch time.
#define GAMES_PROBE_ATTEMPTS 3

#define GAMES_LIST_START_ROW 3
#define GAMES_VISIBLE_ROWS 20

// Colors are u32 values for scr_setfontcolor()/scr_setbgcolor(); exact
// packing/defaults were not verified against real hardware — expect to
// tune these after the first real test.
#define GAMES_COLOR_TEXT 0x80FFFFFF
#define GAMES_COLOR_BG 0x80000000
#define GAMES_COLOR_SELECTED_TEXT 0x80000000
#define GAMES_COLOR_SELECTED_BG 0x8000A0FF

typedef enum { GameMedia_DVD, GameMedia_CD } GameMediaType;

typedef struct {
  char name[GAMES_NAME_LEN];        // Display name
  char path[GAMES_REL_PATH_LEN];    // Full path as scanned, e.g. "mmce0:/DVD/Some Game/game.iso"
  const char *neutrinoDriver;       // Neutrino -bsd= driver ("usb"/"mx4sio"/"mmce"), static lifetime
  GameMediaType media;
  char id[GAMES_ID_LEN];            // Title ID (e.g. "SLUS_202.12"), empty if unknown
} GameEntry;

typedef struct {
  DeviceType device;
  const char *mountFmt;       // printf format for the mountpoint, e.g. "mass%d:"
  const char *neutrinoDriver; // Neutrino -bsd= driver for this device class
  int mountCount;             // Number of indices to probe (0..mountCount-1)
} GamesDeviceEntry;

static const GamesDeviceEntry gamesDevices[] = {
#ifdef USB
    {Device_USB, "mass%d:", "usb", 2},
#endif
#ifdef MX4SIO
    {Device_MX4SIO, "mx4sio%d:", "mx4sio", 1},
#endif
#ifdef MMCE
    {Device_MMCE, "mmce%d:", "mmce", 2},
#endif
};
#define GAMES_DEVICE_COUNT (sizeof(gamesDevices) / sizeof(gamesDevices[0]))

static GameEntry gameList[GAMES_MAX_ENTRIES];
static int gameCount = 0;
static int gameListTruncated = 0;

// Diagnostic info captured during the scan, shown on-screen when no games
// are found so a bad OSDMENU.CNF/folder layout can be diagnosed without a
// serial debug cable
#define GAMES_MAX_DIAG 8
typedef struct {
  char mountpoint[16];
  int available; // mountpoint responded to the initial probe
  int cdOpened;   // fileXioDopen() on the CD folder succeeded
  int cdEntries;  // total directory entries seen (files + folders)
  int dvdOpened;
  int dvdEntries;
} GamesScanDiag;
static GamesScanDiag scanDiag[GAMES_MAX_DIAG];
static int scanDiagCount = 0;

// Copies src into out, dropping a trailing ".iso" and a leading OPL-style
// title ID prefix ("SLUS_202.12." in "SLUS_202.12.BLOODY ROAR 3.iso")
static void makeDisplayName(const char *src, char *out, size_t outSize) {
  if (strlen(src) > 12 && isalpha((int)src[0]) && isalpha((int)src[1]) && isalpha((int)src[2]) && isalpha((int)src[3]) && src[4] == '_' &&
      isdigit((int)src[5]) && isdigit((int)src[6]) && isdigit((int)src[7]) && src[8] == '.' && isdigit((int)src[9]) &&
      isdigit((int)src[10]) && src[11] == '.')
    src += 12;

  strncpy(out, src, outSize - 1);
  out[outSize - 1] = '\0';

  char *ext = strrchr(out, '.');
  if (ext && !strcasecmp(ext, ".iso") && ext != out)
    *ext = '\0';
}

// Looks for a title ID at s, in the "SLUS_202.12"/"SLUS-202.12" or the
// "SLUS-20212" (PFS BatchKit) form, and writes it to out as "SLUS_202.12"
static int parseGameID(const char *s, char *out) {
  for (int i = 0; i < 4; i++)
    if (!isupper((int)s[i]))
      return 0;

  if (((s[4] == '_') || (s[4] == '-')) && isdigit((int)s[5]) && isdigit((int)s[6]) && isdigit((int)s[7]) && (s[8] == '.') &&
      isdigit((int)s[9]) && isdigit((int)s[10])) {
    memcpy(out, s, 11);
    out[4] = '_';
    out[11] = '\0';
    return 1;
  }

  if ((s[4] == '-') && isdigit((int)s[5]) && isdigit((int)s[6]) && isdigit((int)s[7]) && isdigit((int)s[8]) && isdigit((int)s[9])) {
    snprintf(out, GAMES_ID_LEN, "%.4s_%.3s.%.2s", s, &s[5], &s[8]);
    return 1;
  }
  return 0;
}

// Searches the whole string for a title ID
static int findGameID(const char *s, char *out) {
  for (; strlen(s) >= 10; s++)
    if (parseGameID(s, out))
      return 1;
  return 0;
}

static uint32_t readLE32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Reads the title ID from the ISO: the BOOT2 line of SYSTEM.CNF, or any
// title ID-like file name in the root directory as a fallback
static int getISOGameID(const char *isoPath, char *out) {
  static uint8_t buf[8192];
  int found = 0;
  char fallback[GAMES_ID_LEN] = {0};

  int fd = open(isoPath, O_RDONLY);
  if (fd < 0)
    return 0;

  // Primary Volume Descriptor
  if ((lseek(fd, 16 * 2048, SEEK_SET) < 0) || (read(fd, buf, 2048) != 2048) || (buf[0] != 1) || memcmp(&buf[1], "CD001", 5))
    goto out;

  // Root directory record starts at offset 156
  uint32_t rootLBA = readLE32(&buf[156 + 2]);
  uint32_t rootSize = readLE32(&buf[156 + 10]);
  if (rootSize > sizeof(buf))
    rootSize = sizeof(buf);
  if ((lseek(fd, rootLBA * 2048, SEEK_SET) < 0) || (read(fd, buf, rootSize) != (int)rootSize))
    goto out;

  uint32_t cnfLBA = 0, cnfSize = 0;
  char name[64];
  for (uint32_t off = 0; off < rootSize;) {
    uint8_t len = buf[off];
    if (len == 0) { // Records don't cross sector boundaries
      off = (off / 2048 + 1) * 2048;
      continue;
    }
    if ((len < 34) || (off + len > rootSize))
      break;

    uint8_t nameLen = buf[off + 32];
    if (nameLen >= sizeof(name))
      nameLen = sizeof(name) - 1;
    memcpy(name, &buf[off + 33], nameLen);
    name[nameLen] = '\0';

    if (!strncasecmp(name, "SYSTEM.CNF", 10)) {
      cnfLBA = readLE32(&buf[off + 2]);
      cnfSize = readLE32(&buf[off + 10]);
    } else if (!fallback[0])
      findGameID(name, fallback);
    off += len;
  }

  if (cnfLBA) {
    if (cnfSize > 1023)
      cnfSize = 1023;
    if ((lseek(fd, cnfLBA * 2048, SEEK_SET) >= 0) && (read(fd, buf, cnfSize) == (int)cnfSize)) {
      buf[cnfSize] = '\0';
      // BOOT2 = cdrom0:\SLUS_202.12;1
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
  close(fd);
  return found;
}

// Adds a game to the in-memory list if there's room left
static void addGameEntry(const char *dirPath, const char *isoName, const char *displayName, GameMediaType media,
                          const char *neutrinoDriver) {
  if (gameCount >= GAMES_MAX_ENTRIES) {
    gameListTruncated = 1;
    return;
  }

  GameEntry *g = &gameList[gameCount];
  makeDisplayName(displayName, g->name, GAMES_NAME_LEN);

  snprintf(g->path, GAMES_REL_PATH_LEN, "%s/%s", dirPath, isoName);
  g->neutrinoDriver = neutrinoDriver;
  g->media = media;

  // Title ID from the file or folder name, or from the ISO itself
  g->id[0] = '\0';
  if (!findGameID(isoName, g->id) && !findGameID(displayName, g->id))
    getISOGameID(g->path, g->id);
  gameCount++;
}

// Checks whether dirPath contains exactly one *.iso file (case-insensitive).
// Zero or more than one match is treated as ambiguous and skipped — this also
// naturally excludes split ISOs (game.iso.0/game.iso.1/...), which Neutrino
// itself does not support.
static int hasExactlyOneISO(const char *dirPath, char *isoNameOut, size_t isoNameOutSize) {
  int dfd = fileXioDopen(dirPath);
  if (dfd < 0)
    return 0;

  int count = 0;
  iox_dirent_t dirent;
  while (fileXioDread(dfd, &dirent) > 0) {
    if (FIO_S_ISDIR(dirent.stat.mode))
      continue;

    char *ext = strrchr(dirent.name, '.');
    if (!ext || strcasecmp(ext, ".iso"))
      continue;

    count++;
    if (count == 1)
      strncpy(isoNameOut, dirent.name, isoNameOutSize - 1);
    else
      break; // More than one match, no point scanning further
  }
  fileXioDclose(dfd);

  return (count == 1);
}

// Scans one folder (e.g. "mass0:/DVD") for games: flat *.iso files, or
// one-level subfolders containing exactly one *.iso inside (display name is
// then the subfolder name, matching common per-game folder organization).
// Returns 1 if the folder was opened at all (regardless of how many games
// were found in it), 0 if it doesn't exist on this device. *entriesOut is
// set to the total number of directory entries seen (excluding . and ..).
static int scanFolder(const char *mountpoint, const char *folder, GameMediaType media, const char *neutrinoDriver, int *entriesOut) {
  *entriesOut = 0;

  char dirPath[GAMES_REL_PATH_LEN];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", mountpoint, folder);

  int dfd = fileXioDopen(dirPath);
  if (dfd < 0)
    return 0; // Folder doesn't exist on this device — not an error

  iox_dirent_t dirent;
  while (fileXioDread(dfd, &dirent) > 0) {
    if (!strcmp(dirent.name, ".") || !strcmp(dirent.name, ".."))
      continue;
    (*entriesOut)++;

    if (FIO_S_ISDIR(dirent.stat.mode)) {
      char subDirPath[GAMES_REL_PATH_LEN];
      snprintf(subDirPath, sizeof(subDirPath), "%s/%s", dirPath, dirent.name);

      char isoName[GAMES_NAME_LEN];
      if (hasExactlyOneISO(subDirPath, isoName, sizeof(isoName)))
        addGameEntry(subDirPath, isoName, dirent.name, media, neutrinoDriver);
      else {
        DPRINTF("Games: skipping ambiguous subfolder %s\n", subDirPath);
      }
      continue;
    }

    char *ext = strrchr(dirent.name, '.');
    if (ext && !strcasecmp(ext, ".iso"))
      addGameEntry(dirPath, dirent.name, dirent.name, media, neutrinoDriver);
  }
  fileXioDclose(dfd);
  return 1;
}

static int gameEntryCompare(const void *a, const void *b) { return strcasecmp(((const GameEntry *)a)->name, ((const GameEntry *)b)->name); }

// Probes and scans every enabled device/mountpoint combination
static void scanAllDevices(GamesConfig *cfg) {
  gameCount = 0;
  gameListTruncated = 0;
  scanDiagCount = 0;

  for (size_t i = 0; i < GAMES_DEVICE_COUNT; i++) {
    const GamesDeviceEntry *dev = &gamesDevices[i];

#ifdef USB
    if (dev->device == Device_USB && !cfg->useUSB)
      continue;
#endif
#ifdef MX4SIO
    if (dev->device == Device_MX4SIO && !cfg->useMX4SIO)
      continue;
#endif
#ifdef MMCE
    if (dev->device == Device_MMCE && !cfg->useMMCE)
      continue;
#endif

    for (int idx = 0; idx < dev->mountCount; idx++) {
      if (scanDiagCount >= GAMES_MAX_DIAG)
        break;
      GamesScanDiag *diag = &scanDiag[scanDiagCount++];
      memset(diag, 0, sizeof(*diag));
      snprintf(diag->mountpoint, sizeof(diag->mountpoint), dev->mountFmt, idx);

      // MMCE doesn't go through the BDM/FAT layer and enumerates immediately
      // (handler_mc.c:handleMMCE does the same single-shot check when
      // launching apps from MMCE); BDM-backed devices (USB/MX4SIO) may still
      // be settling after initModules()
      int attempts = (dev->device == Device_MMCE) ? 1 : GAMES_PROBE_ATTEMPTS;
      while (attempts > 0) {
        int fd = open(diag->mountpoint, O_DIRECTORY | O_RDONLY);
        if (fd >= 0) {
          close(fd);
          diag->available = 1;
          break;
        }
        attempts--;
        if (attempts > 0)
          sleep(1);
      }
      // The root probe above only gives BDM devices time to settle; still try
      // the folders when it fails, since not every driver necessarily accepts
      // O_DIRECTORY on a bare "<device>:" root (handleMMCE never probes it)
      diag->cdOpened = scanFolder(diag->mountpoint, cfg->cdFolder, GameMedia_CD, dev->neutrinoDriver, &diag->cdEntries);
      diag->dvdOpened = scanFolder(diag->mountpoint, cfg->dvdFolder, GameMedia_DVD, dev->neutrinoDriver, &diag->dvdEntries);
    }
  }

  qsort(gameList, gameCount, sizeof(GameEntry), gameEntryCompare);
}

// Prints what the scan actually saw, per mountpoint, so a misconfigured
// folder/device can be diagnosed from the screen alone (no serial cable)
static void printScanDiagnostics(GamesConfig *cfg) {
  scr_printf(" Nenhum jogo encontrado. Diagnostico:\n");
  if (scanDiagCount == 0) {
    scr_printf(" Nenhum dispositivo habilitado em games_device_*\n");
    return;
  }
  for (int i = 0; i < scanDiagCount; i++) {
    GamesScanDiag *d = &scanDiag[i];
    if (!d->available && !d->cdOpened && !d->dvdOpened) {
      scr_printf(" %s nao respondeu (dispositivo ausente/nao pronto)\n", d->mountpoint);
      continue;
    }
    scr_printf(" %s %s=%s(%d) %s=%s(%d)\n", d->mountpoint, cfg->cdFolder, d->cdOpened ? "ok" : "n/a", d->cdEntries, cfg->dvdFolder,
               d->dvdOpened ? "ok" : "n/a", d->dvdEntries);
  }
}

// Draws the current page of the list, with the selected row highlighted
static void drawGameList(int selected) {
  scr_setXY(0, 0);
  scr_clearline(0);
  scr_printf(" Jogos (%d encontrado%s%s)", gameCount, gameCount == 1 ? "" : "s", gameListTruncated ? ", lista truncada" : "");

  int top = selected - (GAMES_VISIBLE_ROWS / 2);
  if (top > gameCount - GAMES_VISIBLE_ROWS)
    top = gameCount - GAMES_VISIBLE_ROWS;
  if (top < 0)
    top = 0;

  for (int row = 0; row < GAMES_VISIBLE_ROWS; row++) {
    int idx = top + row;
    int y = GAMES_LIST_START_ROW + row;
    scr_setXY(0, y);
    scr_clearline(y);

    if (idx >= gameCount)
      continue;

    if (idx == selected) {
      scr_setbgcolor(GAMES_COLOR_SELECTED_BG);
      scr_setfontcolor(GAMES_COLOR_SELECTED_TEXT);
      scr_printf(" > %s", gameList[idx].name);
      scr_setbgcolor(GAMES_COLOR_BG);
      scr_setfontcolor(GAMES_COLOR_TEXT);
    } else {
      scr_printf("   %s", gameList[idx].name);
    }
  }

  int footerY = GAMES_LIST_START_ROW + GAMES_VISIBLE_ROWS + 1;
  scr_setXY(0, footerY);
  scr_clearline(footerY);
  scr_printf(" Cima/Baixo: navegar   X: jogar   Triangulo/Circulo: voltar");
}

// Returns the storage driver bit that must be loaded to access path, if any.
// Memory cards are part of Device_Basic, which is always loaded.
static DeviceType storageDevice(char *path) {
  DeviceType type = guessDeviceType(path);
  switch (type) {
  case Device_MMCE:
  case Device_USB:
  case Device_MX4SIO:
  case Device_ATA:
    return type;
  default:
    return Device_None;
  }
}

// Launches argv[0]. Only returns on failure.
// ELFs on memory cards are loaded directly: handleMC() would reload the IOP
// with the basic modules only, unloading the drivers Neutrino's quickboot needs.
static void launchGamesELF(int argc, char *argv[]) {
  if (guessDeviceType(argv[0]) != Device_MemoryCard) {
    launchPath(argc, argv);
    return;
  }

  if (argv[0][2] == '?') {
    for (char slot = '0'; slot < '2'; slot++) {
      argv[0][2] = slot;
      if (!tryFile(argv[0]))
        break;
    }
  }
  if (!tryFile(argv[0]))
    LoadELFFromFile(argc, argv);
}

#ifdef MMCE
// Sends the title ID to MMCE devices so they switch to the game's memory card
// (mmce?:/MemoryCards/PS2/<ID>/<ID>-1.mcd), like NHDDL does. Skips the slot
// elfPath is loaded from, since switching would swap the card under it.
static void mmceSetGameID(const char *elfPath, const char *id) {
  char mountpoint[] = "mmceX:";
  for (char slot = '0'; slot < '2'; slot++) {
    if (!strncmp(elfPath, "mc", 2) && ((elfPath[2] == slot) || (elfPath[2] == '?')))
      continue;

    mountpoint[4] = slot;
    // Ping first to make sure the device is present
    if (fileXioDevctl(mountpoint, 0x1, NULL, 0, NULL, 0) < 0)
      continue;
    if (fileXioDevctl(mountpoint, 0x8, (void *)id, strlen(id) + 1, NULL, 0) < 0)
      continue;

    // Wait until the device is done switching the card
    for (int i = 0; i < 15; i++) {
      sleep(1);
      if ((fileXioDevctl(mountpoint, 0x2, NULL, 0, NULL, 0) & 1) == 0)
        break;
    }
  }
}
#endif

// Returns 1 if OPL can autolaunch the game. OPL's argv autolaunch looks for the ISO
// directly in the CD/ or DVD/ folder, so media is set to "CD" or "DVD" and fileName to
// the ISO name. mode is "bdm" for USB/MX4SIO, or "mmce" (with slot "0"/"1") for MMCE,
// which needs a RiptOPL build with MMCE autolaunch support.
static int canLaunchWithOPL(char *isoPath, const char *id, const char **media, const char **fileName, const char **mode, char *slot) {
  DeviceType type = guessDeviceType(isoPath);
  if (!id[0])
    return 0;
  if ((type == Device_USB) || (type == Device_MX4SIO))
    *mode = "bdm";
  else if ((type == Device_MMCE) && (isoPath[4] >= '0') && (isoPath[4] <= '1')) {
    *mode = "mmce";
    slot[0] = isoPath[4];
    slot[1] = '\0';
  } else
    return 0;

  const char *rel = strchr(isoPath, ':');
  if (!rel)
    return 0;
  rel++;
  while (*rel == '/')
    rel++;

  if (!strncasecmp(rel, "CD/", 3))
    *media = "CD";
  else if (!strncasecmp(rel, "DVD/", 4))
    *media = "DVD";
  else
    return 0;

  *fileName = strchr(rel, '/') + 1;
  return (strchr(*fileName, '/') == NULL); // Games in subfolders are not supported
}

// Launches the game via OPL or Neutrino. Only returns on failure.
// Neutrino is started the same way NHDDL (pcm720/nhddl) does it:
//   neutrino.elf -bsd=<driver> -dvd=<full ISO path, e.g. mmce0:/DVD/game.iso> -qb
// With -qb, Neutrino uses the IOP modules loaded by the launcher, so the
// drivers for the ISO and the ELF are loaded together beforehand.
static void launchGame(GamesConfig *cfg, const char *bsd, char *isoPath, const char *id) {
  const char *media = NULL;
  const char *fileName = NULL;
  const char *oplMode = NULL;
  char oplSlot[2] = {0};
  int useOPL = 0;
  if (cfg->useOPL) {
    if (cfg->oplPath && canLaunchWithOPL(isoPath, id, &media, &fileName, &oplMode, oplSlot))
      useOPL = 1;
    else if (cfg->neutrinoPath)
      msg("Games: OPL can't launch this game, using Neutrino\n");
  }

  char *elfPath = useOPL ? cfg->oplPath : cfg->neutrinoPath;
  if (!elfPath) {
    msg("Games: games_neutrino_path is not set in OSDMENU.CNF\n");
    return;
  }

  DeviceType mask = storageDevice(isoPath) | storageDevice(elfPath);
  // OPL switches the MMCE card itself, taking its per-game VMC settings into account
#ifdef MMCE
  if (cfg->mmceGameID && id[0] && !useOPL)
    mask |= Device_MMCE;
#endif
  if (mask && initModules(mask))
    return;

#ifdef MMCE
  if (cfg->mmceGameID && id[0] && !useOPL)
    mmceSetGameID(elfPath, id);
#endif

  if (useOPL) {
    // opl.elf <ISO name> <title ID> <CD/DVD> bdm
    // opl.elf <ISO name> <title ID> <CD/DVD> mmce <slot>
    char *argv[] = {elfPath, (char *)fileName, (char *)id, (char *)media, (char *)oplMode, oplSlot};
    launchGamesELF(oplSlot[0] ? 6 : 5, argv);
    return;
  }

  char bsdArg[16];
  snprintf(bsdArg, sizeof(bsdArg), "-bsd=%s", bsd);
  char dvdArg[GAMES_REL_PATH_LEN + 8];
  snprintf(dvdArg, sizeof(dvdArg), "-dvd=%s", isoPath);

  char *argv[4 + GAMES_MAX_EXTRA_ARGS];
  int argc = 0;
  argv[argc++] = elfPath;
  argv[argc++] = bsdArg;
  argv[argc++] = dvdArg;
  argv[argc++] = "-qb";

  linkedStr *extra = cfg->neutrinoArgs;
  while (extra && argc < 4 + GAMES_MAX_EXTRA_ARGS) {
    argv[argc++] = extra->str;
    extra = extra->next;
  }

  launchGamesELF(argc, argv);
}

// Launches the selected entry of the full-screen list. Only returns on failure.
static void launchSelected(GamesConfig *cfg, int index) {
  GameEntry *g = &gameList[index];

  scr_setXY(0, GAMES_LIST_START_ROW + GAMES_VISIBLE_ROWS + 2);
  scr_printf(" Iniciando %s...\n", g->name);

  launchGame(cfg, g->neutrinoDriver, g->path, g->id);

  scr_printf(" Falha ao iniciar %s\n", g->name);
  sleep(2);
}

// Returns GAMES_CACHE_PATH on the memory card OSDMENU.CNF was loaded from
static void getGamesCachePath(char *path) {
  strcpy(path, GAMES_CACHE_PATH);
  path[2] = (settings.mcHint == 1) ? '1' : '0';
}

// Writes the scan result to GAMES_CACHE_PATH, which the patcher reads on boot
// to show the games as an OSDSYS submenu. Each game is a "game" (display name)
// line followed by its Neutrino "bsd" driver, "dvd" ISO path and optional "id" (title ID) lines.
static int writeGamesCache(void) {
  char path[sizeof(GAMES_CACHE_PATH)];
  getGamesCachePath(path);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0)
    return fd;

  char line[GAMES_NAME_LEN + GAMES_REL_PATH_LEN + 32];
  int res = 0;
  for (int i = 0; (i < gameCount) && !res; i++) {
    int len = snprintf(line, sizeof(line), "game = %s\nbsd = %s\ndvd = %s\n%s%s%s", gameList[i].name, gameList[i].neutrinoDriver, gameList[i].path,
                       gameList[i].id[0] ? "id = " : "", gameList[i].id, gameList[i].id[0] ? "\n" : "");
    if (len >= (int)sizeof(line))
      len = sizeof(line) - 1;
    if (write(fd, line, len) != len)
      res = -EIO;
  }
  close(fd);
  return res;
}

// Launches game idx from GAMES_CACHE_PATH without scanning. Only returns on failure.
static int launchCachedGame(GamesConfig *cfg, int idx) {
  char path[sizeof(GAMES_CACHE_PATH)];
  getGamesCachePath(path);

  FILE *file = fopen(path, "r");
  if (!file) {
    msg("Games: failed to open %s\n", path);
    return -ENOENT;
  }

  // Collect the "bsd", "dvd" and "id" lines that follow the idx-th "game" line
  char line[GAMES_REL_PATH_LEN + 32];
  char bsd[16] = {0};
  char isoPath[GAMES_REL_PATH_LEN] = {0};
  char id[GAMES_ID_LEN] = {0};
  int current = -1;
  while (fgets(line, sizeof(line), file)) {
    char *value = strchr(line, '=');
    if (!value)
      continue;
    do {
      value++;
    } while (isspace((int)*value));
    value[strcspn(value, "\r\n")] = '\0';

    if (!strncmp(line, "game", 4)) {
      if (++current > idx)
        break;
    } else if (current == idx) {
      if (!strncmp(line, "bsd", 3))
        strncpy(bsd, value, sizeof(bsd) - 1);
      else if (!strncmp(line, "dvd", 3))
        strncpy(isoPath, value, sizeof(isoPath) - 1);
      else if (!strncmp(line, "id", 2))
        strncpy(id, value, sizeof(id) - 1);
    }
  }
  fclose(file);

  if ((bsd[0] == '\0') || (isoPath[0] == '\0')) {
    msg("Games: game %d not found in %s, try refreshing the list\n", idx, path);
    return -ENOENT;
  }

  launchGame(cfg, bsd, isoPath, id);
  msg("Games: failed to launch %s\n", isoPath);
  return -ENOENT;
}

// Initializes a single controller for menu navigation
static int gamesPadReady(char *padBuf) {
  if (padInit(0) != 1)
    return 0;

  int retries = 10;
  while (retries-- > 0) {
    if (padPortOpen(0, 0, padBuf) != 0)
      goto ready;
    sleep(1);
  }
  return 0;

ready:
  retries = 10;
  while (retries-- > 0) {
    int state = padGetState(0, 0);
    if ((state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1))
      return 1;
    sleep(1);
  }
  return 0;
}

// Draws the list and handles pad input until the user launches a game or
// backs out to the OSDSYS clock
static void gamesUILoop(GamesConfig *cfg) {
  static char padBuf[256] __attribute__((aligned(64)));
  if (!gamesPadReady(padBuf)) {
    msg("Games: Failed to initialize the controller\n");
    sleep(3);
    ExecOSD(0, NULL);
    return;
  }

  init_scr();
  scr_setCursor(0);
  scr_clear();
  scr_setbgcolor(GAMES_COLOR_BG);
  scr_setfontcolor(GAMES_COLOR_TEXT);

  int selected = 0;
  int redraw = 1;
  uint32_t heldButtons = 0;
  struct padButtonStatus buttons;

  while (1) {
    if (redraw) {
      drawGameList(selected);
      redraw = 0;
    }

    if (padRead(0, 0, &buttons) != 0) {
      uint32_t state = 0xffff ^ buttons.btns;
      uint32_t pressed = state & ~heldButtons;
      heldButtons = state;

      if (pressed & PAD_UP) {
        if (selected > 0) {
          selected--;
          redraw = 1;
        }
      } else if (pressed & PAD_DOWN) {
        if (selected < gameCount - 1) {
          selected++;
          redraw = 1;
        }
      } else if (pressed & PAD_CROSS) {
        launchSelected(cfg, selected);
        redraw = 1; // Only reached if the launch failed
      } else if (pressed & (PAD_TRIANGLE | PAD_CIRCLE)) {
        padEnd();
        ExecOSD(0, NULL);
        return;
      }
    }

    usleep(16000);
  }
}

// Frees GamesConfig fields owned by this handler
static void freeGamesConfig(GamesConfig *cfg) {
  if (cfg->neutrinoPath)
    free(cfg->neutrinoPath);
  if (cfg->neutrinoArgs)
    freeLinkedStr(cfg->neutrinoArgs);
  if (cfg->returnPath)
    free(cfg->returnPath);
  if (cfg->oplPath)
    free(cfg->oplPath);
}

// Frees cfg and returns to OSDMenu, reopening the games submenu. ExecOSD() alone would boot the ROM OSDSYS,
// which doesn't bring OSDMenu back when it was started by a bootloader or an
// autoboot that only runs on power-on. Tries games_return_path, then the path
// the patcher was started from, then GAMES_DEFAULT_RETURN_PATH.
static void returnToMenu(GamesConfig *cfg, const char *patcherPath) {
  char candidates[3][GAMES_REL_PATH_LEN] = {0};
  if (cfg->returnPath)
    strncpy(candidates[0], cfg->returnPath, GAMES_REL_PATH_LEN - 1);
  if (patcherPath)
    strncpy(candidates[1], patcherPath, GAMES_REL_PATH_LEN - 1);
  strncpy(candidates[2], GAMES_DEFAULT_RETURN_PATH, GAMES_REL_PATH_LEN - 1);
  freeGamesConfig(cfg);

  for (int i = 0; i < 3; i++) {
    if (candidates[i][0] == '\0')
      continue;

    // ROM and disc paths would not bring OSDMenu back
    DeviceType type = guessDeviceType(candidates[i]);
    if ((type == Device_None) || (type == Device_ROM) || (type == Device_CDROM))
      continue;

    // Only returns if the file can't be launched.
    // "-games" makes OSDMenu reopen the games submenu; not passed to games_return_path
    char *argv[] = {candidates[i], GAMES_REOPEN_ARG};
    launchPath((i == 0) ? 1 : 2, argv);
  }

  ExecOSD(0, NULL);
}

int handleGames(GamesConfig *cfg, const char *osdmArg) {
  // Split off the patcher path first, since it contains ':' too
  char arg[GAMES_REL_PATH_LEN] = {0};
  strncpy(arg, osdmArg, sizeof(arg) - 1);
  char *patcherPath = strchr(arg, '|');
  if (patcherPath)
    *patcherPath++ = '\0';

  // Mode suffix appended by the patcher's games submenu, see handler_games.h
  const char *mode = strrchr(arg, ':');
  char modeType = (mode && ((mode[1] == 'g') || (mode[1] == 's'))) ? mode[1] : '\0';

  if ((modeType != 's') && !cfg->neutrinoPath && !(cfg->useOPL && cfg->oplPath)) {
    msg("Games: games_neutrino_path is not set in OSDMENU.CNF\n");
    sleep(3);
    if (modeType)
      returnToMenu(cfg, patcherPath);
    else {
      freeGamesConfig(cfg);
      ExecOSD(0, NULL);
    }
    return -EINVAL;
  }

  if (modeType == 'g') {
    int res = launchCachedGame(cfg, atoi(mode + 2));
    sleep(5);
    returnToMenu(cfg, patcherPath);
    return res;
  }

  // Device_Basic must not be in this mask: handleOSDM() has already loaded
  // Device_Basic | Device_CDROM, and initModules() returns early if *any* bit
  // of the requested mask is already loaded, which would skip loading the
  // storage drivers and padman entirely. Basic modules are always reloaded
  // on every IOP reset anyway. padman is only needed for the full-screen list.
  DeviceType scanMask = (modeType == 's') ? Device_None : Device_Games;
#ifdef USB
  if (cfg->useUSB)
    scanMask |= Device_USB;
#endif
#ifdef MX4SIO
  if (cfg->useMX4SIO)
    scanMask |= Device_MX4SIO;
#endif
#ifdef MMCE
  if (cfg->useMMCE)
    scanMask |= Device_MMCE;
#endif

  if (modeType == 's')
    msg("Scanning for games...\n");

  int res = initModules(scanMask);
  if (res) {
    msg("Games: Failed to initialize devices: %d\n", res);
    sleep(3);
    if (modeType)
      returnToMenu(cfg, patcherPath);
    else {
      freeGamesConfig(cfg);
      ExecOSD(0, NULL);
    }
    return res;
  }

  scanAllDevices(cfg);

  if (modeType == 's') {
    // Write the cache even when empty, so the submenu opens with just
    // "< Back"/"Refresh list" instead of rescanning on every open
    res = writeGamesCache();
    if (res < 0)
      msg("Games: failed to write the games list: %d\n", res);
    else
      msg("Found %d game%s\n", gameCount, (gameCount == 1) ? "" : "s");

    if (gameCount == 0) {
      printScanDiagnostics(cfg);
      sleep(15);
    } else
      sleep((res < 0) ? 5 : 1);

    returnToMenu(cfg, patcherPath);
    return res;
  }

  if (gameCount == 0) {
    msg("Games: No games found in %s/%s folders\n", cfg->cdFolder, cfg->dvdFolder);
    printScanDiagnostics(cfg);
    sleep(15); // Long enough to read/photograph before returning to the clock
    freeGamesConfig(cfg);
    ExecOSD(0, NULL);
    return -ENOENT;
  }

  gamesUILoop(cfg); // Only returns after backing out to the clock via ExecOSD, which itself does not return

  freeGamesConfig(cfg);
  return 0;
}
