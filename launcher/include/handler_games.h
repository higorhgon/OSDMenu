#ifndef _HANDLER_GAMES_H_
#define _HANDLER_GAMES_H_

#include "cnf.h"

#define GAMES_FOLDER_NAME_LEN 32

// Settings parsed from OSDMENU.CNF by handler_osdm.c, consumed by handleGames().
// neutrinoPath, neutrinoArgs, returnPath, oplPath and smbConfigPath are heap-allocated and owned by the caller.
typedef enum {
  GamesKind_PS2, // PS2 games from the CD/DVD folders, launched via Neutrino or OPL
  GamesKind_PSX, // PS1 games from EMBER/games, launched via Ember
} GamesKind;

typedef struct {
  GamesKind kind; // Set by handleOSDM from the menu item index
  int psxUseUSB;  // psx_device_* keys
  int psxUseMX4SIO;
  int psxUseMMCE;
  int useUSB;
  int useMX4SIO;
  int useMMCE;
  int useUDPFS;                          // UDPFS server, games launched via Neutrino
  int useSMB;                            // SMB share configured in OPL, games launched via OPL
  char *smbConfigPath;                   // OPL's conf_network.cfg, found automatically when not set
  char cdFolder[GAMES_FOLDER_NAME_LEN];  // PS2 titles released on CD media
  char dvdFolder[GAMES_FOLDER_NAME_LEN]; // PS2 titles released on DVD media
  char *neutrinoPath;                    // Path to the user-installed neutrino.elf
  linkedStr *neutrinoArgs;               // Extra static arguments appended to every launch
  char *returnPath;                      // ELF to run after scanning instead of OSDMenu itself
  int useOPL;                            // Launch games with oplPath instead of Neutrino when possible
  char *oplPath;                         // Path to OPL (e.g. RiptOPL), used when useOPL is set
  int mmceGameID;                        // Send the game ID to MMCE devices so they switch to the game's memory card
  int covers;                            // Convert the ART covers of the MMCE games when scanning (games_covers)
  int coverIco;                          // Use the disc images (_ICO) instead of the case covers (_COV) (games_cover_type = ico)
} GamesConfig;

// handler_games.c
//
// osdmArg is the osdm path that led here. The patcher appends a mode suffix:
//   ":s"  - scan the enabled devices (USB/MX4SIO/MMCE/UDPFS/SMB), write the cache
//           (GAMES_CACHE_PATH or PSX_CACHE_PATH) and return to OSDMenu
//   ":gN" - launch game N from the cache without scanning
// PS2 games come from the CD/DVD folders and run via Neutrino or OPL,
// PS1 games (cfg->kind == GamesKind_PSX) come from EMBER/games and run via Ember.
// followed by an optional "|<patcher path>" used to return to OSDMenu afterwards.
// Without a suffix (e.g. HOSDMenu), scans and shows a full-screen list instead.
// Only returns on error; otherwise control passes to launchPath() or ExecOSD().
int handleGames(GamesConfig *cfg, const char *osdmArg);

#endif
