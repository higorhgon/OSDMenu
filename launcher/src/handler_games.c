#include "common.h"
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
  char relPath[GAMES_REL_PATH_LEN]; // Path relative to the device root, e.g. "/DVD/Some Game/game.iso"
  const char *neutrinoDriver;       // Neutrino -dvd= prefix ("usb"/"mx4sio"/"mmce"), static lifetime
  GameMediaType media;
} GameEntry;

typedef struct {
  DeviceType device;
  const char *mountFmt;       // printf format for the mountpoint, e.g. "mass%d:"
  const char *neutrinoDriver; // Neutrino -dvd= prefix for this device class
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

// Adds a game to the in-memory list if there's room left
static void addGameEntry(const char *relDir, const char *isoName, const char *displayName, GameMediaType media,
                          const char *neutrinoDriver) {
  if (gameCount >= GAMES_MAX_ENTRIES) {
    gameListTruncated = 1;
    return;
  }

  GameEntry *g = &gameList[gameCount];
  makeDisplayName(displayName, g->name, GAMES_NAME_LEN);

  snprintf(g->relPath, GAMES_REL_PATH_LEN, "%s/%s", relDir, isoName);
  g->neutrinoDriver = neutrinoDriver;
  g->media = media;
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

  char relDir[GAMES_REL_PATH_LEN];
  snprintf(relDir, sizeof(relDir), "/%s", folder);

  iox_dirent_t dirent;
  while (fileXioDread(dfd, &dirent) > 0) {
    if (!strcmp(dirent.name, ".") || !strcmp(dirent.name, ".."))
      continue;
    (*entriesOut)++;

    if (FIO_S_ISDIR(dirent.stat.mode)) {
      char subDirPath[GAMES_REL_PATH_LEN];
      snprintf(subDirPath, sizeof(subDirPath), "%s/%s", dirPath, dirent.name);

      char isoName[GAMES_NAME_LEN];
      if (hasExactlyOneISO(subDirPath, isoName, sizeof(isoName))) {
        char subRelDir[GAMES_REL_PATH_LEN];
        snprintf(subRelDir, sizeof(subRelDir), "%s/%s", relDir, dirent.name);
        addGameEntry(subRelDir, isoName, dirent.name, media, neutrinoDriver);
      } else {
        DPRINTF("Games: skipping ambiguous subfolder %s\n", subDirPath);
      }
      continue;
    }

    char *ext = strrchr(dirent.name, '.');
    if (ext && !strcasecmp(ext, ".iso"))
      addGameEntry(relDir, dirent.name, dirent.name, media, neutrinoDriver);
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

// Builds the neutrino.elf argv for the selected game and hands off to the
// existing generic launch pipeline. Only returns on failure.
static void launchSelected(GamesConfig *cfg, int index) {
  GameEntry *g = &gameList[index];

  char dvdArg[GAMES_REL_PATH_LEN + 32];
  snprintf(dvdArg, sizeof(dvdArg), "-dvd=%s:%s", g->neutrinoDriver, g->relPath);

  char *argv[3 + GAMES_MAX_EXTRA_ARGS];
  int argc = 0;
  argv[argc++] = cfg->neutrinoPath;
  argv[argc++] = dvdArg;
  argv[argc++] = "-qb";

  linkedStr *extra = cfg->neutrinoArgs;
  while (extra && argc < 3 + GAMES_MAX_EXTRA_ARGS) {
    argv[argc++] = extra->str;
    extra = extra->next;
  }

  scr_setXY(0, GAMES_LIST_START_ROW + GAMES_VISIBLE_ROWS + 2);
  scr_printf(" Iniciando %s...\n", g->name);

  // If the path is valid, launchPath() never returns
  launchPath(argc, argv);

  scr_printf(" Falha ao iniciar %s\n", g->name);
  sleep(2);
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
}

int handleGames(GamesConfig *cfg) {
  if (!cfg->neutrinoPath) {
    msg("Games: games_neutrino_path is not set in OSDMENU.CNF\n");
    sleep(3);
    freeGamesConfig(cfg);
    ExecOSD(0, NULL);
    return -EINVAL;
  }

  // Device_Basic must not be in this mask: handleOSDM() has already loaded
  // Device_Basic | Device_CDROM, and initModules() returns early if *any* bit
  // of the requested mask is already loaded, which would skip loading the
  // storage drivers and padman entirely. Basic modules are always reloaded
  // on every IOP reset anyway.
  DeviceType scanMask = Device_Games; // Device_Games loads padman
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

  int res = initModules(scanMask);
  if (res) {
    msg("Games: Failed to initialize devices: %d\n", res);
    sleep(3);
    freeGamesConfig(cfg);
    ExecOSD(0, NULL);
    return res;
  }

  scanAllDevices(cfg);

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
