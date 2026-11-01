# DivaMovie

PD-Loader plugin that fixes movie playback in Project DIVA Arcade Future Tone (7.10).

It replaces the game's Media Foundation movie player with an FFmpeg-based one. That makes WMV and MP4 movies play under
Wine/Proton (Linux, Steam Deck), where Media Foundation and the WGL_NV_DX_interop extension are missing or broken. It
also works on Windows.

What it does:

- **FFmpeg movie player** (default): decodes song and attract-mode movies with FFmpeg and uploads frames straight to an
  OpenGL texture. Video is decoded on its own thread and catches up by dropping frames on slow CPUs. Optional movie audio
  goes through waveOut and drives the clock, so audio and video stay in sync.
- **Interop emulation**: if the FFmpeg player is off or unavailable and the driver lacks `WGL_NV_DX_interop` (e.g. Wine),
  it copies Direct3D movie frames to OpenGL through system memory instead of showing black.
- **Diagnostics**: with `debug=1`, everything is logged to `plugins\DivaMovie.log`.

If the FFmpeg DLLs are missing or the wrong version, DivaMovie falls back to the game's own movie player (plus interop
emulation), so the game still runs.

## FFmpeg DLLs (the version matters)

DivaMovie loads FFmpeg at runtime from the `plugins` folder. It looks for these exact file names:

| File               | Library          | Required major version |
|--------------------|------------------|------------------------|
| `avutil-61.dll`    | libavutil        | 61                     |
| `swresample-7.dll` | libswresample    | 7                      |
| `swscale-10.dll`   | libswscale       | 10                     |
| `avcodec-63.dll`   | libavcodec       | 63                     |
| `avformat-63.dll`  | libavformat      | 63                     |

**The number in each file name is the FFmpeg ABI major version, and it must match exactly.** DivaMovie checks the
version reported by each DLL and refuses to use a mismatched one. Minor versions don't matter.

These are **FFmpeg master (development) builds from autumn 2026**. DivaMovie was built and tested against
FFmpeg master `ba987fe` (2026-10-10): libavutil 61.13, libavcodec 63.18, libavformat 63.8, libswresample 7, libswscale 10.

Where to get them:

- [BtbN/FFmpeg-Builds releases](https://github.com/BtbN/FFmpeg-Builds/releases): `ffmpeg-master-latest-win64-gpl-shared.zip`
  (or `-lgpl-shared`). Copy the five DLLs listed above from its `bin` folder into `plugins`, next to `DivaMovie.dva`.
- Release builds named `n8.x` use the older majors (`avcodec-62.dll` etc.) and **will not work**.
- FFmpeg master bumps these majors from time to time. If a newer "latest" build ships e.g. `avcodec-64.dll`, use a dated
  autobuild from around October 2026 if one is still listed, or rebuild DivaMovie against the new headers (see below).

The other DLLs in that zip (`avfilter`, `avdevice`, `postproc`) and the `.exe` files are not needed.

With `debug=1`, `plugins\DivaMovie.log` prints either `FFmpeg loaded (avcodec 63.x.x)` or
`FFmpeg not available (...)` with the reason (the missing file or the version that didn't match).

## Options (`plugins\DivaMovie.ini`, section `[general]`)

| Option                      | Default | Meaning |
|-----------------------------|---------|---------|
| `ffmpeg_player`             | 1       | Play movies with FFmpeg instead of Media Foundation. |
| `movie_audio`               | 0       | Play the movie's own audio track (e.g. attract-mode movies). Never used during songs, where the video follows the song. |
| `force_interop_emulation`   | 0       | Copy frames through system memory even if the driver supports `WGL_NV_DX_interop` (only matters when the FFmpeg player is off). |
| `disable_interop_emulation` | 0       | Never emulate `WGL_NV_DX_interop`. |
| `debug`                     | 0       | Log to the console and `plugins\DivaMovie.log` (slower). |

These options also appear in the PD-Loader launcher's plugin settings.

## Building

1. Get the **same** FFmpeg shared build whose DLLs you'll ship (e.g. `ffmpeg-master-latest-win64-gpl-shared.zip`) and copy
   its `include` folder to `source-code/dependencies/ffmpeg/include`. You end up with
   `source-code/dependencies/ffmpeg/include/libavcodec/avcodec.h`, etc.
2. Build `DivaMovie.vcxproj` (Release|x64) as with the other plugins.

No `.lib` import libraries are needed, because the DLLs are loaded with `LoadLibrary`/`GetProcAddress`. The required
major versions above come from the headers in `include`, so headers and DLLs must come from the same FFmpeg build. To
switch to a newer FFmpeg, update the headers, rebuild, and ship the matching DLLs.

## Licensing note

FFmpeg is LGPL; `gpl-shared` builds are GPL. DivaMovie does not link FFmpeg statically, and the DLLs are separate
downloads. If you redistribute FFmpeg DLLs with DivaMovie, follow the licence of the build you ship. The `lgpl-shared`
build is enough for WMV/MP4 playback.
