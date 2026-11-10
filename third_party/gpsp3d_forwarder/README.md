# gpSP 3D forwarder stub

The broader upstream reference is [bheppe/gpsp-3d](https://github.com/bheppe/gpsp-3d), which also contains the gpSP 3D IconMaker. This directory contains only its forwarder stub, build file, and RSF configuration from the `gpsp3ds/forwarder/` subdirectory, at source revision `717b5372f3bd9f613017d3aa71630fdbc5cea7a2`.

The stub reads the patched ROM path from its `GP3D-FWD-SLOT-1` data slot, writes that path to the configured emulator launch file, and launches the emulator title. The generated CIA templates in `romfs/` use this code for both GBA and SNES forwarders. The source and derived binaries are covered by the GNU GPL version 2; see `third_party/licenses/gpsp-3d-COPYING.txt`.
