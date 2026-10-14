# Tessera — notes for Claude Code

Tessera is a lightweight Windows 10/11 photo viewer: one ~650 KB native exe with no runtime or installer. It's a personal project for Djamel, who uses it as his default photo viewer. `README.md` is the user-facing description, with the full shortcut list.

## What the owner asked for (keep these true)

1. **Very lightweight.** Starts instantly, zero CPU when idle (redraw only on change), and no dependencies beyond what ships with Windows.
2. **Can be the Windows default photo viewer.** Per-user registration, no admin rights.
3. **Opens any image type without issues.** Windows' own decoders (WIC) first, then bundled fallbacks.
4. **Tiles.** Open any number of images at once. They tile automatically, and each zooms and pans independently.
5. **Fullscreen shows only the images.** No bars, labels or cursor: "I just want to see all the pics".
6. **Pixel-exact zoom.** At 100%+ every source pixel is a crisp square (nearest-neighbour), never smoothed. This was the main complaint about the Windows Photos app. Zoom steps above 100% are integers so every pixel is the same size. Zooming *out* (<100%) uses high-quality cubic downsampling, which is correct.
7. **Rotate and crop** inside the app.

A separate "Tessera Enhance" AI-upscaling build was explored and dropped. **Ignore it**: it's not part of this project. Don't reintroduce it unless asked.

## Building

Windows (from the repo root):

```
powershell -ExecutionPolicy Bypass -File tools\get-toolchain.ps1   # once: downloads llvm-mingw into .toolchain\
build.bat            # release  -> bin\Tessera.exe
build.bat debug      # -O0 -g
```

From Linux/WSL with llvm-mingw on PATH, use `./build.sh` or `./build.sh debug`.

- Compiler: clang via [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20260922 (UCRT, x86_64). Statically linked, so the exe needs only DLLs that ship with Windows.
- The build must stay warning-free under `-Wall -Wextra` (excluding the vendored headers).
- **MSVC has never been tried.** The code should be close, but expect small fixes (`-municode` → `/ENTRY:wWinMainCRTStartup`, linked libs via `#pragma comment(lib, ...)`). Don't switch toolchains without a reason.
- `bin\Tessera.exe` is a prebuilt copy of the current source. The owner's installed copy, registered as default viewer, lives at `C:\Users\Djamel\Documents\Tessera\Tessera.exe`. Don't overwrite it without asking: it's the one Windows file associations point at, and it may be running.

## Layout

```
src/main.cpp          the whole app (~2100 lines, sections marked "// ------ name")
src/stb_image.h       vendored (public domain): TGA, PSD, HDR, PNM… fallback decoder; don't edit
src/qoi.h             vendored (MIT): QOI decoder; don't edit
src/tessera.rc        icon, manifest, version info
src/tessera.manifest  PerMonitorV2 DPI, long paths, asInvoker, Win10+ compatibility
src/tessera.ico       app icon (16–256 px)
tools/get-toolchain.ps1
build.bat / build.sh
```

### `main.cpp` by section

- **utilities**: `Com<T>` smart pointer, file-type list `kExts` (drives both opening and registration), path helpers.
- **pixels**: `Pixels` holds straight-alpha BGRA, top-down. `Remap()` implements the 8 EXIF orientations (6 = 90° CW, 8 = 90° CCW) and is also used for rotate.
- **decoding**: `DecodeAny()` tries WIC (honours EXIF orientation), then QOI, then stb_image. It returns friendly errors naming the Microsoft Store extension needed for HEIC/AVIF/JXL/WebP/RAW.
- **background loader**: thread pool (≤4, below-normal priority). Results come back via `WM_APP_LOADED` and are matched by `tileId` + `gen` so stale loads are dropped.
- **app state**: `Tile` (one per open image: path, pixels, GPU chunks, view, undo stack, folder siblings) and the global `App g`.
- **settings**: `HKCU\Software\Tessera\Settings` (Sync, Grid, Labels, window Placement).
- **default-app registration**: everything under HKCU: ProgId `Tessera.Image`, `Applications\Tessera.exe`, `Software\Tessera\Capabilities`, `RegisteredApplications`. Windows doesn't let apps set themselves as default, so the app registers and then opens `ms-settings:defaultapps`. It self-registers on first run, or when the exe path changes (`IsRegisteredHere`). There are `--register` / `--unregister` CLI switches.
- **device resources**: Direct2D HWND render target (+ `ID2D1DeviceContext` for the cubic interpolation mode). `BuildChunks()` uploads pixels as one or more bitmaps, split at the GPU max texture size. Once an *unedited* image is on the GPU, its CPU copy is freed; `EnsureCpu()` re-decodes from disk when an edit needs it.
- **view math**: per-tile `fit` / `zoom` / centre (`cx`, `cy` in image pixels). `FitZoom()` floors to an integer when ≥1. Offsets are rounded to whole device pixels so nearest-neighbour stays crisp. The `kLadder` zoom steps are integers above 1.
- **layout**: tries every column count and picks the one that maximises total displayed image area. Rows can have different counts (the last row is the remainder).
- **loading / folder navigation**: `OpenFiles()` expands folders. Arrow keys walk the focused tile's folder in natural (Explorer) sort order.
- **editing**: rotate and crop produce new `Pixels`, with up to 4 undo steps per tile. `t.edits` drives the "•" unsaved marker and the save prompt.
- **saving**: WIC encode to a temp file, then `MoveFileEx` replace. JPEG is written at 95% quality with metadata copied and orientation reset to 1. `PixelSource` is a zero-copy `IWICBitmapSource`. It can only write JPEG/PNG/BMP/TIFF; other formats get "save a copy".
- **rendering**: `Render()` → `DrawTile()` per tile (clip, checkerboard for alpha, chunks, pixel grid ≥800%, crop overlay, label) → status bar, toast, help overlay. Device loss (`D2DERR_RECREATE_TARGET`) drops GPU resources and reloads lazily.
- **input**: wheel zooms the tile under the cursor (or all tiles with Sync). Left-drag pans, middle-click closes, right-click opens the context menu, double-click toggles fullscreen. Keyboard in `OnKey`. `WM_COPYDATA` receives files from a second instance.
- **startup**: single instance through a named mutex plus `WM_COPYDATA`. When Explorer launches one process per selected file, files arriving within 1.5 s are added as tiles instead of replacing the view. This is how multi-select + Enter opens them together.

## Conventions

- Plain Win32 + COM, C++17, no frameworks and no new dependencies without a strong reason. Everything stays in one exe.
- Keep idle cost at zero: no timers or polling except the fullscreen cursor-hide and toast timers.
- All UI sizes are multiplied by `g.scale` (DPI / 96).
- Never smooth when magnifying. Any new drawing path must use `NEAREST_NEIGHBOR` when zoom ≥ 1.
- User-visible text is UK English and plain ("Can't open…", "Not enough memory…").
- Errors are shown in the tile or as a toast, never as a crash. Allocations of image-sized buffers are wrapped in try/catch.

## Testing

There are no automated tests. After a change, build, then check by hand on Windows:

- Open 1, 2, 5 and 12 images (multi-select + Enter in Explorer, drag-drop, Ctrl+O). Tiling should look sensible for mixed portrait/landscape.
- Zoom to 100%, 300% and 1600%: pixels must be hard-edged, with no seams between GPU chunks on huge images (>16k px).
- Fullscreen shows nothing but images, the cursor hides after 1.5 s, and Esc exits.
- Rotate, crop, undo, then save (and save a copy) for a JPEG with EXIF orientation 6. Reopen it and it should be upright.
- Transparent PNG shows a checkerboard. Animated GIF shows its first frame.
- Missing codec: a .heic without the Store extension shows the install hint in the tile.

## Known gaps / ideas (not started)

- Animated GIF/WebP play only the first frame.
- No colour management: embedded ICC profiles are ignored, so wide-gamut photos look slightly off.
- 16-bit and HDR images are reduced to 8-bit for display.
- The folder list is read once per tile; files added later aren't seen until the tile changes folder.
- No pinch-zoom gesture handling beyond what arrives as mouse-wheel messages.
- No delete-to-recycle-bin, slideshow or EXIF info panel yet.
