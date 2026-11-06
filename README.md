# 3DSX Icon Maker

This is the gpSP 3D Icon Maker application and source as the base: it keeps
Icon Maker's browser, tabs, dialogs, previews, banner styles, artwork fitting,
sidecar image discovery, batch creation, and installed-icon management. The
file browser now selects `.3dsx` apps and its existing generated icon/banner
buffers are written into an installable CIA forwarder.

Browse to a `.3dsx` on the SD card and choose **A** to preview and make its
HOME Menu icon. Matching images are read beside the app: `App.png`,
`App.jpg`, or `App.jpeg` for the icon, and `App.banner.png`, `.jpg`, or
`.jpeg` for the banner. The HOME Menu title and publisher are copied from the
SMDH embedded in the `.3dsx`, or a same-basename `App.smdh` when the `.3dsx`
has no embedded SMDH. The title falls back to the filename and publisher to
blank when neither SMDH is available. Icon selection priority is the SMDH
icon embedded in the `.3dsx`, then a same-basename `App.smdh`, then the
existing custom PNG/JPEG sidecar, then Icon Maker's generated fallback.
Icon Maker's existing fit/crop and banner-style controls are retained. The
**L button has no action**; press **Y** for batch icon creation. The selected
`.3dsx` must stay at the same SD path.
The original Icon Maker 3D banner asset and renderer are used for 3D mode;
flat/custom banners continue through Icon Maker's existing banner renderer.

The CIA template uses Luma's `hb:ldr` service to launch the installed
Homebrew Launcher loader (`000400000D921E00`) with the selected `.3dsx` path.
The app installs the generated CIA directly; it does not save a separate CIA
file. Settings, the last folder, timing log, and the path-to-title-ID registry
are stored under `sdmc:/3ds/3DSX-Forwarder/`.

## Requirements and limitations

- Luma3DS custom firmware with signature patches enabled to install the CIA.
- The Homebrew Launcher loader title (`000400000D921E00`) must be installed.
- The `.3dsx` path must be shorter than 256 bytes and remain unchanged.
- Homebrew apps that require command-line arguments are not supported; the
  forwarder passes an empty content/ROM path.
- The forwarder template's signatures are not regenerated, so stock firmware
  cannot install it.

## Build

With devkitPro, devkitARM, libctru, and 3DS portlibs installed, run:

```sh
make
```

The build produces `3DSX-Forwarder.3dsx`.

## Licenses

The Icon Maker and gpSP-derived application sources retain their GPL-2.0
license in `licenses/gpsp-3d-COPYING`. The CIA patching and SHA-256 modules
and embedded forwarder template retain the MIT license in `LICENSE`.
