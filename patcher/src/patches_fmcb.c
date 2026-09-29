// FMCB 1.8 OSDSYS patches by Neme
// FMCB 1.9 patches by sp193
#include "patches_fmcb.h"
#include "init.h"
#include "launcher.h"
#include "patches_common.h"
#include "patches_osdmenu.h"
#include "patches_pad.h"
#include "patterns_fmcb.h"
#include "settings.h"
#ifdef GAMES_MENU
#include "defaults.h"
#include "livescan.h"
#include <stddef.h>
#endif
#include <kernel.h>
#include <loadfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t osdMenu[4 + CUSTOM_ITEMS * 2];

struct OSDMenuInfo {
  uint32_t unknown1;
  uint32_t *menuPtr;
  uint32_t entryCount;
  uint32_t unknown2;
  uint32_t currentEntry;
};

static struct OSDMenuInfo *menuInfo = NULL;
#define OSD_MAGIC 0x39390000 // arbitrary number to identify added menu items

#ifndef HOSD
// Games submenu currently replacing the custom menu entries, NULL when the regular entries are shown
static GamesSubmenu *activeMenu = NULL;
// Whether the games submenu only shows "Scanning..." (games_live_scan)
static int liveScanActive = 0;
#endif

// Number of custom entries currently shown after "Browser" and "System Configuration"
static int customItemCount(void) {
#ifndef HOSD
  if (activeMenu && liveScanActive)
    return 1; // "Scanning..."
  if (activeMenu)
    return activeMenu->count + 2; // games + "back" + "refresh"
#endif
  return settings.menuItemCount;
}

// Launches the launcher for the OSDMENU.CNF entry with the given index.
// suffix is appended to the osdm path and interpreted by the launcher's games handler
static void launchMenuItem(int idx, const char *suffix) {
  char item[128] = {0};
#ifdef EMBED_CNF
  // osdm:a<8-char address>:<8-char CNF size>:<3-char idx>
  // Relocate the CNF file to the memory unused by the launcher code
  memcpy((void *)(EXTRA_RELOC_ADDR + size_launcher_elf), (void *)embedded_cnf, size_embedded_cnf);
  sprintf(item, "osdm:a%08lX:%08lX:%d%s", (uint32_t)(EXTRA_RELOC_ADDR + size_launcher_elf), (uint32_t)size_embedded_cnf, idx, suffix);
#else
  // osdm:d<1-char slot>:<3-char idx>
#ifndef HOSD
  int slot = settings.mcSlot;
#else
  int slot = 9;
#endif
  sprintf(item, "osdm:d%d:%d%s", slot, idx, suffix);
#endif

  launchItem(item);
}

#ifndef HOSD
// Launches the submenu entry in the given launcher mode (":s" or ":g<N>").
// The patcher path is appended after '|' so the launcher can return to OSDMenu afterwards
static void launchGamesMode(GamesSubmenu *menu, const char *mode) {
  char suffix[16 + sizeof(settings.bootPath)];
  // The sort order ('r'ecent or 'n'ame) follows the mode so the launcher saves it in the cache
  snprintf(suffix, sizeof(suffix), "%s%c%s%s", mode, menu->sortRecent ? 'r' : 'n', settings.bootPath[0] ? "|" : "", settings.bootPath);
  launchMenuItem(menu->itemIdx, suffix);
}

static uint32_t gamesMenuReturnEntry = 0;
static int menuUsesStringPointers = 0; // Protokernel menus store string pointers instead of string indices

// Points custom menu entry pos (0-based, after "Browser" and "System Configuration") at menuItemName[slot]
static void setMenuEntry(int pos, int slot) {
  if (menuUsesStringPointers) {
    osdMenu[4 + pos * 2] = (uint32_t)settings.menuItemName[slot];
    osdMenu[5 + pos * 2] = (uint32_t)settings.menuItemName[slot];
  } else {
    osdMenu[4 + pos * 2] = OSD_MAGIC + slot;
    osdMenu[5 + pos * 2] = 0;
  }
}

static void closeGamesMenu(void);

// Fills menu->order: games in cache order (by name), or most recently played first,
// followed by the games that were never played in name order
static void sortGames(GamesSubmenu *menu) {
  for (int i = 0; i < menu->count; i++)
    menu->order[i] = i;
  if (!menu->sortRecent)
    return;

  // Stable insertion sort by the "played" counter, highest first
  for (int i = 1; i < menu->count; i++) {
    uint8_t idx = menu->order[i];
    int j = i;
    while ((j > 0) && (menu->played[menu->order[j - 1]] < menu->played[idx])) {
      menu->order[j] = menu->order[j - 1];
      j--;
    }
    menu->order[j] = idx;
  }
}

// Shows "< Back", the games and "Refresh list" of the submenu as the custom entries.
// OSDSYS reads the entry table and cursor from menuInfo every frame, so this takes
// effect immediately without leaving the OSD. The "< Back" label shows the sort order
static void showGamesEntries(GamesSubmenu *menu) {
  int pos = 0;
  strcpy(settings.menuItemName[menu->base + menu->count], menu->sortRecent ? "< Back  [Recent]" : "< Back  [A-Z]");
  setMenuEntry(pos++, menu->base + menu->count); // "< Back"
  for (int i = 0; i < menu->count; i++)
    setMenuEntry(pos++, menu->base + menu->order[i]);
  setMenuEntry(pos++, menu->base + menu->count + 1); // "Refresh list"

  menuInfo->entryCount = 2 + pos;
  menuInfo->currentEntry = 3; // First game, or "Refresh list" when there are none
}

// Replaces the custom entries with the submenu
static void openGamesMenu(GamesSubmenu *menu) {
  gamesMenuReturnEntry = menuInfo->currentEntry;
  sortGames(menu);
  showGamesEntries(menu);
  activeMenu = menu;
}

// Switches the active submenu between sorting by name and by most recently played,
// keeping the cursor on the same game
static void toggleGamesSort(void) {
  GamesSubmenu *menu = activeMenu;
  int pos = (int)menuInfo->currentEntry - 3;
  int selected = ((pos >= 0) && (pos < menu->count)) ? menu->order[pos] : -1;

  menu->sortRecent = !menu->sortRecent;
  sortGames(menu);
  uint32_t cursor = menuInfo->currentEntry;
  showGamesEntries(menu);
  menuInfo->currentEntry = cursor;

  for (int i = 0; (selected >= 0) && (i < menu->count); i++) {
    if (menu->order[i] == selected) {
      menuInfo->currentEntry = 3 + i;
      break;
    }
  }
}

// Moves the cursor a page (OSDSYS_num_displayed_items) up or down within the submenu
static void pageGamesMenu(int direction) {
  int page = (settings.displayedItems > 1) ? settings.displayedItems : 1;
  int entry = (int)menuInfo->currentEntry + direction * page;
  int first = 2;                                // "< Back"
  int last = (int)menuInfo->entryCount - 1;     // "Refresh list"
  if (entry < first)
    entry = first;
  if (entry > last)
    entry = last;
  menuInfo->currentEntry = entry;
}

// Handles the submenu buttons OSDSYS doesn't: Circle/Triangle to go back,
// Square to change the sort order, Left/Right to move a page
static void handleGamesMenuButtons(void) {
  uint16_t pressed = padNewPresses();
  if (!pressed)
    return;

  if (pressed & padBackButtons())
    closeGamesMenu();
  else if (pressed & PADB_SQUARE)
    toggleGamesSort();
  else if (pressed & PADB_RIGHT)
    pageGamesMenu(1);
  else if (pressed & PADB_LEFT)
    pageGamesMenu(-1);
}

// Writes the "< Back" and "Refresh list" labels after the games
static void setGamesLabels(GamesSubmenu *menu, const char *refreshLabel) {
  int base = menu->base + menu->count;
  strcpy(settings.menuItemName[base], "< Back");
  snprintf(settings.menuItemName[base + 1], NAME_LEN, "%s", refreshLabel);
}

#ifdef GAMES_MENU
//
// Experimental live scan (games_live_scan), see livescan.h.
// gamescan.irx is loaded before OSDSYS starts and is controlled by writing
// directly into its LiveScanShared structure in IOP RAM, since the patcher
// can't use SIF RPC while OSDSYS is running.
//
#define LIVESCAN_TIMEOUT_FRAMES (60 * 60) // ~60 seconds
#define LIVESCAN_HEARTBEAT_FRAMES 90      // ~1.5 seconds

static uint32_t liveScanAddr = 0; // LiveScanShared address in the EE's view of IOP RAM
static int liveScanDisabled = 0;  // Set after a failure, "Refresh list" then uses the launcher
static int liveScanFrames = 0;
static uint32_t liveScanHeartbeat = 0;
#define liveScanMenu (&settings.submenus[SUBMENU_GAMES]) // Only PS2 games are scanned live

// IOP RAM is accessed with 32-bit uncached reads and writes only.
// LIVESCAN_IOP_RAM is a kernel segment address, so like PS2SDK's smem_read()/smem_write(),
// every access disables interrupts and switches to kernel mode, since OSDSYS runs in user mode
#define LIVESCAN_FIELD(field) (liveScanAddr + offsetof(LiveScanShared, field))
static uint32_t iopRead(uint32_t addr) {
  DI();
  ee_kmode_enter();
  uint32_t value = *(volatile uint32_t *)addr;
  ee_kmode_exit();
  EI();
  return value;
}

static void iopWrite(uint32_t addr, uint32_t value) {
  DI();
  ee_kmode_enter();
  *(volatile uint32_t *)addr = value;
  ee_kmode_exit();
  EI();
}

// Writes str into a NUL-padded field of size bytes (a multiple of 4)
static void iopWriteString(uint32_t addr, const char *str, int size) {
  for (int i = 0; i < size; i += 4) {
    uint32_t word = 0;
    for (int j = 0; j < 4; j++) {
      char c = ((i + j) < (size - 1)) ? *str : '\0';
      if (c)
        str++;
      word |= (uint32_t)(uint8_t)c << (j * 8);
    }
    iopWrite(addr + i, word);
  }
}

// Reads a string field of size bytes (a multiple of 4) into out
static void iopReadString(uint32_t addr, char *out, int size) {
  for (int i = 0; i < size; i += 4) {
    uint32_t word = iopRead(addr + i);
    for (int j = 0; j < 4; j++)
      out[i + j] = (word >> (j * 8)) & 0xff;
  }
  out[size - 1] = '\0';
}

static uint32_t findLiveScan(void) {
  const uint32_t chunk = 0x10000; // Interrupts are re-enabled between chunks
  uint32_t found = 0;
  for (uint32_t start = LIVESCAN_IOP_RAM; !found && (start < LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE); start += chunk) {
    uint32_t end = start + chunk;
    if (end > LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - sizeof(LiveScanShared))
      end = LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - sizeof(LiveScanShared);

    DI();
    ee_kmode_enter();
    for (uint32_t addr = start; addr < end; addr += 16) {
      volatile uint32_t *w = (volatile uint32_t *)addr;
      if ((w[0] == LIVESCAN_MAGIC0) && (w[1] == LIVESCAN_MAGIC1) && (w[2] == LIVESCAN_MAGIC2) && (w[3] == LIVESCAN_MAGIC3)) {
        found = addr;
        break;
      }
    }
    ee_kmode_exit();
    EI();
  }
  return found;
}

// Ends the live scan with an error shown in the "Refresh list" label.
// The next "Refresh list" falls back to scanning with the launcher
static void failLiveScan(const char *label) {
  liveScanActive = 0;
  liveScanDisabled = 1;
  setGamesLabels(liveScanMenu, label);
  sortGames(liveScanMenu);
  showGamesEntries(liveScanMenu);
  menuInfo->currentEntry = 2 + liveScanMenu->count + 1; // Keep the cursor on "Refresh list"
}

// Returns 1 if gamescan.irx is in IOP RAM. Used right after loading it, before OSDSYS starts
int liveScanProbe(void) { return findLiveScan() != 0; }

// Starts a live scan of the menu and shows "Scanning..." until it's done.
// Returns 0 if the live scan is not enabled for it, so the launcher scans instead
static int startLiveScan(GamesSubmenu *menu) {
  if ((menu != liveScanMenu) || !settings.gamesLiveScan || liveScanDisabled || !settings.gamesUseMMCE)
    return 0;

  if (!menu->cacheLoaded) {
    menu->count = 0;
    setGamesLabels(menu, "Refresh list");
  }

  if (!liveScanAddr)
    liveScanAddr = findLiveScan();
  if (!liveScanAddr) {
    char label[NAME_LEN];
    if (settings.liveScanBoot == LIVESCAN_BOOT_NOT_LOADED)
      strcpy(label, "Refresh list (live scan: modules not loaded)");
    else if (settings.liveScanBoot == LIVESCAN_BOOT_NOT_FOUND)
      strcpy(label, "Refresh list (live scan: loaded but not found)");
    else if (settings.liveScanBoot < 0)
      snprintf(label, sizeof(label), "Refresh list (live scan: load error %d)", settings.liveScanBoot);
    else // Found before OSDSYS started, so OSDSYS must have reset the IOP
      strcpy(label, "Refresh list (live scan: lost after OSD start)");
    failLiveScan(label);
    return 1;
  }

  iopWrite(LIVESCAN_FIELD(devices), LIVESCAN_DEV_MMCE);
  iopWriteString(LIVESCAN_FIELD(cdFolder), settings.gamesCdFolder, LIVESCAN_FOLDER_LEN);
  iopWriteString(LIVESCAN_FIELD(dvdFolder), settings.gamesDvdFolder, LIVESCAN_FOLDER_LEN);
  char cachePath[] = GAMES_CACHE_PATH;
  if (settings.mcSlot == 1)
    cachePath[2] = '1';
  iopWriteString(LIVESCAN_FIELD(cachePath), cachePath, LIVESCAN_PATH_LEN);
  iopWrite(LIVESCAN_FIELD(status), LIVESCAN_STATUS_IDLE);
  iopWrite(LIVESCAN_FIELD(request), 1);

  liveScanHeartbeat = iopRead(LIVESCAN_FIELD(heartbeat));
  liveScanFrames = 0;
  liveScanActive = 1;

  // Show "Scanning..." as the only entry, using the "Refresh list" label slot
  int slot = menu->base + menu->count + 1;
  strcpy(settings.menuItemName[slot], "Scanning...");
  setMenuEntry(0, slot);
  menuInfo->entryCount = 3;
  menuInfo->currentEntry = 2;
  return 1;
}

// Called once per frame while the live scan is running
static void pollLiveScan(void) {
  liveScanFrames++;
  if ((liveScanFrames == LIVESCAN_HEARTBEAT_FRAMES) && (iopRead(LIVESCAN_FIELD(heartbeat)) == liveScanHeartbeat)) {
    failLiveScan("Refresh list (live scan: module not responding)");
    return;
  }

  if ((iopRead(LIVESCAN_FIELD(status)) != LIVESCAN_STATUS_DONE) || iopRead(LIVESCAN_FIELD(request))) {
    if (liveScanFrames > LIVESCAN_TIMEOUT_FRAMES)
      failLiveScan("Refresh list (live scan: timeout)");
    return;
  }

  GamesSubmenu *menu = liveScanMenu;
  int result = (int)iopRead(LIVESCAN_FIELD(result));
  int count = iopRead(LIVESCAN_FIELD(count));
  if (count > menu->max)
    count = menu->max;
  if (count > LIVESCAN_MAX_GAMES)
    count = LIVESCAN_MAX_GAMES;

  if (result < 0) {
    // The cache the launcher reads wasn't updated, so the list can't be launched from
    char label[NAME_LEN];
    snprintf(label, sizeof(label), "Refresh list (live scan: write error %d)", result);
    menu->count = 0;
    failLiveScan(label);
    return;
  }

  // gamescan.irx doesn't keep the "played" counters, so the list is shown by name
  for (int i = 0; i < count; i++) {
    iopReadString(LIVESCAN_FIELD(names) + i * LIVESCAN_NAME_LEN, settings.menuItemName[menu->base + i], LIVESCAN_NAME_LEN);
    menu->played[i] = 0;
  }
  menu->count = count;
  menu->cacheLoaded = 1;

  liveScanActive = 0;
  setGamesLabels(menu, "Refresh list");
  sortGames(menu);
  showGamesEntries(menu);
}
#endif

// Returns the submenu shown by the menu item index, or NULL
static GamesSubmenu *findSubmenu(int idx) {
  for (int i = 0; i < SUBMENU_COUNT; i++)
    if ((settings.submenus[i].itemIdx >= 0) && (settings.submenus[i].itemIdx == idx))
      return &settings.submenus[i];
  return NULL;
}

// Opens the submenu requested by GAMES_REOPEN_ARG/PSX_REOPEN_ARG once the menu is on screen,
// with its "Games >"/"PSX >" entry as the entry "< Back" returns to
static void reopenGamesMenu(void) {
  GamesSubmenu *menu = &settings.submenus[settings.reopenSubmenu - 1];
  settings.reopenSubmenu = 0;
  if ((menu->itemIdx < 0) || !menu->cacheLoaded || !menuInfo)
    return;

  for (int i = 0; i < settings.menuItemCount; i++) {
    if (settings.menuItemIdx[i] == menu->itemIdx) {
      menuInfo->currentEntry = 2 + i;
      openGamesMenu(menu);
      return;
    }
  }
}

// Restores the regular custom entries and the cursor position
static void closeGamesMenu(void) {
  for (int i = 0; i < settings.menuItemCount; i++)
    setMenuEntry(i, i);

  menuInfo->entryCount = 2 + settings.menuItemCount;
  menuInfo->currentEntry = gamesMenuReturnEntry;
  activeMenu = NULL;
}

// Handles X on an entry of the active submenu
static void handleGamesMenuEntry(int pos) {
  GamesSubmenu *menu = activeMenu;
  if (pos == 0) {
    closeGamesMenu();
    return;
  }

  if (pos == menu->count + 1) {
#ifdef GAMES_MENU
    if (startLiveScan(menu))
      return;
#endif
    launchGamesMode(menu, ":s"); // Rescan
    return;
  }

  // The launcher takes the game's index in the cache
  char mode[16];
  sprintf(mode, ":g%d", menu->order[pos - 1]);
  launchGamesMode(menu, mode);
}
#endif

// Handles custom menu entries
int handleMenuEntry(int selected) {
  if (selected == 1)
    return 1;

  int pos = selected - 2;
  if (pos < 0)
    return 0;

#ifndef HOSD
  if (activeMenu) {
    if (liveScanActive)
      return 0; // "Scanning..." is not selectable
    if (pos < customItemCount())
      handleGamesMenuEntry(pos);
    return 0;
  }
#endif

  if (pos >= settings.menuItemCount)
    return 0;

  if (settings.menuItemName[pos][0] == '$' && settings.menuItemName[pos][1] == '!')
    return 0;

  int idx = settings.menuItemIdx[pos];

#ifndef HOSD
  GamesSubmenu *menu = findSubmenu(idx);
  if (menu) {
    if (menu->cacheLoaded) {
      openGamesMenu(menu);
      return 0;
    }
    // No cache yet: scan live if possible, else the launcher scans, writes it and returns to the OSD
#ifdef GAMES_MENU
    gamesMenuReturnEntry = menuInfo->currentEntry;
    activeMenu = menu;
    if (startLiveScan(menu))
      return 0;
    activeMenu = NULL;
#endif
    launchGamesMode(menu, ":s");
    return 0;
  }
#endif

  launchMenuItem(idx, "");
  return 0;
}

// Returns the pointer to OSD string
const char *getStringPointer(const char **strings, uint32_t index) {
  if ((index & 0xffff0000) == OSD_MAGIC) {
    char *str = settings.menuItemName[index & 0xffff];
    if (str[0] == '$' && str[1] == '!')
      return str + 2;
    return str;
  }

  return strings[index];
}

// Patches OSD menu to include custom menu entries
void patchMenu(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, menuAddr, osdstrAddr, entryAddr, i;

  // Try to find the menu info struct
  for (tmp = 0; tmp < 0x100000; tmp = (uint32_t)(ptr - osd + 4)) {
    ptr = findPatternWithMask(osd + tmp, 0x100000 - tmp, (uint8_t *)patternMenuInfo, (uint8_t *)patternMenuInfo_mask, sizeof(patternMenuInfo));
    if (!ptr)
      return;

// Found if the current address points to the pointer to "Browser" string
#ifndef HOSD
    if (_lw((uint32_t)ptr + 4) == (uint32_t)ptr - 4 * 4)
#else
    if (_lw((uint32_t)ptr + 4) == (uint32_t)ptr - 8 * 4)
#endif
      break;
  }
  menuAddr = (uint32_t)ptr;

  menuInfo = (struct OSDMenuInfo *)menuAddr;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternOSDString, (uint8_t *)patternOSDString_mask, sizeof(patternOSDString));
  if (!ptr)
    return;
  osdstrAddr = (uint32_t)ptr;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternUserInputHandler, (uint8_t *)patternUserInputHandler_mask,
                            sizeof(patternUserInputHandler));
  if (!ptr)
    return;
  entryAddr = (uint32_t)ptr;

  // Patch the OSD string function
  tmp = 0x0c000000;
  tmp |= ((uint32_t)getStringPointer >> 2);
  _sw(0x0200282d, osdstrAddr + 2 * 4); // daddu  a1, s0, zero
                                       // lw     a0, $xxxx(v0)
  _sw(tmp, osdstrAddr + 4 * 4);        // jal    getStringPointer
  _sw(0x00000000, osdstrAddr + 5 * 4); // nop

  // Patch the user input handling function
  tmp = 0x0c000000;
  tmp |= ((uint32_t)handleMenuEntry >> 2);
  _sw(tmp, entryAddr + 2 * 4);           // jal    handleMenuEntry
  tmp = _lw(entryAddr + 3 * 4) & 0xffff; //
  tmp |= 0x8c440000;                     //
  _sw(tmp, entryAddr + 3 * 4);           // lw     a0, $xxxx(v0)
  _sw(0x1040000a, entryAddr + 4 * 4);    // beq    v0, zero, exit

// Build the OSD menu
#ifndef HOSD
  osdMenu[0] = _lw(menuAddr - 4 * 4); // "Browser"
  osdMenu[1] = _lw(menuAddr - 3 * 4);
  osdMenu[2] = _lw(menuAddr - 2 * 4); // "System Configuration"
  osdMenu[3] = _lw(menuAddr - 1 * 4);
#else
  // HDD-OSD uses four values per entry, but
  // patchDrawMenu fixes this to two values per entry
  osdMenu[0] = _lw(menuAddr - 8 * 4); // "Browser" string index
  osdMenu[1] = _lw(menuAddr - 7 * 4);
  osdMenu[2] = _lw(menuAddr - 4 * 4); // "System Configuration" string index
  osdMenu[3] = _lw(menuAddr - 3 * 4);
#endif

  for (i = 0; i < settings.menuItemCount; i++) {
    osdMenu[4 + i * 2] = OSD_MAGIC + i;
    osdMenu[5 + i * 2] = 0;
  }

  menuInfo->menuPtr = osdMenu;                       // store menu pointer
  menuInfo->entryCount = 2 + settings.menuItemCount; // store number of menu items
}

static uint32_t colorSelected[4] __attribute__((aligned(16)));
static uint32_t colorUnselected[4] __attribute__((aligned(16)));

static void (*DrawMenuItem)(int X, int Y, uint32_t *color, int alpha, const char *string);
// Protokernel DrawMenuItem expects a pointer to string address, not the string address
// For later consoles, this function is set to DrawMenuItem
static void (*DrawMenuItemStringPtr)(int X, int Y, uint32_t *color, int alpha, const char *string);
static int dx = 0;
static int vel, acc;
static int offsY = 0;
static int fontHeight = 16;

// Draws selected items
void drawMenuItemSelected(int X, int Y, uint32_t *color, int alpha, const char *string, int num) {
#ifndef HOSD
  if (settings.reopenSubmenu)
    reopenGamesMenu();
#endif
#if !defined(HOSD) && defined(GAMES_MENU)
  if (liveScanActive && (num == 0))
    pollLiveScan();
  else if (activeMenu && (num == 0))
    handleGamesMenuButtons();
#endif
#ifdef HOSD
  asm volatile("move %0, $s1" : "=r"(num)::); // For HDD-OSD, get menu index from s1 register
  num *= 8;                                   // Multiply by 8 to align with OSDSYS behavior
#endif
  int i;

  for (i = 0; i < 4; i++)
    colorSelected[i] = settings.colorSelected[i];

  if (alpha > 0x80)
    alpha = 0x80;

  if (!(settings.patcherFlags & FLAG_SCROLL_MENU)) { // Old style menu
    DrawMenuItem(settings.menuX, Y - customItemCount() * 10, colorSelected, alpha, string);
  } else { // New style menu
    if (num == 0) {
      int amount;
      if (offsY < 0) {
        amount = -offsY >> 2;
        offsY += (amount > 0 ? amount : 1);
      } else if (offsY > 0) {
        amount = offsY >> 2;
        offsY -= (amount > 0 ? amount : 1);
      }
    }
    Y = (num << 1) - offsY;
    if ((Y < ((settings.displayedItems + 1) * (fontHeight / 2))) && (Y > -((settings.displayedItems + 1) * (fontHeight / 2)))) {
      vel -= acc;
      if (vel < -settings.cursorMaxVelocity || vel > settings.cursorMaxVelocity)
        acc = -acc;
      dx += vel;
      DrawMenuItem(settings.menuX, settings.menuY + Y, colorSelected, alpha, string);
      DrawMenuItemStringPtr(settings.menuX - 220 + (dx >> 8), settings.menuY + Y, colorSelected, alpha, settings.leftCursor);
      DrawMenuItemStringPtr(settings.menuX + 220 - (dx >> 8), settings.menuY + Y, colorSelected, alpha, settings.rightCursor);
    }
    DrawMenuItemStringPtr(settings.menuX, settings.menuY - (settings.displayedItems * (fontHeight / 2) + (fontHeight / 2)), colorSelected, alpha,
                          settings.menuDelimiterTop);
    DrawMenuItemStringPtr(settings.menuX, settings.menuY + (settings.displayedItems * (fontHeight / 2) + (fontHeight / 2)), colorSelected, alpha,
                          settings.menuDelimiterBottom);
  }
}

// Draws unselected items
void drawMenuItemUnselected(int X, int Y, uint32_t *color, int alpha, const char *string, int num) {
#ifndef HOSD
  if (settings.reopenSubmenu)
    reopenGamesMenu();
#endif
#if !defined(HOSD) && defined(GAMES_MENU)
  if (liveScanActive && (num == 0))
    pollLiveScan();
  else if (activeMenu && (num == 0))
    handleGamesMenuButtons();
#endif
#ifdef HOSD
  asm volatile("move %0, $s1" : "=r"(num)::); // For HDD-OSD, get menu index from s1 register
  num *= 8;                                   // Multiply by 8 to align with OSDSYS behavior
#endif
  int i;

  for (i = 0; i < 4; i++)
    colorUnselected[i] = settings.colorUnselected[i];

  if (!(settings.patcherFlags & FLAG_SCROLL_MENU)) { // Old style menu
    DrawMenuItem(settings.menuX, Y - customItemCount() * 10, colorUnselected, alpha, string);
  } else { // New style menu
    if (num == 0) {
      int amount, destY = menuInfo->currentEntry << 4;
      if (offsY < destY) {
        amount = (destY - offsY) >> 2;
        offsY += (amount > 0 ? amount : 1);
      } else if (offsY > destY) {
        amount = (offsY - destY) >> 2;
        offsY -= (amount > 0 ? amount : 1);
      }
    }
    Y = (num << 1) - offsY;
    if ((Y < ((settings.displayedItems + 1) * (fontHeight / 2))) && (Y > -((settings.displayedItems + 1) * (fontHeight / 2)))) {
      if (Y < 0)
        alpha = 128 + (Y * (128 / ((settings.displayedItems + 1) * (fontHeight / 2))));
      else
        alpha = 128 - (Y * (128 / ((settings.displayedItems + 1) * (fontHeight / 2))));
      if (alpha < 0)
        alpha = 0;

      DrawMenuItem(settings.menuX, settings.menuY + Y, colorUnselected, alpha, string);
    }
  }
}

// Patches menu drawing functions
// You can change menu items' color and position for selected and unselected
// items separately in
// drawMenuItemSelected()
// drawMenuItemUnselected()
//
// Be careful what you write in these functions as they get called every
// frame for every menu item!  For positioning the menu, update both
// functions with the same calculations, using the X/Y variables.
//
// Default values of the variables in V12 OSDSYS:
// X = 430 (this is the center of the menu)
// Y = 110 (this is the Y of the first item)
// alpha = 128 (smaller value is more transparency)
void patchMenuDraw(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, pSelItem, pUnselItem;

  vel = settings.cursorMaxVelocity;
  acc = settings.cursorAcceleration;

  settings.displayedItems |= 1; // must be odd value
  if (settings.displayedItems < 1)
    settings.displayedItems = 1;
  if (settings.displayedItems > 15)
    settings.displayedItems = 15;

  if (!menuInfo)
    return;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternDrawMenuItem, (uint8_t *)patternDrawMenuItem_mask, sizeof(patternDrawMenuItem));
  if (!ptr)
    return;
  pSelItem = (uint32_t)ptr; // code for selected menu item

  ptr = findPatternWithMask(ptr + 4, 256, (uint8_t *)patternDrawMenuItem, (uint8_t *)patternDrawMenuItem_mask, sizeof(patternDrawMenuItem));
#ifndef HOSD
  if (ptr != (uint8_t *)(pSelItem + 48))
#else
  if (ptr != (uint8_t *)(pSelItem + 44))
#endif
    return;
  pUnselItem = (uint32_t)ptr; // code for unselected menu item

  tmp = _lw(pSelItem + 32); // get the OSD's DrawMenuItem function pointer
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawMenuItem = (void *)tmp;
  DrawMenuItemStringPtr = DrawMenuItem;

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawMenuItemSelected >> 2);
  _sw(tmp, pSelItem + 32); // overwrite the function call for selected item

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawMenuItemUnselected >> 2);
  _sw(tmp, pUnselItem + 32); // overwrite the function call for unselected item

#ifndef HOSD
  _sw(0x001048c0, pSelItem);       // make menu item's number the sixth param
  _sw(0x01231021, pSelItem + 4);   // by loading it into t1 (multiplied by 8):
  _sw(0x001048c0, pUnselItem);     // sll   t1, s0, 3
  _sw(0x01231021, pUnselItem + 4); // addu  v0, t1, v1
#else
  // HDD-OSD uses four values per menu entry
  // Adjust this behavior to match OSDSYS (two values per entry)
  // by changing the instruction that increments the string index.
  tmp = _lw(pUnselItem + 48); // Must be addiu ??,??,0x10
  if ((tmp & 0xff0000ff) == 0x26000010)
    _sw((tmp & 0xffffff00) | 0x08, pUnselItem + 48); // Modify to addiu ??,??,0x08
#endif
}

static void (*DrawNonSelectableItem)(int X, int Y, uint32_t *color, int alpha, const char *string);
static void (*DrawIcon)(int type, int X, int Y, int alpha);
static void (*DrawButtonPanelGetOSDLang)(void);

int ButtonsPanel_Type = 0;

#define MAINMENU_PANEL 1
#define SYSCONF_PANEL 8

// getButtonsPanelType() is called prior to functions above and determine panel type :
// 	- Main menu type : 1
// 	- Sys conf screen type : 8
void getButtonsPanelType(int type) {
  // Get Buttons Panel type (catch it in a0)
  ButtonsPanel_Type = type;

  // Call original function that was overridden
  DrawButtonPanelGetOSDLang();
}

#ifndef HOSD
// Button prompts of the games submenus: "Back" replaces "Version", and "Sort" is added in between.
// The icon types of Circle and Square are not known, so they are derived from the types OSDSYS
// uses for Enter (Cross, or Circle on Japanese consoles) and Version (Triangle), assuming the
// icons are consecutive in either the Circle, Cross, Square, Triangle or the Triangle, Circle,
// Cross, Square order. In both, Circle is right before Cross and Square right after it.
// If the difference between Enter and Version fits neither, only the texts are shown.
static int enterIconType = -1;
static int versionIconType = -1;
static int backIconType = -1;
static int sortIconType = -1;

static void deriveSubmenuIcons(void) {
  backIconType = sortIconType = -1;
  if ((enterIconType < 0) || (versionIconType < 0))
    return;

  int diff = versionIconType - enterIconType;
  if ((diff == 2) || (diff == -2)) { // Enter is Cross: back with Circle
    backIconType = enterIconType - 1;
    sortIconType = enterIconType + 1;
  } else if ((diff == 3) || (diff == -1)) { // Enter is Circle: back with Cross
    backIconType = enterIconType + 1;
    sortIconType = enterIconType + 2;
  }
  if ((backIconType < 0) || (sortIconType < 0))
    backIconType = sortIconType = -1;
}

// Returns 1 when the main menu button panel shows a games submenu
static int showSubmenuPrompts(void) { return (ButtonsPanel_Type == MAINMENU_PANEL) && activeMenu && !liveScanActive; }

// X coordinate of the "Sort" prompt, between Enter and Back
static int sortPromptX(void) { return (settings.enterX + settings.versionX) / 2; }
#endif

// drawNonselectableEntryLeft() is called for all items less the last
void drawNonselectableEntryLeft(int X, int Y, uint32_t *color, int alpha, const char *string) {
  if (ButtonsPanel_Type == MAINMENU_PANEL) {
    if (settings.enterX == -1)
      settings.enterX = X - 28;

    if (settings.enterY == -1)
      settings.enterY = Y;

#ifndef HOSD
    DrawNonSelectableItem(settings.enterX + 28, settings.enterY, color, alpha, string);
#else
    // Adding 1 to Y offset to align text with the icon
    DrawNonSelectableItem(settings.enterX + 28, settings.enterY + 1, color, alpha, string);
#endif
  } else
    DrawNonSelectableItem(X, Y, color, alpha, string);
}

// drawNonselectableEntryRight() is called only for last item (Options or Version)
void drawNonselectableEntryRight(int X, int Y, uint32_t *color, int alpha, const char *string) {
  if (ButtonsPanel_Type == MAINMENU_PANEL) {
    if (settings.versionX == -1)
      settings.versionX = X - 28;

    if (settings.versionY == -1)
      settings.versionY = Y;

#ifndef HOSD
    if (showSubmenuPrompts()) {
      char sortLabel[40];
      if (settings.buttonDebug)
        snprintf(sortLabel, sizeof(sortLabel), "Sort: %s (%d/%d)", activeMenu->sortRecent ? "[Recent]" : "[A-Z]", enterIconType, versionIconType);
      else
        snprintf(sortLabel, sizeof(sortLabel), "Sort: %s", activeMenu->sortRecent ? "[Recent]" : "[A-Z]");
      DrawNonSelectableItem(sortPromptX() + 28, settings.versionY, color, alpha, sortLabel);
      DrawNonSelectableItem(settings.versionX + 28, settings.versionY, color, alpha, "Back");
      return;
    }
    DrawNonSelectableItem(settings.versionX + 28, settings.versionY, color, alpha, string);
#else
    // Adding 1 to Y offset to align text with the icon
    DrawNonSelectableItem(settings.versionX + 28, settings.versionY + 1, color, alpha, string);
#endif
  } else
    DrawNonSelectableItem(X, Y, color, alpha, string);
}

// drawIconLeft() is called for all button icons less the last
void drawIconLeft(int type, int X, int Y, int alpha) {
  if (ButtonsPanel_Type == MAINMENU_PANEL) {
    if (settings.enterX == -1)
      settings.enterX = X;

    if (settings.enterY == -1)
      settings.enterY = Y;

#ifndef HOSD
    if (enterIconType != type) {
      enterIconType = type;
      deriveSubmenuIcons();
    }
#endif
    DrawIcon(type, settings.enterX, settings.enterY, alpha);
  } else
    DrawIcon(type, X, Y, alpha);
}

// drawIconRight() is called for only for last button icon (Options or Version)
void drawIconRight(int type, int X, int Y, int alpha) {
  if (ButtonsPanel_Type == MAINMENU_PANEL) {
    if (settings.versionX == -1)
      settings.versionX = X;

    if (settings.versionY == -1)
      settings.versionY = Y;

#ifndef HOSD
    // OSDSYS always passes the Version (Triangle) icon here, even while a submenu is shown
    if (versionIconType != type) {
      versionIconType = type;
      deriveSubmenuIcons();
    }
    if (showSubmenuPrompts()) {
      if (sortIconType >= 0)
        DrawIcon(sortIconType, sortPromptX(), settings.versionY, alpha);
      if (backIconType >= 0)
        DrawIcon(backIconType, settings.versionX, settings.versionY, alpha);
      return;
    }
#endif
    DrawIcon(type, settings.versionX, settings.versionY, alpha);
  } else
    DrawIcon(type, X, Y, alpha);
}

// Patches OSDSYS button prompts
//
// OSDSYS defaults are
// 	- Enter icon X : 188
// 	- Enter icon Y : 230 on PAL
// 	- Enter text X : 216
// 	- Enter text Y : 230 on PAL
// 	- Version icon X : 501
// 	- Version icon Y : 230 on PAL
// 	- Version text X : 529
// 	- Version text Y : 230 on PAL
void patchMenuButtonPanel(uint8_t *osd) {
  uint8_t *ptr;
  uint8_t *firstPtr;
  uint32_t tmp;
  uint32_t addr, pButtonsPanelType, pBottomRightIcon, pBottomLeftIcon, pBottomRightItem, pBottomLeftItem;
  uint32_t pattern[1];
  uint32_t mask[1];

  // Search and overwrite 1st function call in DrawButtonPanel function
  firstPtr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternDrawButtonPanel_1, (uint8_t *)patternDrawButtonPanel_1_mask,
                                 sizeof(patternDrawButtonPanel_1));
  if (!firstPtr)
    return;
  pButtonsPanelType = (uint32_t)firstPtr;

  tmp = _lw(pButtonsPanelType + 32);
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawButtonPanelGetOSDLang = (void *)tmp; // get original function call

  tmp = 0x0c000000;
  tmp |= ((uint32_t)getButtonsPanelType >> 2);
  _sw(tmp, pButtonsPanelType + 32); // overwrite the function call

  // Search and overwrite 1st DrawIcon function call in DrawButtonPanel function
  ptr = findPatternWithMask(firstPtr, 0x1000, (uint8_t *)patternDrawButtonPanel_2, (uint8_t *)patternDrawButtonPanel_2_mask,
                            sizeof(patternDrawButtonPanel_2));
  if (!ptr)
    return;
  pBottomRightIcon = (uint32_t)ptr; // code for bottom right icon

  tmp = _lw(pBottomRightIcon + 24);
  addr = tmp; // save function call code
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawIcon = (void *)tmp;

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawIconRight >> 2);
  _sw(tmp, pBottomRightIcon + 24); // overwrite the function call for bottom right icon

  // Make pattern with function call code saved above
  pattern[0] = addr;
  mask[0] = 0xffffffff;

  // Search and overwrite 2nd DrawIcon function call in DrawButtonPanel function
  ptr = findPatternWithMask(ptr + 28, 0x1000, (uint8_t *)pattern, (uint8_t *)mask, sizeof(pattern));
  if (!ptr)
    return;
  pBottomLeftIcon = (uint32_t)ptr; // code for bottom left icons

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawIconLeft >> 2);
  _sw(tmp, pBottomLeftIcon); // overwrite the function call for bottom left icons

  // Search and overwrite 1st DrawNonSelectableItem function call in DrawButtonPanel function
  ptr = findPatternWithMask(firstPtr, 0x1000, (uint8_t *)patternDrawButtonPanel_3, (uint8_t *)patternDrawButtonPanel_3_mask,
                            sizeof(patternDrawButtonPanel_3));
  if (!ptr)
    return;
  pBottomRightItem = (uint32_t)ptr; // code for bottom right item

  tmp = _lw(pBottomRightItem + 20); // get the OSD's DrawNonSelectableItem function pointer
  addr = tmp;                       // save function call code
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawNonSelectableItem = (void *)tmp;

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawNonselectableEntryRight >> 2);
  _sw(tmp, pBottomRightItem + 20); // overwrite the function call for bottom right item

  // Make pattern with function call code saved above
  pattern[0] = addr;
  mask[0] = 0xffffffff;

  // Search and overwrite 2nd DrawNonSelectableItem function call in DrawButtonPanel function
  ptr = findPatternWithMask(ptr + 24, 0x1000, (uint8_t *)pattern, (uint8_t *)mask, sizeof(pattern));
  if (!ptr)
    return;
  pBottomLeftItem = (uint32_t)ptr; // code for bottom left item

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawNonselectableEntryLeft >> 2);
  _sw(tmp, pBottomLeftItem); // overwrite the function call for bottom left item
}

// An array that stores what function to call for each disc type.
// Later PS2 ROMs:
// uint32_t *discLaunchHandlers[7] = {
//    exec_ps2_game_disc,		// PS2 game DVD
//    exec_ps2_game_disc,		// PS2 game CD
//    exec_ps1_game_disc,		// PS1 game CD
//    exec_dvdv_disc,			  // DVD Video
//    do_nothing,				    // none (return 1)
//    do_nothing,				    // none (return 1)
//    exec_hdd_stuff			  // HDDLOAD
//}
// HDD-OSD:
// uint32_t *discLaunchHandlers[8] = {
//    exec_ps2_game_disc,		// PS2 game DVD
//    exec_ps2_game_disc,		// PS2 game CD
//    exec_ps1_game_disc,		// PS1 game CD
//    exec_dvdv_disc,			  // DVD Video
//    do_nothing,				    // none (return 1)
//    do_nothing,				    // none (return 1)
//    exec_hdd_stuff			  // HDDLOAD
//    exec_hdd_app          // Execute HDD application
//}
// Patches the disc launch handlers to load discs with the launcher
void patchDiscLaunch(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, pFn;
  static uint32_t *discLaunchHandlers = NULL;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternExecuteDisc, (uint8_t *)patternExecuteDisc_mask, sizeof(patternExecuteDisc));
  if (!ptr)
    return;

  pFn = (uint32_t)ptr; // address of the ExecuteDisc function

  tmp = _lw(pFn + 40) << 16;
  tmp += (int16_t)(_lw(pFn + 44) & 0xffff);
  discLaunchHandlers = (uint32_t *)tmp;

  discLaunchHandlers[0] = (uint32_t)launchDisc; // Overwrite PS2 DVD function pointer
  discLaunchHandlers[1] = (uint32_t)launchDisc; // Overwrite PS2 CD function pointer
  discLaunchHandlers[2] = (uint32_t)launchDisc; // Overwrite PS1 function pointer
#ifdef HOSD
  discLaunchHandlers[7] = (uint32_t)launchHDDApplication; // Overwrite HDD application function pointer
#endif

  // Patch DVD key check on ROM 1.60+
  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternCheckDVDKey, (uint8_t *)patternCheckDVDKey_mask, sizeof(patternCheckDVDKey));
  if (!ptr)
    return;
  _sw(0x24020000, (uint32_t)ptr + 4); // patch the function call to return 0 instead
}

static uint32_t menuLoopPatch_1[7] = {0x8e04fff8, 0x8e05fff0, 0x0004102a, 0x0082280b, 0x20a3ffff, 0x1000000c, 0xae03fff8};
static uint32_t menuLoopPatch_2[9] = {
    0x1040000e, 0x30620020, 0x8e05fff8, 0x8e02fff0, 0x24a30001, 0x0062102a, 0x0002180a, 0x00000000, 0xae03fff8,
};

// Patches menu scrolling
void patchMenuInfiniteScrolling(uint8_t *osd, int isProtokernel) {
  int i;
  uint8_t *ptr;
  uint32_t *addr, *src, *dst;

#ifndef HOSD
  if (isProtokernel)
    ptr = findPatternWithMask(osd + PROTOKERNEL_MENU_OFFSET, 0x100000, (uint8_t *)patternMenuLoop_Proto, (uint8_t *)patternMenuLoop_mask,
                              sizeof(patternMenuLoop_Proto));
  else
#endif
    ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternMenuLoop, (uint8_t *)patternMenuLoop_mask, sizeof(patternMenuLoop));

  if (!ptr)
    return;

  addr = (uint32_t *)ptr;

#ifndef HOSD
  if (addr[9] == 0x30624000 && addr[20] == 0x24045200)
#else
  if (addr[9] == 0x30624000 && addr[20] == 0x24046300)
#endif
  {
    src = menuLoopPatch_1;
    dst = addr + 2;
    for (i = 0; i < 7; i++)
      *(dst++) = *(src++);

    src = menuLoopPatch_2;
    dst = addr + 11;
    for (i = 0; i < 9; i++)
      *(dst++) = *(src++);

    addr[10] = addr[-1]; // reload normal pad variable
    addr[-1] += 8;       // get key repeat variable
  }
}

// Forces the video mode
void patchVideoMode(uint8_t *osd, GSVideoMode mode) {
  uint8_t *ptr;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternVideoMode, (uint8_t *)patternVideoMode_mask, sizeof(patternVideoMode));
  if (!ptr)
    return;

  if (mode == GS_MODE_PAL)
    _sw(0x24020001, (uint32_t)ptr + 20); // set return value to 1
  else
    _sw(0x0000102d, (uint32_t)ptr + 20); // set return value to 0 to force NTSC
}

#ifndef HOSD
// Patches HDD update code for ROMs not supporting "SkipHdd" arg
void patchSkipHDD(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t addr;

  // Search code near MC Update & HDD load
  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternHDDLoad, (uint8_t *)patternHDDLoad_mask, sizeof(patternHDDLoad));
  if (!ptr)
    return;
  addr = (uint32_t)ptr;

  // Place "beq zero, zero, Exit_HddLoad" just after CheckMcUpdate() call
  _sw(0x10000000 + ((signed short)(_lw(addr + 28) & 0xffff) + 5), addr + 8);
}

//
// Protokernel patches reverse engineered from FMCB1.9
// All menu code seems to be located starting from 0x600000
//

// Patches OSD menu to include custom menu entries
void patchMenuProtokernel(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, menuAddr, entryAddr, i;

  // Try to find the menu info struct
  for (tmp = 0; tmp < 0x100000; tmp = (uint32_t)(ptr - osd + 4)) {
    ptr = findPatternWithMask(osd + PROTOKERNEL_MENU_OFFSET + tmp, 0x100000 - tmp, (uint8_t *)patternMenuInfo_Proto,
                              (uint8_t *)patternMenuInfo_Proto_mask, sizeof(patternMenuInfo_Proto));
    if (!ptr)
      return;

    // Found if the current address points to the pointer to "Browser" string
    if (_lw((uint32_t)ptr) == (uint32_t)ptr - 4 * 8)
      break;
  }
  menuAddr = (uint32_t)ptr - 4;
  menuInfo = (struct OSDMenuInfo *)menuAddr;
  menuUsesStringPointers = 1;

  ptr = findPatternWithMask(osd + PROTOKERNEL_MENU_OFFSET, 0x100000, (uint8_t *)patternUserInputHandler, (uint8_t *)patternUserInputHandler_mask,
                            sizeof(patternUserInputHandler));
  if (!ptr)
    return;
  entryAddr = (uint32_t)ptr;

  // Patch the user input handling function
  tmp = 0x0c000000;
  tmp |= ((uint32_t)handleMenuEntry >> 2);
  _sw(tmp, entryAddr + 2 * 4);           // jal    handleMenuEntry
  tmp = _lw(entryAddr + 3 * 4) & 0xffff; //
  tmp |= 0x8c440000;                     //
  _sw(tmp, entryAddr + 3 * 4);           // lw     a0, $xxxx(v0)
  _sw(0x1040000a, entryAddr + 4 * 4);    // beq    v0, zero, exit

  // Build the OSD menu
  osdMenu[0] = _lw(menuAddr - 4 * 7); // "Browser"
  osdMenu[1] = _lw(menuAddr - 4 * 6);
  osdMenu[2] = _lw(menuAddr - 4 * 4); // "System Configuration"
  osdMenu[3] = _lw(menuAddr - 4 * 3);

  for (i = 0; i < settings.menuItemCount; i++) {
    if (settings.menuItemName[i][0] == '\0')
      continue;

    osdMenu[4 + i * 2] = (uint32_t)settings.menuItemName[i];
    osdMenu[5 + i * 2] = (uint32_t)settings.menuItemName[i];
  }

  menuInfo->menuPtr = osdMenu;                       // store menu pointer
  menuInfo->entryCount = 2 + settings.menuItemCount; // store number of menu items
}

// Protokernel drawing functions don't pass anything indicating the entry index.
// However, s0 register contains menu index
void drawMenuItemSelectedProtokernel(int X, int Y, uint32_t *color, int alpha, const char *string) {
  int num = 0;
  asm volatile("move %0, $s0" : "=r"(num)::); // Get menu index from s0 register
  num *= 8;                                   // Multiply by 8 to align with later OSDSYS behavior
  drawMenuItemSelected(X, Y, color, alpha, string, num);
}
void drawMenuItemUnselectedProtokernel(int X, int Y, uint32_t *color, int alpha, const char *string) {
  int num = 0;
  asm volatile("move %0, $s0" : "=r"(num)::); // Get menu index from s0 register
  num *= 8;                                   // Multiply by 8 to align with later OSDSYS behavior
  drawMenuItemUnselected(X, Y, color, alpha, string, num);
}

// Protokernel DrawMenuItem expects a pointer to string address, not the string address
// ROMs 1.00 and 1.01 differ in how they handle menu item strings.
// ROM 1.00 seems to use whatever's at the next address for our custom header and footer
// ROM 1.01 uses the passed address.
// Support both by passing array address that has both entries pointing to the same string.
const char *drawMenuItemLanding[2] = {};
void drawMenuItemProtokernel(int X, int Y, uint32_t *color, int alpha, const char *string) {
  drawMenuItemLanding[0] = string;
  drawMenuItemLanding[1] = string;
  DrawMenuItem(X, Y, color, alpha, (char *)&drawMenuItemLanding);
}

// Patches menu drawing functions
void patchMenuDrawProtokernel(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, pSelItem, pUnselItem;

  vel = settings.cursorMaxVelocity;
  acc = settings.cursorAcceleration;

  settings.displayedItems |= 1; // must be odd value
  if (settings.displayedItems < 1)
    settings.displayedItems = 1;
  if (settings.displayedItems > 15)
    settings.displayedItems = 15;

  if (!menuInfo)
    return;

  ptr = findPatternWithMask(osd + PROTOKERNEL_MENU_OFFSET, 0x100000, (uint8_t *)patternDrawMenuItem_Proto, (uint8_t *)patternDrawMenuItem_Proto_mask,
                            sizeof(patternDrawMenuItem_Proto));
  if (!ptr)
    return;
  pSelItem = (uint32_t)ptr;

  ptr = findPatternWithMask(ptr + 0x18, 0x100, (uint8_t *)patternDrawMenuItem_Proto, (uint8_t *)patternDrawMenuItem_Proto_mask,
                            sizeof(patternDrawMenuItem_Proto));
  if (!ptr)
    return;
  pUnselItem = (uint32_t)ptr;

  tmp = _lw(pSelItem + 0x10); // get the OSD's DrawMenuItem function pointer
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawMenuItem = (void *)tmp;
  DrawMenuItemStringPtr = &drawMenuItemProtokernel;

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawMenuItemSelectedProtokernel >> 2);
  _sw(tmp, pSelItem + 0x10); // overwrite the function call for selected item

  tmp = 0x0c000000;
  tmp |= ((uint32_t)drawMenuItemUnselectedProtokernel >> 2);
  _sw(tmp, pUnselItem + 0x10); // overwrite the function call for unselected item

  // Protokernels use three values per menu entry
  // Adjust this behavior to match later OSDSYS (two values per entry)
  // by changing the instruction that increments the string index.
  tmp = _lw(pUnselItem + 32); // Must be addiu ??,??,0xc
  if ((tmp & 0xff0000ff) == 0x2600000c)
    _sw((tmp & 0xffffff00) | 0x08, pUnselItem + 32); // Modify to addiu ??,??,0x08
}

// An array that stores what function to call for each disc type.
// Protokernels:
// uint32_t *discLaunchHandlers[6] = {
//    reboot,					      // perform LoadExecPS2(NULL, 0, NULL)
//    do_nothing,				    // none (return 1)
//    exec_ps2_game_disc,		// PS2 game DVD
//    exec_ps2_game_disc,		// PS2 game CD
//    exec_ps1_game_disc,		// PS1 game CD
//    exec_dvdv_disc			  // DVD Video
//}
// Patches the disc launch handlers to load discs with the launcher
void patchDiscLaunchProtokernel(uint8_t *osd) {
  uint8_t *ptr;
  uint32_t tmp, pFn;
  static uint32_t *discLaunchHandlers = NULL;

  ptr = findPatternWithMask(osd, 0x100000, (uint8_t *)patternExecuteDiscProto, (uint8_t *)patternExecuteDiscProto_mask,
                            sizeof(patternExecuteDiscProto));
  if (!ptr)
    return;

  pFn = (uint32_t)ptr;

  tmp = _lw(pFn + 28) << 16;
  tmp += (signed short)(_lw(pFn + 36) & 0xffff);
  discLaunchHandlers = (uint32_t *)tmp;

  discLaunchHandlers[2] = (uint32_t)launchDisc; // Overwrite protokernel PS2 DVD function pointer
  discLaunchHandlers[3] = (uint32_t)launchDisc; // Overwrite protokernel PS2 CD function pointer
  discLaunchHandlers[4] = (uint32_t)launchDisc; // Overwrite protokernel PS1 function pointer
}

// Finds some drawing functions. Unused.
void patchMenuButtonPanelProtokernel(uint8_t *osd) {
  uint8_t *ptr = findPatternWithMask(osd + PROTOKERNEL_MENU_OFFSET, 0x100000, (uint8_t *)patternDrawButtonPanel_2_Proto,
                                     (uint8_t *)patternDrawButtonPanel_2_Proto_mask, sizeof(patternDrawButtonPanel_2_Proto));
  if (!ptr)
    return;

  uint8_t *ptr2 = findPatternWithMask(ptr, 0x100, (uint8_t *)patternDrawButtonPanel_3_Proto, (uint8_t *)patternDrawButtonPanel_3_Proto_mask,
                                      sizeof(patternDrawButtonPanel_3_Proto));
  if (!ptr2)
    return;

  uint32_t tmp = _lw((uint32_t)ptr + 0x30);
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawIcon = (void *)tmp;

  tmp = _lw((uint32_t)ptr2 + 0x50);
  tmp &= 0x03ffffff;
  tmp <<= 2;
  DrawNonSelectableItem = (void *)tmp;
}
#endif
