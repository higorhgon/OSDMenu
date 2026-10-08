#ifndef _INIT_H_
#define _INIT_H_
#include <stdint.h>

#define USER_MEM_START_ADDR 0x110100
#define USER_MEM_END_ADDR 0x2000000

#define EXTRA_SECTION_START USER_MEM_START_ADDR
#define EXTRA_SECTION_END 0x200000
#define EXTRA_RELOC_ADDR 0x1200000

// Loads IOP modules
int initModules();

// Resets IOP before loading OSDSYS
void resetModules();

#ifdef LIVESCAN
// Writes the modules used by the experimental live games scan to the memory card
int writeLiveScanModules();

// Writes mc?:/SYS-CONF/OSDMLIVE.LOG with the OSDSYS module loading code (games_live_scan = 2)
void writeLiveScanDiagnostics(uint8_t *osd);

// Returns the report written by writeLiveScanDiagnostics() and liveScanReportAppendV()
const char *liveScanBootReport(int *length);
// Appends to the report
#include <stdarg.h>
void liveScanReportAppendV(const char *fmt, va_list args);

// Empties the report
void liveScanReportClear(void);
// The report buffer while games_live_scan isn't 2, or NULL
void *liveScanReportSpare(int *size);
#endif

#ifdef HOSD
// Inits SIF RPC and fileXio without rebooting the IOP. Assumes all modules are already loaded
void shortInit();
#endif

#endif
