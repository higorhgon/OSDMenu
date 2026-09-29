# Licenses and third-party notices

This is an **unofficial fork** of [OSDMenu](https://github.com/pcm720/OSDMenu) by pcm720, modified by higorhgon
(games submenus, network games, menu groups and related changes). It is not endorsed by the OSDMenu authors.

## OSDMenu components

| Component | License | File |
|---|---|---|
| Patcher (`osdmenu.elf`, `hosdmenu.elf`), based on Free McBoot 1.8 | Free McBoot License | [`patcher/LICENSE`](patcher/LICENSE) |
| Launcher, MBR, common code, loader, installer | Academic Free License 3.0 | [`launcher/LICENSE`](launcher/LICENSE) and the `LICENSE` file of each directory |

The Free McBoot License requires, among other things, that OSDMenu and its derivatives are distributed free of
charge (no fees or donations in exchange for it), keep the Free McBoot credits, stay open source and are
distributed with an exact copy of the license.

## Embedded third-party code

| Code | Origin | License | Notice |
|---|---|---|---|
| mmceman (MMCE driver) | [MMCE Authors](https://github.com/ps2-mmce/mmceman) | MIT | [`launcher/iop/mmceman/LICENSE`](launcher/iop/mmceman/LICENSE) |
| UDPFS/UDPBD drivers, smap and ministack (`launcher/iop/udpfs`) | [Neutrino](https://github.com/rickgaiser/neutrino) by Rick Gaiser | Academic Free License 3.0 | [`launcher/LICENSE`](launcher/LICENSE) |
| Embedded GSM (`utils/loader`) | Based on Neutrino GSM by Rick Gaiser | Academic Free License 3.0 | [`utils/loader/LICENSE`](utils/loader/LICENSE) |
| scePadPortOpen patterns (`patcher/include/patterns_pad.h`) | [Open PS2 Loader](https://github.com/ps2homebrew/Open-PS2-Loader) by Ifcaro, jimmikaelkael & Polo | Academic Free License 3.0 | [`launcher/LICENSE`](launcher/LICENSE) |
| PS2SDK libraries and IOP modules (iomanX, fileXio, netman, smap, ps2ips, smbman and others) | [PS2SDK](https://github.com/ps2dev/ps2sdk) | Academic Free License 2.0 | [`licenses/PS2SDK-AFL-2.0.txt`](licenses/PS2SDK-AFL-2.0.txt) |
| lwIP TCP/IP stack (in PS2SDK's `ps2ip-nm.irx`, used for the SMB games list) | [lwIP](https://savannah.nongnu.org/projects/lwip/), Swedish Institute of Computer Science | BSD 3-Clause | [`licenses/lwIP-BSD.txt`](licenses/lwIP-BSD.txt) |

## Programs that are launched, not distributed

Neutrino, Open PS2 Loader/RiptOPL, Ember, DKWDRV, wLaunchELF and the UDPFS server are separate programs installed
by the user. OSDMenu only starts them and does not include any of their code, other than what is listed above.
