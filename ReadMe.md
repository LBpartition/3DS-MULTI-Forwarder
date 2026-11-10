# 3DS MULTI-Forwarder
#### Tested on New 3DS & Old 3DS XL
### Create, Install & Manage Custom Forwarders for 3DS
- 3DSX - `.3dsx`
    - Requires [HomeBrew Launcher](https://github.com/PabloMK7/homebrew_launcher_dummy/releases)
- .NDS - `.nds`
    - Requires [Twilight Menu](https://github.com/DS-Homebrew/TWiLightMenu)
- GBA - `.gba`
    - Requires [gpSP-3D](https://github.com/bheppe/gpsp-3d/releases/latest)
- SNES - `.smc`, `.sfc`, `.fig`, `.bs`, `.bsx`.
    - Requires [Snes9x-3D](https://github.com/bheppe/snes9x_3ds/releases/latest)

### Features
- Preview on top screen before creating
- Custom Title Names & Publisher Names
- [Custom Icons, Banners & Banner Audio support](https://github.com/LBpartition/3DS-MULTI-Forwarder/tree/main#custom-icons-banners--banner-audio)
- Manage your installed Forwarders, with the option to uninstall

**3DSX / GBA / SNES**
- 3 Different Banner Styles - Switch between them with `Select`
- Create your own custom Icons and Banners with `.png`/`.jpg` & `.wav` files

**NDS**
- Create your own custom Icons (also used for NDS banners) with `.bmp` files
  - [Create them with NDS Custom Icon Converter](https://lbpartition.github.io/3DS-MULTI-Forwarder/NDS-Icon-Converter.html) 
- Manage NDS Forwarders / installed TWL Titles
  - Keep track of the 3DS's limit of 40 TWL Titles, with the option to uninstall
---

## How to Use:
1. Download the [Latest MULTI-Forwarder.zip](https://github.com/LBpartition/MULTI-Forwarder/releases/latest),
2. Extract the zip and copy the `3ds` folder to your 3DS SD card.
3. Launch `MULTI-Forwarder` from the Homebrew Launcher
4. Choose a .3dsx/.nds/.gba/.smc/.sfc/.*etc* file to forward,
   - Press `A` and go through the prompts to create the forwarder
      - *Custom Title Name?*
      - *Custom Publisher?*
- Preview Banner & Title on the Top Screen before creating the Forwarder
  - 3 Different Banner Styles: Switch between them with `Select`
    - 4th Banner style *Custom* is shown automatically if custom banner files are found
    - NDS Forwarders only have 2 banner styles, Custom & Default. No custom banner audio.
      - See [Custom Icons, Banners & Banner Audio]((https://github.com/LBpartition/3DS-MULTI-Forwarder/tree/main#custom-icons-banners--banner-audio)) for more info
## Gallery:

|  |  |
|---|---|
| Manage & Uninstall Forwarders: | |
| <img width="400" height="480" alt="0-Installed" src="https://github.com/user-attachments/assets/4542f51b-8780-46ec-ade1-f8a304d70056" /> |  <img width="400" height="480" alt="0-TWL-Manager" src="https://github.com/user-attachments/assets/1436bae0-c5c3-4ae9-9ed3-0d981436761f" /> |
| Preview Banners & Icons Before Creating: |  |
| <img width="400" height="480" alt="Style1-PREVIEW" src="https://github.com/user-attachments/assets/4a9c61ba-e369-42b8-a293-9d42e71ba236" /> | <img width="400" height="480" alt="Style1-RESULT" src="https://github.com/user-attachments/assets/87cadb9e-77f8-4364-a120-bba46519c6ef" /> |
| <img width="400" height="480" alt="Style2-PREVIEW" src="https://github.com/user-attachments/assets/0a328ecc-ceae-4be7-aac9-c01174603281" /> | <img width="400" height="480" alt="Style2-RESULT" src="https://github.com/user-attachments/assets/a0fffc07-844d-47f1-bdf8-b58833137210" /> |
| <img width="400" height="480" alt="Style3-PREVIEW" src="https://github.com/user-attachments/assets/ad8cff74-9bdc-4ff4-8727-18b5e69f0368" /> | <img width="400" height="480" alt="Style3-RESULT" src="https://github.com/user-attachments/assets/050a9b12-6df9-4915-8f1f-c0e1a973589c" /> |
| Custom 3DSX / GBA / SNES Icons, Banners & Banner Audio |  |
| <img width="400" height="480" alt="StyleCUSTOM-PREVIEW" src="https://github.com/user-attachments/assets/fa600177-1335-49cf-aef8-5242732cfd29" /> | <img width="400" height="480" alt="StyleCUSTOM-RESULT" src="https://github.com/user-attachments/assets/614cf6ac-ca18-4e52-bc75-0077515ab460" /> |
| Custom NDS Icons (Also used for banners) |  |
| <img width="400" height="480" alt="ndsCustom-PREVIEW" src="https://github.com/user-attachments/assets/e5cfbe21-518a-488a-b993-23e570c63eb7" /> | <img width="400" height="480" alt="ndsCustom-RESULT" src="https://github.com/user-attachments/assets/1ae1b66d-5969-4654-8e64-74f5218c46bc" /> |

## Custom Icons, Banners & Banner Audio

#### Not a fan of the included banner styles? - Create your own! 

### 3DSX / GBA / SNES
- Place your `.png`/`.jpg` & `.wav` files in the same folder as whatever you're forwarding & name them like this:
  - *Example:*
    - `fallout2.3dsx` - 3dsx / .nds / .gba / .*etc*
    - `fallout2.png` - Icon (48x48)
    - `fallout2.banner.png` - Banner (256x128)
    - `fallout2.banner.wav` - Banner Audio (3 seconds max)

- **Note:** Banners are silent if no `*.banner.wav` file is found.
- **Note:** Icons and Banners can be `*.png` OR `*.jpg`. (& `*.banner.png` OR `*.banner.jpg`)

### NDS Custom Icons

- **Note:** NDS Forwarders only have 2 banner styles, (Custom & Default.) No custom Banner Audio.
  - Place your `.bmp` file in the same folder as your .nds rom & name it like this:
    - `Pokemon - Storm Silver.nds` - .nds
    - `Pokemon - Storm Silver.bmp` - Icon (32x32) (Also used for NDS Banners)

- **Note:** NDS Custom Icon Requirements:
  - `.bmp` only. - [Create them with NDS Custom Icon Converter](https://lbpartition.github.io/3DS-MULTI-Forwarder/NDS-Icon-Converter.html)
    - **4 bits per pixel**: indexed/paletted, up to 16 colors.
    - **40-byte `BITMAPINFOHEADER`** and **BI_RGB compression** (`0`).
    - At most 16 palette entries. If the palette count is zero, the code assumes 16.
    - Pixel data must contain 512 bytes (32 rows × 16 bytes); standard padding is not needed at this width.
--- 

## Bugs & Suggestions
[Post a new issue here](https://github.com/LBpartition/MULTI-Forwarder/issues) to report bugs or suggest features

## Credits
- bheppe - GUI, Custom Icons & Banners + GBA/SNES Forwarders - Based on [IconMaker for gpSP-3D](https://github.com/bheppe/gpsp-3d) & [Snes9x](github.com/bheppe/snes9x_3ds)
- Fadgiras - 3DSX Forwarders - Based on [RetroArchForwarder3DS](https://github.com/Fadgiras/RetroArchForwarder3DS)
- Fishand73 - NDS Forwarders + Custom Icons - Based on [ndsForwarder Fork](https://github.com/fishand73/ndsForwarder)
- Claude - its vibecoded
