// Diagnostics written on boot with games_live_scan = 2 to mc?:/SYS-CONF/OSDMLIVE.LOG.
// It has the libpad functions the patcher found (used to hide Triangle from OSDSYS in the submenus),
// and the OSDSYS image is written once for the game covers investigation.
#include "settings.h"
#include "patches_common.h"
#include "patches_pad.h"
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>

#define DIAG_SCAN_SIZE 0x100000

static char report[16 * 1024] __attribute__((aligned(16))); // The end of a long live scan log is cut
static int reportLen = 0;

// The report is only written with games_live_scan = 2, so otherwise its buffer is lent
// (as the stack of the thread that loads the live scan modules). Returns NULL when it's in use
void *liveScanReportSpare(int *size) {
  if (settings.gamesLiveScan == 2)
    return NULL;
  *size = sizeof(report);
  return report;
}

void liveScanReportAppendV(const char *fmt, va_list args) {
  if (reportLen < (int)sizeof(report) - 1)
    reportLen += vsnprintf(&report[reportLen], sizeof(report) - reportLen, fmt, args);
  if (reportLen > (int)sizeof(report) - 1)
    reportLen = sizeof(report) - 1;
}

static void out(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  liveScanReportAppendV(fmt, args);
  va_end(args);
}

// Covers investigation: the OSDSYS image as loaded (code and initialized data), to find how OSDSYS uploads
// textures and where the video memory is free without a log round trip per function.
// Written once, as mc?:/SYS-CONF/OSDSYS.BIN, since writing 1 MB to the memory card takes a few seconds
#define DIAG_IMAGE_CHUNK 0x10000

static void writeOSDSYSImage(uint8_t *osd) {
  char path[] = "mc0:/SYS-CONF/OSDSYS.BIN";
  if (settings.mcSlot == 1)
    path[2] = '1';
  int fd = fioOpen(path, FIO_O_RDONLY);
  if (fd >= 0) {
    fioClose(fd);
    return;
  }
  if ((fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC)) < 0)
    return;
  for (int offset = 0; offset < DIAG_SCAN_SIZE; offset += DIAG_IMAGE_CHUNK)
    if (fioWrite(fd, osd + offset, DIAG_IMAGE_CHUNK) != DIAG_IMAGE_CHUNK)
      break;
  fioClose(fd);
}

// Returns the report written on boot, followed by what was appended with liveScanReportAppendV(),
// which is sent as the games_live_scan = 2 log
const char *liveScanBootReport(int *length) {
  *length = reportLen;
  return report;
}

// Empties the report, for the log written after a scan
void liveScanReportClear(void) { reportLen = 0; }

void writeLiveScanDiagnostics(uint8_t *osd) {
  reportLen = 0;
  out("OSDMenu live scan diagnostics\nROMVER %s\nOSDSYS at %08x\nlive scan boot %d\n\n", settings.romver, (uint32_t)osd, settings.liveScanBoot);

  // What the patcher found (see patches_pad.c)
  out("libpad: scePadPortOpen %08x, scePadGetDmaStr %08x, scePadRead %08x (%d calls redirected)\n", padPortOpenAddr, padDmaStrAddr,
      padReadAddr, padReadRedirects);

  char path[] = "mc0:/SYS-CONF/OSDMLIVE.LOG";
  if (settings.mcSlot == 1)
    path[2] = '1';
  int fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
  if (fd < 0)
    return;
  fioWrite(fd, report, reportLen);
  fioClose(fd);

  writeOSDSYSImage(osd);
}
