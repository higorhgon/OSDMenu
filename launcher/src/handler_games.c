#include "common.h"
#include "defaults.h"
#include "eigr.h"
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
#ifdef SMB
#include <ps2ips.h>
#include <ps2smb.h>
#endif

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
  uint32_t played;                  // "played" counter carried over from the previous cache
  int fav;                          // Favorite, carried over from the previous cache
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
#ifdef UDPFS
    {Device_UDPFS, "udpfs:", "udpfs", 1},
#endif
#ifdef SMB
    // The mountpoint is smbRoot, which includes OPL's ETH prefix. "smb" games are launched via OPL
    {Device_SMB, "smb0:", "smb", 1},
#endif
};
#define GAMES_DEVICE_COUNT (sizeof(gamesDevices) / sizeof(gamesDevices[0]))

// Network devices are scanned separately, each after its own IOP reset:
// UDPFS and SMB use different drivers for the same network adapter
#define GAMES_NETWORK_DEVICES (Device_UDPFS | Device_SMB)

#ifdef SMB
#define SMB_MOUNTPOINT "smb0:"
static char smbRoot[80] = SMB_MOUNTPOINT; // SMB_MOUNTPOINT + OPL's ETH prefix
#endif

static GameEntry gameList[GAMES_MAX_ENTRIES];
static int gameCount = 0;
static int gameListTruncated = 0;

// Diagnostic info captured during the scan, shown on-screen when no games
// are found so a bad OSDMENU.CNF/folder layout can be diagnosed without a
// serial debug cable
#define GAMES_MAX_DIAG 8
typedef struct {
  char mountpoint[80];
  const char *error; // why a network device couldn't be scanned
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

// Returns 1 if dirPath contains a *.cue file
static int hasCueFile(const char *dirPath) {
  int dfd = fileXioDopen(dirPath);
  if (dfd < 0)
    return 0;

  int found = 0;
  iox_dirent_t dirent;
  while (!found && (fileXioDread(dfd, &dirent) > 0)) {
    if (FIO_S_ISDIR(dirent.stat.mode))
      continue;
    char *ext = strrchr(dirent.name, '.');
    found = (ext && !strcasecmp(ext, ".cue"));
  }
  fileXioDclose(dfd);
  return found;
}

// Scans <mountpoint>/EMBER/games for PS1 games: subfolders with a *.cue file inside.
// The folder name is Ember's launch argument, and Ember looks for it relative to its
// own folder, so games are only listed when <mountpoint>/EMBER/ember.elf exists.
// Sets *emberFound, returns 1 if the games folder was opened.
static int scanEmberFolder(const char *mountpoint, int *emberFound, int *entriesOut) {
  *entriesOut = 0;

  char path[GAMES_REL_PATH_LEN];
  snprintf(path, sizeof(path), "%s/" PSX_EMBER_FOLDER "/" PSX_EMBER_ELF, mountpoint);
  *emberFound = !tryFile(path);
  if (!*emberFound)
    return 0;

  char gamesPath[GAMES_REL_PATH_LEN];
  snprintf(gamesPath, sizeof(gamesPath), "%s/" PSX_EMBER_FOLDER "/games", mountpoint);
  int dfd = fileXioDopen(gamesPath);
  if (dfd < 0)
    return 0;

  iox_dirent_t dirent;
  while (fileXioDread(dfd, &dirent) > 0) {
    if (!FIO_S_ISDIR(dirent.stat.mode) || (dirent.name[0] == '.'))
      continue;
    (*entriesOut)++;

    // A truncated folder name wouldn't launch
    if (((strlen(gamesPath) + strlen(dirent.name) + 2) > GAMES_REL_PATH_LEN) || (strlen(dirent.name) >= GAMES_NAME_LEN))
      continue;
    snprintf(path, sizeof(path), "%s/%s", gamesPath, dirent.name);
    if (!hasCueFile(path))
      continue;

    if (gameCount >= GAMES_MAX_ENTRIES) {
      gameListTruncated = 1;
      break;
    }
    GameEntry *g = &gameList[gameCount++];
    snprintf(g->name, GAMES_NAME_LEN, "%s", dirent.name);
    snprintf(g->path, GAMES_REL_PATH_LEN, "%s", path);
    g->neutrinoDriver = NULL;
    g->media = GameMedia_CD;
    g->id[0] = '\0';
  }
  fileXioDclose(dfd);
  return 1;
}

#ifdef SMB
static DeviceType storageDevice(char *path);

// Reads config (conf_network.cfg), trying both slots for "mc?:" and "mmce?:" paths
static int tryOPLNetConfig(const char *config) {
  char path[GAMES_REL_PATH_LEN];
  snprintf(path, sizeof(path), "%s", config);
  char *colon = strchr(path, ':');
  if (!colon || (colon == path) || (colon[-1] != '?'))
    return readOPLNetConfig(path);

  for (char slot = '0'; slot < '2'; slot++) {
    colon[-1] = slot;
    if (!readOPLNetConfig(path))
      return 0;
  }
  return -ENOENT;
}

// Returns the path of conf_network.cfg next to games_opl_path in path
static int oplNetConfigNextToOPL(GamesConfig *cfg, char *path, size_t pathSize) {
  if (!cfg->oplPath)
    return 0;
  const char *slash = strrchr(cfg->oplPath, '/');
  if (!slash)
    slash = strchr(cfg->oplPath, ':');
  if (!slash)
    return 0;
  snprintf(path, pathSize, "%.*sconf_network.cfg", (int)(slash - cfg->oplPath + 1), cfg->oplPath);
  return 1;
}

// Returns the storage driver needed to read OPL's network settings
static DeviceType smbConfigDevice(GamesConfig *cfg) {
  if (cfg->smbConfigPath)
    return storageDevice(cfg->smbConfigPath);
  return cfg->oplPath ? storageDevice(cfg->oplPath) : Device_None;
}

// Reads OPL's network settings from games_smb_config, from the OPL folder
// (games_opl_path) or from mc?:/OPL/, and sets smbRoot to the games folder
static int loadSMBConfig(GamesConfig *cfg) {
  char path[GAMES_REL_PATH_LEN];
  int res = -ENOENT;
  if (cfg->smbConfigPath)
    res = tryOPLNetConfig(cfg->smbConfigPath);
  else {
    if (oplNetConfigNextToOPL(cfg, path, sizeof(path)))
      res = tryOPLNetConfig(path);
    if (res)
      res = tryOPLNetConfig("mc?:/OPL/conf_network.cfg");
  }
  if (res)
    return res;

  // smbman accepts both separators
  const char *prefix = oplNetConfig.ethPrefix;
  while ((*prefix == '/') || (*prefix == '\\'))
    prefix++;
  if (*prefix)
    snprintf(smbRoot, sizeof(smbRoot), SMB_MOUNTPOINT "/%s", prefix);
  else
    strcpy(smbRoot, SMB_MOUNTPOINT);
  int len = strlen(smbRoot);
  while ((len > 0) && ((smbRoot[len - 1] == '/') || (smbRoot[len - 1] == '\\')))
    smbRoot[--len] = '\0';
  return 0;
}

// Enables DHCP on the ps2ip-nm interface and waits for an address
static int smbWaitDHCP(void) {
  t_ip_info info;
  ps2ip_init();
  if (ps2ip_getconfig("sm0", &info) < 0)
    return -EIO;

  memset(&info.ipaddr, 0, sizeof(info.ipaddr));
  memset(&info.netmask, 0, sizeof(info.netmask));
  memset(&info.gw, 0, sizeof(info.gw));
  info.dhcp_enabled = 1;
  if (ps2ip_setconfig(&info) < 0)
    return -EIO;

  for (int i = 0; i < 20; i++) {
    sleep(1);
    if ((ps2ip_getconfig("sm0", &info) >= 0) && info.ipaddr.s_addr)
      return 0;
  }
  return -ETIMEDOUT;
}

// Logs on to the SMB server and opens the share from OPL's network settings,
// the same way OPL does. Returns NULL on success or an error description.
// Needs initModules(Device_SMB) to be called first.
static const char *smbConnect(void) {
  OPLNetConfig *c = &oplNetConfig;
  if (c->useNBNS || !c->smbIP[0])
    return "smb_ip nao definido (nomes NetBIOS nao suportados)";
  if (!c->smbShare[0])
    return "smb_share nao definido";
  if (c->dhcp && smbWaitDHCP())
    return "DHCP falhou";

  static smbLogOn_in_t logon;
  static smbOpenShare_in_t openShare;
  memset(&logon, 0, sizeof(logon));
  memset(&openShare, 0, sizeof(openShare));
  strncpy(logon.serverIP, c->smbIP, sizeof(logon.serverIP) - 1);
  logon.serverPort = c->smbPort;
  strncpy(logon.User, c->smbUser[0] ? c->smbUser : "GUEST", sizeof(logon.User) - 1);
  logon.PasswordType = openShare.PasswordType = NO_PASSWORD;
  if (c->smbPass[0]) {
    static smbGetPasswordHashes_in_t passwd;
    static smbGetPasswordHashes_out_t hashes;
    strncpy(passwd.password, c->smbPass, sizeof(passwd.password) - 1);
    if (fileXioDevctl(SMB_MOUNTPOINT, SMB_DEVCTL_GETPASSWORDHASHES, &passwd, sizeof(passwd), &hashes, sizeof(hashes)) == 0) {
      memcpy(logon.Password, &hashes, sizeof(hashes));
      memcpy(openShare.Password, &hashes, sizeof(hashes));
      logon.PasswordType = openShare.PasswordType = HASHED_PASSWORD;
    } else {
      strncpy(logon.Password, c->smbPass, sizeof(logon.Password) - 1);
      strncpy(openShare.Password, c->smbPass, sizeof(openShare.Password) - 1);
      logon.PasswordType = openShare.PasswordType = PLAINTEXT_PASSWORD;
    }
  }

  // The network link can take a few seconds to come up after loading the drivers
  int res = -SMB_DEVCTL_LOGON_ERR_CONN;
  for (int i = 0; (i < 10) && (res == -SMB_DEVCTL_LOGON_ERR_CONN); i++) {
    if (i)
      sleep(1);
    res = fileXioDevctl(SMB_MOUNTPOINT, SMB_DEVCTL_LOGON, &logon, sizeof(logon), NULL, 0);
  }
  if (res < 0)
    return (res == -SMB_DEVCTL_LOGON_ERR_CONN) ? "servidor SMB nao respondeu" : "login SMB falhou";

  strncpy(openShare.ShareName, c->smbShare, sizeof(openShare.ShareName) - 1);
  if (fileXioDevctl(SMB_MOUNTPOINT, SMB_DEVCTL_OPENSHARE, &openShare, sizeof(openShare), NULL, 0) < 0)
    return "falha ao abrir smb_share";
  return NULL;
}
#endif

// Returns 1 if the device is enabled for the kind of games being scanned
static int isDeviceEnabled(GamesConfig *cfg, DeviceType device) {
  if (cfg->kind == GamesKind_PSX)
    return ((device == Device_USB) && cfg->psxUseUSB) || ((device == Device_MX4SIO) && cfg->psxUseMX4SIO) ||
           ((device == Device_MMCE) && cfg->psxUseMMCE);
  return ((device == Device_USB) && cfg->useUSB) || ((device == Device_MX4SIO) && cfg->useMX4SIO) || ((device == Device_MMCE) && cfg->useMMCE) ||
         ((device == Device_UDPFS) && cfg->useUDPFS) || ((device == Device_SMB) && cfg->useSMB);
}

static int gameEntryCompare(const void *a, const void *b) { return strcasecmp(((const GameEntry *)a)->name, ((const GameEntry *)b)->name); }

// Adds a diagnostic entry for mountpoint, returns NULL if there's no room left
static GamesScanDiag *addScanDiag(const char *mountpoint) {
  if (scanDiagCount >= GAMES_MAX_DIAG)
    return NULL;
  GamesScanDiag *diag = &scanDiag[scanDiagCount++];
  memset(diag, 0, sizeof(*diag));
  snprintf(diag->mountpoint, sizeof(diag->mountpoint), "%s", mountpoint);
  return diag;
}

// Probes and scans every enabled device/mountpoint combination in devices
static void scanDevices(GamesConfig *cfg, DeviceType devices) {
  for (size_t i = 0; i < GAMES_DEVICE_COUNT; i++) {
    const GamesDeviceEntry *dev = &gamesDevices[i];
    if (!(dev->device & devices) || !isDeviceEnabled(cfg, dev->device))
      continue;

    for (int idx = 0; idx < dev->mountCount; idx++) {
      if (scanDiagCount >= GAMES_MAX_DIAG)
        break;
      GamesScanDiag *diag = &scanDiag[scanDiagCount++];
      memset(diag, 0, sizeof(*diag));
      snprintf(diag->mountpoint, sizeof(diag->mountpoint), dev->mountFmt, idx);
#ifdef SMB
      if (dev->device == Device_SMB)
        snprintf(diag->mountpoint, sizeof(diag->mountpoint), "%s", smbRoot);
#endif

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
      if (cfg->kind == GamesKind_PSX) {
        // cdOpened: ember.elf found, dvdOpened: EMBER/games opened
        diag->dvdOpened = scanEmberFolder(diag->mountpoint, &diag->cdOpened, &diag->dvdEntries);
        continue;
      }
      diag->cdOpened = scanFolder(diag->mountpoint, cfg->cdFolder, GameMedia_CD, dev->neutrinoDriver, &diag->cdEntries);
      diag->dvdOpened = scanFolder(diag->mountpoint, cfg->dvdFolder, GameMedia_DVD, dev->neutrinoDriver, &diag->dvdEntries);
    }
  }
}

// Prints what the scan actually saw, per mountpoint, so a misconfigured
// folder/device can be diagnosed from the screen alone (no serial cable)
static void printScanDiagnostics(GamesConfig *cfg) {
  scr_printf(" Nenhum jogo encontrado. Diagnostico:\n");
  if (scanDiagCount == 0) {
    scr_printf(" Nenhum dispositivo habilitado em %s_device_*\n", (cfg->kind == GamesKind_PSX) ? "psx" : "games");
    return;
  }
  for (int i = 0; i < scanDiagCount; i++) {
    GamesScanDiag *d = &scanDiag[i];
    if (d->error) {
      scr_printf(" %s: %s\n", d->mountpoint, d->error);
      continue;
    }
    if (!d->available && !d->cdOpened && !d->dvdOpened) {
      scr_printf(" %s nao respondeu (dispositivo ausente/nao pronto)\n", d->mountpoint);
      continue;
    }
    if (cfg->kind == GamesKind_PSX) {
      scr_printf(" %s " PSX_EMBER_FOLDER "/" PSX_EMBER_ELF "=%s " PSX_EMBER_FOLDER "/games=%s(%d)\n", d->mountpoint, d->cdOpened ? "ok" : "n/a",
                 d->dvdOpened ? "ok" : "n/a", d->dvdEntries);
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
  case Device_UDPFS:
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
// the ISO name. mode is "bdm" for USB/MX4SIO, "mmce" (with slot "0"/"1") for MMCE or
// "smb" for SMB, which need a RiptOPL build with MMCE/SMB autolaunch support.
static int canLaunchWithOPL(char *isoPath, const char *id, const char **media, const char **fileName, const char **mode, char *slot) {
  DeviceType type = guessDeviceType(isoPath);
  if (!id[0])
    return 0;
#ifdef SMB
  if (!strncmp(isoPath, "smb", 3)) {
    // OPL's games folder is <share>/<ETH prefix>/CD|DVD, check the folder the ISO is in
    *mode = "smb";
    char *name = strrchr(isoPath, '/');
    if (!name)
      return 0;
    char *folder = name;
    while ((folder > isoPath) && (folder[-1] != '/') && (folder[-1] != ':'))
      folder--;
    if (((name - folder) == 2) && !strncasecmp(folder, "CD", 2))
      *media = "CD";
    else if (((name - folder) == 3) && !strncasecmp(folder, "DVD", 3))
      *media = "DVD";
    else
      return 0;
    *fileName = name + 1;
    return 1;
  }
#endif
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
  if (!strcmp(bsd, "smb")) {
    // Games on SMB shares can only be launched via OPL
    if (!cfg->oplPath) {
      msg("Games: games_opl_path is not set in OSDMENU.CNF\n");
      return;
    }
    if (!canLaunchWithOPL(isoPath, id, &media, &fileName, &oplMode, oplSlot)) {
      msg("Games: OPL can't launch %s (the ISO must be in CD/DVD with a known title ID)\n", isoPath);
      return;
    }
    useOPL = 1;
  } else if (cfg->useOPL) {
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
  // OPL's MMCE mode switches the card itself, taking its per-game VMC settings into account.
  // Its bdm/smb autolaunch modes don't load the MMCE driver, so the card is switched here
#ifdef MMCE
  int setGameID = cfg->mmceGameID && id[0] && !(useOPL && !strcmp(oplMode, "mmce"));
  if (setGameID)
    mask |= Device_MMCE;
#endif
  if (mask && initModules(mask))
    return;

#ifdef MMCE
  if (setGameID)
    mmceSetGameID(elfPath, id);
#endif

  if (useOPL) {
    // opl.elf <ISO name> <title ID> <CD/DVD> bdm
    // opl.elf <ISO name> <title ID> <CD/DVD> mmce <slot>
    // opl.elf <ISO name> <title ID> <CD/DVD> smb
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

// Returns the ELF the Ember in-game reset returns to: the first of games_return_path, the path OSDMenu
// was started from and GAMES_DEFAULT_RETURN_PATH that is on a memory card and exists, since only the ROM memory
// card drivers are loaded after the IOP reset. Returns NULL if there's none
static char *igrReturnPath(GamesConfig *cfg) {
  static char path[GAMES_REL_PATH_LEN];
  const char *candidates[] = {cfg->returnPath, cfg->patcherPath, GAMES_DEFAULT_RETURN_PATH};
  for (int i = 0; i < 3; i++) {
    if (!candidates[i] || strncmp(candidates[i], "mc", 2) || (strlen(candidates[i]) >= EIGR_RETURN_PATH_MAX))
      continue;
    snprintf(path, sizeof(path), "%s", candidates[i]);
    for (char slot = '0'; slot < '2'; slot++) {
      if ((candidates[i][2] == '?') || (candidates[i][2] == slot)) {
        path[2] = slot;
        if (!tryFile(path))
          return path;
      }
    }
  }
  return NULL;
}

// Launches a PS1 game with Ember. gamePath is the game folder, <device>:/EMBER/games/<name>.
// Ember takes the folder name as its only argument and finds it relative to argv[0],
// using the storage drivers that are already loaded. Only returns on failure.
// Ember doesn't reset the IOP, so igr.irx (psx_igr) keeps watching the controller while the game runs
static void launchEmber(GamesConfig *cfg, const char *gamePath) {
  const char *games = strstr(gamePath, "/games/");
  if (!games) {
    msg("PSX: invalid game path %s\n", gamePath);
    return;
  }

  char elfPath[GAMES_REL_PATH_LEN];
  char folder[GAMES_NAME_LEN];
  snprintf(elfPath, sizeof(elfPath), "%.*s/" PSX_EMBER_ELF, (int)(games - gamePath), gamePath);
  snprintf(folder, sizeof(folder), "%s", games + 7);

  DeviceType mask = storageDevice(elfPath);
  if (mask && initModules(mask))
    return;
  // In-game reset: igr.irx reads the controller, and eIGR, installed by the loader, returns to OSDMenu.
  // Without them, the game still starts
  if (cfg->psxIGR) {
    if (loadIGRModule() < 0)
      DPRINTF("PSX: failed to load igr.irx\n");
    else
      settings.igrReturnPath = igrReturnPath(cfg);
  }

  char *argv[] = {elfPath, folder};
  launchGamesELF(2, argv);
}

// Launches the selected entry of the full-screen list. Only returns on failure.
static void launchSelected(GamesConfig *cfg, int index) {
  GameEntry *g = &gameList[index];

  scr_setXY(0, GAMES_LIST_START_ROW + GAMES_VISIBLE_ROWS + 2);
  scr_printf(" Iniciando %s...\n", g->name);

  if (cfg->kind == GamesKind_PSX)
    launchEmber(cfg, g->path);
  else
    launchGame(cfg, g->neutrinoDriver, g->path, g->id);

  scr_printf(" Falha ao iniciar %s\n", g->name);
  sleep(2);
}

// Returns the cache path for cfg->kind on the memory card OSDMENU.CNF was loaded from
static void getGamesCachePath(GamesConfig *cfg, char *path) {
  strcpy(path, (cfg->kind == GamesKind_PSX) ? PSX_CACHE_PATH : GAMES_CACHE_PATH);
  path[2] = (settings.mcHint == 1) ? '1' : '0';
}

// A "key = value" line of a cache file
typedef struct {
  char *key;
  char *value;
} CacheLine;

// Reads the cache file and splits it into lines. The keys and values point into the returned buffer.
// Returns NULL if the file can't be read; *lines must be freed along with the buffer
static char *readGamesCache(const char *path, CacheLine **lines, int *lineCount) {
  *lines = NULL;
  *lineCount = 0;

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  int size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  char *buf = (size > 0) ? malloc(size + 1) : NULL;
  if (!buf || (read(fd, buf, size) != size)) {
    close(fd);
    free(buf);
    return NULL;
  }
  close(fd);
  buf[size] = '\0';

  int maxLines = 1;
  for (int i = 0; i < size; i++)
    if (buf[i] == '\n')
      maxLines++;
  *lines = malloc(maxLines * sizeof(CacheLine));
  if (!*lines) {
    free(buf);
    return NULL;
  }

  char *line = buf;
  while (line && *line && (*lineCount < maxLines)) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = '\0';

    char *value = strchr(line, '=');
    if (value) {
      // Trim "key = value\r"
      char *keyEnd = value;
      while ((keyEnd > line) && isspace((int)keyEnd[-1]))
        keyEnd--;
      *keyEnd = '\0';
      value++;
      while (isspace((int)*value))
        value++;
      value[strcspn(value, "\r\n")] = '\0';
      (*lines)[*lineCount].key = line;
      (*lines)[*lineCount].value = value;
      (*lineCount)++;
    }
    line = next;
  }
  return buf;
}

// Returns the path key of a game ("dvd" for PS2 games, "psx" for PS1 games)
static const char *cachePathKey(GamesConfig *cfg) { return (cfg->kind == GamesKind_PSX) ? "psx" : "dvd"; }

// Writes the scan result to GAMES_CACHE_PATH or PSX_CACHE_PATH, which the patcher reads on boot
// to show the games as an OSDSYS submenu. The file starts with the "sort" order ("name" or "recent")
// chosen in the submenu. Each game is a "game" (display name) line followed by its Neutrino "bsd"
// driver, "dvd" ISO path and optional "id" (title ID) lines for PS2 games, or by its "psx" game folder
// line for PS1 games, and by an optional "played" counter, higher for the most recently played games.
// sortRecent is 1 or 0 to set the sort order, -1 to keep the one from the previous cache, and the
// "played" counters are carried over from the previous cache for the games that are still there.
static int writeGamesCache(GamesConfig *cfg, int sortRecent) {
  char path[32];
  getGamesCachePath(cfg, path);

  // Carry over the sort order and the "played" counters
  CacheLine *lines;
  int lineCount;
  char *oldCache = readGamesCache(path, &lines, &lineCount);
  for (int i = 0; i < gameCount; i++) {
    gameList[i].played = 0;
    gameList[i].fav = 0;
  }
  if (oldCache) {
    const char *pathKey = cachePathKey(cfg);
    const char *gamePath = NULL;
    for (int i = 0; i < lineCount; i++) {
      if (!strcmp(lines[i].key, "sort")) {
        if (sortRecent < 0)
          sortRecent = !strcmp(lines[i].value, "recent");
      } else if (!strcmp(lines[i].key, "game"))
        gamePath = NULL;
      else if (!strcmp(lines[i].key, pathKey))
        gamePath = lines[i].value;
      else if (!strcmp(lines[i].key, "played") && gamePath) {
        for (int j = 0; j < gameCount; j++) {
          if (!strcmp(gameList[j].path, gamePath)) {
            gameList[j].played = strtoul(lines[i].value, NULL, 10);
            break;
          }
        }
      } else if (!strcmp(lines[i].key, "fav") && gamePath && atoi(lines[i].value)) {
        for (int j = 0; j < gameCount; j++) {
          if (!strcmp(gameList[j].path, gamePath)) {
            gameList[j].fav = 1;
            break;
          }
        }
      }
    }
    free(lines);
    free(oldCache);
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0)
    return fd;

  char line[GAMES_NAME_LEN + GAMES_REL_PATH_LEN + 64];
  int res = 0;
  int len = snprintf(line, sizeof(line), "sort = %s\n", (sortRecent > 0) ? "recent" : "name");
  if (write(fd, line, len) != len)
    res = -EIO;

  for (int i = 0; (i < gameCount) && !res; i++) {
    if (cfg->kind == GamesKind_PSX)
      len = snprintf(line, sizeof(line), "game = %s\npsx = %s\n", gameList[i].name, gameList[i].path);
    else
      len = snprintf(line, sizeof(line), "game = %s\nbsd = %s\ndvd = %s\n%s%s%s", gameList[i].name, gameList[i].neutrinoDriver, gameList[i].path,
                     gameList[i].id[0] ? "id = " : "", gameList[i].id, gameList[i].id[0] ? "\n" : "");
    if (len >= (int)sizeof(line))
      len = sizeof(line) - 1;
    if (gameList[i].played && (len < (int)sizeof(line) - 24))
      len += snprintf(&line[len], sizeof(line) - len, "played = %u\n", (unsigned int)gameList[i].played);
    if (gameList[i].fav && (len < (int)sizeof(line) - 12))
      len += snprintf(&line[len], sizeof(line) - len, "fav = 1\n");
    if (write(fd, line, len) != len)
      res = -EIO;
  }
  close(fd);
  return res;
}

// Returns 1 if game idx is set in favMask, the hex mask the patcher passes after 'f' in the mode:
// one digit per four games by cache index, bit 0 for the first one
static int maskHasGame(const char *favMask, int idx) {
  if ((idx / 4) >= (int)strlen(favMask))
    return 0;
  char c = favMask[idx / 4];
  int nibble = isdigit((int)c) ? (c - '0') : ((tolower((int)c) >= 'a') && (tolower((int)c) <= 'f')) ? (tolower((int)c) - 'a' + 10) : 0;
  return (nibble >> (idx % 4)) & 1;
}

// Replaces the "fav" lines of the cache with the favorites in favMask, toggled in the submenu
// and not saved yet. A failure only loses the favorites, so it's not reported
static void applyFavorites(GamesConfig *cfg, const char *favMask) {
  char path[32];
  getGamesCachePath(cfg, path);

  CacheLine *lines;
  int lineCount;
  char *cache = readGamesCache(path, &lines, &lineCount);
  if (!cache)
    return;

  int outSize = 32;
  for (int i = 0; i < lineCount; i++)
    outSize += strlen(lines[i].key) + strlen(lines[i].value) + 4 + 8; // Room for a "fav = 1" line per game

  char *out = malloc(outSize);
  if (out) {
    int len = 0;
    int current = -1;
    for (int i = 0; i < lineCount; i++) {
      if (!strcmp(lines[i].key, "fav"))
        continue;
      if (!strcmp(lines[i].key, "game")) {
        if ((current >= 0) && maskHasGame(favMask, current))
          len += sprintf(&out[len], "fav = 1\n");
        current++;
      }
      len += sprintf(&out[len], "%s = %s\n", lines[i].key, lines[i].value);
    }
    if ((current >= 0) && maskHasGame(favMask, current))
      len += sprintf(&out[len], "fav = 1\n");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      write(fd, out, len);
      close(fd);
    }
    free(out);
  }
  free(lines);
  free(cache);
}

// Marks game idx as the most recently played one and saves the sort order in the cache.
// A failure only loses the "played" order, so it's not reported
static void markGamePlayed(GamesConfig *cfg, int idx, int sortRecent) {
  char path[32];
  getGamesCachePath(cfg, path);

  CacheLine *lines;
  int lineCount;
  char *cache = readGamesCache(path, &lines, &lineCount);
  if (!cache)
    return;

  uint32_t maxPlayed = 0;
  int outSize = 32;
  for (int i = 0; i < lineCount; i++) {
    if ((sortRecent < 0) && !strcmp(lines[i].key, "sort"))
      sortRecent = !strcmp(lines[i].value, "recent");
    if (!strcmp(lines[i].key, "played")) {
      uint32_t played = strtoul(lines[i].value, NULL, 10);
      if (played > maxPlayed)
        maxPlayed = played;
    }
    outSize += strlen(lines[i].key) + strlen(lines[i].value) + 4;
  }

  char *out = malloc(outSize + 32);
  if (out) {
    int len = sprintf(out, "sort = %s\n", (sortRecent > 0) ? "recent" : "name");
    int current = -1;
    for (int i = 0; i < lineCount; i++) {
      if (!strcmp(lines[i].key, "sort"))
        continue;
      if ((current == idx) && !strcmp(lines[i].key, "played"))
        continue; // Replaced below
      if (!strcmp(lines[i].key, "game")) {
        if (current == idx) // The game's lines ended
          len += sprintf(&out[len], "played = %u\n", (unsigned int)(maxPlayed + 1));
        current++;
      }
      len += sprintf(&out[len], "%s = %s\n", lines[i].key, lines[i].value);
    }
    if (current == idx) // Last game
      len += sprintf(&out[len], "played = %u\n", (unsigned int)(maxPlayed + 1));

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      write(fd, out, len);
      close(fd);
    }
    free(out);
  }
  free(lines);
  free(cache);
}

// Launches game idx from the cache without scanning. Only returns on failure.
static int launchCachedGame(GamesConfig *cfg, int idx) {
  char path[32];
  getGamesCachePath(cfg, path);

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
      else if (!strncmp(line, "psx", 3))
        strncpy(isoPath, value, sizeof(isoPath) - 1); // PS1 game folder
    }
  }
  fclose(file);

  if (cfg->kind == GamesKind_PSX) {
    if (isoPath[0] == '\0') {
      msg("PSX: game %d not found in %s, try refreshing the list\n", idx, path);
      return -ENOENT;
    }
    launchEmber(cfg, isoPath);
    msg("PSX: failed to launch %s\n", isoPath);
    return -ENOENT;
  }

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
  if (cfg->smbConfigPath)
    free(cfg->smbConfigPath);
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
  char *reopenArg = (cfg->kind == GamesKind_PSX) ? PSX_REOPEN_ARG : GAMES_REOPEN_ARG;
  freeGamesConfig(cfg);

  for (int i = 0; i < 3; i++) {
    if (candidates[i][0] == '\0')
      continue;

    // ROM and disc paths would not bring OSDMenu back
    DeviceType type = guessDeviceType(candidates[i]);
    if ((type == Device_None) || (type == Device_ROM) || (type == Device_CDROM))
      continue;

    // Only returns if the file can't be launched.
    // "-games"/"-psx" makes OSDMenu reopen the submenu; not passed to games_return_path
    char *argv[] = {candidates[i], reopenArg};
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

  // The mode ends with the submenu's sort order: 'r' (recently played) or 'n' (name),
  // and the favorites that weren't saved yet: 'f' and a hex mask by cache index
  int sortRecent = -1;
  if (modeType) {
    const char *sort = mode + 2;
    while (isdigit((int)*sort))
      sort++;
    if (*sort == 'r')
      sortRecent = 1;
    else if (*sort == 'n')
      sortRecent = 0;
    if (*sort)
      sort++;
    if (*sort == 'f')
      applyFavorites(cfg, sort + 1);
  }

  if ((cfg->kind == GamesKind_PS2) && (modeType != 's') && !cfg->neutrinoPath && !(cfg->oplPath && (cfg->useOPL || cfg->useSMB))) {
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
    cfg->patcherPath = patcherPath;
    markGamePlayed(cfg, atoi(mode + 2), sortRecent);
    int res = launchCachedGame(cfg, atoi(mode + 2));
    sleep(5);
    returnToMenu(cfg, patcherPath);
    return res;
  }

  // Local devices are scanned first, then each network device after its own IOP reset,
  // since UDPFS and SMB drive the network adapter with different drivers.
  // Device_Basic must not be in these masks: handleOSDM() has already loaded
  // Device_Basic | Device_CDROM, and initModules() returns early if *any* bit
  // of the requested mask is already loaded, which would skip loading the
  // storage drivers and padman entirely. Basic modules are always reloaded
  // on every IOP reset anyway. padman is only needed for the full-screen list,
  // so it's loaded with the last group of drivers.
  DeviceType localMask = Device_None;
  DeviceType networkMask = Device_None;
  for (size_t i = 0; i < GAMES_DEVICE_COUNT; i++)
    if (isDeviceEnabled(cfg, gamesDevices[i].device)) {
      if (gamesDevices[i].device & GAMES_NETWORK_DEVICES)
        networkMask |= gamesDevices[i].device;
      else
        localMask |= gamesDevices[i].device;
    }
#ifdef SMB
  // The SMB settings are read from OPL's folder while the local drivers are loaded
  if (networkMask & Device_SMB)
    localMask |= smbConfigDevice(cfg);
#endif
  DeviceType uiMask = (modeType == 's') ? Device_None : Device_Games;

  if (modeType == 's')
    msg((cfg->kind == GamesKind_PSX) ? "Scanning for PS1 games...\n" : "Scanning for games...\n");

  int res = 0;
  if (localMask || !networkMask)
    res = initModules(localMask | (networkMask ? Device_None : uiMask));
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

  gameCount = 0;
  gameListTruncated = 0;
  scanDiagCount = 0;
  scanDevices(cfg, localMask);

#ifdef SMB
  const char *smbError = NULL;
  if ((networkMask & Device_SMB) && loadSMBConfig(cfg))
    smbError = "conf_network.cfg do OPL nao encontrado (games_smb_config)";
#endif
#ifdef UDPFS
  if (networkMask & Device_UDPFS) {
    msg("Connecting to the UDPFS server...\n");
    // padman is loaded with the SMB drivers when both are enabled
    if (initModules(Device_UDPFS | ((networkMask & Device_SMB) ? Device_None : uiMask))) {
      GamesScanDiag *diag = addScanDiag("udpfs:");
      if (diag)
        diag->error = "falha ao iniciar a rede (IPCONFIG.DAT ou OPL/conf_network.cfg)";
    } else
      scanDevices(cfg, Device_UDPFS);
  }
#endif
#ifdef SMB
  if (networkMask & Device_SMB) {
    msg("Connecting to the SMB share...\n");
    if (!smbError && initModules(Device_SMB | uiMask))
      smbError = "falha ao iniciar a rede";
    if (!smbError)
      smbError = smbConnect();
    if (smbError) {
      GamesScanDiag *diag = addScanDiag(smbRoot);
      if (diag)
        diag->error = smbError;
    } else
      scanDevices(cfg, Device_SMB);
  }
#endif

  qsort(gameList, gameCount, sizeof(GameEntry), gameEntryCompare);

  if (modeType == 's') {
    // Write the cache even when empty, so the submenu opens with just
    // "< Back"/"Refresh list" instead of rescanning on every open
    res = writeGamesCache(cfg, sortRecent);
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
    if (cfg->kind == GamesKind_PSX)
      msg("PSX: No games found in " PSX_EMBER_FOLDER "/games\n");
    else
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
