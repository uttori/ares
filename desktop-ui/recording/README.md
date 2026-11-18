# Native SNES recording

In a recorder-enabled desktop build, select **Load → Record Next Game**, then open a Super Famicom game normally. The status bar shows when recording is armed and when it starts. Assign **Record Video** under **Settings → Hotkeys**. Press the assigned key to finish and save, or choose **Tools → Stop Recording**. The same key arms capture when no game is loaded and starts a new take during play. From the command line, use `ares --record-next-game path/to/game.sfc`.

Movies are saved beside screenshots. Each Matroska file contains lossless FFV1 RGB video at the game's NTSC or PAL field cadence and stereo 16-bit PCM at the mixer rate. Window scaling, shaders, volume and mute affect playback only. A pre-armed recording starts with the first picture and includes sound generated before that picture. Pauses add no media time. Loading a state, resetting or unloading the game, quitting, or changing the mixer rate finalizes the recording.

A 256-dot field stays 256 pixels wide. Hires and mixed fields preserve all 512 dots. The game selects 224 or 239 active lines. Interlace produces one full-height picture per field using the preceding field's opposite rows. On entering interlace the first field supplies both sets of rows. A geometry change starts a numbered sibling MKV in the same directory. Join the parts in numerical order after scaling them to a common size. The status bar reports the last part saved. An interrupted or failed write can leave an `.mkv.partial` file.

## Build

Enable `-DARES_ENABLE_FFV1=ON` with the `sfc` core selected. CMake uses pkg-config to find FFmpeg 6 or later with the FFV1 encoder, Matroska muxer and file protocol. The default build has no FFmpeg dependency.

For a self-contained macOS app, build shared FFmpeg libraries with relocatable install names. The local build uses the [FFmpeg 8.0.1 source release](https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz). Run the following from the FFmpeg source directory with `prefix` set to an absolute installation directory.

```sh
./configure --prefix="$prefix" --disable-all --disable-autodetect --disable-network \
  --disable-programs --disable-doc --enable-avcodec --enable-avformat \
  --enable-encoder=ffv1 --enable-muxer=matroska --enable-protocol=file \
  --enable-shared --disable-static --install-name-dir=@rpath
make -j && make install
```

Set the FFmpeg build's deployment target to match the app. From the ares checkout, configure and build with the same options used for your desktop build.

```sh
PKG_CONFIG_PATH="$prefix/lib/pkgconfig" cmake -S . -B build -DARES_ENABLE_FFV1=ON
cmake --build build --target desktop-ui
```

The macOS target embeds the three FFmpeg dylibs and their LGPL notice in the app. Other platforms use the FFmpeg libraries available to their runtime loader.

## Verify a cold boot

With Python 3, FFmpeg and ffprobe available, create the diagnostic cartridge by running `python3 tests/native-recording/check.py Boot.sfc`. Arm the recorder, load the cartridge, let it run for at least six frames with a 48 kHz mixer, and stop with the hotkey. Check the resulting movie from the ares source directory.

```sh
python3 tests/native-recording/check.py Boot.sfc --movie "path/to/Boot.mkv"
```

The checker verifies the red power-on picture, each subsequent RGB frame, field timestamps and the duration of the stereo PCM track. Use `--pal` in both commands for a PAL cartridge.

Add `--modes` to generation and verification for a cartridge that changes geometry. Record at least eight fields and pass the first MKV. The checker reads the numbered files in sequence and checks individual high-resolution dots, a mid-field mode change, overscan and interlaced row replacement.

If you have the headless reference frontend, add `--headless /path/to/ares-headless` to either verification command for a sample-by-sample audio comparison against a fresh boot. For this comparison, pass `--accurate` if pixel accuracy was enabled during capture.
