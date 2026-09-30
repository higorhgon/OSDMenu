#ifndef _LIVESCAN_H_
#define _LIVESCAN_H_

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

// gamescan.irx loads mmceman itself (see LIVESCAN_SCAN_LOAD_MMCE), since OSDSYS's rom0:SIO2MAN
// is the older thread-based sio2man 1.2, which mmceman can't share the SIO2 with. While mmceman
// runs, gamescan.irx suspends the threads of the modules that use the SIO2 (controller, memory
// card, multitap and remote), which stops the controller for the duration of the scan.
//
// Module files on the memory card OSDMENU.CNF was loaded from ('?' is replaced with the slot)
#define LIVESCAN_IRX_IOMANX "mc?:/SYS-CONF/LSIOMANX.IRX"
#define LIVESCAN_IRX_MMCEMAN "mc?:/SYS-CONF/LSMMCE.IRX"
#define LIVESCAN_IRX_GAMESCAN "mc?:/SYS-CONF/LSGSCAN.IRX"

#define LIVESCAN_MAGIC0 0x4d44534f // "OSDM"
#define LIVESCAN_MAGIC1 0x4556494c // "LIVE"
#define LIVESCAN_MAGIC2 0x4e414353 // "SCAN"
#define LIVESCAN_MAGIC3 0x34303030 // "0004"

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

// request
#define LIVESCAN_SCAN 1      // Scan and write the cache
#define LIVESCAN_LIST 2      // Only list the threads (threads, threadCount), for the diagnostics log

// stage: what gamescan.irx is doing, shown while scanning
#define LIVESCAN_STAGE_IDLE 0
#define LIVESCAN_STAGE_READ_MMCE 1 // Reading mmceman from the memory card
#define LIVESCAN_STAGE_PAUSE 2     // Suspending the SIO2 threads
#define LIVESCAN_STAGE_LOAD_MMCE 3 // Loading mmceman from IOP RAM
#define LIVESCAN_STAGE_SCAN 4      // Scanning the MMCE
#define LIVESCAN_STAGE_RESUME 5    // Resuming the SIO2 threads
#define LIVESCAN_STAGE_WRITE 6     // Writing the cache

// result errors other than iomanX's
#define LIVESCAN_ERR_MMCE_READ -1000  // - errno: mmceman couldn't be read
#define LIVESCAN_ERR_MMCE_LOAD -2000  // - error: LoadModuleBuffer() failed
#define LIVESCAN_ERR_MMCE_START -3000 // StartModule() failed or mmceman didn't stay resident
#define LIVESCAN_ERR_NO_THREADS -4000 // No SIO2 thread found to suspend

#define LIVESCAN_MAX_THREADS 48

#define LIVESCAN_MAX_TAGS 64

// Word with the thread tag found in IOP RAM, for the diagnostics log
typedef struct {
  unsigned int addr;
  unsigned int word;
  int handle;       // Thread ID made from addr and the ID in word
  int result;       // ReferThreadStatus() result
  unsigned int status;
  unsigned int initPriority;
  unsigned int entry;
} LiveScanTag;

// Thread found in IOP RAM (all words, so the EE can read them one at a time)
typedef struct {
  int handle;
  unsigned int entry;
  unsigned int status;   // THS_* when listed
  unsigned int priority; // Current priority
  int sio2;              // 1 if the thread belongs to a module that uses the SIO2
  int suspended;         // SuspendThread() result, or 1 if the thread was left running
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
  volatile unsigned int request;   // LIVESCAN_SCAN or LIVESCAN_LIST, set by the EE and cleared by the IOP
  volatile unsigned int status;    // LIVESCAN_STATUS_*
  volatile int result;             // Cache write result: 0 on success, < 0 on error
  unsigned int devices;            // LIVESCAN_DEV_* to scan, set by the EE
  unsigned int count;              // Number of games in names
  char cdFolder[LIVESCAN_FOLDER_LEN];
  char dvdFolder[LIVESCAN_FOLDER_LEN];
  char cachePath[LIVESCAN_PATH_LEN]; // GAMES_CACHE_PATH on the right memory card, set by the EE
  char mmcePath[LIVESCAN_PATH_LEN];  // LIVESCAN_IRX_MMCEMAN on the right memory card, set by the EE
  volatile unsigned int stage;       // LIVESCAN_STAGE_*
  int mmceLoaded;                    // StartModule() result once mmceman is loaded, 0 before
  int suspendedCount;                // Threads suspended by the last scan
  int watchdogResumed;               // Set if the threads were resumed by the watchdog (scan stuck for 20 seconds)
  unsigned int tagCount;             // Words with the thread tag found in IOP RAM
  unsigned int threadCount;          // Threads in threads
  LiveScanThread threads[LIVESCAN_MAX_THREADS];
  int ownThreads[2];                 // gamescan.irx's own thread IDs (scan, watchdog), to compare with the tags
  unsigned int ownTcb[16];           // Words around the address the scan thread ID points to, from 8 bytes before
  LiveScanTag tags[LIVESCAN_MAX_TAGS];
  volatile unsigned int logRequest;  // LIVESCAN_LOG_*
  volatile int logResult;            // Bytes written, or < 0 on error
  unsigned int logLength;            // Bytes in logBuffer
  char logName[LIVESCAN_LOG_NAME_LEN]; // Directory (up to 20 characters) set by the EE, full path set by the IOP
  char logBuffer[LIVESCAN_LOG_CHUNK];
  char names[LIVESCAN_MAX_GAMES][LIVESCAN_NAME_LEN];
} LiveScanShared;

#endif
