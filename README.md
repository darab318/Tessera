# Tessera

A lightweight photo viewer for Windows 10/11 that tiles any number of images and shows true pixels when you zoom.

- **Single 650 KB .exe**, no installer, no runtime. Uses only what ships with Windows (Direct2D, WIC).
- **Idle CPU is zero**: it only redraws when something changes. Images decode on background threads.
- **Tiles**: open as many images as you like; they arrange themselves to use the screen best. Each tile zooms and pans on its own (or press `S` to sync them for side-by-side comparison).
- **Pixel-exact zoom**: at 100% and above every pixel is drawn as a crisp square (nearest-neighbour), never smoothed. Zoom steps above 100% are whole numbers (200%, 300%, 400%...) so every pixel is the same size. A pixel grid appears from 800% (`G` to toggle). Zooming *out* uses high-quality downsampling, which is the accurate way to shrink an image.
- **Fullscreen** (`F`, `F11` or double-click) shows only the images: no bars, no labels, and the cursor hides itself.
- **Rotate** (`R` / `Shift+R`), **crop** (`C`, drag, `Enter`), **undo** (`Ctrl+Z`), **save** (`Ctrl+S`) or **save a copy** (`Ctrl+Shift+S`).
- Respects phone-camera orientation (EXIF) automatically.
- **Animated GIFs play**, in every tile at once. Rotate and crop apply to the whole animation; saving writes a still of the first frame.

## Setting it as your default photo viewer

1. Put `Tessera.exe` somewhere permanent, e.g. `C:\Program Files\Tessera\` or `%LOCALAPPDATA%\Tessera\`. (If you move it later, repeat these steps.)
2. Run it, right-click anywhere, choose **Make default photo viewer…**.
3. Windows Settings opens on Tessera's page. Click **Set default** (Windows doesn't let apps do this step themselves).

No admin rights are needed. **Remove file associations** in the same menu undoes it.

## Opening several images at once

- Select multiple files in Explorer and press **Enter**. They open together as tiles in one window.
- Or drag files or whole folders onto the window, or use `Ctrl+O` (multi-select). These *add* to what's open.
- Double-clicking a single file while Tessera is already open replaces the current view.

## Formats

Built in: JPEG, PNG, BMP, GIF (animated GIFs play), TIFF, ICO, JPEG XR, DDS, TGA, PSD, HDR, PNM/PPM/PGM, QOI, and WebP on Windows 11.

Windows provides these through free Microsoft Store extensions, which Tessera uses automatically once installed: HEIC (*HEIF Image Extensions* + *HEVC Video Extensions*), AVIF (*AV1 Video Extension*), JPEG XL (*JPEG XL Image Extension*), camera RAW (*Raw Image Extension*), WebP on Windows 10 (*WebP Image Extensions*). If one is missing, the tile tells you which to install.

Saving writes JPEG, PNG, BMP or TIFF. Saving a JPEG re-encodes it at 95% quality and keeps its camera metadata.

## Shortcuts

| Key | Action |
|---|---|
| Wheel / drag | Zoom tile under cursor / pan |
| `1` / `0` | Actual pixels / fit |
| `+` `−` | Zoom in / out |
| `←` `→` | Previous / next image in the same folder |
| `Tab` | Focus next tile |
| `S` `G` `I` | Sync tiles · pixel grid · filename labels |
| `F` `F11` `Esc` | Fullscreen on / off |
| `R` `Shift+R` | Rotate right / left |
| `C` → `Enter` | Crop (`Shift` = square, `Esc` cancels) |
| `Ctrl+Z` `Ctrl+S` `Ctrl+Shift+S` `Ctrl+C` | Undo · save · save a copy · copy |
| `Ctrl+W` / middle-click | Close tile |
| `H` | Shortcut overlay |

## Building

On Windows: run `tools\get-toolchain.ps1` once (downloads the llvm-mingw compiler into `.toolchain\`), then `build.bat`. The result is `bin\Tessera.exe`. From Linux/WSL with llvm-mingw on PATH: `./build.sh`.

The source is one file, `src/main.cpp`, plus `stb_image.h` (public domain) and `qoi.h` (MIT) for formats Windows has no codec for.
