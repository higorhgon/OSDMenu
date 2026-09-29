#ifndef _HANDLER_GAMES_H_
#define _HANDLER_GAMES_H_

#include "cnf.h"

#define GAMES_FOLDER_NAME_LEN 32

// Settings parsed from OSDMENU.CNF by handler_osdm.c, consumed by handleGames().
// neutrinoPath, neutrinoArgs, returnPath and oplPath are heap-allocated and owned by the caller.
typedef struct {
  int useUSB;
  int useMX4SIO;
  int useMMCE;
  char cdFolder[GAMES_FOLDER_NAME_LEN];  // PS2 titles released on CD media
  char dvdFolder[GAMES_FOLDER_NAME_LEN]; // PS2 titles released on DVD media
  char *neutrinoPath;                    // Path to the user-installed neutrino.elf
  linkedStr *neutrinoArgs;               // Extra static arguments appended to every launch
  char *returnPath;                      // ELF to run after scanning instead of OSDMenu itself
  int useOPL;                            // Launch games with oplPath instead of Neutrino when possible
  char *oplPath;                         // Path to OPL (e.g. RiptOPL), used when useOPL is set
  int mmceGameID;                        // Send the game ID to MMCE devices so they switch to the game's memory card
} GamesConfig;

// handler_games.c
//
// osdmArg is the osdm path that led here. The patcher appends a mode suffix:
//   ":s"  - scan the configured CD/DVD folders on the enabled devices
//           (USB/MX4SIO/MMCE), write GAMES_CACHE_PATH and return to OSDMenu
//   ":gN" - launch game N from GAMES_CACHE_PATH via neutrino.elf, no scan
// followed by an optional "|<patcher path>" used to return to OSDMenu afterwards.
// Without a suffix (e.g. HOSDMenu), scans and shows a full-screen list instead.
// Only returns on error; otherwise control passes to launchPath() or ExecOSD().
int handleGames(GamesConfig *cfg, const char *osdmArg);

#endif
