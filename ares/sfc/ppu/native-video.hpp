#pragma once

#include <cstdint>

namespace ares::SuperFamicom {

// One completed active field, borrowed synchronously before the Screen swaps its buffers.
// Pixels contain SNES BGR555 and four brightness bits. Strides are measured in uint32_t samples.
struct NativeVideoFrame {
  const uint32_t* pixels;
  unsigned pitch, step, width, lines;
  bool interlace, odd, pal;
  unsigned clocks;  // Master clocks since the previous field boundary, or since power-on.
  double frequency;
};

// Only the emulation thread (or a frontend holding its execution guard) accesses this observer.
struct NativeVideoCapture {
  void (*receive)(const NativeVideoFrame&) = nullptr;
  void (*discontinuity)() = nullptr;
  unsigned clocks = 0;
  bool hires = false;

  auto present(const uint32_t* canvas, bool pal, bool overscan, bool interlace, bool odd, double frequency) -> void {
    unsigned pitch = interlace ? 1128 : 564;
    unsigned top = 1 + (overscan ? 0 : 8) + (pal ? 20 : 0);
    auto pixels = canvas + top * pitch + (pal ? 22 : 26);
    if(interlace && odd) pixels += 564;
    receive({pixels, pitch, hires ? 1u : 2u, hires ? 512u : 256u, overscan ? 239u : 224u,
             interlace, odd, pal, clocks, frequency});
    clocks = 0;
    hires = false;
  }
};

inline NativeVideoCapture nativeVideo;

}
