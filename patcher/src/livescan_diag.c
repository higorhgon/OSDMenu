// Diagnostics for the experimental live games scan (games_live_scan = 2).
// OSDSYS resets the IOP when it starts, removing the modules the patcher loads.
// To load them again from within OSDSYS, the patcher needs OSDSYS's own module
// loading and IOP reboot functions. This writes a report with every "rom0:" module
// path in OSDSYS, the code that references it and the functions called right after,
// so these functions can be identified without the OSDSYS binary.
#include "settings.h"
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>

#define DIAG_MAX_STRINGS 48
#define DIAG_MAX_TARGETS 24
#define DIAG_SCAN_SIZE 0x100000

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

static uint32_t targets[DIAG_MAX_TARGETS];
static int targetCount = 0;

static void addTarget(uint32_t target) {
  for (int i = 0; i < targetCount; i++)
    if (targets[i] == target)
      return;
  if (targetCount < DIAG_MAX_TARGETS)
    targets[targetCount++] = target;
}

// Logs the code that loads the address of the string at strAddr and the calls that follow
static void findReferences(uint8_t *osd, uint32_t strAddr) {
  uint32_t hi = ((strAddr + 0x8000) >> 16) & 0xffff;
  uint32_t lo = strAddr & 0xffff;
  uint32_t *code = (uint32_t *)osd;
  int refs = 0;

  for (uint32_t i = 0; (i < DIAG_SCAN_SIZE / 4) && (refs < 4); i++) {
    uint32_t insn = code[i];
    if (((insn >> 26) != 0x0f) || ((insn & 0xffff) != hi)) // lui rt, hi
      continue;
    uint32_t reg = (insn >> 16) & 0x1f;

    // addiu/ori reg, reg, lo within the next 8 instructions
    for (uint32_t j = i + 1; (j < i + 9) && (j < DIAG_SCAN_SIZE / 4); j++) {
      uint32_t lo_insn = code[j];
      uint32_t op = lo_insn >> 26;
      if (((op == 0x09) || (op == 0x0d)) && (((lo_insn >> 21) & 0x1f) == reg) && ((lo_insn & 0xffff) == lo)) {
        refs++;
        out("    ref at %08x:", (uint32_t)&code[i]);
        // Calls within the next 16 instructions
        for (uint32_t k = j; (k < j + 16) && (k < DIAG_SCAN_SIZE / 4); k++) {
          if ((code[k] >> 26) == 0x03) { // jal
            uint32_t target = ((code[k] & 0x03ffffff) << 2) | ((uint32_t)&code[k] & 0xf0000000);
            out(" jal %08x@+%d", target, (int)(k - i));
            addTarget(target);
          }
        }
        out("\n");
        break;
      }
    }
  }
  if (!refs)
    out("    no references\n");
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
  targetCount = 0;
  out("OSDMenu live scan diagnostics\nROMVER %s\nOSDSYS at %08x\nlive scan boot %d\n\n", settings.romver, (uint32_t)osd, settings.liveScanBoot);

  int strings = 0;
  for (uint32_t i = 0; (i < DIAG_SCAN_SIZE - 5) && (strings < DIAG_MAX_STRINGS); i++) {
    if (memcmp(&osd[i], "rom0:", 5))
      continue;
    char str[49];
    int len = 0;
    while ((len < 48) && (osd[i + len] >= 0x20) && (osd[i + len] < 0x7f)) {
      str[len] = osd[i + len];
      len++;
    }
    str[len] = '\0';
    out("%08x \"%s\"%s\n", (uint32_t)&osd[i], str, ((i > 0) && osd[i - 1]) ? " (inside a string)" : "");
    findReferences(osd, (uint32_t)&osd[i]);
    strings++;
    i += len;
  }

  out("\nCalled functions:\n");
  for (int i = 0; i < targetCount; i++) {
    out("%08x:", targets[i]);
    uint32_t *fn = (uint32_t *)targets[i];
    if (((uint32_t)fn < (uint32_t)osd) || ((uint32_t)fn >= (uint32_t)osd + DIAG_SCAN_SIZE - 64)) {
      out(" outside OSDSYS\n");
      continue;
    }
    for (int w = 0; w < 16; w++)
      out(" %08x", fn[w]);
    out("\n");
  }

  char path[] = "mc0:/SYS-CONF/OSDMLIVE.LOG";
  if (settings.mcSlot == 1)
    path[2] = '1';
  int fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
  if (fd < 0)
    return;
  fioWrite(fd, report, reportLen);
  fioClose(fd);
}
