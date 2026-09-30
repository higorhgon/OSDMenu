# Builds mmceman for the live scan without its sio2man transfer hook.
#
# mmceman defines PORT_NR 3 in sio2man_hook.c, which hooks sio2man's transfer function (export 25)
# and turns every transfer to port 3 (memory card slot 2) into a no-op. After a live scan,
# OSDSYS would no longer see the memory card in slot 2. The live scan doesn't need the hook:
# gamescan.irx holds sio2man's transfer lock while mmceman drives the SIO2.
#
# Usage: cmake -DFILE=<mmceman copy>/src/sio2man_hook.c -P mmceman_noportblock.cmake

file(READ "${FILE}" content)

set(define "#define PORT_NR 3")
string(FIND "${content}" "${define}" define_pos)
if(define_pos EQUAL -1)
  message(FATAL_ERROR "mmceman_noportblock: '${define}' not found in ${FILE}")
endif()
string(REPLACE "${define}" "// Live scan: no port 3 transfer hook (see patcher/iop/mmceman_noportblock.cmake)" content "${content}")

file(WRITE "${FILE}" "${content}")
