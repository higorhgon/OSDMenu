# OSDMenu Launcher

A fully-featured ELF launcher that handles launching ELFs and CD/DVD discs.  
Supports passing arbitrary arguments to an ELF and can also be used standalone as a forwarder.

Supported paths are:
- `mmce?:` — MMCE devices. Can be `mmce0`, `mmce1` or `mmce?`
- `mc?:` — Memory Cards. Can be `mc0`, `mc1` or `mc?`
- `mass?:` and `usb?:` — USB devices (supported via BDM)
- `ata?:` — internal exFAT-formatted HDD (supported via BDM)
- `mx4sio:` — MX4SIO (supported via BDM)
- `ilink:` — i.Link mass storage (supported via BDM, disabled in OSDMenu/HOSDMenu)
- `udpbd:` — UDPBD (supported via BDM, disabled in OSDMenu/HOSDMenu)
- `udpfs:` — UDPFS (disabled in OSDMenu/HOSDMenu)
- `hdd?:` — internal APA-formatted HDD. Both `:pfs:` and `:PATINFO` paths are supported
- `rom?:` — ROM binaries (`rom1:` and `rom2:` require ADDDRV and ADDROM2 modules in `rom0:`)
- `xfrom:` — XFROM (PSX)
- `cdrom` — CD/DVD discs
- `osdm` — special path for OSDMenu patcher

Device support can be enabled and disabled by changing build-time configuration options (see [Makefile](Makefile))  
Supports [OSDMenu-specific SYSTEM.CNF extensions](../mbr/README.md#systemcnf-extensions-for-partition-attribute-area-patinfo-paths).

## Global arguments

The launcher supports the following global arguments:

- `-gsm=<options>` — runs the target ELF via the [embedded Neutrino GSM](../utils/egsm/).  
  See [this README](../utils/loader/README.md#egsm) for more information on the argument format.   
  Must always be the last argument. Does not apply to `rom?:` paths.
- `-appid` — enables visual Game ID for applications. The ID is generated from the ELF name (up to 11 characters).
- `-patinfo` — when handling PATINFO paths, will ignore the `BOOT2`/`path` line from SYSTEM.CNF and use the first argument as target ELF path.
- `-titleid=` — when launching ELFs, will use this title ID for the history file and visual game ID (up to 11 characters). Only valid IDs will be written to the history file
- `-skip_argv0` — if set to `1`, the target ELF will only receive argv built from `arg` lines
- `-dev9=` — will pass DEV9 shutdown flags to the loader. Supported values are:
  - `NICHDD` — will keep both the network adapter and HDD on
  - `NIC` — will keep only the network adapter on 

## Handlers

### `udpbd` and `udpfs` handlers

Reads PS2 IP address from `mc?:/SYS-CONF/IPCONFIG.DAT`

### `cdrom` handler

Waits for the disc to be detected and launches it.
Supports the following arguments:
- `-nologo` — launches the game executable directly, bypassing `rom0:PS2LOGO`
- `-nogameid` — disables visual game ID
- `-dkwdrv` — when PS1 disc is detected, launches DKWDRV from `mc?:/BOOT/DKWDRV.ELF` instead of `rom0:PS1DRV`
- `-dkwdrv=<path to DKWDRV>` — same as `-dkwdrv`, but with custom DKWDRV path
- `-ps1fast` — will force fast disc speed for PS1 discs when not using DKWDRV
- `-ps1smooth` — will force texture smoothing for PS1 discs when not using DKWDRV
- `-ps1vneg` — will run PS1DRV via the PS1DRV Video Mode Negator

For PS1 CDs with generic executable name (e.g. `PSX.EXE`), attempts to guess the game ID using the volume creation date
stored in the Primary Volume Descriptor, based on the table from [TonyHax International](https://github.com/alex-free/tonyhax/blob/master/loader/gameid-psx-exe.c).

For PS2 CDs/DVDs, the `cdrom` handler will look for the embedded Neutrino GSM setting in
- `mc?:/SYS-CONF/OSDGSM.CNF`
- `hdd0:__sysconf/osdmenu/OSDGSM.CNF` (only when running from HDD)  
- `xfrom:/osdmenu/OSDGSM.CNF` (only when running from XFROM)  

See [this](#osdgsmcnf) for more details.

### `osdm` handler
When the launcher receives `osdm:d0:<idx>`, `osdm:d1:<idx>`, `osdm:d2:<idx>`, or `osdm:d9:<idx>`  path as `argv[0]`, it reads `OSDMENU.CNF` from the respective memory card, XFROM (`osdm:d2`) or the hard drive (`osdm:d9`),
searches for `path?_OSDSYS_ITEM_<idx>` and `arg_OSDSYS_ITEM_<idx>` entries and attempts to launch the ELF.

Additionally, the launcher supports parsing the configuration from an arbitrary address when receiving `osdm:a<address>:<CNF file size>:<idx>` as `argv[0]`.

Respects `cdrom_skip_ps2logo`, `cdrom_disable_gameid` and `cdrom_use_dkwdrv` for `cdrom` paths, but only if there are no custom arguments for this entry (`arg_OSDSYS_ITEM`).

### `games` handler
Handles the patcher's automatic "Games >" entry, which is passed as the reserved item index 9999
(`osdm:d<slot>:9999`, see `GAMES_MENU_IDX`), and launches PS2 games via a user-installed standalone
[Neutrino](https://github.com/rickgaiser/neutrino) (`neutrino.elf`, not bundled with OSDMenu).

In OSDMenu, the games are shown as a submenu of the OSDSYS menu itself. Scanning storage devices is not possible
from within OSDSYS, so the launcher does it and caches the result in `mc?:/SYS-CONF/OSDGAMES.CNF` (on the memory card
`OSDMENU.CNF` was loaded from), which the patcher reads on boot. The patcher appends a mode to the `osdm` path:
- `:s` — scans the devices, writes the cache and returns to OSDMenu. Used by the "Refresh list" submenu entry,
  and when "Games >" is opened for the first time without a cache
- `:g<N>` — launches game `N` from the cache without scanning

The mode is followed by `|<path>`, the path OSDMenu was started from (e.g. `osdm:d0:9999:s|mc0:/BOOT/osdmenu.elf`).
Calling `ExecOSD` would boot the original OSDSYS instead of OSDMenu, so after scanning or a failed launch
the launcher tries, in order:
1. `games_return_path` from `OSDMENU.CNF`
2. the path passed by the patcher
3. `mc?:/BOOT/osdmenu.elf`
4. the original OSDSYS

OSDMenu is relaunched with `-games` as the last argument (except for `games_return_path`), which makes it
reopen the games submenu right away.

Without a mode (HOSDMenu), the launcher scans and shows its own full-screen list instead.

For each enabled device (`games_device_usb`, `games_device_mx4sio`, `games_device_mmce`), it scans two folders
relative to the device root: `games_cd_folder` (default `CD`) and `games_dvd_folder` (default `DVD`). Both hold
**PS2** games — the split is only about the original release media (e.g. Bloody Roar 3 was released on CD), not a
PS1/PS2 distinction.

A game is either:
- a `.iso` file directly inside the folder, or
- a subfolder containing **exactly one** `.iso` file (display name = subfolder name). Zero or more than one `.iso`
  in a subfolder is treated as ambiguous and skipped.

An OPL-style title ID prefix and the `.iso` extension are hidden from display names
(`SLUS_202.12.BLOODY ROAR 3.iso` is shown as `BLOODY ROAR 3`).
ISO images split into multiple part files are not supported, since Neutrino doesn't support them either.

The title ID of each game (e.g. `SLUS_202.12`) is taken from the ISO file or folder name (`SLUS_202.12.Name.iso`,
`SLUS-20212`) or, if it isn't there, from the `BOOT2` line of `SYSTEM.CNF` inside the ISO, and stored in the cache.
Before launching, the drivers for the ISO and the ELF are loaded, and the title ID is sent to MMCE devices
(`games_mmce_gameid`, enabled by default) so they switch to the game's memory card, like NHDDL does.
The slot the ELF is loaded from is skipped when the ELF is on a memory card (`mc?:`).

Selecting a game launches `games_neutrino_path` the same way [NHDDL](https://github.com/pcm720/nhddl) does:
```
neutrino.elf -bsd=<usb|mx4sio|mmce> -dvd=<full ISO path, e.g. mmce0:/DVD/game.iso> -qb [games_neutrino_arg ...]
```

With `games_launcher = opl`, `games_opl_path` (e.g. [RiptOPL](https://github.com/NathanNeurotic/Open-PS2-Loader)) is
launched instead using OPL's argv autolaunch:
```
opl.elf <ISO file name> <title ID> <CD|DVD> bdm
opl.elf <ISO file name> <title ID> <CD|DVD> mmce <slot>
```
OPL looks for the ISO directly in the `CD/` or `DVD/` folder, so games in subfolders, in custom folders or without a
known title ID fall back to Neutrino. For BDM devices (USB/MX4SIO), OPL uses the first BDM device.
The `mmce` mode needs a RiptOPL build with MMCE autolaunch support; other OPL builds open their menu instead.
When launching through OPL, the MMCE card switch is left to OPL, which also handles IGR (including switching the
MMCE back to the boot card).

**Neutrino v1.8.0 and MMCE**: v1.8.0 takes the MMCE slot from the *last* digit anywhere in the ISO path instead of
the one before `:`, so ISOs with digits in their name (e.g. `Bloody Roar 3.iso`) hang on a black screen when loaded
from MMCE. This was fixed upstream after v1.8.0 (commit `cedc060`, "FIX MMCE"). Use the "Latest development build"
or v1.7.0 instead.

**Current limitations**: no cover art (the OSDSYS custom menu has no per-item icon) and no network storage
(SMB/FTP/SFTP), which Neutrino doesn't support.

#### PS1 games ("PSX >")
The patcher's "PSX >" entry is passed as item index 9998 (`PSX_MENU_IDX`) and works the same way, with the
`psx_device_usb`, `psx_device_mx4sio` and `psx_device_mmce` keys and the `mc?:/SYS-CONF/OSDPSX.CNF` cache.
PS1 games are launched via [Ember](https://github.com/Gageformer/Ember), which is not bundled with OSDMenu.
For each enabled device, the launcher lists the subfolders of `<device>:/EMBER/games/` that contain a `.cue` file,
but only if `<device>:/EMBER/ember.elf` exists, since Ember loads games relative to its own folder:
```
<device>:/EMBER/ember.elf <game folder name>
```
Ember uses the storage drivers that are already loaded, so the launcher loads the device's drivers before starting it.

### Config handler
When the launcher receives a path that ends with `.CNF`, `.cnf`, `.CFG` or `.cfg`,
it will run the [quickboot handler](#quickboot-handler) using this file.

Config file can be located at any device as long as the device mountpoint is one of the listed above.

### Quickboot handler
When the launcher is started without any arguments, it tries to open `<ELF file name>.CNF`
file at the current working directory
and attempts to launch every path in order.

Quickboot file syntax example:
```ini
boot=boot.elf
path=mmce?:/apps/wle.elf
path=mmce?:/apps/wle2.elf
path=ata:/apps/wle.elf
path=mc?:/BOOT/BOOT.ELF
arg=-testarg
arg=-testarg2
```

`boot` — path relative to the config file  
`path` — absolute paths  
`arg` — global launcher arguments or arguments that will be passed to the ELF file
