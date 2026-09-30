# Makes mmceman lock the SIO2 through the sio2man 1.2 interface when the 2.x one isn't loaded.
#
# mmceman locks the SIO2 with sio2_pad_transfer_init()/sio2_transfer_reset() (exports 23 and 26)
# of the sio2man 2.x interface only. The live games scan loads mmceman while OSDSYS is running,
# with OSDSYS's rom0:SIO2MAN, which only has the 1.2 interface. Without the lock, mmceman's
# transfers run in the middle of padman's and the controller stops responding.
# The 1.2 interface has the same exports, which mmceman already saves in g_hook_data[0].
#
# Usage: cmake -DFILE=<mmceman copy>/src/sio2man_hook.c -P mmceman_sio2lock.cmake

file(READ "${FILE}" content)

set(lock_old "g_hook_data[1].m_23_psio2_pad_transfer_init")
set(unlock_old "g_hook_data[1].m_26_psio2_transfer_reset")
string(FIND "${content}" "${lock_old}" lock_pos)
string(FIND "${content}" "${unlock_old}" unlock_pos)
if(lock_pos EQUAL -1 OR unlock_pos EQUAL -1)
  message(FATAL_ERROR "mmceman_sio2lock: lock/unlock code not found in ${FILE}")
endif()

string(REPLACE "${lock_old}" "SIO2_LOCK_HOOK.m_23_psio2_pad_transfer_init" content "${content}")
string(REPLACE "${unlock_old}" "SIO2_LOCK_HOOK.m_26_psio2_transfer_reset" content "${content}")

set(decl "static struct sio2man_hook_data g_hook_data[2];")
string(FIND "${content}" "${decl}" decl_pos)
if(decl_pos EQUAL -1)
  message(FATAL_ERROR "mmceman_sio2lock: g_hook_data declaration not found in ${FILE}")
endif()
string(REPLACE "${decl}" "${decl}\n// Live scan: lock with the sio2man 2.x interface, or 1.2 when it's the only one\n#define SIO2_LOCK_HOOK g_hook_data[g_hook_data[1].m_lib ? 1 : 0]" content "${content}")

file(WRITE "${FILE}" "${content}")
