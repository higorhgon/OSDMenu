#ifndef _HANDLER_GAMES_H_
#define _HANDLER_GAMES_H_

#include "cnf.h"

#define GAMES_FOLDER_NAME_LEN 32

// Settings parsed from OSDMENU.CNF by handler_osdm.c, consumed by handleGames().
// neutrinoPath and neutrinoArgs are heap-allocated and owned by the caller.
typedef struct {
  int useUSB;
  int useMX4SIO;
  int useMMCE;
  char cdFolder[GAMES_FOLDER_NAME_LEN];  // PS2 titles released on CD media
  char dvdFolder[GAMES_FOLDER_NAME_LEN]; // PS2 titles released on DVD media
  char *neutrinoPath;                    // Path to the user-installed neutrino.elf
  linkedStr *neutrinoArgs;               // Extra static arguments appended to every launch
} GamesConfig;

// handler_games.c
//
// Scans the configured CD/DVD folders on the enabled devices (USB/MX4SIO/MMCE),
// shows a full-screen list and launches the selected game via neutrino.elf.
// Only returns on error; on success or when the user backs out, control passes
// to launchPath()/ExecOSD() and never returns.
int handleGames(GamesConfig *cfg);

#endif
