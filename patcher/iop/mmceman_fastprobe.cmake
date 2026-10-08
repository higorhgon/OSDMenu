# Builds mmceman for the live scan and the covers with fewer attempts to find the MMCE devices.
#
# mmceman pings each port up to 6 times when it starts, waiting up to 200 ms for every ping, so a port
# without an MMCE device (empty, or with a regular memory card) can add more than a second. It's started
# from the OSDSYS menu, long after the devices are up, while gamescan.irx holds the SIO2 lock and the
# controller isn't read, so 2 attempts are enough.
#
# Usage: cmake -DFILE=<mmceman copy>/src/mmceman.c -P mmceman_fastprobe.cmake

file(READ "${FILE}" content)

set(loop "for (int i = 0; i < 6; i++) {")
string(FIND "${content}" "${loop}" loop_pos)
if(loop_pos EQUAL -1)
  message(FATAL_ERROR "mmceman_fastprobe: '${loop}' not found in ${FILE}")
endif()
string(REPLACE "${loop}" "for (int i = 0; i < 2; i++) { // Live scan: 2 attempts (see patcher/iop/mmceman_fastprobe.cmake)" content "${content}")

file(WRITE "${FILE}" "${content}")
