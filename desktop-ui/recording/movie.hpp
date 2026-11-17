#pragma once

#include <filesystem>
#include <memory>
#include <vector>
#include <sfc/ppu/native-video.hpp>

namespace recording {

// One forward emulation interval. All calls are serialized by the frontend.
// Writes Matroska parts synchronously; an exception leaves the current partial file for recovery.
class Movie {
public:
  Movie(std::filesystem::path path, unsigned sampleRate);
  ~Movie();
  auto video(const ares::SuperFamicom::NativeVideoFrame& frame) -> void;
  auto audio(double left, double right) -> void;
  auto finish() -> void;
  auto sampleRate() const -> unsigned { return rate; }
  // UTF-8 for the status message. While writing, this identifies the recoverable partial file.
  auto path() const -> std::string;

private:
  struct Part;
  auto writeAudio(size_t frames) -> void;
  auto closePart() -> void;
  std::unique_ptr<Part> part;
  std::filesystem::path base, output;
  unsigned rate, number = 0;
  int64_t clocks = 0, partClock = 0, samples = 0, partSample = 0;
  std::vector<int16_t> pending;
};

}
