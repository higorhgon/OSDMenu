#ifndef _LIVESCAN_H_
#define _LIVESCAN_H_
#include "covers_raw.h"

// Experimental live games scan (games_live_scan).
//
// OSDSYS resets the IOP when it starts, so modules loaded by the patcher are lost.
// Instead, the patcher writes iomanX, mmceman and gamescan.irx to the memory card
// (LIVESCAN_IRX_*) before starting OSDSYS, and loads iomanX and gamescan.irx from there
// when the scan is first requested, through OSDSYS's own sceSifLoadModule().
// OSDSYS owns the SIF RPC stack once it's running, so the patcher talks to
// gamescan.irx through this structure in IOP RAM, which the EE can access directly at
// LIVESCAN_IOP_RAM (uncached). The structure is located by its magic words,
// which gamescan.irx only writes at runtime so the module image never contains them.

// gamescan.irx loads mmceman itself, since OSDSYS's rom0:SIO2MAN is the older thread-based
// sio2man 1.2, which mmceman can't share the SIO2 with. While mmceman runs, gamescan.irx holds
// sio2man's transfer lock, which stops the controller and memory card drivers for the duration of the scan.
//
// Module files on the memory card OSDMENU.CNF was loaded from ('?' is replaced with the slot)
#define LIVESCAN_IRX_IOMANX "mc?:/SYS-CONF/LSIOMANX.IRX"
#define LIVESCAN_IRX_MMCEMAN "mc?:/SYS-CONF/LSMMCE.IRX"
#define LIVESCAN_IRX_GAMESCAN "mc?:/SYS-CONF/LSGSCAN.IRX"

#define LIVESCAN_MAGIC0 0x4d44534f // "OSDM"
#define LIVESCAN_MAGIC1 0x4556494c // "LIVE"
#define LIVESCAN_MAGIC2 0x4e414353 // "SCAN"
#define LIVESCAN_MAGIC3 0x38303030 // "0008"

#define LIVESCAN_IOP_RAM 0xbc000000 // IOP RAM as seen from the EE (uncached)
#define LIVESCAN_IOP_RAM_SIZE 0x200000

#define LIVESCAN_MAX_GAMES 128
#define LIVESCAN_NAME_LEN 80 // Same as the patcher's NAME_LEN
#define LIVESCAN_FOLDER_LEN 32
#define LIVESCAN_PATH_LEN 32
#define LIVESCAN_COVER_NAME_LEN 80 // Game name
#define LIVESCAN_COVER_SUFFIX_LEN 12 // COVER_RAW_*_SUFFIX
#define LIVESCAN_COVER_CHUNK 4096
#define LIVESCAN_COVER_CANCELLED -125 // ECANCELED

// status
#define LIVESCAN_STATUS_IDLE 0
#define LIVESCAN_STATUS_BUSY 1
#define LIVESCAN_STATUS_DONE 2

// devices
#define LIVESCAN_DEV_MMCE (1 << 0)

// kind: what to scan
#define LIVESCAN_KIND_PS2 0 // PS2 ISOs in cdFolder and dvdFolder (GAMES_CACHE_PATH)
#define LIVESCAN_KIND_PSX 1 // PS1 games for Ember in EMBER/games/<game>/ with a .cue file (PSX_CACHE_PATH)

// request
#define LIVESCAN_SCAN 1     // Scan and write the cache, keeping the sort order, "played" counters and favorites of the games still found
#define LIVESCAN_LIST 2     // Only list the threads (threads, threadCount), for the diagnostics log
#define LIVESCAN_SAVE_FAV 3 // Rewrite the favorites of the cache at cachePath from fav (by cache index)
#define LIVESCAN_COVER 4    // Read the cover of game coverIndex of the cache at cachePath into the buffer at coverAddr (games_covers)
#define LIVESCAN_PREPARE 5  // Load mmceman from mmcePath ahead of the first cover (games_covers), the result in result
#define LIVESCAN_READ_MMCE 6 // Only read mmceman from mmcePath into IOP RAM, which doesn't pause the controller, the result in result

// stage: what gamescan.irx is doing, shown while scanning
#define LIVESCAN_STAGE_IDLE 0
#define LIVESCAN_STAGE_READ_MMCE 1 // Reading mmceman from the memory card
#define LIVESCAN_STAGE_PAUSE 2     // Waiting for sio2man's transfer lock
#define LIVESCAN_STAGE_LOAD_MMCE 3 // Loading mmceman from IOP RAM
#define LIVESCAN_STAGE_SCAN 4      // Scanning the MMCE
#define LIVESCAN_STAGE_RESUME 5    // Handing the SIO2 back
#define LIVESCAN_STAGE_WRITE 6     // Writing the cache

// result errors other than iomanX's
#define LIVESCAN_ERR_MMCE_READ -1000  // - errno: mmceman couldn't be read
#define LIVESCAN_ERR_MMCE_LOAD -2000  // - error: LoadModuleBuffer() failed
#define LIVESCAN_ERR_MMCE_START -3000 // StartModule() failed or mmceman didn't stay resident
#define LIVESCAN_ERR_NO_LOCK -4000    // sio2man's transfer lock not found

#define LIVESCAN_MAX_THREADS 48

// Thread found in IOP RAM (all words, so the EE can read them one at a time)
typedef struct {
  int handle;
  unsigned int entry;
  unsigned int status;   // THS_* when listed
  unsigned int priority; // Current priority
  unsigned int waitType; // TSW_* when waiting
  int sio2;              // 1 if the thread belongs to a module that uses the SIO2
} LiveScanThread;

// logRequest: the EE sends a log in chunks of up to LIVESCAN_LOG_CHUNK bytes, which gamescan.irx
// writes to <logName directory>OSDMLIVE-<YYMMDD>-<HHMMSS>.LOG (console clock), with the full path
// written back to logName. Cleared by the IOP when the chunk is written
#define LIVESCAN_LOG_CHUNK 4096
#define LIVESCAN_LOG_NAME_LEN 48
#define LIVESCAN_LOG_FIRST 1 // Creates the file with the first chunk
#define LIVESCAN_LOG_NEXT 2  // Appends the next chunk
#define LIVESCAN_LOG_LAST 4  // Closes the file after the chunk (combined with FIRST or NEXT)

typedef struct {
  unsigned int magic[4];
  volatile unsigned int heartbeat; // Incremented by the IOP thread on every poll
  volatile unsigned int request;   // LIVESCAN_*, set by the EE and cleared by the IOP
  volatile unsigned int status;    // LIVESCAN_STATUS_*
  volatile int result;             // Cache write result: 0 on success, < 0 on error
  unsigned int devices;            // LIVESCAN_DEV_* to scan, set by the EE
  unsigned int kind;               // LIVESCAN_KIND_*, set by the EE
  unsigned int count;              // Number of games in names
  char cdFolder[LIVESCAN_FOLDER_LEN];
  char dvdFolder[LIVESCAN_FOLDER_LEN];
  char cachePath[LIVESCAN_PATH_LEN]; // GAMES_CACHE_PATH or PSX_CACHE_PATH on the right memory card, set by the EE
  char mmcePath[LIVESCAN_PATH_LEN];  // LIVESCAN_IRX_MMCEMAN on the right memory card, set by the EE
  volatile unsigned int stage;       // LIVESCAN_STAGE_*
  int mmceLoaded;                    // StartModule() result once mmceman is loaded, 0 before
  int watchdogResumed;               // Set if the watchdog handed the SIO2 back (scan stuck for 20 seconds)
  unsigned int sio2Version;          // sio2man version the lock functions were taken from
  unsigned int sio2Lock;             // sio2man's memory card transfer init (export 24) and transfer reset (26)
  unsigned int sio2Unlock;
  unsigned int sio2Intr;             // SIO2 interrupt handler and argument, from intrman
  unsigned int sio2IntrArg;
  unsigned int threadCount;          // Threads in threads
  LiveScanThread threads[LIVESCAN_MAX_THREADS];
  volatile unsigned int logRequest;  // LIVESCAN_LOG_*
  volatile int logResult;            // Bytes written, or < 0 on error
  unsigned int logLength;            // Bytes in logBuffer
  char logName[LIVESCAN_LOG_NAME_LEN]; // Directory (up to 20 characters) set by the EE, full path set by the IOP
  char logBuffer[LIVESCAN_LOG_CHUNK];
  unsigned int favInput; // Set by the EE with LIVESCAN_SCAN when fav holds favorites not saved yet, by index in the previous cache
  unsigned int played[LIVESCAN_MAX_GAMES];  // "played" counter of each game in names, 0 if never played
  unsigned int fav[LIVESCAN_MAX_GAMES / 32]; // Favorites bitmask by index in names, also the input of LIVESCAN_SAVE_FAV
  char names[LIVESCAN_MAX_GAMES][LIVESCAN_NAME_LEN];
  // Game covers (games_covers): the EE sets cachePath, coverIndex, coverName, coverSuffix and coverSeq with LIVESCAN_COVER.
  // The IOP reads mmce0/1:/COVER_RAW_DIR/<title ID of the game in the cache><coverSuffix>, or <coverName><coverSuffix>
  // when the game has no ID, and sets coverDone to coverSeq once coverResult (bytes read into coverAddr,
  // up to COVER_RAW_MAX_SIZE, or < 0) is set.
  // It's read LIVESCAN_COVER_CHUNK bytes at a time, handing the SIO2 back in between so the controller keeps being read,
  // and the EE stops the read by setting coverCancel to coverSeq (coverResult is then LIVESCAN_COVER_CANCELLED)
  volatile unsigned int coverSeq;
  volatile unsigned int coverDone;
  volatile int coverResult;
  volatile unsigned int coverAddr;
  unsigned int coverIndex;
  char coverName[LIVESCAN_COVER_NAME_LEN];
  char coverSuffix[LIVESCAN_COVER_SUFFIX_LEN];
  volatile unsigned int coverCancel;
  unsigned int mmceReadMs;  // Time it took to read mmceman from the memory card
  unsigned int mmceStartMs; // Time it took to start mmceman (with the SIO2 locked), mostly looking for the MMCE devices
} LiveScanShared;

#endif
