# MULTI-FORWARDER

Create and manage HOME Menu forwarders for 3DSX, GBA, SNES, and NDS games. The app keeps its settings, records, and NDS-builder data in `sdmc:/3ds/MULTI-Forwarder`.

## Supported forwarders

| Type | Files | Launch target |
|---|---|---|
| 3DSX | `.3dsx` | Homebrew Launcher Loader installed as a CIA |
| GBA | `.gba`, `.zip` | gpSP 3D installed as a CIA |
| SNES | `.sfc`, `.smc`, `.fig`, `.bs`, `.bsx` | Snes9x 3D installed as a CIA |
| NDS | `.nds`, `.srl` | TWL forwarder installed to NAND; NDS Bootstrap forwarder pack required |

SNES ZIP files are not supported by Snes9x 3D. ZIP files are treated as GBA games. 

PicoDrive3D3DS & armGBA were inspected, but cannot currently be supported.

GBA forwarders write the selected ROM path to `sdmc:/3ds/gpsp3d/launch.txt` before launching gpSP 3D. SNES forwarders use `sdmc:/3ds/snes9x_3ds/launch.txt`. These launch files are written as needed; the app does not import, inspect, or delete the emulators' databases. NDS forwarders are limited to 40 installed TWL titles.

GBA and SNES forwarders use Icon Maker's path-derived title IDs when available, and installed GBA/SNES/3DSX forwarders in the `0xC0000–0xFEFFF` title-ID range are detected from HOME Menu metadata. The title-ID-to-ROM path record for forwarders created by Multi-Forwarder remains in `sdmc:/3ds/MULTI-Forwarder/title_ids.tsv`.

## Install and use

1. Copy `MULTI-Forwarder.3dsx` and `MULTI-Forwarder.smdh` into `sdmc:/3ds/MULTI-Forwarder/`.
2. Launch **MULTI-FORWARDER** from the Homebrew Launcher.
3. Browse to a supported game and select it to preview and create its forwarder. Install the required target emulator CIA first. Custom titles may use one or two lines; those title lines are rendered on the banner. A custom publisher is one line of HOME Menu metadata and is not drawn on the banner.
4. Use **Installed** to view or remove app-created 3DSX/GBA/SNES forwarders. Press **Y** to cycle `ALL > 3DSX > GBA > SNES > ALL`. Selecting an entry shows its title ID; 3DSX entries identify **3DSX Homebrew Launcher** on the top screen.
5. Use **TWL** to view installed TWL/DSiWare titles from system memory and microSD; the list reads each title's banner name and icon from its installation media, titles created by Multi-Forwarder are labeled **NDS**, and other titles are labeled **TWL**. Selecting a title shows its ID and identifies app-created NDS forwarders as **Twilight Menu** or other user TWL titles as **DSiWare**. Press **X** and confirm to uninstall any listed user TWL title; an NDS forwarder's ROM file remains on the SD card.

**L/R** switches tabs. Prompts use the D-pad to select an option, **A** to confirm, and **B** to cancel. **SELECT** cycles banner styles for 3DSX/GBA/SNES previews; for NDS it switches between the default and custom icon when a valid custom BMP exists.

## Custom 3DSX, GBA, and SNES artwork

Place artwork beside the game file and use its base name. For `Super Game.gba`, for example:

- `Super Game.png`, `.jpg`, or `.jpeg` — icon
- `Super Game.banner.png`, `.jpg`, or `.jpeg` — banner
- `Super Game.banner.wav` — banner audio

The same naming convention works for 3DSX and supported GBA/SNES files. Custom WAV audio applies to flat and 3D banners, with or without a custom banner image; when no matching WAV exists, the template audio is kept. Images may be any size; they are fitted with padding rather than cropped. WAV audio must be 16-bit PCM and no longer than three seconds. The top screen previews the selected artwork before installation.

NDS forwarders use the NDS builder's artwork convention: a same-name `.bmp` icon and `.bin` banner beside the ROM, or in `sdmc:/3ds/MULTI-Forwarder/icons/` and `banners/`. Custom BMP icons must be 32x32, uncompressed, 4-bit indexed BMPs; palette index 0 is treated as transparent in previews. A valid custom BMP is selected by default when browsing an NDS game; **SELECT** toggles between it and the ROM's default icon. The PNG/JPEG/WAV artwork pipeline above does not apply to NDS forwarders.

## Old 3DSX-Forwarder data

On startup, the app checks only `sdmc:/3ds/3DSX-Forwarder`. If it finds files, it asks whether to import the folder into `sdmc:/3ds/MULTI-Forwarder`. Existing title-ID records are merged by ROM path; differing files already in the new folder are kept, with the old copies saved using a `.legacy` suffix. After a successful import, it separately asks whether to delete the old folder. Choosing **NO**, canceling, or an import failure leaves the old folder in place. No gpSP/Snes9x Icon Maker database or `sdmc:/3ds/forwarder` data is imported or deleted.

## Building

Build with devkitPro/devkitARM from this directory:

```sh
make
```

The build produces `MULTI-Forwarder.3dsx` and `MULTI-Forwarder.smdh`. The GBA/SNES CIA templates in `romfs/` are generated from the GPL-licensed gpSP 3D forwarder stub included in `third_party/gpsp3d_forwarder/`.

## Sources and licenses

- [LBpartition/3DSX-Forwarder](https://github.com/LBpartition/3DSX-Forwarder) — base project.
- [fishand73/ndsForwarder](https://github.com/fishand73/ndsForwarder) — NDS/TWL builder source; see `third_party/licenses/ndsForwarder-GPL-3.0.txt`.
- [bheppe/gpsp-3d](https://github.com/bheppe/gpsp-3d) — gpSP 3D and its IconMaker source, referenced for the app's artwork/forwarder workflow and launch conventions; the included GBA forwarder stub and license are in `third_party/gpsp3d_forwarder/` and `third_party/licenses/gpsp-3d-COPYING.txt`.
- [bheppe/snes9x_3ds](https://github.com/bheppe/snes9x_3ds) — SNES title and launch convention. Snes9x 3D is an external dependency, not bundled here; review its [license](https://github.com/bheppe/snes9x_3ds/blob/master/LICENSE.md), which limits use of the emulator to non-commercial purposes.
