#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include "gs.h"
#include <stdint.h>

// Embedded CNF file
#ifdef EMBED_CNF
extern unsigned char embedded_cnf[] __attribute__((aligned(16))) __attribute__((aligned(16)));
extern uint32_t size_embedded_cnf;
// By default, points to embedded_cnf
// Relocated by the patcher to EXTRA_RELOC_ADDR during startup
extern uint8_t *embedded_cnf_addr;
#endif

#define CUSTOM_ITEMS 200 // Max number of items in custom menu
#define NAME_LEN 80      // Max menu item length (incl. the string terminator)

typedef enum {
  FLAG_SKIP_DISC = (1 << 0),      // Disable disc autolaunch
  FLAG_SCROLL_MENU = (1 << 1),    // Enable infinite scrolling
  FLAG_SKIP_PS2_LOGO = (1 << 2),  // Skip PS2LOGO when booting discs
  FLAG_DISABLE_GAMEID = (1 << 3), // Disable PixelFX game ID
  FLAG_USE_DKWDRV = (1 << 4),     // Use DKWDRV for PS1 discs
  FLAG_PS1DRV_FAST = (1 << 5),    // If set, will force PS1DRV fast disc speed
  FLAG_PS1DRV_SMOOTH = (1 << 6),  // If set, will force PS1DRV texture smoothing
  FLAG_PS1DRV_USE_VN = (1 << 7),  // If set, run PS1DRV via the PS1DRV Video Mode Negator
  FLAG_APP_GAMEID = (1 << 8),     // Used to show OSDMenu game ID on the splash screen
  FLAG_CUSTOM_MENU = (1 << 9),    // Apply menu patches
  FLAG_PSX = (1 << 10),           // Used to selectively apply patches on PSX
} PatcherFlags;

typedef enum {
  OSD_REGION_DEFAULT,
  OSD_REGION_JAP,
  OSD_REGION_USA,
  OSD_REGION_EUR,
} OSDRegion;

typedef enum {
  OSD_BOOT_DEFAULT,
  OSD_BOOT_OPENING,
  OSD_BOOT_CLOCK,
  OSD_BOOT_BROWSER,
} OSDBoot;

// settings.liveScanBoot values besides the loadLiveScanModules() result
#define LIVESCAN_BOOT_NOT_LOADED 1 // Modules were not loaded
#define LIVESCAN_BOOT_NOT_FOUND 2  // Modules were loaded, but gamescan.irx wasn't found in IOP RAM

// Games submenus, shown by the automatic "Games >" and "PSX >" entries
#define SUBMENU_GAMES 0 // PS2 games (GAMES_MENU_IDX, GAMES_CACHE_PATH)
#define SUBMENU_PSX 1   // PS1 games (PSX_MENU_IDX, PSX_CACHE_PATH)
#define SUBMENU_COUNT 2

// Game names are stored in menuItemName after the regular menu items, each submenu
// in its own fixed region of max + 2 slots: the names, then the "< Back" and "Refresh list" labels
typedef struct {
  int itemIdx;                   // GAMES_MENU_IDX or PSX_MENU_IDX when the entry is shown, -1 otherwise
  int base;                      // First menuItemName slot of the region
  int max;                       // Maximum number of names
  int count;                     // Number of names loaded
  int cacheLoaded;               // Whether the cache file was loaded (or the list was scanned live)
  int sortRecent;                // Sort by most recently played instead of by name (toggled with Square)
  uint32_t played[CUSTOM_ITEMS]; // "played" counter of each game in cache order, 0 if never played
  uint8_t order[CUSTOM_ITEMS];   // Cache index of the game shown at each position
} GamesSubmenu;

// Patcher settings struct, contains all configurable patch settings and menu items
typedef struct {
  uint32_t colorSelected[4];                 // The menu items color when selected
  uint32_t colorUnselected[4];               // The menu items color when not selected
  int menuX;                                 // Menu X coordinate (menu center)
  int menuY;                                 // Menu Y coordinate (menu center), only for scroll menu
  int enterX;                                // "Enter" button X coordinate (at main OSDSYS menu)
  int enterY;                                // "Enter" button Y coordinate (at main OSDSYS menu)
  int versionX;                              // "Version" button X coordinate (at main OSDSYS menu)
  int versionY;                              // "Version" button Y coordinate (at main OSDSYS menu)
  int cursorMaxVelocity;                     // The cursors movement amplitude, only for scroll menu
  int cursorAcceleration;                    // The cursors speed, only for scroll menu
  int displayedItems;                        // The number of menu items displayed, only for scroll menu
  int menuItemIdx[CUSTOM_ITEMS];             // Item index in the config file
  int menuItemCount;                         // Total number of valid menu items
  uint16_t patcherFlags;                     // Patcher options
  char leftCursor[20];                       // The left cursor text, only for scroll menu
  char rightCursor[20];                      // The right cursor text, only for scroll menu
  char menuDelimiterTop[NAME_LEN];           // The top menu delimiter text, only for scroll menu
  char menuDelimiterBottom[NAME_LEN];        // The bottom menu delimiter text, only for scroll menu
  char menuItemName[CUSTOM_ITEMS][NAME_LEN]; // Menu items text
  GSVideoMode videoMode;                     // OSDSYS Video mode (0 for auto)
  OSDRegion region;                          // OSD Region
  OSDBoot boot;                              // OSD boot flags
  char romver[15];                           // ROMVER string, initialized before patching
#ifndef HOSD
  char dkwdrvPath[50]; // Path to DKWDRV
  uint8_t mcSlot;      // Memory card slot contaning currently loaded OSDMENU.CNF
  GamesSubmenu submenus[SUBMENU_COUNT];
  char bootPath[64]; // argv[0] of the patcher, passed to the launcher so it can return to OSDMenu
  int reopenSubmenu; // SUBMENU_* + 1 to open as soon as the menu is shown (GAMES_REOPEN_ARG/PSX_REOPEN_ARG), 0 for none
  int buttonDebug;   // Show the OSDSYS button icon types in the submenu prompts (games_button_debug)
  // Experimental live scan (games_live_scan), see livescan.h
  int gamesLiveScan;
  int liveScanBoot; // loadLiveScanModules() result, or LIVESCAN_BOOT_* (for diagnostics)
  int gamesUseMMCE;
  char gamesCdFolder[32];
  char gamesDvdFolder[32];
#endif
} PatcherSettings;

// Stores patcher settings and OSDSYS menu items
extern PatcherSettings settings;

int loadConfig(void);
void initConfig(void);
#ifndef HOSD
// Reserves the menuItemName regions of the games submenus and loads the names from their caches
void loadGamesCache(void);
#endif

#endif
