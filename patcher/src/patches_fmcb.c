// FMCB 1.8 OSDSYS patches by Neme
// FMCB 1.9 patches by sp193
#include "patches_fmcb.h"
#include "covers.h"
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
#include <stdarg.h>
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
// Menu group currently replacing the custom menu entries, NULL when the regular entries are shown
static MenuGroup *activeGroup = NULL;
#endif

// Number of custom entries currently shown after "Browser" and "System Configuration"
static int customItemCount(void) {
#ifndef HOSD
  if (activeMenu && liveScanActive)
    return 1; // "Scanning..."
  if (activeMenu)
    return activeMenu->count + 2; // games + "back" + "refresh"
  if (activeGroup)
    return activeGroup->count + 1; // "back" + items
#endif
  return settings.menuItemCount;
}

// Launches the launcher for the OSDMENU.CNF entry with the given index.
// suffix is appended to the osdm path and interpreted by the launcher's games handler
static void launchMenuItem(int idx, const char *suffix) {
  char item[256] = {0};
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
  // Favorites that weren't saved yet follow as 'f' and a hex mask by cache index,
  // one digit per four games (bit 0 for the first one), with the trailing zeros left out
  char favorites[2 + CUSTOM_ITEMS / 4] = "";
  if (menu->favDirty) {
    int digits = 0;
    favorites[0] = 'f';
    for (int i = 0; i < menu->count; i += 4) {
      int nibble = 0;
      for (int b = 0; (b < 4) && (i + b < menu->count); b++)
        if (menu->fav[i + b])
          nibble |= 1 << b;
      favorites[1 + i / 4] = "0123456789abcdef"[nibble];
      if (nibble)
        digits = 1 + i / 4;
    }
    favorites[1 + (digits ? digits : 1)] = '\0';
    if (!digits)
      favorites[1] = '0';
  }

  char suffix[16 + sizeof(favorites) + sizeof(settings.bootPath)];
  // The sort order ('r'ecent or 'n'ame) follows the mode so the launcher saves it in the cache
  snprintf(suffix, sizeof(suffix), "%s%c%s%s%s", mode, menu->sortRecent ? 'r' : 'n', favorites, settings.bootPath[0] ? "|" : "",
           settings.bootPath);
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

static void closeSubmenu(void);

// Fills menu->order: games in cache order (by name), or most recently played first,
// followed by the games that were never played in name order. Favorites come first in both
static void sortGames(GamesSubmenu *menu) {
  for (int i = 0; i < menu->count; i++)
    menu->order[i] = i;

  // Stable insertion sort by favorite and, when sorting by recently played, by the "played" counter, highest first
  for (int i = 1; i < menu->count; i++) {
    uint8_t idx = menu->order[i];
    int j = i;
    while (j > 0) {
      uint8_t prev = menu->order[j - 1];
      int before = (menu->fav[idx] > menu->fav[prev]) ||
                   ((menu->fav[idx] == menu->fav[prev]) && menu->sortRecent && (menu->played[prev] < menu->played[idx]));
      if (!before)
        break;
      menu->order[j] = prev;
      j--;
    }
    menu->order[j] = idx;
  }
}

// Shows "< Back", the games and "Refresh list" of the submenu as the custom entries.
// OSDSYS reads the entry table and cursor from menuInfo every frame, so this takes
// effect immediately without leaving the OSD. The sort order is shown in the "Sort" button prompt
static void showGamesEntries(GamesSubmenu *menu) {
  int pos = 0;
  strcpy(settings.menuItemName[menu->base + menu->count], "< Back");
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

// Returns the cache index of the game under the cursor in the active submenu, or -1
static int selectedGame(void) {
  int pos = (int)menuInfo->currentEntry - 3;
  return ((pos >= 0) && (pos < activeMenu->count)) ? activeMenu->order[pos] : -1;
}

// Sorts and shows the active submenu again, keeping the cursor on the game with cache index selected
static void resortGames(int selected) {
  GamesSubmenu *menu = activeMenu;
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

// Switches the active submenu between sorting by name and by most recently played,
// keeping the cursor on the same game
static void toggleGamesSort(void) {
  int selected = selectedGame();
  activeMenu->sortRecent = !activeMenu->sortRecent;
  resortGames(selected);
}

#ifdef GAMES_MENU
static void saveLiveScanFavorites(GamesSubmenu *menu);
#endif

// Adds the game under the cursor to the favorites or removes it, keeping the cursor on it.
// Favorites are saved right away by gamescan.irx when the live scan loaded it, or else
// passed to the launcher the next time a game is launched or the list is refreshed
static void toggleFavorite(void) {
  GamesSubmenu *menu = activeMenu;
  int selected = selectedGame();
  if (selected < 0)
    return;

  menu->fav[selected] = !menu->fav[selected];
  menu->favDirty = 1;
  markFavorites(menu);
  resortGames(selected);
#ifdef GAMES_MENU
  saveLiveScanFavorites(menu);
#endif
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

// Handles the submenu buttons OSDSYS doesn't: Circle (Cross on Japanese consoles) to go back,
// Square to change the sort order and Triangle to add a favorite in a games submenu, Left/Right to move a page
static void handleSubmenuButtons(void) {
  uint16_t pressed = padNewPresses();
  if (!pressed)
    return;

  if ((pressed & PADB_TRIANGLE) && activeMenu)
    toggleFavorite();
  else if (pressed & padBackButtons())
    closeSubmenu();
  else if ((pressed & PADB_SQUARE) && activeMenu)
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
// OSDSYS resets the IOP when it starts, so iomanX and gamescan.irx are loaded from the memory card
// with OSDSYS's own sceSifLoadModule() when the scan is first requested, and gamescan.irx loads mmceman
// with the controller and memory card threads suspended.
// gamescan.irx is then controlled by writing directly into its LiveScanShared structure
// in IOP RAM, since the patcher can't use SIF RPC while OSDSYS is running.
//
#define LIVESCAN_TIMEOUT_FRAMES (60 * 60) // ~60 seconds
#define LIVESCAN_HEARTBEAT_FRAMES 90      // ~1.5 seconds

static uint32_t liveScanAddr = 0; // LiveScanShared address in the EE's view of IOP RAM
static int liveScanDisabled = 0;  // Set after a failure, "Refresh list" then uses the launcher
static int liveScanLoadFrames = 0; // > 0 while "Loading modules..." is shown, before the modules are loaded
static int liveScanWaitFrames = 0; // > 0 while waiting for gamescan.irx to start after loading it
static int liveScanLoadResult = 0; // loadLiveScanModules() result
static int liveScanModulesLoaded = 0; // Modules are only loaded once
#define LIVESCAN_LOAD_DELAY_FRAMES 3 // Let "Loading modules..." be drawn before OSDSYS blocks on the loads
#define LIVESCAN_START_FRAMES 180    // ~3 seconds for gamescan.irx to write its structure
static int liveScanFrames = 0;
static uint32_t liveScanHeartbeat = 0;
static uint32_t liveScanRequest = 0; // LIVESCAN_SCAN or LIVESCAN_LIST
static uint32_t liveScanStage = 0;   // Last LIVESCAN_STAGE_* shown
static GamesSubmenu *liveScanMenu = &settings.submenus[SUBMENU_GAMES]; // Submenu being scanned

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

// Returns the EE address of the LiveScanShared structure between the EE addresses start and end, or 0
static uint32_t findLiveScan(uint32_t start, uint32_t end) {
  uint32_t found = 0;
  if (end > LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - sizeof(LiveScanShared))
    end = LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - sizeof(LiveScanShared);
  DI();
  ee_kmode_enter();
  for (uint32_t addr = start & ~3; addr < end; addr += 4) {
    volatile uint32_t *w = (volatile uint32_t *)addr;
    if ((w[0] == LIVESCAN_MAGIC0) && (w[1] == LIVESCAN_MAGIC1) && (w[2] == LIVESCAN_MAGIC2) && (w[3] == LIVESCAN_MAGIC3)) {
      found = addr;
      break;
    }
  }
  ee_kmode_exit();
  EI();
  return found;
}

// Shows label in "Refresh list" and the games again, keeping the cursor on "Refresh list"
static void showLiveScanLabel(const char *label) {
  liveScanActive = 0;
  liveScanLoadFrames = liveScanWaitFrames = 0;
  setGamesLabels(liveScanMenu, label);
  sortGames(liveScanMenu);
  showGamesEntries(liveScanMenu);
  menuInfo->currentEntry = 2 + liveScanMenu->count + 1;
}

// Ends the live scan with an error shown in the "Refresh list" label.
// The next "Refresh list" falls back to scanning with the launcher
static void failLiveScan(const char *label) {
  showLiveScanLabel(label);
  liveScanDisabled = 1;
}

// Returns 1 if value looks like the address of an IOP function: in IOP RAM (with or without the kseg0 bit) or in ROM
static int isIopFunctionAddress(uint32_t value) {
  if (!value || (value & 3))
    return 0;
  if ((value & 0x7fffffff) < LIVESCAN_IOP_RAM_SIZE)
    return 1;
  return (value & 0xfff00000) == 0xbfc00000;
}

// IRX export and import tables have the same header (magic, next, version, mode, name[8]).
// Export tables hold function addresses, import tables instruction pairs: "jr ra; addiu zero, zero, <index>"
// before they're linked and "j <function>; nop" after
#define IRX_EXPORT_MAGIC 0x41c00000
#define IRX_IMPORT_MAGIC 0x41e00000

// Returns 1 if the IOP RAM at addr (EE address) holds an export table. Registered tables become loadcore's
// iop_library_t, where the magic and next words are replaced with the previous library and the importers:
//   +0 export magic or previous library, +4 next table or importer list, +8 version, +10 flags, +12 name[8],
//   +20 exports: function addresses or NULL, with at least one function among exports 4-7
// The name must be at least 3 characters from [a-z0-9_] (IOP library names are lowercase)
static int isExportTable(volatile uint32_t *w) {
  if ((w[0] != IRX_EXPORT_MAGIC) && w[0] && !isIopFunctionAddress(w[0]))
    return 0;
  if (w[1] && !isIopFunctionAddress(w[1]))
    return 0;
  if (!(w[2] & 0xffff) || ((w[2] & 0xffff) > 0x0fff))
    return 0; // Version, like 0x0102

  int nameLen = 0;
  for (; nameLen < 8; nameLen++) {
    char c = ((volatile char *)&w[3])[nameLen];
    if (!c)
      break;
    if (!(((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_')))
      return 0;
  }
  if (nameLen < 3)
    return 0;
  for (int i = nameLen; i < 8; i++)
    if (((volatile char *)&w[3])[i])
      return 0; // Padded with NUL

  int functions = 0;
  for (int i = 0; i < 8; i++) {
    uint32_t value = w[5 + i];
    if (!value)
      continue;
    if (!isIopFunctionAddress(value))
      return 0; // An instruction: import table
    if (i >= 4)
      functions++;
  }
  return functions > 0;
}

// Calls found(address, userdata) for every export table in IOP RAM until it returns non-zero,
// and returns the address it stopped at, or 0
static uint32_t iopForEachExportTable(int (*found)(uint32_t addr, void *userdata), void *userdata) {
  const uint32_t chunk = 0x10000; // Interrupts are re-enabled between chunks
  for (uint32_t start = LIVESCAN_IOP_RAM; start < LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - 64; start += chunk) {
    uint32_t end = start + chunk;
    if (end > LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - 64)
      end = LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - 64;

    for (uint32_t addr = start; addr < end; addr += 4) {
      DI();
      ee_kmode_enter();
      int isTable = isExportTable((volatile uint32_t *)addr);
      ee_kmode_exit();
      EI();
      if (isTable && found(addr, userdata))
        return addr;
    }
  }
  return 0;
}

static int matchExportName(uint32_t addr, void *userdata) {
  uint32_t *nameWords = userdata;
  return (iopRead(addr + 12) == nameWords[0]) && (iopRead(addr + 16) == nameWords[1]);
}

// Returns the EE address of the export table of the IOP library name (up to 8 characters), or 0 if it's not loaded
static uint32_t iopFindExportTable(const char *name) {
  uint32_t nameWords[2] = {0, 0};
  memcpy(nameWords, name, (strlen(name) < 8) ? strlen(name) : 8);
  return iopForEachExportTable(matchExportName, nameWords);
}

// Reads a word of IOP code at the IOP address addr
static uint32_t iopReadCode(uint32_t addr) { return iopRead(LIVESCAN_IOP_RAM + (addr & 0x1ffffc)); }

// GetLoadcoreInternalData() (loadcore export 3) returns the address of loadcore's internal data,
// loaded with lui/addiu, which holds the list of loaded modules at +16. Returns its IOP address, or 0
static uint32_t iopLoadcoreInternals(void) {
  static uint32_t internals = 0;
  if (internals)
    return internals;
  uint32_t loadcore = iopFindExportTable("loadcore"); // One of the first tables in IOP RAM
  if (!loadcore)
    return 0;
  uint32_t func = iopRead(loadcore + 20 + 3 * 4);
  uint32_t hi = 0;
  for (int i = 0; i < 6; i++) {
    uint32_t insn = iopReadCode(func + i * 4);
    if (((insn >> 26) == 0x0f) && (((insn >> 16) & 0x1f) == 2)) // lui v0, hi
      hi = insn << 16;
    else if (((insn >> 26) == 0x09) && (((insn >> 16) & 0x1f) == 2)) // addiu v0, v0, lo
      return internals = (hi + (int16_t)(insn & 0xffff)) & 0x1fffff;
  }
  return 0;
}

// Returns the IOP address of the registered library name (up to 8 characters) from loadcore's list
// (internals +0, linked through the first word), or 0 if it's not loaded.
// Much faster than looking for its export table in the whole IOP RAM
static uint32_t iopFindLibrary(const char *name) {
  uint32_t nameWords[2] = {0, 0};
  memcpy(nameWords, name, (strlen(name) < 8) ? strlen(name) : 8);
  uint32_t internals = iopLoadcoreInternals();
  uint32_t library = internals ? (iopReadCode(internals) & 0x1fffff) : 0;
  for (int n = 0; library && (library < LIVESCAN_IOP_RAM_SIZE) && (n < 128); n++) {
    if ((iopReadCode(library + 12) == nameWords[0]) && (iopReadCode(library + 16) == nameWords[1]))
      return library;
    library = iopReadCode(library) & 0x1fffff;
  }
  return 0;
}

// Returns the version of the IOP library name, or -1 if it's not loaded
static int iopLibraryVersion(const char *name) {
  uint32_t library = iopFindLibrary(name);
  return library ? (int)(iopReadCode(library + 8) & 0xffff) : -1;
}

// Finds the IOP module name (up to 16 characters) in loadcore's module list (internals +16)
// and returns the IOP address of its text, with the size of its text, data and bss in size, or 0
static uint32_t iopFindModule(const char *name, uint32_t *size) {
  int nameLen = strlen(name);
  uint32_t internals = iopLoadcoreInternals();
  uint32_t module = internals ? (iopReadCode(internals + 16) & 0x1fffff) : 0;
  for (int n = 0; module && (module < LIVESCAN_IOP_RAM_SIZE) && (n < 64); n++) {
    char moduleName[20] = {0};
    uint32_t namePtr = iopReadCode(module + 4) & 0x1fffff;
    for (int i = 0; namePtr && (i < 20); i += 4) {
      uint32_t word = iopReadCode(namePtr + i);
      memcpy(&moduleName[i], &word, 4);
    }
    if (!memcmp(moduleName, name, nameLen + 1)) {
      *size = iopReadCode(module + 28) + iopReadCode(module + 32) + iopReadCode(module + 36);
      return iopReadCode(module + 24) & 0x1fffff;
    }
    module = iopReadCode(module) & 0x1fffff;
  }
  return 0;
}

// Returns the EE address of gamescan.irx's LiveScanShared, looking for it only in gamescan.irx's memory
static uint32_t locateLiveScan(void) {
  uint32_t size = 0;
  uint32_t start = iopFindModule("gamescan", &size);
  if (!start || (size > 0x40000))
    return 0;
  return findLiveScan(LIVESCAN_IOP_RAM + start, LIVESCAN_IOP_RAM + start + size);
}

// EE cycle counter (COP0 Count, 294.912 MHz), to time the module loads for the log
static uint32_t eeCycles(void) {
  uint32_t count;
  asm volatile("mfc0 %0, $9" : "=r"(count));
  return count;
}
#define EE_CYCLES_PER_MS 294912
static uint32_t liveScanLoadMs[2]; // Time OSDSYS was blocked loading iomanX and gamescan.irx

// Loads live scan module i (0: iomanX, 1: gamescan) from the memory card
// with OSDSYS's sceSifLoadModule(). Returns its result
static int loadLiveScanModule(int i) {
  int (*sceSifLoadModule)(const char *path, int argLength, const char *args) = (void *)settings.liveScanLoader;
  static const char *paths[] = {LIVESCAN_IRX_IOMANX, LIVESCAN_IRX_GAMESCAN};

  if ((i == 0) && (iopLibraryVersion("iomanx") >= 0))
    return 0; // Already there

  char path[32];
  strcpy(path, paths[i]);
  path[2] = (settings.mcSlot == 1) ? '1' : '0';
  return sceSifLoadModule(path, 0, NULL);
}

// Loads iomanX and gamescan.irx, which loads mmceman itself. Returns 0 on success, or
// -(module number * 1000 + error) where module number is 1 for iomanX and 2 for gamescan
static int loadLiveScanModules(void) {
  liveScanModulesLoaded = 1;
  for (int i = 0; i < 2; i++) {
    uint32_t start = eeCycles();
    int ret = loadLiveScanModule(i);
    liveScanLoadMs[i] = (eeCycles() - start) / EE_CYCLES_PER_MS;
    if (ret < 0)
      return -((i + 1) * 1000 - ret);
  }
  return 0;
}

// Waits for gamescan.irx to start after loading it, then scans
static void waitForLiveScanModule(GamesSubmenu *menu) {
  int slot = menu->base + menu->count + 1;
  strcpy(settings.menuItemName[slot], "Starting...");
  setMenuEntry(0, slot);
  menuInfo->entryCount = 3;
  menuInfo->currentEntry = 2;
  liveScanActive = 1;
  liveScanWaitFrames = LIVESCAN_START_FRAMES;
  liveScanFrames = 0;
}

// IOP libraries whose exports are named in the sio2man diagnostics
static const char *iopDiagLibNames[] = {"thsemap", "thevent", "thbase", "intrman", "sysmem", "loadcore", "dmacman", "sysclib"};
#define IOP_DIAG_LIBS (sizeof(iopDiagLibNames) / sizeof(iopDiagLibNames[0]))
static uint32_t iopDiagLibTables[IOP_DIAG_LIBS];


// games_live_scan = 2 log: the boot report (livescan_diag.c) followed by the IOP state,
// in the report buffer of livescan_diag.c, since the patcher's own memory is limited
static const char *liveScanLog = NULL;
static int liveScanLogLen = 0;
static int liveScanLogSent = 0;
static int liveScanLogPending = 0; // The first "Refresh list" with games_live_scan = 2 writes the log instead of scanning
static int liveScanLogWritten = 0; // The next ones scan, followed by a log of the scan
static int liveScanLogging = 0;    // The log is being sent to gamescan.irx
static char liveScanLogLabel[NAME_LEN]; // "Refresh list" label shown with the log name once it's written
static int liveScanLogFail = 0;         // The live scan is disabled once the log is written

static void logPrintf(const char *fmt, ...) {
#ifdef LIVESCAN
  va_list args;
  va_start(args, fmt);
  liveScanReportAppendV(fmt, args);
  va_end(args);
#endif
}

// Adds a line to the log and, while there's room, to the Games submenu entries
static int diagLines = 0;
static void diagLine(GamesSubmenu *menu, const char *fmt, ...) {
  char line[NAME_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  logPrintf("%s\n", line);
  if (diagLines < menu->max) {
    strcpy(settings.menuItemName[menu->base + diagLines], line);
    menu->played[diagLines] = 0;
    menu->fav[diagLines] = 0;
    menu->favDirty = 0; // The entries aren't games anymore
    diagLines++;
  }
}

static int logExportTable(uint32_t addr, void *userdata) {
  char name[9] = {0};
  uint32_t words[2] = {iopRead(addr + 12), iopRead(addr + 16)};
  memcpy(name, words, 8);
  int count = 4;
  while ((count < 256) && iopRead(addr + 20 + count * 4))
    count++;
  logPrintf("  %-8s v%04lx @%05lx exports %3d [%08lx %08lx]\n", name, iopRead(addr + 8) & 0xffff, addr - LIVESCAN_IOP_RAM, count, iopRead(addr),
            iopRead(addr + 4));
  return 0; // Keep going
}

// Copies the name of the IOP module whose code has the IOP address addr into name (17 bytes)
static void iopModuleName(uint32_t internals, uint32_t addr, char *name) {
  strcpy(name, "?");
  addr &= 0x1fffff;
  uint32_t module = internals ? (iopReadCode(internals + 16) & 0x1fffff) : 0;
  for (int n = 0; module && (module < LIVESCAN_IOP_RAM_SIZE) && (n < 64); n++) {
    uint32_t start = iopReadCode(module + 24) & 0x1fffff;
    if ((addr >= start) && (addr < start + iopReadCode(module + 28))) {
      uint32_t namePtr = iopReadCode(module + 4) & 0x1fffff;
      for (int i = 0; namePtr && (i < 16); i += 4) {
        uint32_t word = iopReadCode(namePtr + i);
        memcpy(&name[i], &word, 4);
      }
      name[16] = '\0';
      for (int i = 0; name[i]; i++)
        if ((name[i] < 0x20) || (name[i] > 0x7e))
          name[i] = '?';
      return;
    }
    module = iopReadCode(module) & 0x1fffff;
  }
}

// Logs the threads gamescan.irx found, the sio2man lock functions and the SIO2 interrupt handler
static void logLiveScanThreads(void) {
  uint32_t internals = iopLoadcoreInternals();
  char name[17];
  uint32_t intr = iopRead(LIVESCAN_FIELD(sio2Intr));
  iopModuleName(internals, intr, name);
  logPrintf("\nsio2man %lx lock %05lx unlock %05lx, SIO2 interrupt handler %05lx (%s) arg %05lx\n", iopRead(LIVESCAN_FIELD(sio2Version)),
            iopRead(LIVESCAN_FIELD(sio2Lock)) & 0x1fffff, iopRead(LIVESCAN_FIELD(sio2Unlock)) & 0x1fffff, intr & 0x1fffff, name,
            iopRead(LIVESCAN_FIELD(sio2IntrArg)) & 0x1fffff);

  uint32_t count = iopRead(LIVESCAN_FIELD(threadCount));
  if (count > LIVESCAN_MAX_THREADS)
    count = LIVESCAN_MAX_THREADS;
  logPrintf("\nThreads (%lu):\n  handle   entry  status wait prio sio2 module\n", count);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t thread = LIVESCAN_FIELD(threads) + i * sizeof(LiveScanThread);
    uint32_t entry = iopRead(thread + offsetof(LiveScanThread, entry));
    iopModuleName(internals, entry, name);
    logPrintf("  %08lx %05lx %6lx %4ld %4ld %4ld %s\n", iopRead(thread + offsetof(LiveScanThread, handle)), entry & 0x1fffff,
              iopRead(thread + offsetof(LiveScanThread, status)), iopRead(thread + offsetof(LiveScanThread, waitType)),
              iopRead(thread + offsetof(LiveScanThread, priority)), iopRead(thread + offsetof(LiveScanThread, sio2)), name);
  }
}

// Lists the library versions, the number of exports of the thread libraries and the IOP modules
// OSDSYS loaded (name, text start and size) in the log and the Games submenu entries
static int showIopDiagnostics(GamesSubmenu *menu) {
  for (unsigned int lib = 0; lib < IOP_DIAG_LIBS; lib++)
    iopDiagLibTables[lib] = iopFindExportTable(iopDiagLibNames[lib]);

  diagLines = 0;
  logPrintf("\nIOP state after OSDSYS started\n");
  uint32_t sio2 = iopFindExportTable("sio2man");
  diagLine(menu, "sio2man %x @%lx pad %x mc %x iox %x ld %lx", sio2 ? (int)(iopRead(sio2 + 8) & 0xffff) : -1,
           sio2 ? (sio2 - LIVESCAN_IOP_RAM) : 0, iopLibraryVersion("padman"), iopLibraryVersion("mcman"), iopLibraryVersion("iomanx"),
           settings.liveScanLoader);

  // Number of exports of the thread libraries, counted up to the NULL that ends the table
  // (export 3 can be NULL too): thbase needs 48 for GetThreadmanIdList() (export 47)
  int exportCounts[3] = {0, 0, 0};
  for (int lib = 0; lib < 3; lib++) {
    uint32_t table = iopDiagLibTables[(lib == 0) ? 2 : (lib - 1)]; // thbase, thsemap, thevent
    int count = 4;
    while (table && (count < 128) && iopRead(table + 20 + count * 4))
      count++;
    exportCounts[lib] = table ? count : -1;
  }

  uint32_t internals = iopLoadcoreInternals();
  diagLine(menu, "thbase %d thsemap %d thevent %d lc %lx", exportCounts[0], exportCounts[1], exportCounts[2], internals);

  // Registered libraries from loadcore's list (internals +0, linked through the first word)
  logPrintf("\nLibraries (loadcore list):\n");
  uint32_t library = internals ? (iopReadCode(internals) & 0x1fffff) : 0;
  for (int n = 0; library && (library < LIVESCAN_IOP_RAM_SIZE) && (n < 64); n++) {
    uint32_t words[2] = {iopReadCode(library + 12), iopReadCode(library + 16)};
    char name[9] = {0};
    memcpy(name, words, 8);
    for (int i = 0; name[i]; i++)
      if ((name[i] < 0x20) || (name[i] > 0x7e))
        name[i] = '?';
    logPrintf("  %-8s v%04lx @%05lx\n", name, iopReadCode(library + 8) & 0xffff, library);
    library = iopReadCode(library) & 0x1fffff;
  }

  // Every export table in IOP RAM, registered or not
  logPrintf("\nExport tables:\n");
  iopForEachExportTable(logExportTable, NULL);

  // Raw headers of the export tables that are still unregistered (with the export magic), marked E if they pass
  logPrintf("\nTables with the export magic (first 30):\n");
  int tables = 0;
  for (uint32_t addr = LIVESCAN_IOP_RAM; (addr < LIVESCAN_IOP_RAM + LIVESCAN_IOP_RAM_SIZE - 64) && (tables < 30); addr += 4) {
    if (iopRead(addr) != IRX_EXPORT_MAGIC)
      continue;
    uint32_t words[2] = {iopRead(addr + 12), iopRead(addr + 16)};
    char name[9] = {0};
    memcpy(name, words, 8);
    for (int i = 0; name[i]; i++)
      if ((name[i] < 0x20) || (name[i] > 0x7e))
        name[i] = '?';
    DI();
    ee_kmode_enter();
    int isExport = isExportTable((volatile uint32_t *)addr);
    ee_kmode_exit();
    EI();
    logPrintf("  %c @%05lx %-8s v%04lx:", isExport ? 'E' : '?', addr - LIVESCAN_IOP_RAM, name, iopRead(addr + 8) & 0xffff);
    for (int e = 0; e < 8; e++)
      logPrintf(" %08lx", iopRead(addr + 20 + e * 4));
    logPrintf("\n");
    tables++;
  }

  // sio2man's transfer lock (23, 24), transfer (25) and unlock (26) exports and their code
  if (sio2) {
    logPrintf("\nsio2man exports 4-%d:", 30);
    for (int e = 4; e < 31; e++)
      logPrintf("%s%lx", (e % 8) ? " " : "\n  ", iopRead(sio2 + 20 + e * 4));
    logPrintf("\n");
    for (int e = 23; e <= 26; e++) {
      uint32_t func = iopRead(sio2 + 20 + e * 4);
      logPrintf("sio2man export %d at %lx:", e, func);
      for (int w = 0; func && (w < 32); w++)
        logPrintf("%s%08lx", (w % 8) ? " " : "\n  ", iopReadCode(func + w * 4));
      logPrintf("\n");
    }
  }

  // Loaded IOP modules: name, text start and size
  logPrintf("\nModules:\n");
  uint32_t module = internals ? (iopReadCode(internals + 16) & 0x1fffff) : 0;
  for (int n = 0; module && (module < LIVESCAN_IOP_RAM_SIZE) && (n < 64); n++) {
    char name[17] = {0};
    uint32_t namePtr = iopReadCode(module + 4) & 0x1fffff;
    for (int i = 0; namePtr && (i < 16); i += 4) {
      uint32_t word = iopReadCode(namePtr + i);
      memcpy(&name[i], &word, 4);
    }
    name[16] = '\0';
    for (int i = 0; name[i]; i++)
      if ((name[i] < 0x20) || (name[i] > 0x7e))
        name[i] = '?';
    diagLine(menu, "%.16s %lx+%lx data %lx bss %lx id %lx", name, iopReadCode(module + 24), iopReadCode(module + 28), iopReadCode(module + 32),
             iopReadCode(module + 36), iopReadCode(module + 12) & 0xffff);
    module = iopReadCode(module) & 0x1fffff;
  }
  menu->count = diagLines;
  return diagLines;
}

// Copies len bytes of data into IOP RAM at the EE address addr (a multiple of 4)
static void iopWriteBytes(uint32_t addr, const char *data, int len) {
  for (int i = 0; i < len; i += 4) {
    uint32_t word = 0;
    for (int j = 0; (j < 4) && ((i + j) < len); j++)
      word |= (uint32_t)(uint8_t)data[i + j] << (j * 8);
    iopWrite(addr + i, word);
  }
}

// Starts sending the report to gamescan.irx, which writes it with the console clock in its name.
// Once written, label is shown with the log name, and the live scan is disabled if fail is set
static void sendLiveScanLog(GamesSubmenu *menu, const char *label, int fail) {
  strncpy(liveScanLogLabel, label, sizeof(liveScanLogLabel) - 1);
  liveScanLogLabel[sizeof(liveScanLogLabel) - 1] = '\0';
  liveScanLogFail = fail;
  liveScanLog = NULL;
  liveScanLogLen = 0;
#ifdef LIVESCAN
  liveScanLog = liveScanBootReport(&liveScanLogLen);
#endif

  char dir[] = "mc?:/SYS-CONF/";
  dir[2] = (settings.mcSlot == 1) ? '1' : '0';
  iopWriteString(LIVESCAN_FIELD(logName), dir, LIVESCAN_LOG_NAME_LEN);
  liveScanLogSent = 0;
  liveScanLogging = 1;
  liveScanActive = 1;
  liveScanFrames = 0;

  int slot = menu->base + menu->count + 1;
  strcpy(settings.menuItemName[slot], "Writing log...");
  setMenuEntry(0, slot);
  menuInfo->entryCount = 3;
  menuInfo->currentEntry = 2;
}

// First "Refresh list" with games_live_scan = 2: the IOP state on screen and the threads in the log,
// listed without suspending them. The next "Refresh list" scans
static void startLiveScanLog(GamesSubmenu *menu) {
  showIopDiagnostics(menu);
  // The boot report and the IOP state were already checked, so the log only has the threads,
  // which need most of the report buffer
#ifdef LIVESCAN
  liveScanReportClear();
#endif
  logPrintf("OSDMenu live scan threads\nROMVER %s\nModule loads: iomanX %lu ms, gamescan %lu ms\n", settings.romver, liveScanLoadMs[0],
            liveScanLoadMs[1]);
  logLiveScanThreads();
  liveScanLogWritten = 1;
  sendLiveScanLog(menu, "Refresh list", 0);
}

// Sends the next log chunk once gamescan.irx is done with the previous one. Called every frame
static void pollLiveScanLog(void) {
  if (iopRead(LIVESCAN_FIELD(logRequest)))
    return; // Still writing
  if (++liveScanFrames > LIVESCAN_TIMEOUT_FRAMES) {
    liveScanLogging = 0;
    failLiveScan("Refresh list [log: timeout]");
    return;
  }

  if (liveScanLogSent) {
    int result = (int)iopRead(LIVESCAN_FIELD(logResult));
    if (result < 0) {
      char label[NAME_LEN];
      snprintf(label, sizeof(label), "Refresh list [log error %d]", result);
      liveScanLogging = 0;
      failLiveScan(label);
      return;
    }
  }

  if (liveScanLogSent >= liveScanLogLen) {
    // Done: show the file name
    char name[LIVESCAN_LOG_NAME_LEN];
    char label[NAME_LEN];
    iopReadString(LIVESCAN_FIELD(logName), name, LIVESCAN_LOG_NAME_LEN);
    const char *file = strrchr(name, '/');
    snprintf(label, sizeof(label), "%.48s [%.28s]", liveScanLogLabel, file ? file + 1 : name);
    liveScanLogging = 0;
    if (liveScanLogFail)
      failLiveScan(label);
    else
      showLiveScanLabel(label);
    return;
  }

  int length = liveScanLogLen - liveScanLogSent;
  if (length > LIVESCAN_LOG_CHUNK)
    length = LIVESCAN_LOG_CHUNK;
  uint32_t request = liveScanLogSent ? LIVESCAN_LOG_NEXT : LIVESCAN_LOG_FIRST;
  if (liveScanLogSent + length >= liveScanLogLen)
    request |= LIVESCAN_LOG_LAST;
  iopWriteBytes(LIVESCAN_FIELD(logBuffer), &liveScanLog[liveScanLogSent], length);
  iopWrite(LIVESCAN_FIELD(logLength), length);
  iopWrite(LIVESCAN_FIELD(logRequest), request);
  liveScanLogSent += length;
}

// OSDSYS's sceSifLoadModule(path, argLength, args) and sceSifLoadModuleEncrypted() are consecutive
// stubs calling the same function with the LOADFILE modes 0 (module) and 4 (encrypted module)
static uint32_t patternLoadModule[] = {
    0x27bdfff0, // addiu sp, sp, -16
    0xffbf0000, // sd    ra, 0(sp)
    0x0c000000, // jal   load
    0x0000382d, // move  a3, zero
    0xdfbf0000, // ld    ra, 0(sp)
    0x03e00008, // jr    ra
    0x27bd0010, // addiu sp, sp, 16
    0x00000000, // nop
    0x27bdfff0, // addiu sp, sp, -16
    0xffbf0000, // sd    ra, 0(sp)
    0x0c000000, // jal   load
    0x24070004, // li    a3, 4
};
static uint32_t patternLoadModule_mask[] = {0xffffffff, 0xffffffff, 0xfc000000, 0xffffffff, 0xffffffff, 0xffffffff,
                                            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xfc000000, 0xffffffff};

void findLiveScanLoader(uint8_t *osd) {
  uint32_t *ptr = (uint32_t *)findPatternWithMask(osd, 0x100000, (uint8_t *)patternLoadModule, (uint8_t *)patternLoadModule_mask,
                                                  sizeof(patternLoadModule));
  if (ptr && (ptr[2] == ptr[10]))
    settings.liveScanLoader = (uint32_t)ptr;
}

// Shown while scanning, by LIVESCAN_STAGE_*
static const char *liveScanStageLabels[] = {"Scanning...",      "Reading mmceman...",    "Locking the SIO2...", "Loading mmceman...",
                                            "Scanning MMCE...", "Unlocking the SIO2...", "Saving the list..."};
#define LIVESCAN_STAGES (sizeof(liveScanStageLabels) / sizeof(liveScanStageLabels[0]))

// Describes a gamescan.irx error
static const char *liveScanErrorName(int result) {
  if (result == LIVESCAN_ERR_NO_LOCK)
    return "sio2man lock not found";
  if (result == LIVESCAN_ERR_MMCE_START)
    return "mmceman didn't start";
  if (result <= LIVESCAN_ERR_MMCE_LOAD)
    return "mmceman load error";
  if (result <= LIVESCAN_ERR_MMCE_READ)
    return "mmceman read error";
  return "write error";
}

// Sends the request (LIVESCAN_SCAN or LIVESCAN_LIST) to gamescan.irx and shows its progress until it's done
// Whether the live scan can scan the submenu's games: only MMCE devices are supported
static int liveScanUsesMMCE(GamesSubmenu *menu) {
  return (menu == &settings.submenus[SUBMENU_PSX]) ? settings.psxUseMMCE : settings.gamesUseMMCE;
}

// Writes the submenu's cache path and kind, and its favorites by cache index
static void setLiveScanTarget(GamesSubmenu *menu) {
  int psx = (menu == &settings.submenus[SUBMENU_PSX]);
  char cachePath[32];
  strcpy(cachePath, psx ? PSX_CACHE_PATH : GAMES_CACHE_PATH);
  if (settings.mcSlot == 1)
    cachePath[2] = '1';
  iopWriteString(LIVESCAN_FIELD(cachePath), cachePath, LIVESCAN_PATH_LEN);
  iopWrite(LIVESCAN_FIELD(kind), psx ? LIVESCAN_KIND_PSX : LIVESCAN_KIND_PS2);
  for (int word = 0; word < LIVESCAN_MAX_GAMES / 32; word++) {
    uint32_t bits = 0;
    for (int b = 0; b < 32; b++)
      if ((word * 32 + b < menu->count) && menu->fav[word * 32 + b])
        bits |= 1u << b;
    iopWrite(LIVESCAN_FIELD(fav) + word * 4, bits);
  }
  iopWrite(LIVESCAN_FIELD(favInput), menu->favDirty);
}

// Saves the favorites of the submenu with gamescan.irx when it's loaded, without waiting for it.
// When it's busy or not loaded, they stay unsaved and are saved by the next scan or launcher call
static void saveLiveScanFavorites(GamesSubmenu *menu) {
  if (!liveScanAddr || liveScanActive || liveScanDisabled || iopRead(LIVESCAN_FIELD(request)) ||
      (iopRead(LIVESCAN_FIELD(status)) == LIVESCAN_STATUS_BUSY))
    return;
  setLiveScanTarget(menu);
  iopWrite(LIVESCAN_FIELD(request), LIVESCAN_SAVE_FAV);
  menu->favDirty = 0;
}

static void requestLiveScan(GamesSubmenu *menu, uint32_t request) {
  iopWrite(LIVESCAN_FIELD(devices), LIVESCAN_DEV_MMCE);
  iopWriteString(LIVESCAN_FIELD(cdFolder), settings.gamesCdFolder, LIVESCAN_FOLDER_LEN);
  iopWriteString(LIVESCAN_FIELD(dvdFolder), settings.gamesDvdFolder, LIVESCAN_FOLDER_LEN);
  setLiveScanTarget(menu);
  char mmcePath[] = LIVESCAN_IRX_MMCEMAN;
  mmcePath[2] = (settings.mcSlot == 1) ? '1' : '0';
  iopWriteString(LIVESCAN_FIELD(mmcePath), mmcePath, LIVESCAN_PATH_LEN);
  iopWrite(LIVESCAN_FIELD(status), LIVESCAN_STATUS_IDLE);
  iopWrite(LIVESCAN_FIELD(request), request);
  liveScanRequest = request;
  liveScanStage = LIVESCAN_STAGE_IDLE;

  liveScanHeartbeat = iopRead(LIVESCAN_FIELD(heartbeat));
  liveScanFrames = 0;
  liveScanActive = 1;

  // Show the progress as the only entry, using the "Refresh list" label slot
  int slot = menu->base + menu->count + 1;
  strcpy(settings.menuItemName[slot], (request == LIVESCAN_LIST) ? "Listing IOP threads..." : "Scanning...");
  setMenuEntry(0, slot);
  menuInfo->entryCount = 3;
  menuInfo->currentEntry = 2;
}

// Starts a live scan of the menu and shows "Scanning..." until it's done.
// Returns 0 if the live scan is not enabled for it, so the launcher scans instead
static int startLiveScan(GamesSubmenu *menu) {
  if (!settings.gamesLiveScan || liveScanDisabled || liveScanActive || !liveScanUsesMMCE(menu))
    return 0;
  liveScanMenu = menu;

  if (!menu->cacheLoaded) {
    menu->count = 0;
    setGamesLabels(menu, "Refresh list");
  }

  // With games_live_scan = 2, the first "Refresh list" writes a diagnostics log instead of scanning,
  // and the next ones write a log of the scan
  liveScanLogPending = (settings.gamesLiveScan == 2) && !liveScanLogWritten;

  if (!liveScanAddr)
    liveScanAddr = locateLiveScan();
  if (liveScanAddr) {
    requestLiveScan(menu, liveScanLogPending ? LIVESCAN_LIST : LIVESCAN_SCAN);
    return 1;
  }

  char label[NAME_LEN];
  if (settings.liveScanBoot) {
    if (settings.liveScanBoot == LIVESCAN_BOOT_NOT_LOADED)
      strcpy(label, "Refresh list (live scan: modules not written)");
    else
      snprintf(label, sizeof(label), "Refresh list (live scan: module write error %d)", settings.liveScanBoot);
    failLiveScan(label);
    return 1;
  }
  if (!settings.liveScanLoader) {
    failLiveScan("Refresh list (live scan: OSDSYS module loader not found)");
    return 1;
  }
  if (liveScanModulesLoaded) {
    failLiveScan("Refresh list (live scan: module not found)");
    return 1;
  }

  // Show "Loading modules..." and load them a few frames later, since OSDSYS stops drawing while they load
  int slot = menu->base + menu->count + 1;
  strcpy(settings.menuItemName[slot], "Loading modules...");
  setMenuEntry(0, slot);
  menuInfo->entryCount = 3;
  menuInfo->currentEntry = 2;
  liveScanActive = 1;
  liveScanLoadFrames = LIVESCAN_LOAD_DELAY_FRAMES;
  return 1;
}

// Called once per frame while the live scan is running
static void pollLiveScan(void) {
  if (liveScanLogging) {
    pollLiveScanLog();
    return;
  }

  if (liveScanLoadFrames) {
    if (--liveScanLoadFrames)
      return;
    liveScanLoadResult = loadLiveScanModules();
    if (liveScanLoadResult < 0) {
      char label[NAME_LEN];
      snprintf(label, sizeof(label), "Refresh list (live scan: load error %d)", liveScanLoadResult);
      failLiveScan(label);
      return;
    }
    waitForLiveScanModule(liveScanMenu);
    return;
  }

  if (liveScanWaitFrames) {
    // gamescan.irx writes its structure once its thread starts
    if (!(liveScanFrames++ % 15) && (liveScanAddr = locateLiveScan())) {
      liveScanWaitFrames = 0;
      requestLiveScan(liveScanMenu, liveScanLogPending ? LIVESCAN_LIST : LIVESCAN_SCAN);
      return;
    }
    if (!--liveScanWaitFrames)
      failLiveScan("Refresh list (live scan: loaded, but not started)");
    return;
  }

  liveScanFrames++;
  if ((liveScanFrames == LIVESCAN_HEARTBEAT_FRAMES) && (iopRead(LIVESCAN_FIELD(heartbeat)) == liveScanHeartbeat)) {
    failLiveScan("Refresh list (live scan: module not responding)");
    return;
  }

  if ((iopRead(LIVESCAN_FIELD(status)) != LIVESCAN_STATUS_DONE) || iopRead(LIVESCAN_FIELD(request))) {
    // Show what gamescan.irx is doing
    uint32_t stage = iopRead(LIVESCAN_FIELD(stage));
    if ((liveScanRequest == LIVESCAN_SCAN) && (stage != liveScanStage) && (stage < LIVESCAN_STAGES)) {
      liveScanStage = stage;
      strcpy(settings.menuItemName[liveScanMenu->base + liveScanMenu->count + 1], liveScanStageLabels[stage]);
    }
    if (liveScanFrames > LIVESCAN_TIMEOUT_FRAMES)
      failLiveScan("Refresh list (live scan: timeout)");
    return;
  }

  GamesSubmenu *menu = liveScanMenu;
  if (liveScanRequest == LIVESCAN_LIST) {
    startLiveScanLog(menu);
    return;
  }

  int result = (int)iopRead(LIVESCAN_FIELD(result));
  int count = iopRead(LIVESCAN_FIELD(count));
  if (count > menu->max)
    count = menu->max;
  if (count > LIVESCAN_MAX_GAMES)
    count = LIVESCAN_MAX_GAMES;

  char label[NAME_LEN] = "Refresh list";
  if (result < 0) {
    // The cache the launcher reads wasn't updated, so the list can't be launched from
    snprintf(label, sizeof(label), "Refresh list (live scan: %s %d)", liveScanErrorName(result), result);
    count = 0;
    menu->favDirty = 0; // The favorites mask is by cache index, which doesn't match an empty list
  } else if (iopRead(LIVESCAN_FIELD(watchdogResumed)))
    strcpy(label, "Refresh list (live scan: controller resumed by the watchdog)");

  // gamescan.irx keeps the "played" counters and favorites of the games that are still there
  for (int i = 0; i < count; i++) {
    iopReadString(LIVESCAN_FIELD(names) + i * LIVESCAN_NAME_LEN, settings.menuItemName[menu->base + i], LIVESCAN_NAME_LEN);
    menu->played[i] = iopRead(LIVESCAN_FIELD(played) + i * 4);
    menu->fav[i] = (iopRead(LIVESCAN_FIELD(fav) + (i / 32) * 4) >> (i % 32)) & 1;
  }
  menu->count = count;
  if (result >= 0) {
    menu->cacheLoaded = 1;
    menu->favDirty = 0;
    markFavorites(menu);
    setSubmenuEntryLabel(menu);
  }

  if (settings.gamesLiveScan == 2) {
    // Log of the scan: its result, and the threads after it
#ifdef LIVESCAN
    liveScanReportClear();
#endif
    logPrintf("OSDMenu live scan log\nROMVER %s\n\nScan result %d, games %d, watchdog %ld, mmceman %ld\n", settings.romver, result, count,
              iopRead(LIVESCAN_FIELD(watchdogResumed)), iopRead(LIVESCAN_FIELD(mmceLoaded)));
    logLiveScanThreads();
    logPrintf("\nGames:\n");
    for (int i = 0; i < count; i++)
      logPrintf("  %s\n", settings.menuItemName[menu->base + i]);
    sendLiveScanLog(menu, label, result < 0);
    return;
  }

  if (result < 0) {
    failLiveScan(label);
    return;
  }
  liveScanActive = 0;
  setGamesLabels(menu, label);
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
static void closeSubmenu(void) {
  for (int i = 0; i < settings.menuItemCount; i++)
    setMenuEntry(i, i);

  menuInfo->entryCount = 2 + settings.menuItemCount;
  menuInfo->currentEntry = gamesMenuReturnEntry;
  activeMenu = NULL;
  activeGroup = NULL;
}

// Replaces the custom entries with "< Back" and the items of the group
static void openGroupMenu(MenuGroup *group) {
  gamesMenuReturnEntry = menuInfo->currentEntry;
  setMenuEntry(0, settings.groupBackSlot);
  for (int i = 0; i < group->count; i++)
    setMenuEntry(1 + i, group->first + i);

  menuInfo->entryCount = 3 + group->count;
  menuInfo->currentEntry = (group->count > 0) ? 3 : 2; // First item
  activeGroup = group;
}

// Handles X on an entry of the active group
static void handleGroupMenuEntry(int pos) {
  if (pos == 0) {
    closeSubmenu();
    return;
  }

  int slot = activeGroup->first + pos - 1;
  if (settings.menuItemName[slot][0] == '$' && settings.menuItemName[slot][1] == '!')
    return;
  launchMenuItem(settings.menuItemIdx[slot], "");
}

// Handles X on an entry of the active submenu
static void handleGamesMenuEntry(int pos) {
  GamesSubmenu *menu = activeMenu;
  if (pos == 0) {
    closeSubmenu();
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

#ifndef HOSD
// Called once per frame: polls the live scan, or the buttons of the games submenu or menu group
static void pollSubmenu(void) {
#ifdef GAMES_MENU
  if (liveScanActive) {
    pollLiveScan();
    return;
  }
#endif
  // OSDSYS would open the Version screen with Triangle
  padHideTriangle(activeMenu || activeGroup);
  if (activeMenu || activeGroup)
    handleSubmenuButtons();
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
  if (activeGroup) {
    if (pos < customItemCount())
      handleGroupMenuEntry(pos);
    return 0;
  }
#endif

  if (pos >= settings.menuItemCount)
    return 0;

  if (settings.menuItemName[pos][0] == '$' && settings.menuItemName[pos][1] == '!')
    return 0;

  int idx = settings.menuItemIdx[pos];

#ifndef HOSD
  if ((idx >= GROUP_MENU_IDX_BASE) && (idx < GROUP_MENU_IDX_BASE + settings.groupCount)) {
    openGroupMenu(&settings.groups[idx - GROUP_MENU_IDX_BASE]);
    return 0;
  }

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
#ifndef HOSD
  if (num == 0)
    pollSubmenu();
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
#ifndef HOSD
  if (num == 0)
    pollSubmenu();
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
// Button prompts of the games submenus: "Back" replaces "Version", and the sort order (Square)
// and "Favorite" (Triangle) are added in between.
// OSDSYS only draws the Enter (Cross, or Circle on Japanese consoles) and Version (Triangle) icons
// in the main menu, so Circle and Square are derived from Version. The icon types are Square,
// Triangle, Cross, Circle in that order (seen on ROM 2.30: 2, 3, 4, 5).
// If Enter is neither Cross nor Circle in that order, only the texts are shown.
static int enterIconType = -1;
static int versionIconType = -1;
static int backIconType = -1;
static int sortIconType = -1;
// Icon types seen in other panels (System Configuration shows Circle and Square), for games_button_debug
static int seenIconTypes[8];
static int seenIconCount = 0;

static void recordIconType(int type) {
  for (int i = 0; i < seenIconCount; i++)
    if (seenIconTypes[i] == type)
      return;
  if (seenIconCount < (int)(sizeof(seenIconTypes) / sizeof(seenIconTypes[0])))
    seenIconTypes[seenIconCount++] = type;
}

static void deriveSubmenuIcons(void) {
  backIconType = sortIconType = -1;
  if ((enterIconType < 0) || (versionIconType < 0))
    return;

  int cross = versionIconType + 1;
  int circle = versionIconType + 2;
  if (enterIconType == cross) // Enter is Cross: back with Circle
    backIconType = circle;
  else if (enterIconType == circle) // Enter is Circle: back with Cross
    backIconType = cross;
  else
    return;
  sortIconType = versionIconType - 1; // Square
  if (sortIconType < 0)
    backIconType = sortIconType = -1;
}

// Returns 1 when the main menu button panel shows a games submenu
static int showSubmenuPrompts(void) { return (ButtonsPanel_Type == MAINMENU_PANEL) && ((activeMenu && !liveScanActive) || activeGroup); }

// X coordinates of the sort and "Favorite" prompts: Enter, sort, "Favorite" and Back are evenly spaced
static int sortPromptX(void) { return settings.enterX + (settings.versionX - settings.enterX) / 3; }
static int favPromptX(void) { return settings.enterX + (settings.versionX - settings.enterX) * 2 / 3; }
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
    if (showSubmenuPrompts() && activeGroup) {
      DrawNonSelectableItem(settings.versionX + 28, settings.versionY, color, alpha, "Back");
      return;
    }
    if (showSubmenuPrompts()) {
      if (settings.buttonDebug) {
        // "e<Enter> v<Version> b<Back> s<Sort> seen:<types seen in other panels>", on its own line above the prompts
        char debug[80];
        int len = snprintf(debug, sizeof(debug), "e%d v%d b%d s%d r%lx/%d n%lu seen:", enterIconType, versionIconType, backIconType,
                           sortIconType, padReadAddr, padReadRedirects, padReadCalls % 10000);
        for (int i = 0; (i < seenIconCount) && (len < (int)sizeof(debug) - 4); i++)
          len += snprintf(&debug[len], sizeof(debug) - len, "%s%d", i ? "," : "", seenIconTypes[i]);
        DrawNonSelectableItem(settings.enterX, settings.versionY - 18, color, alpha, debug);
        if (settings.gamesCovers) {
          // "cov<1 when ready> s<sprite submit> t<set texture> l<load image> y<sync path>"
          snprintf(debug, sizeof(debug), "cov%d s%lx t%lx l%lx y%lx", coversReady, coverSpriteAddr, coverTextureAddr, coverLoadImageAddr,
                   coverSyncPathAddr);
          DrawNonSelectableItem(settings.enterX, settings.versionY - 36, color, alpha, debug);
        }
      }
      // Short texts, since the four prompts share the space of two
      DrawNonSelectableItem(sortPromptX() + 28, settings.versionY, color, alpha, activeMenu->sortRecent ? "[Recent]" : "[A-Z]");
      DrawNonSelectableItem(favPromptX() + 28, settings.versionY, color, alpha, "Favorite");
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
    // Game covers, drawn before the icon so DrawIcon() selects its own texture again
    static GamesSubmenu *coversMenu = NULL;
    if (settings.gamesCovers && showSubmenuPrompts() && activeMenu) {
      coversDraw(coversMenu != activeMenu, alpha);
      coversMenu = activeMenu;
    } else
      coversMenu = NULL;
#endif
    DrawIcon(type, settings.enterX, settings.enterY, alpha);
  } else {
#ifndef HOSD
    recordIconType(type);
#endif
    DrawIcon(type, X, Y, alpha);
  }
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
      if ((sortIconType >= 0) && activeMenu) {
        DrawIcon(sortIconType, sortPromptX(), settings.versionY, alpha);
        DrawIcon(versionIconType, favPromptX(), settings.versionY, alpha); // Triangle
      }
      if (backIconType >= 0)
        DrawIcon(backIconType, settings.versionX, settings.versionY, alpha);
      return;
    }
#endif
    DrawIcon(type, settings.versionX, settings.versionY, alpha);
  } else {
#ifndef HOSD
    recordIconType(type);
#endif
    DrawIcon(type, X, Y, alpha);
  }
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
#ifndef HOSD
  if (settings.gamesCovers)
    coversInit((uint32_t)DrawIcon);
#endif

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
