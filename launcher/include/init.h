#ifndef _INIT_H_
#define _INIT_H_

#include "common.h"

// Reboots the IOP and executes a path from ROM via LoadExecPS2
int execROMPath(int argc, char *argv[]);

// Shuts down the console. Needs initModules(Device_Basic) to be called first.
void shutdownPS2();

// Sets IOP emulation flags for Deckard consoles. Needs initModules(Device_Basic) to be called first
void applyXPARAM(char *gameID);

// Initializes IOP modules for given device type
int initModules(DeviceType device);

#if defined(UDPFS) || defined(SMB)
// Network settings read from OPL's conf_network.cfg (and the ETH prefix from conf_opl.cfg).
// Used for the SMB games list, and for the UDPFS PS2 IP address when IPCONFIG.DAT doesn't exist
typedef struct {
  int dhcp; // ps2_ip_use_dhcp
  char ip[16];
  char netmask[16];
  char gateway[16];
  char smbIP[16];
  int smbPort;
  int useNBNS; // smb_share_use_nbns, not supported
  char smbShare[64];
  char smbUser[64];
  char smbPass[64];
  char ethPrefix[64]; // eth_prefix: games folder relative to the share root
} OPLNetConfig;

extern OPLNetConfig oplNetConfig;
extern int oplNetConfigLoaded;

// Reads configPath (OPL's conf_network.cfg) into oplNetConfig
int readOPLNetConfig(const char *configPath);
#endif

#endif
