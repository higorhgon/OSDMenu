#include "settings.h"
#include "defaults.h"
#include "gs.h"
#include <stdlib.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>

PatcherSettings settings;

// Defined in common/defaults.h
char xfromConfigPath[] = "xfrom:" HOSD_CONF_PATH;
#ifndef HOSD
char cnfPath[] = CONF_PATH;
#else
char cnfPath[] = "pfs0:" HOSD_CONF_PATH;
#endif

#ifdef EMBED_CNF
uint8_t *embedded_cnf_addr = embedded_cnf;
#endif

#ifdef GAMES_MENU
static int gamesEnabled = 0; // Set when any games_device_* key is enabled
#endif

// getCNFString is the main CNF parser called for each CNF variable in a CNF file.
// Input and output data is handled via its pointer parameters.
// The return value flags 'false' when no variable is found. (normal at EOF)
// pToken/pName/pValue are unsigned char* rather than char* so that bytes >= 0x80
// (e.g. a UTF-8 multi-byte character in a comment or menu item name) compare as
// large positive values instead of negative ones on toolchains where char is
// signed (the default for the MIPS ps2dev toolchain) — otherwise such a byte can
// satisfy `> '\0'` as false and get the "seek line end"/"skip whitespace" loops
// below stuck forever without ever reaching '\0'/'\r'/'\n'.
int getCNFString(char **cnfPos, char **name, char **value) {
  unsigned char *pName, *pValue, *pToken = (unsigned char *)*cnfPos;

nextLine:
  while ((*pToken <= ' ') && (*pToken > '\0'))
    pToken += 1; // Skip leading whitespace, if any
  if (*pToken == '\0')
    return 0; // Exit at EOF

  pName = pToken;      // Current pos is potential name
  if (*pToken < 'A') { // If line is a comment line
    while ((*pToken != '\r') && (*pToken != '\n') && (*pToken > '\0'))
      pToken += 1; // Seek line end
    goto nextLine; // Go back to try next line
  }

  while ((*pToken >= 'A') || ((*pToken >= '0') && (*pToken <= '9')))
    pToken += 1; // Seek name end
  if (*pToken == '\0')
    return 0; // Exit at EOF

  while ((*pToken <= ' ') && (*pToken > '\0'))
    *pToken++ = '\0'; // Zero and skip post-name whitespace
  if (*pToken != '=')
    return 0;       // Exit (syntax error) if '=' missing
  *pToken++ = '\0'; // Zero '=' (possibly terminating name)

  while ((*pToken <= ' ') && (*pToken > '\0')      // Skip pre-value whitespace, if any
         && (*pToken != '\r') && (*pToken != '\n') // but do not pass the end of the line
         && (*pToken != '\7')                      // allow ctrl-G (BEL) in value
  )
    pToken += 1;
  if (*pToken == '\0')
    return 0;      // Exit at EOF
  pValue = pToken; // Current pos is potential value

  while ((*pToken != '\r') && (*pToken != '\n') && (*pToken != '\0'))
    pToken += 1; // Seek line end
  if (*pToken != '\0')
    *pToken++ = '\0'; // Terminate value (passing if not EOF)
  while ((*pToken <= ' ') && (*pToken > '\0'))
    pToken += 1; // Skip following whitespace, if any

  *cnfPos = (char *)pToken; // Set new CNF file position
  *name = (char *)pName;    // Set found variable name
  *value = (char *)pValue;  // Set found variable value
  return 1;
}

// Loads config file from the memory card
int loadConfig(void) {
  // Load CNF at a fixed address, guaranteed not to be used by OSDMenu
  // because the memory range is limited to <0x100000 in the linker script
  char *cnfPos = (void *)0x1000000;

#ifdef EMBED_CNF
  // Embedded config file.
  // Copy it to cnfPos to avoid the parser mangling the config
  memcpy(cnfPos, embedded_cnf_addr, size_embedded_cnf);
  size_t cnfSize = size_embedded_cnf;
#else
// Read config from the HDD or one of the memory cards
#ifndef HOSD
  if (settings.mcSlot == 1)
    cnfPath[2] = '1';
  else
    cnfPath[2] = '0';

  // Try XFROM first
  int fd = fioOpen(xfromConfigPath, FIO_O_RDONLY);
  if (fd >= 0)
    // XFROM device number is 2
    settings.mcSlot = 2;
  else
    // Try to open config from MC
    fd = fioOpen(cnfPath, FIO_O_RDONLY);
  if (fd < 0) {
    // If CNF doesn't exist on boot MC, try the other slot
    if (settings.mcSlot == 1)
      cnfPath[2] = '0';
    else
      cnfPath[2] = '1';
    if ((fd = fioOpen(cnfPath, FIO_O_RDONLY)) < 0)
      return -1;

    // Change mcSlot to point to the device contaning the config file
    settings.mcSlot = cnfPath[2] - '0';
  }

#else
  int fd = fioOpen(cnfPath, FIO_O_RDONLY);
  if (fd < 0)
    return -1;
#endif

  // Read the CNF as one long string
  size_t cnfSize = fioLseek(fd, 0, FIO_SEEK_END);
  fioLseek(fd, 0, FIO_SEEK_SET);
  fioRead(fd, cnfPos, cnfSize);
  fioClose(fd);
#endif
  cnfPos[cnfSize] = '\0'; // Terminate the CNF string

  char *name, *value;
  char valueBuf[4];
  int i, j;
  while (getCNFString(&cnfPos, &name, &value)) {
    if (!strcmp(name, "OSDSYS_menu_x")) {
      settings.menuX = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_menu_y")) {
      settings.menuY = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_enter_x")) {
      settings.enterX = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_enter_y")) {
      settings.enterY = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_version_x")) {
      settings.versionX = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_version_y")) {
      settings.versionY = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_cursor_max_velocity")) {
      settings.cursorMaxVelocity = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_cursor_acceleration")) {
      settings.cursorAcceleration = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_left_cursor")) {
      strncpy(settings.leftCursor, value, (sizeof(settings.leftCursor) / sizeof(char)) - 1);
      continue;
    }
    if (!strcmp(name, "OSDSYS_right_cursor")) {
      strncpy(settings.rightCursor, value, (sizeof(settings.rightCursor) / sizeof(char)) - 1);
      continue;
    }
    if (!strcmp(name, "OSDSYS_menu_top_delimiter")) {
      strncpy(settings.menuDelimiterTop, value, (sizeof(settings.menuDelimiterTop) / sizeof(char)) - 1);
      continue;
    }
    if (!strcmp(name, "OSDSYS_menu_bottom_delimiter")) {
      strncpy(settings.menuDelimiterBottom, value, (sizeof(settings.menuDelimiterBottom) / sizeof(char)) - 1);
      continue;
    }
    if (!strcmp(name, "OSDSYS_num_displayed_items")) {
      settings.displayedItems = atoi(value);
      continue;
    }
    if (!strcmp(name, "OSDSYS_selected_color")) {
      for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
          valueBuf[j] = value[j];
        }
        settings.colorSelected[i] = strtol(valueBuf, NULL, 16);
        value += 5;
      }
      continue;
    }
    if (!strcmp(name, "OSDSYS_unselected_color")) {
      for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
          valueBuf[j] = value[j];
        }
        settings.colorUnselected[i] = strtol(valueBuf, NULL, 16);
        value += 5;
      }
      continue;
    }
    if (!strncmp(name, "name_OSDSYS_ITEM_", 17) && (strlen(value) > 0)) {
      // Ignore all subsequent entries if the number of items has been maxed out
      if (settings.menuItemCount == CUSTOM_ITEMS)
        continue;

      // Process only non-empty values
      j = atoi(&name[17]);
      strncpy(settings.menuItemName[settings.menuItemCount], value, NAME_LEN - 1);
      settings.menuItemIdx[settings.menuItemCount] = j;
      settings.menuItemCount++;
      continue;
    }
#ifdef GAMES_MENU
    if (!strncmp(name, "games_device_", 13)) {
      if (atoi(value))
        gamesEnabled = 1;
      continue;
    }
#endif
#ifndef HOSD
    if (!strcmp(name, "path_DKWDRV_ELF")) {
      if (strlen(value) < 4 || strncmp(value, "mc", 2))
        continue; // Accept only memory card paths

      strncpy(settings.dkwdrvPath, value, (sizeof(settings.dkwdrvPath) / sizeof(char)) - 1);
      continue;
    }
#endif
    if (!strcmp(name, "OSDSYS_video_mode")) {
      if (!strcmp(value, "AUTO"))
        settings.videoMode = 0;
      else if (!strcmp(value, "NTSC"))
        settings.videoMode = GS_MODE_NTSC;
      else if (!strcmp(value, "PAL"))
        settings.videoMode = GS_MODE_PAL;
      else if (!strcmp(value, "480p"))
        settings.videoMode = GS_MODE_DTV_480P;
      else if (!strcmp(value, "1080i"))
        settings.videoMode = GS_MODE_DTV_1080I;

      continue;
    }
    if (!strcmp(name, "OSDSYS_region")) {
      if (!strcmp(value, "AUTO"))
        settings.region = OSD_REGION_DEFAULT;
      else if (!strcmp(value, "jap"))
        settings.region = OSD_REGION_JAP;
      else if (!strcmp(value, "usa"))
        settings.region = OSD_REGION_USA;
      else if (!strcmp(value, "eur"))
        settings.region = OSD_REGION_EUR;

      continue;
    }
    if (!strcmp(name, "OSDSYS_boot")) {
      if (!strcmp(value, "opening"))
        settings.boot = OSD_BOOT_OPENING;
      else if (!strcmp(value, "clock"))
        settings.boot = OSD_BOOT_CLOCK;
      else if (!strcmp(value, "browser"))
        settings.boot = OSD_BOOT_BROWSER;
    }
    if (!strcmp(name, "OSDSYS_custom_menu")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_CUSTOM_MENU;
      else
        settings.patcherFlags &= ~(FLAG_CUSTOM_MENU);
      continue;
    }
    if (!strcmp(name, "OSDSYS_scroll_menu")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_SCROLL_MENU;
      else
        settings.patcherFlags &= ~(FLAG_SCROLL_MENU);
      continue;
    }
    if (!strcmp(name, "OSDSYS_Skip_Disc")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_SKIP_DISC;
      else
        settings.patcherFlags &= ~(FLAG_SKIP_DISC);
      continue;
    }
    if (!strcmp(name, "cdrom_skip_ps2logo")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_SKIP_PS2_LOGO;
      else
        settings.patcherFlags &= ~(FLAG_SKIP_PS2_LOGO);
      continue;
    }
    if (!strcmp(name, "cdrom_disable_gameid")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_DISABLE_GAMEID;
      else
        settings.patcherFlags &= ~(FLAG_DISABLE_GAMEID);
      continue;
    }
    if (!strcmp(name, "cdrom_use_dkwdrv")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_USE_DKWDRV;
      else
        settings.patcherFlags &= ~(FLAG_USE_DKWDRV);
      continue;
    }
    if (!strcmp(name, "ps1drv_enable_fast")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_PS1DRV_FAST;
      else
        settings.patcherFlags &= ~(FLAG_PS1DRV_FAST);
      continue;
    }
    if (!strcmp(name, "ps1drv_enable_smooth")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_PS1DRV_SMOOTH;
      else
        settings.patcherFlags &= ~(FLAG_PS1DRV_SMOOTH);
      continue;
    }
    if (!strcmp(name, "ps1drv_use_ps1vn")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_PS1DRV_USE_VN;
      else
        settings.patcherFlags &= ~(FLAG_PS1DRV_USE_VN);
      continue;
    }
    if (!strcmp(name, "app_gameid")) {
      if (atoi(value))
        settings.patcherFlags |= FLAG_APP_GAMEID;
      else
        settings.patcherFlags &= ~(FLAG_APP_GAMEID);
      continue;
    }
  }

  // Clean up
  memset(cnfPos, 0, cnfSize);

#ifdef GAMES_MENU
  // Add "Games >" as the first custom entry when any games_device_* is enabled
  if (gamesEnabled && (settings.menuItemCount < CUSTOM_ITEMS)) {
    memmove(settings.menuItemName[1], settings.menuItemName[0], settings.menuItemCount * NAME_LEN);
    memmove(&settings.menuItemIdx[1], &settings.menuItemIdx[0], settings.menuItemCount * sizeof(settings.menuItemIdx[0]));
    strcpy(settings.menuItemName[0], "Games >");
    settings.menuItemIdx[0] = GAMES_MENU_IDX;
    settings.menuItemCount++;
#ifndef HOSD
    settings.gamesItemIdx = GAMES_MENU_IDX;
#endif
  }
#endif
  return 0;
}

#ifndef HOSD
// Loads game names for the games submenu from GAMES_CACHE_PATH (written by the
// launcher after a scan) into menuItemName, right after the regular items
void loadGamesCache(void) {
  if (settings.gamesItemIdx < 0)
    return;

  // Two slots are reserved for the "back" and "refresh" labels.
  // Without room for them, the entry falls back to the launcher's own list.
  int maxGames = CUSTOM_ITEMS - settings.menuItemCount - 2;
  if (maxGames < 0) {
    settings.gamesItemIdx = -1;
    return;
  }

  char path[] = GAMES_CACHE_PATH;
  if (settings.mcSlot == 1)
    path[2] = '1';

  int fd = fioOpen(path, FIO_O_RDONLY);
  if (fd < 0)
    return;

  // Same scratch area as loadConfig(), which is done with it by now
  char *cnfPos = (void *)0x1000000;
  char *cnfStart = cnfPos;
  size_t cnfSize = fioLseek(fd, 0, FIO_SEEK_END);
  fioLseek(fd, 0, FIO_SEEK_SET);
  fioRead(fd, cnfPos, cnfSize);
  fioClose(fd);
  cnfPos[cnfSize] = '\0';

  char *name, *value;
  while (getCNFString(&cnfPos, &name, &value)) {
    if (strcmp(name, "game") || (settings.gamesCount >= maxGames))
      continue;

    strncpy(settings.menuItemName[settings.menuItemCount + settings.gamesCount], value, NAME_LEN - 1);
    settings.gamesCount++;
  }
  memset(cnfStart, 0, cnfSize);

  strcpy(settings.menuItemName[settings.menuItemCount + settings.gamesCount], "< Back");
  strcpy(settings.menuItemName[settings.menuItemCount + settings.gamesCount + 1], "Refresh list");
  settings.gamesCacheLoaded = 1;
}
#endif

// Initializes static variables
void initVariables() {
  // Init ROMVER
  int fdn = 0;
  if ((fdn = fioOpen("rom0:ROMVER", FIO_O_RDONLY)) > 0) {
    fioRead(fdn, settings.romver, 14);
    settings.romver[14] = '\0';
    fioClose(fdn);
  }
}

// Loads defaults
void initConfig(void) {
  settings.patcherFlags = FLAG_CUSTOM_MENU | FLAG_SCROLL_MENU | FLAG_SKIP_DISC;
  settings.videoMode = 0;
  settings.region = OSD_REGION_DEFAULT;
  settings.boot = OSD_BOOT_DEFAULT;
  settings.menuX = 320;
  settings.menuY = 110;
  settings.enterX = 30;
  settings.enterY = -1;
  settings.versionX = -1;
  settings.versionY = -1;
  settings.cursorMaxVelocity = 1000;
  settings.cursorAcceleration = 100;
  settings.leftCursor[0] = '\0';
  settings.rightCursor[0] = '\0';
  settings.menuDelimiterTop[0] = '\0';
  settings.menuDelimiterBottom[0] = '\0';
  settings.colorSelected[0] = 0x10;
  settings.colorSelected[1] = 0x80;
  settings.colorSelected[2] = 0xe0;
  settings.colorSelected[3] = 0x80;
  settings.colorUnselected[0] = 0x33;
  settings.colorUnselected[1] = 0x33;
  settings.colorUnselected[2] = 0x33;
  settings.colorUnselected[3] = 0x80;
  settings.displayedItems = 7;
  for (int i = 0; i < CUSTOM_ITEMS; i++) {
    settings.menuItemName[i][0] = '\0';
    settings.menuItemIdx[i] = 0;
  }
  settings.menuItemCount = 0;
  settings.romver[0] = '\0';
#ifndef HOSD
  settings.dkwdrvPath[0] = '\0'; // Can be null
  settings.mcSlot = 0;
  settings.gamesItemIdx = -1;
  settings.gamesCount = 0;
  settings.gamesCacheLoaded = 0;
#endif

  initVariables();
}
