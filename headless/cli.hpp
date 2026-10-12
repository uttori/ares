#pragma once

#include "settings.hpp"
#include <nall/arguments.hpp>
#include <vector>

namespace headless {

// Press one named port-1 button during [begin, end), measured in primary video presentations from launch.
struct InputPress {
  u64 begin = 0;
  u64 end = 0;
  nall::string button;
};

// A PAR read override; physical RAM and ordinary CPU writes remain independently observable.
struct RamCheat { u32 address = 0; u8 value = 0; };

struct CliOptions {
  nall::string systemOverride;
  nall::string gamePath;
  nall::string audioDumpPath;
  double benchmarkDuration = 0.0;
  u64 benchmarkFrameTarget = 0;
  u64 runFramesTarget = 0;
  u32 stateSlot = 0;
  bool saveOnExit = false;
  u32 saveOnExitSlot = 1;
  u32 verbosity = 1;
  nall::string saveLastFramePath;
  bool audioChecksum = false;
  bool videoChecksum = false;
  bool showHelp = false;
  bool listSystemAliases = false;
  bool showVersion = false;
  LaunchSettings launchSettings;
  std::vector<InputPress> inputPresses;
  std::vector<RamCheat> ramCheats;
};

auto parseCliOptions(nall::Arguments& arguments, CliOptions& options, nall::string& error) -> bool;
auto printHeadlessUsage() -> void;

}
