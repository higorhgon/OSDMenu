# OSDMenu Patcher

This is a slimmed-down and refactored version of OSDSYS patches from FMCB 1.8 for modern PS2SDK with some new patches sprinkled in.
It patches the OSDSYS/HDD OSD binary and applies the following patches:
- Custom OSDSYS menu with up to 200 entries
- Infinite scrolling
- Custom button prompts and menu header
- Automatic disc launch bypass
- Force GS video mode to PAL, NTSC, 1080i or line-doubled 480p (with half the vertical resolution).  
  _Due to how to OSDSYS renders everything, "true" 480p can't be implemented easily_
- HDD update check bypass
- Override PS1 and PS2 disc launch functions with the launcher, bringing the following features to OSDSYS/HDD-OSD:
  - Skip the PlayStation 2 logo
  - Patch the PlayStation 2 logo to work with all disc regions and master discs
  - Display the visual Game ID for the PixelFX RetroGM
  - Run disc-based games via the embedded Neutrino GSM (eGSM)
  - Run PS1 discs via the PS1 Video Mode Negator or DKWDRV
- Additional system information in version submenu (Video mode, ROM version, EE, GS and MechaCon revision)  
- Set PS1 driver options to values from `OSDMENU.CNF` on every boot
- Force OSD region on non-protokernel systems

**OSDMenu**:
- The OSDMenu configuration file can be embedded into the patcher at compile time for memory card-independent setups and faster boot times
- Launch SAS-compatible applications from the memory card browser if directory name is   
  `BOOT`, `<3-letter SAS prefix>_<appname>` or if file name ends with `.ELF` or `.elf`.  
  This patch swaps around the "Enter" and "Options" menus and substitutes file properties submenu with the launcher.  
  To launch an app, just press "Enter" after selecting the app icon.  
  To copy or delete the save file, just use the triangle button.  
- Load ROM 2.20 OSDSYS from memory card or XFROM

**HOSDMenu**:
- Launch SAS-compatible applications and ELF files from directories in the `hdd0:__common` partition or the memory card browser
  if directory name is `BOOT`, `<3-letter SAS prefix>_<appname>` or if file name ends with `.ELF` or `.elf`.  
  To launch an app, just press "Enter" after selecting the app icon.
- ATAD driver is replaced to bypass security checks and support drives larger than 1TB.
- HDD partitions that end with `.HIDDEN` are not shown in the HDD Browser.  
    
  Note that HDD OSD will not see more than 1048448 MB. For larger drives, [APA Jail](https://www.psx-place.com/threads/apa-jail.34847/) is recommended.  
  You can also check out [PSBBN Definitive English Patch](https://github.com/CosmicScale/PSBBN-Definitive-English-Patch) for more automated APA Jail experience and easy-to-use HDD OSD+Broadband Navigator setup.

  HOSDMenu will skip the full IOP initialization when it receives `-mbrboot` as the last argument (`argv[argc - 1]`), improving boot times when running from a compatible `__mbr`.

Patches not supported/limited on protokernel systems:
- Automatic disc launch bypass
- Button prompt customization
- PAL video mode

**OSDMenu** version of the patcher reads settings from `mc?:/SYS-CONF/OSDMENU.CNF` or `xfrom:/osdmenu/OSDMENU.CNF` (if the config file is not embedded) and patches the `rom0:OSDSYS` binary.  
**HOSDMenu** version reads settings from `hdd0:__sysconf:pfs:/osdmenu/OSDMENU.CNF` and patches `hdd0:__system:pfs:/osd100/OSDSYS_A.XLF` or `hdd0:__system:pfs:/osd100/hosdsys.elf`

### Configuration

See the list for supported `OSDMENU.CNF` options [here](#osdmenucnf).  
OSDMenu will run the embedded launcher and pass the menu index to it for every menu item and disc launch.

The embedded Neutrino GSM (eGSM) can be configured using the `OSDGSM.CNF` file, see the additional information [here](#osdgsmcnf).  

### Running OSDMenu with external OSDSYS

OSDMenu supports loading ROM 2.20 OSDSYS from external file.  
Use the [`rom_to_osdr.py`](../utils/scripts/rom_to_osdr.py) Python script to repack OSDSYS resources into the OSDMenu OSD resource file (`osdsys.bin`)
and place it into `xfrom:/osdmenu/osdsys.bin` or `mc?:/SYS-CONF/osdsys.bin`.

See the main [README](../README.md#osdmenu-on-psx-desr--osdmenu-with-external-osdsys) for more details.

## OSDGSM.CNF

OSDMenu supports running disc-based PS2 games via the embedded [Neutrino GSM](../utils/loader/README.md#egsm).

**OSDMenu** loads the per-title options from `mc?:/SYS-CONF/OSDGSM.CNF` or `xfrom:/osdmenu/OSDGSM.CNF`.  
**HOSDMenu** loads the per-title options from `hdd0:__sysconf:pfs:/osdmenu/OSDGSM.CNF`, with fallback to `mc?:/SYS-CONF/OSDGSM.CNF` if the file on the HDD doesn't exist.

See the sample configuraton [here](../examples/OSDGSM.CNF) and [this](../utils/loader/README.md#egsm) README for more information on the argument format.

## OSDMENU.CNF

Most of `OSDMENU.CNF` settings are directly compatible with those from FMCB 1.8 `FREEMCB.CNF`.

### Character limits

OSDMenu supports up to 200 custom menu entries, each up to 79 characters long.  
Note that left and right cursors are limited to 19 characters and top and bottom delimiters are limited to 79 characters.  
DKWDRV and custom payload paths are limited to 49 characters.

### Configuration options

#### OSDSYS behavior modifiers
1. `OSDSYS_video_mode` — force OSDSYS mode. Valid values are `AUTO`, `PAL`, `NTSC`, `480p` or `1080i`
2. `OSDSYS_Skip_Disc` — enables/disables automatic CD/DVD launch
3. `OSDSYS_boot` — makes OSDSYS boot directly into the opening (save data towers), clock (main menu) or the Browser. Valid values are `opening`, `clock`, `browser`. The default value is unset.
4. `OSDSYS_region` — forces OSDSYS region if set. Valid values are `AUTO`, `jap`, `usa`, `eur`.  
   This changes the video mode, button prompts and available languages (on some consoles)

#### OSDSYS custom menu options
5. `OSDSYS_custom_menu` — enables or disables custom menu
6. `OSDSYS_scroll_menu` — enables or disables infinite scrolling in custom menu
7. `OSDSYS_menu_x` — menu X center coordinate
8. `OSDSYS_menu_y` — menu Y center coordinate
9. `OSDSYS_enter_x` — `Enter` button X coordinate (at main OSDSYS menu)
10. `OSDSYS_enter_y` — `Enter` button Y coordinate (at main OSDSYS menu)
11. `OSDSYS_version_x` — `Version` button X coordinate (at main OSDSYS menu)
12. `OSDSYS_version_y` — `Version` button Y coordinate (at main OSDSYS menu)
13. `OSDSYS_cursor_max_velocity` — max cursor speed
14. `OSDSYS_cursor_acceleration` — cursor speed
15. `OSDSYS_left_cursor` — left cursor text
16. `OSDSYS_right_cursor` — right cursor text
17. `OSDSYS_menu_top_delimiter` — top menu delimiter text
18. `OSDSYS_menu_bottom_delimiter` — bottom menu delimiter text
19. `OSDSYS_num_displayed_items` — the number of menu items displayed
20. `OSDSYS_selected_color` — color of selected menu entry
21. `OSDSYS_unselected_color` — color of unselected menu entry
22. `name_OSDSYS_ITEM_???` — menu entry name
23. `path?_OSDSYS_ITEM_???` — path to ELF. Also supports the following special paths: `cdrom`, `OSDSYS`, `POWEROFF`
24. `arg_OSDSYS_ITEM_???` — custom argument to be passed to the ELF. Each argument needs a separate entry.

#### Disc/application launch modifiers
25. `cdrom_skip_ps2logo` — enables or disables running discs via `rom0:PS2LOGO`
26. `cdrom_disable_gameid` — disables or enables visual Game ID
27. `cdrom_use_dkwdrv` — enables or disables launching DKWDRV for PS1 discs
28. `ps1drv_enable_fast` — will enable fast disc speed for PS1 discs when not using DKWDRV
29. `ps1drv_enable_smooth` — will enable texture smoothing for PS1 discs when not using DKWDRV
30. `ps1drv_use_ps1vn` — will run PS1DRV using the PS1DRV Video Mode Negator
32. `app_gameid` — if enabled, visual Game ID will be displayed for ELF applications launched from OSDMenu. The ID is generated from the ELF name (up to 11 characters).
32. `path_DKWDRV_ELF` — custom path to DKWDRV.ELF (exclsuive to OSDMenu). The path **must** be on the memory card or XFROM, the default value is `mc?:/BOOT/DKWDRV.ELF` or `xfrom:/osdmenu/DKWDRV.ELF` when running from XFROM

#### Games menu

Enabling any of the `games_device_*` options adds a "Games >" entry at the top of the custom menu. It opens a submenu with PS2 games found on USB/MX4SIO/MMCE devices, launched via a user-installed standalone [Neutrino](https://github.com/rickgaiser/neutrino) (`neutrino.elf`, not bundled with OSDMenu). The list is cached in `mc?:/SYS-CONF/OSDGAMES.CNF` and only rescanned when it doesn't exist yet or when "Refresh list" is selected, so it adds no boot-time cost beyond reading that file. "< Back" returns to the main menu. HOSDMenu shows a full-screen list from the launcher instead. The entry is not shown by builds without the games menu (`LAUNCHER_GAMES_MENU`). See [launcher/README.md](../launcher/README.md#games-handler) for scan rules and current limitations (no cover art).

Enabling any of the `psx_device_*` options adds a "PSX >" entry right below "Games >". It works the same way for PS1 games,
launched via [Ember](https://github.com/Gageformer/Ember) (not bundled with OSDMenu) and cached in `mc?:/SYS-CONF/OSDPSX.CNF`.
On each enabled device, Ember must be installed as `EMBER/ember.elf` with its games in `EMBER/games/<game>/`
(one folder per game, with a `.cue` file inside), as described in Ember's documentation.
When both submenus are enabled, the menu slots left after the regular entries are split evenly between them.

In both submenus:
- Circle (Cross on Japanese consoles) or Triangle goes back to the main menu
- Square switches between sorting by name and by most recently played, shown in the "< Back" label.
  The order is saved in the cache the next time a game is launched or the list is refreshed
- Left/Right move the cursor a page (`OSDSYS_num_displayed_items`) up or down

The button prompts at the bottom show "Back" instead of "Version" and the current sort order while a submenu is open.
The Circle and Square icons are derived from the icons OSDSYS uses for Enter and Version; if they can't be derived,
only the texts are shown. `games_button_debug = 1` shows the icon types in a line above the prompts: `e` (Enter),
`v` (Version), `b` (Back), `s` (Sort) and `seen` (icons seen in other screens such as System Configuration, which shows
Circle and Square).

These options are also read by the **launcher**:
- `games_device_usb`, `games_device_mx4sio`, `games_device_mmce` — enable scanning each device type (default: all disabled)
- `games_device_udpfs` — enable scanning the [UDPFS](https://github.com/rickgaiser/neutrino#udpfs--udpbd-pc-server) server over the network adapter; games are launched via Neutrino (`-bsd=udpfs`)
- `games_device_smb` — enable scanning the SMB share configured in OPL's network settings; games are launched via `games_opl_path`, which needs a RiptOPL build with SMB autolaunch support
- `games_smb_config` — path to OPL's `conf_network.cfg`. When not set, it's looked up next to `games_opl_path`, then in `mc?:/OPL/`
- `games_cd_folder`, `games_dvd_folder` — folder names (relative to each device's root) to scan for games; both hold PS2 titles, split only by original release media (default: `CD`, `DVD`)
- `games_neutrino_path` — path to the installed `neutrino.elf` (required to launch games)
- `games_neutrino_arg` — extra Neutrino argument passed to every launch, repeatable (e.g. `-gsm=fp2`)
- `games_mmce_gameid` — if enabled (default), sends the game's title ID to MMCE devices before launching, so they switch to the game's own memory card (`mmce?:/MemoryCards/PS2/<ID>/<ID>-1.mcd`), like NHDDL does
- `games_launcher` — `neutrino` (default) or `opl`. With `opl`, games are launched via `games_opl_path` when OPL can autolaunch them, falling back to Neutrino otherwise (see [launcher/README.md](../launcher/README.md#games-handler))
- `games_opl_path` — path to OPL, e.g. [RiptOPL](https://github.com/NathanNeurotic/Open-PS2-Loader) (required for `games_launcher = opl`)
- `games_live_scan` — **experimental**, MMCE only. If enabled, "Refresh list" scans without leaving OSDMenu.
  OSDSYS resets the IOP when it starts, so the patcher writes `iomanX`, `mmceman` and its own `gamescan.irx` to
  `mc?:/SYS-CONF/` (`LSIOMANX.IRX`, `LSMMCE.IRX`, `LSGSCAN.IRX`, only when they change) and, on the first "Refresh list",
  loads `iomanX` and `gamescan.irx` with OSDSYS's own `sceSifLoadModule()` ("Loading modules..."). It then controls
  `gamescan.irx` through shared IOP memory, showing what it's doing until the list is ready.
  OSDSYS's `rom0:SIO2MAN` (ROM 2.30) is the older `sio2man` 1.2, where every SIO2 transfer goes through `sio2man`'s
  thread, so `mmceman` can't share the SIO2 with the controller and memory card drivers. Instead, for every scan,
  `gamescan.irx` takes `sio2man`'s memory card transfer lock, which makes the controller, memory card, multitap and
  remote drivers wait for their turn, loads `mmceman` the first time (read into IOP RAM before, since the memory card
  can't be read while the lock is held), scans, hands the SIO2 back and then writes the list.
  The IOP kernel doesn't implement `SuspendThread()`, so the drivers' threads can't be paused instead.
  The embedded `mmceman` is built without the `sio2man` hook that turns transfers to memory card slot 2 into no-ops,
  so OSDSYS still sees the memory card in slot 2 after a scan.
  **The controller doesn't respond while scanning.** If the scan takes more than 20 seconds, the lock is released anyway.
  Errors are shown in the "Refresh list" label, and the next "Refresh list" falls back to the launcher.
  With `games_live_scan = 2`, the first "Refresh list" writes a diagnostics log instead of scanning, without taking the
  lock: `gamescan.irx` writes `mc?:/SYS-CONF/OSDMLIVE-<YYMMDD>-<HHMMSS>.LOG` (console clock) with the `sio2man` lock
  functions, the SIO2 interrupt handler and the IOP threads (entry point, module, status and priority).
  The log name is shown in the "Refresh list" label, and the IOP state (library versions and every IOP module OSDSYS
  loaded) in the Games submenu. The next "Refresh list" scans and writes another log with the scan result and the
  threads after it. `mc?:/SYS-CONF/OSDMLIVE.LOG` is also written on boot.
  May break OSDSYS memory card or controller access
- `games_return_path` — ELF to run after "Refresh list" or a failed game launch (e.g. `mc?:/BOOT/BOOT.ELF`). When not set, the launcher reopens OSDMenu (with the games submenu open) from the path it was started from, then tries `mc?:/BOOT/osdmenu.elf`, and only falls back to the original OSDSYS if both fail
- `psx_device_usb`, `psx_device_mx4sio`, `psx_device_mmce` — enable scanning each device type for PS1 games (default: all disabled)

To add a custom separator to the menu, add a `name_OSDSYS_ITEM_???` entry that starts with `$!`.  
This will make the entry inactive, but still show it in the OSD without the `$!` prefix.

To group entries into submenus, add a `group_OSDSYS_ITEM_???` entry with the group name to them (OSDMenu only,
other FMCB-style launchers ignore it):
```ini
name_OSDSYS_ITEM_2 = OPL
path1_OSDSYS_ITEM_2 = mc?:/BOOT/OPL.ELF
group_OSDSYS_ITEM_2 = Apps

name_OSDSYS_ITEM_10 = DKWDRV
path1_OSDSYS_ITEM_10 = mc?:/BOOT/DKWDRV.ELF
group_OSDSYS_ITEM_10 = Apps
```
Each group is shown as a `<group> >` entry that opens a submenu with "< Back" and its entries in `OSDMENU.CNF` order.
The groups are listed in alphabetical order after "Games >" and "PSX >", followed by the entries without a group.
Circle/Triangle go back and Left/Right move a page, like in the games submenus. Up to 32 groups are supported.
Group names can contain spaces, and are case-sensitive (`Apps` and `apps` are different groups).

By default, OSDMenu uses custom menu coordinates to make the menu appear in the center of the screen.  
To get the original OSDSYS look, set the following values in `OSDMENU.CNF`:
```
OSDSYS_menu_x = 430
OSDSYS_menu_y = 110
OSDSYS_enter_x = -1
OSDSYS_enter_y = -1
OSDSYS_version_x = -1
OSDSYS_version_y = -1
```
