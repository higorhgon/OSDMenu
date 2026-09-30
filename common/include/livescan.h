#ifndef _LIVESCAN_H_
#define _LIVESCAN_H_

// Experimental live games scan (games_live_scan).
//
// OSDSYS resets the IOP when it starts, so modules loaded by the patcher are lost.
// Instead, the patcher writes iomanX, mmceman and gamescan.irx to the memory card
// (LIVESCAN_IRX_*) before starting OSDSYS, and loads them from there when the scan
// is first requested, through OSDSYS's own sceSifLoadModule().
// OSDSYS owns the SIF RPC stack once it's running, so the patcher talks to
// gamescan.irx through this structure in IOP RAM, which the EE can access directly at
// LIVESCAN_IOP_RAM (uncached). The structure is located by its magic words,
// which gamescan.irx only writes at runtime so the module image never contains them.

// Module files on the memory card OSDMENU.CNF was loaded from ('?' is replaced with the slot)
#define LIVESCAN_IRX_IOMANX "mc?:/SYS-CONF/LSIOMANX.IRX"
#define LIVESCAN_IRX_MMCEMAN "mc?:/SYS-CONF/LSMMCE.IRX"
#define LIVESCAN_IRX_GAMESCAN "mc?:/SYS-CONF/LSGSCAN.IRX"

#define LIVESCAN_MAGIC0 0x4d44534f // "OSDM"
#define LIVESCAN_MAGIC1 0x4556494c // "LIVE"
#define LIVESCAN_MAGIC2 0x4e414353 // "SCAN"
#define LIVESCAN_MAGIC3 0x31303030 // "0001"

#define LIVESCAN_IOP_RAM 0xbc000000 // IOP RAM as seen from the EE (uncached)
#define LIVESCAN_IOP_RAM_SIZE 0x200000

#define LIVESCAN_MAX_GAMES 128
#define LIVESCAN_NAME_LEN 80 // Same as the patcher's NAME_LEN
#define LIVESCAN_FOLDER_LEN 32
#define LIVESCAN_PATH_LEN 32

// status
#define LIVESCAN_STATUS_IDLE 0
#define LIVESCAN_STATUS_BUSY 1
#define LIVESCAN_STATUS_DONE 2

// devices
#define LIVESCAN_DEV_MMCE (1 << 0)

typedef struct {
  unsigned int magic[4];
  volatile unsigned int heartbeat; // Incremented by the IOP thread on every poll
  volatile unsigned int request;   // Set to 1 by the EE to start a scan, cleared by the IOP
  volatile unsigned int status;    // LIVESCAN_STATUS_*
  volatile int result;             // Cache write result: 0 on success, < 0 on error
  unsigned int devices;            // LIVESCAN_DEV_* to scan, set by the EE
  unsigned int count;              // Number of games in names
  char cdFolder[LIVESCAN_FOLDER_LEN];
  char dvdFolder[LIVESCAN_FOLDER_LEN];
  char cachePath[LIVESCAN_PATH_LEN]; // GAMES_CACHE_PATH on the right memory card, set by the EE
  char names[LIVESCAN_MAX_GAMES][LIVESCAN_NAME_LEN];
} LiveScanShared;

#endif
