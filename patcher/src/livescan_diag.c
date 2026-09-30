// Diagnostics written on boot with games_live_scan = 2 to mc?:/SYS-CONF/OSDMLIVE.LOG.
// The "rom0:" module loading report that found OSDSYS's module loader is no longer needed,
// so the report now has the libpad code that follows scePadPortOpen(), to check how the patcher
// finds scePadRead() (used to hide Triangle from OSDSYS in the submenus) without the OSDSYS binary.
#include "settings.h"
#include "patches_pad.h"
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>

#define DIAG_SCAN_SIZE 0x100000
#define DIAG_LIBPAD_SIZE 0x800 // Same range the patcher searches

static char report[24 * 1024];
static int reportLen = 0;

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
  uint32_t start = padPortOpenAddr;
  out("libpad: scePadPortOpen %08x, scePadGetDmaStr %08x, scePadRead %08x (%d calls redirected)\n", padPortOpenAddr, padDmaStrAddr,
      padReadAddr, padReadRedirects);
  if (!start || (start < (uint32_t)osd) || (start + DIAG_LIBPAD_SIZE > (uint32_t)osd + DIAG_SCAN_SIZE)) {
    out("scePadPortOpen not found\n");
    goto write;
  }

  // OSDSYS calls into the libpad range, by target (calls already redirected to the hooks aren't counted)
  // The patcher's own memory is limited, so the counters use the scratch area loadConfig() uses
  uint32_t *calls = (uint32_t *)0x1000000;
  uint32_t *firstCaller = calls + DIAG_LIBPAD_SIZE / 4;
  memset(calls, 0, DIAG_LIBPAD_SIZE);
  for (uint32_t *p = (uint32_t *)osd; p < (uint32_t *)(osd + DIAG_SCAN_SIZE); p++) {
    uint32_t target;
    if ((*p & 0xfc000000) == 0x0c000000)
      target = ((uint32_t)p & 0xf0000000) | ((*p & 0x03ffffff) << 2);
    else
      target = *p; // Function pointer
    if ((target < start) || (target >= start + DIAG_LIBPAD_SIZE) || (target & 3))
      continue;
    int i = (target - start) / 4;
    if (!calls[i]++)
      firstCaller[i] = (uint32_t)p;
  }
  out("\nCalls into the libpad range (target: calls, first caller):\n");
  for (int i = 0; i < DIAG_LIBPAD_SIZE / 4; i++)
    if (calls[i])
      out("  %08x: %lu, %08lx%s\n", start + i * 4, calls[i], firstCaller[i],
          ((firstCaller[i] >= start) && (firstCaller[i] < start + DIAG_LIBPAD_SIZE)) ? " (inside)" : "");

  memset(calls, 0, DIAG_LIBPAD_SIZE * 2);

  // The libpad code itself
  out("\nlibpad code:\n");
  for (uint32_t addr = start; addr < start + DIAG_LIBPAD_SIZE; addr += 32) {
    out("%08x:", addr);
    for (int w = 0; w < 8; w++)
      out(" %08x", ((uint32_t *)addr)[w]);
    out("\n");
  }

write:;
  char path[] = "mc0:/SYS-CONF/OSDMLIVE.LOG";
  if (settings.mcSlot == 1)
    path[2] = '1';
  int fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
  if (fd < 0)
    return;
  fioWrite(fd, report, reportLen);
  fioClose(fd);
}
