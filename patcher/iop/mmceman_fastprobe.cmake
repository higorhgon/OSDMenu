# Builds mmceman for the live scan and the covers so it finds the MMCE devices faster.
#
# mmceman pings each port up to 6 times when it starts, waiting up to 200 ms for every ping, so a port
# without an MMCE device (empty, or with a regular memory card) added 1.2 seconds (measured with
# games_button_debug). It's started from the OSDSYS menu, long after the devices are up, while
# gamescan.irx holds the SIO2 lock and the controller isn't read, so it tries twice, waiting 50 ms:
# an MMCE device answers within the transfer. timeout_200ms is only used by the ping.
#
# Usage: cmake -DDIR=<mmceman copy>/src -P mmceman_fastprobe.cmake

function(patch_file file from to)
  file(READ "${file}" content)
  string(FIND "${content}" "${from}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mmceman_fastprobe: '${from}' not found in ${file}")
  endif()
  string(REPLACE "${from}" "${to}" content "${content}")
  file(WRITE "${file}" "${content}")
endfunction()

patch_file("${DIR}/mmceman.c" "for (int i = 0; i < 6; i++) {"
           "for (int i = 0; i < 2; i++) { // Live scan: 2 attempts (see patcher/iop/mmceman_fastprobe.cmake)")
patch_file("${DIR}/mmce_sio2.c" "timeout_200ms.lo = 0x708000;"
           "timeout_200ms.lo = 0x1c2000; // Live scan: 50 ms (see patcher/iop/mmceman_fastprobe.cmake)")
