# OSDMenu

> **Unofficial fork** of [OSDMenu](https://github.com/pcm720/OSDMenu) by pcm720, modified by higorhgon: games submenus,
> network games and menu groups. Not endorsed by the OSDMenu authors; report issues with this fork here, not upstream.
> See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the licenses.

Patches for OSDSYS and HDD OSD (Browser 2.0) based on Free McBoot 1.8.  

## Usage

### OSDMenu
1. Copy `osdmenu.elf` to `mc?:/BOOT/` or copy and paste `SYS_OSDMENU.psu` via wLaunchELF using `psuPaste`
   Copy DKWDRV to `mc?:/BOOT/DKWDRV.ELF` _(optional)_ 
2. Edit `mc?:/SYS-CONF/OSDMENU.CNF` [as you see fit](patcher/README.md#osdmenucnf)
3. Configure PS2BBL to launch `mc?:/BOOT/osdmenu.elf` or launch it manually from anywhere

### OSDMenu as the System Update

You can install OSDMenu as the System Update instead of FMCB or PS2BBL to get faster boot times.  
To install OSDMenu as the System Update, you can use the **latest development build** of [KELFBinder](https://github.com/israpps/KELFBinder).  

The release archive contains ready-to-use KELFBinder install script and directory structure:
  - `patcher/kelfbinder/EXTINST.lua` — custom installation script that installs OSDMenu as the system update and copies wLaunchELF from KELFBinder release into `BOOT/BOOT.ELF`
  - `patcher/kelfbinder/KELF/SYSTEM.XLF` — encrypted and signed OSDMenu executable to be installed as `osd???.elf` or `osdmain.elf`
  - `patcher/kelfbinder/ASSETS/osdmenu.icn` and `*.sys` files — OSDMenu icon assets for the PS2 Browser
  - `patcher/kelfbinder/ASSETS/SYS-CONF/OSDMENU.CNF`, `icon.sys` and `list.icn` — the [example config file](examples/OSDMENU.CNF) and `SYS-CONF` icons

Copy the contents of the `patcher/kelfbinder` directory into KELFBinder's `INSTALL` directory, replacing all existing files.  
Consult the KELFBinder documentation on how to use KELFBinder to install the system update on your memory card.

### HOSDMenu — OSDMenu for HDD OSD
1. Install HDD OSD **1.10U**  
   Make sure HDD OSD binaries are installed into `hdd0:__system:pfs:/osd100/` and `hosdsys.elf`/`OSDSYS_A.XLF` is present.  
   SHA-256 hashes of `hosdsys.elf`/`OSDSYS_A.XLF` known to work:
   - `acc905233f79678b9d7c1de99b0aee2409136197d13e7d78bf8978cd85b736ae` — original binary from the official HDD Utility Disc Version 1.10
   - `65360a6c210b36def924770f23d5565382b5fa4519ef0bb8ddf5c556531eec14` — cracked HDD OSD with 48-bit LBA support from the Sony Utility Disc Compilation 4 disc

   When using the unmodified binary on non-NTSC-U consoles, you will have to decrypt and re-encrypt the original binary with [`kelftool`](https://github.com/ps2homebrew/kelftool)
   to change the MagicGate region to 0xff (region free).  
   Decrypted binaries are also supported.
2. Copy `hosdmenu.elf` to `hdd0:__system:pfs:/osdmenu/`  
   Copy DKWDRV to `hdd0:__system:pfs:/osdmenu/DKWDRV.ELF` _(optional, set the DKWDRV flag in config)_ 
3. Edit `hdd0:__sysconf:pfs:/osdmenu/OSDMENU.CNF` [as you see fit](patcher/README.md#osdmenucnf)
4. Configure your bootloader to launch `hdd0:__system:pfs:/osdmenu/hosdmenu.elf` or launch it manually from anywhere  

### OSDMenu on PSX DESR / OSDMenu with external OSDSYS

You can configure OSDMenu to boot OSDSYS from 2.20 ROM stored on a memory card or XFROM.  
On PSX, this method allows OSDMenu to boot directly from the XFROM device, bypassing the need for a memory card entirely.

#### Requirements
- Python 3 installed on your machine
- One of the following PS2 2.20 ROMs:
  - `ps2-0220a-20060905.bin` (SHA-256: `e76d9c8f4019041fb17f41fff25d57d608c1a3f205c99bda9eb47402c43e8c93`)
  - `ps2-0220jd-20060905.bin` (SHA-256: `79614b495dcae4e6867f6aff40466a331c6408e966ffac1f2af0bf4dee94b027`)

Other 2.20 ROMs might also work.

#### Setup process

1. Run the `rom_to_osdr.py` Python script in `utils/scripts`:
   ```bash
   python3 rom_to_osdr.py ps2-0220jd-20060905.bin osdsys.bin
   ```
   Replace `ps2-0220jd-20060905.bin` with your ROM file name.
2. Copy `osdsys.bin` to either `xfrom:/osdmenu/osdsys.bin` or `mc?:/SYS-CONF/osdsys.bin`
3. Copy `OSDMENU.CNF` to either `xfrom:/osdmenu/OSDMENU.CNF` or `mc?:/SYS-CONF/OSDMENU.CNF`

Additionally, for PSX users (optional):  

4. Copy `osdmenu.elf` to `xfrom:/osdmenu/osdmenu.elf`  
5. Configure PSXBBL or OSDMenu MBR to boot `xfrom:/osdmenu/osdmenu.elf`

Note: with the 2.20JD OSDSYS, booting OSDMenu while holding the L1 + L2 + R1 + R2 buttons will open up the console region change menu if `OSDSYS_boot` is set to `opening`.

### OSDMenu MBR

You can install OSDMenu MBR into the `__mbr` partition of your HDD for faster HOSDMenu boot times and improved PSBBN support.  
The release archive contains the following files:
  - `osdmbr/OSDMBR.XLF` — encrypted and signed OSDMenu MBR executable to be installed as `__mbr`
  - `osdmbr/XOSDMBR.XLF` — encrypted and signed OSDMenu MBR executable to be installed as `xfrom:/BIEXEC-SYSTEM/xosdmain.elf` (PSX DESR-only)
  - `osdmbr/osdmbr-installer.elf` — installer that will automatically install the MBR, enable the MBR boot and copy the example configuration file
  - `osdmbr/payloads/` — encrypted binaries for advanced users

For DKWDRV support, copy DKWDRV to `hdd0:__system:pfs:/osdmenu/DKWDRV.ELF` and set the DKWDRV flag in config _(optional)_.

See the MBR [README](mbr/README.md) for more details. 

### OSDMenu Launcher

OSDMenu Launcher can be used as a standalone forwarder for launching applications from any supported device.
The release archive contains the following files:
  - `launcher/launcher.elf`--- standalone OSDMenu Launcher 
  - `launcher/OSDMBR.XLF` — encrypted and signed OSDMenu Launcher executable for PATINFO injection

See the launcher [README](launcher/README.md) for more details. 

## Key differences from FMCB 1.8
- All OSD initialization code is removed
- USB support is dropped from the patcher, so only memory cards are checked for `OSDMENU.CNF`
- No ESR support
- No support for launching ELFs by holding a gamepad button
- ELF paths are not checked by the patcher, so every named entry from FMCB config file is displayed in hacked OSDSYS menu
- Support for launching applications from MMCE, MX4SIO and APA- and exFAT-formatted HDDs
- CD/DVD support was extended to support skipping PS2LOGO, mounting VMCs on MMCE devices, showing visual GameID for PixelFX devices and booting DKWDRV for PS1 discs
- Enhanced support for MechaPwn-patched systems:
  - PS2LOGO is patched to always use the disc region
  - OSDSYS disc key check is patched out to fix DVD master discs being detected as "invalid"
- Integrated Neutrino GSM for disc games and applications
- Built-in "Games >" OSDSYS submenu listing PS2 games from `CD`/`DVD` folders on USB/MX4SIO/MMCE devices, a UDPFS server or an SMB share, launched directly via a user-installed standalone Neutrino or OPL
- Built-in "PSX >" OSDSYS submenu listing PS1 games from `EMBER/games` on USB/MX4SIO/MMCE devices, launched directly via a user-installed Ember
- Menu entries can be grouped into OSDSYS submenus with `group_OSDSYS_ITEM_???`
- "Unlimited" number of paths for each entry
- Support for 1080i and 480p (as line-doubled 240p) video modes
- Support for "protokernel" systems (SCPH-10000, SCPH-15000) ported from Free McBoot 1.9 by reverse-engineering
- Support for launching applications from the memory card browser
- Support for setting PS1 driver options on every boot
- Support for HDD OSD 1.10U
- Ability to force OSD region

## Configuration

Comment out lines in config files by prefixing them with `#`. Comments must be on their own line.  
For a step-by-step setup of the games menus, network games and menu groups, see the [tutorial](#tutorial-games-menus-network-games-and-menu-groups).

### R3CONFIGURATOR

You can edit OSDMenu config files directly on your PS2 with the [R3CONFIGURATOR](https://github.com/saildot4k/R3CONFIGURATOR/releases).  

You can reuse an existing Free McBoot or Free HD Boot config file by renaming it to `OSDMENU.CNF` and placing it in the respective paths: `FREEMCB.CNF` → `mc?:/SYS-CONF/` for OSDMenu, `FREEHDB.CNF` → `hdd0:__sysconf:pfs:/osdmenu/` for HOSDMenu.  
The configurator will import your custom menu entries, paths and compatible options and re-format the config for OSDMenu.

### OSDMenu and HOSDMenu

See the patcher [README](patcher/README.md) for more details.

### OSDMenu MBR

OSDMenu comes with the fully-featured MBR that supports running HOSDMenu, HDD-OSD and PSBBN natively.  
It also supports running arbitrary paths from the HDD and memory cards.  
See the MBR [README](mbr/README.md) for more details.

### OSDMenu Launcher

OSDMenu comes with the fully-featured launcher that supports running applications from all devices supported by homebrew drivers.  
See the launcher [README](launcher/README.md) for more details.


## Tutorial: games menus, network games and menu groups

This walkthrough sets up OSDMenu with the "Games >" and "PSX >" submenus, network games and menu groups.
Every part is optional: enable only the devices and features you use.

### 1. What to download

| Project | Needed for | Where to get it |
|---|---|---|
| OSDMenu ([fork releases](https://github.com/higorhgon/OSDMenu/releases)) | Everything | `osdmenu-<tag>.zip` from the releases, or the `artifacts` of a `Build` workflow run |
| [Neutrino](https://github.com/rickgaiser/neutrino/releases) | PS2 games (default launcher), UDPFS games | The **latest development build**: v1.8.0 hangs on MMCE ISOs with digits in their names |
| RiptOPL with MMCE/SMB argv autolaunch ([fork releases](https://github.com/higorhgon/Open-PS2-Loader/releases), branch [`feat/mmce-smb-autolaunch`](https://github.com/higorhgon/Open-PS2-Loader/tree/feat/mmce-smb-autolaunch)) | `games_launcher = opl`, MMCE/SMB games through OPL, IGR | `RIPTOPL-<tag>-OFFICIALPINNED.zip` from the releases, or the `OPL-OFFICIALPINNED` artifact of a `build-flavours` run on that branch. Upstream [RiptOPL](https://github.com/NathanNeurotic/Open-PS2-Loader) only supports the `bdm` autolaunch mode |
| [Ember](https://github.com/Gageformer/Ember) | PS1 games ("PSX >") | Release archive |
| Neutrino's `pc/udpfs_server.py` | UDPFS network games | [Neutrino repository](https://github.com/rickgaiser/neutrino/tree/master/pc) (needs Python 3) |
| An SMB1 share (Samba or Windows) | SMB network games | Samba with `server min protocol = NT1`, or Windows with the "SMB 1.0/CIFS" feature enabled |
| [wLaunchELF](https://github.com/ps2homebrew/wLaunchELF) _(optional)_ | Copying files to the memory card | Release archive |

OSDMenu itself must be started by something: PS2BBL, the System Update (see [above](#osdmenu-as-the-system-update)),
or the boot card of an MMCE device such as the PSXMemcard GEN2 (`BOOT/BOOT.ELF`).

### 2. File structure

The paths below match the example config in the next step. Folder names can be changed in `OSDMENU.CNF`.

**Memory card** (`mc0:`, or the MMCE boot card):
```
mc0:/
├── BOOT/
│   └── BOOT.ELF              ← osdmenu.elf (or mc?:/BOOT/osdmenu.elf, started by PS2BBL)
└── SYS-CONF/
    ├── OSDMENU.CNF           ← your config (see examples/OSDMENU.CNF)
    ├── IPCONFIG.DAT          ← optional: PS2 IP address for UDPFS ("192.168.1.10 255.255.255.0 192.168.1.1")
    ├── OSDGAMES.CNF          ← created by "Refresh list" (PS2 games cache)
    └── OSDPSX.CNF            ← created by "Refresh list" (PS1 games cache)
```

**Storage device** (MMCE SD card `mmce0:`/`mmce1:`, USB `mass0:` or MX4SIO `mx4sio0:`):
```
mmce0:/
├── APPS/
│   ├── neutrino/             ← the whole Neutrino release folder, not just the ELF
│   │   ├── neutrino.elf
│   │   ├── config/
│   │   └── modules/
│   └── OPL/                  ← RiptOPL and its settings (conf_opl.cfg, conf_network.cfg)
│       └── RIPTOPL.ELF
├── CD/                       ← PS2 games released on CD
│   └── SLUS_202.12.Bloody Roar 3.iso
├── DVD/                      ← PS2 games released on DVD
│   ├── Shadow of the Colossus.iso
│   └── Final Fantasy XII/    ← a folder with exactly one ISO also works (Neutrino only)
│       └── SLUS_214.61.iso
├── EMBER/                    ← PS1 games
│   ├── ember.elf
│   └── games/
│       └── Crash Bandicoot/  ← one folder per game, name shown in the menu
│           ├── Crash Bandicoot.cue
│           └── Crash Bandicoot.bin
└── MemoryCards/PS2/<ID>/     ← created by the MMCE for per-game memory cards
```

- `CD/` and `DVD/` both hold **PS2** games. The split only follows the original release media, like in OPL.
- A title ID in the file name (`SLUS_202.12.Name.iso`) is hidden in the menu. Without it, the ID is read from the ISO.
- OPL needs the ISO directly in `CD/` or `DVD/` and a title ID. Other games fall back to Neutrino.

**Network share** (UDPFS folder or SMB share), same layout as a storage device:
```
<share>/                      ← or <share>/<eth_prefix>/ for SMB when OPL's ETH prefix is set
├── CD/
└── DVD/
```

### 3. Configure `OSDMENU.CNF`

**PS2 games from MMCE, launched with Neutrino:**
```ini
games_device_mmce = 1
games_neutrino_path = mmce0:/APPS/neutrino/neutrino.elf
# Optional: Neutrino arguments for every game (PS2 logo, video mode)
games_neutrino_arg = -logo
```

**Launch with RiptOPL instead**: OPL handles IGR (L1+L2+R1+R2+Start+Select) and switches the MMCE back to the boot card.
Set RiptOPL's IGR Path (General & System) to the path of OSDMenu, e.g. `mc0:/BOOT/BOOT.ELF`.
```ini
games_launcher = opl
games_opl_path = mmce0:/APPS/OPL/RIPTOPL.ELF
# Neutrino is still used for the games OPL can't autolaunch
games_neutrino_path = mmce0:/APPS/neutrino/neutrino.elf
```

**PS1 games with Ember:**
```ini
psx_device_mmce = 1
```

**Network games**: the PS2 needs a network adapter (built into slim consoles).
```ini
# UDPFS server, games launched with Neutrino
games_device_udpfs = 1
# SMB share configured in RiptOPL's network settings, games launched with RiptOPL
games_device_smb = 1
games_opl_path = mmce0:/APPS/OPL/RIPTOPL.ELF
# Only needed when conf_network.cfg is not next to games_opl_path or in mc?:/OPL/
# games_smb_config = mmce0:/APPS/OPL/conf_network.cfg
```
- **UDPFS**: run the server on the PC, pointed at the folder that holds `CD/` and `DVD/`:
  ```
  python udpfs_server.py -d /path/to/ps2games
  ```
  The PS2 IP address comes from `IPCONFIG.DAT`, or from OPL's static IP setting when it doesn't exist.
  Also set the same address in Neutrino's `config/bsd-udpfs.toml` (`ip=...`), which Neutrino uses in-game.
- **SMB**: configure the share once in RiptOPL (Network Settings: PS2 IP or DHCP, server IP, share, user, password)
  and OSDMenu reads the same settings. The server must be set by IP address, not by NetBIOS name.

**Menu groups**: move entries into submenus with `group_OSDSYS_ITEM_???`. Every entry needs its own index:
```ini
name_OSDSYS_ITEM_2 = RiptOPL
path1_OSDSYS_ITEM_2 = mmce0:/APPS/OPL/RIPTOPL.ELF
group_OSDSYS_ITEM_2 = Apps

name_OSDSYS_ITEM_3 = wLaunchELF
path1_OSDSYS_ITEM_3 = mc?:/BOOT/WLE.ELF
group_OSDSYS_ITEM_3 = Apps

name_OSDSYS_ITEM_10 = DKWDRV
path1_OSDSYS_ITEM_10 = mc?:/BOOT/DKWDRV.ELF
group_OSDSYS_ITEM_10 = PS1 Tools

name_OSDSYS_ITEM_200 = Shutdown
path1_OSDSYS_ITEM_200 = POWEROFF
```
This results in the following menu:
```
Games >
PSX >
Apps >        → < Back, RiptOPL, wLaunchELF
PS1 Tools >   → < Back, DKWDRV
Shutdown
```

### 4. Using the menus

1. Open "Games >" (or "PSX >"). The first time, the devices are scanned and OSDMenu restarts with the submenu open.
2. Select a game with X. After adding or removing games, select "Refresh list" at the bottom of the submenu.

| Button | Action in a submenu |
|---|---|
| X | Launch the game or entry |
| Circle / "< Back" | Back to the main menu |
| Square | Sort games by name `[A-Z]` or by recently played `[Recent]` (games submenus) |
| Triangle | Add the game to the favorites (marked with `*` and listed first) or remove it (games submenus) |
| Left / Right | Previous / next page |

### 5. Troubleshooting

- **No games found**: the scan screen lists every device and folder it tried, with the number of entries it saw and
  the network errors, if any. Check the `games_device_*` keys and the folder names.
- **Black screen when launching from MMCE**: use Neutrino's latest development build instead of v1.8.0.
- **"Refresh list" or a failed launch boots the Sony OSD**: set `games_return_path` to the path OSDMenu is started from.
- **IGR opens the memory card browser**: set RiptOPL's IGR Path to the path OSDMenu is started from.
- **SMB game doesn't start**: the game needs a title ID and must be directly in `CD/` or `DVD/`, and RiptOPL must be a
  build with SMB autolaunch support.

See the [patcher README](patcher/README.md#games-menu) for all options and the
[launcher README](launcher/README.md#games-handler) for the scan rules and launch arguments.
  
## Credits

- Everyone involved in developing the original Free MC Boot and OSDSYS patches, especially Neme and jimmikaelkael
- Julian Uy for mapping out significant parts of HDD OSD for [osdsys_re](https://github.com/ps2re/osdsys_re) project and the [self-contained OSDSYS implementation](https://github.com/ps2repack/scosdsys)
- [TonyHax International](https://github.com/alex-free/tonyhax) developers for PS1 game ID detection for generic executables.
- Rick Gaiser/Maximus32 for creating [Neutrino](https://github.com/rickgaiser/neutrino), parts of which are used by OSDMenu 
- Matías Israelson for creating [PS2BBL](https://github.com/israpps/PlayStation2-Basic-BootLoader)
- CosmicScale for [RetroGEM Disc Launcher](https://github.com/CosmicScale/Retro-GEM-PS2-Disc-Launcher), [PSBBN Definitive English Patch](https://github.com/CosmicScale/PSBBN-Definitive-English-Patch) and extensive testing  
- Ripto for creating OSDMenu Browser icons and Yornn for collecting all files required for the PSU package    
- Alex Parrado for creating [SoftDev2 installer](https://github.com/parrado/SoftDev2)
- [R3Z3N/Saildot4K](https://github.com/saildot4k) for testing OSDMenu with various modchips, Crystal Chip PBT script, suggestions on documentation, release packaging improvements and R3CONFIGURATOR
- l_oliveira for advices on fixing PS2LOGO for master discs
- GhostTownUS- for testing OSDMenu and OSDMenu MBR on PSX
