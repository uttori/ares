// The companion owns sparse visuals, serial controller evidence, a portable
// synchronized state, and an independent full-capture graphics stream. It never
// changes the instruction ACT2 dictionary or presents WRAM differences as writes.
namespace {
struct CaptureContext {
  std::filesystem::path directory;
  std::ofstream inputs, milestones;
  std::ifstream replayInputs;
  ares_capture::Capture graphics;
  ares_capture::FileCapture graphicsFile;
  std::string session, identity, startPosition, endPosition;
  bool prepared = false, recording = false, stopping = false, replaying = false;
  bool replayFailed = false;
  std::vector<u8> replayEndWram;
  u64 startClock = 0, endClock = 0, nextScreenshot = 0;
  u32 pictures = 0;

  auto path(const char* name) const -> std::string { return (directory / name).string(); }
  auto machine() const -> std::string {
    return std::string("{\"region\":\"") + (Region::PAL() ? "PAL" : "NTSC") +
      "\",\"accuratePpu\":" + (ppu.accurate ? "true" : "false") + "}";
  }
  auto frameClocks() const -> u64 { return 1364 * (Region::PAL() ? 312 : 262); }
  auto position() const -> std::string {
    auto& capture = nall::GDB::server.capture;
    return "{\"masterClock\":\"" + std::to_string(cpu.captureClock) +
      "\",\"relativeClock\":\"" + std::to_string(cpu.captureClock - startClock) +
      "\",\"instructionOrdinal\":\"" + std::to_string(capture.instructionCount()) +
      "\",\"eventThrough\":\"" + std::to_string(capture.eventCount()) +
      "\",\"pc\":" + std::to_string(u32(cpu.r.pc.d)) + "}";
  }
  static auto write(const std::string& file, const void* data, size_t size) -> void {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), size);
    out.close();
    if(!out) throw std::runtime_error("context-write");
  }
  auto writeText(const char* name, const std::string& text) -> void { write(path(name), text.data(), text.size()); }
  auto wram(const char* name) -> void {
    // Physical storage bypasses MMIO, aliases, trace hooks and all bus side effects.
    static_assert(sizeof(cpu.wram) == 131072);
    write(path(name), cpu.wram, sizeof(cpu.wram));
  }
  auto screenshot(const std::string& labelHex) -> void {
    u32 width = 0, height = 0;
    std::array<u64, 2> imageMarker{};
    auto pixels = ppu.screen()->capturePixels(width, height, imageMarker);
    std::string file;
    if(!pixels.empty()) {
      file = "screen-" + std::to_string(pictures) + ".png";
      if(!Encode::PNG::RGB8((directory / file).string().c_str(), pixels.data(), width * 4, width, height))
        throw std::runtime_error("context-screenshot");
    }
    // Before the first presented frame, retain an explicit unavailable milestone.
    // The screenshot is the last presented viewport, not an invented mid-scanline image.
    milestones << "{\"labelHex\":\"" << labelHex << "\",\"image\":\"" << file
      << "\",\"imageTiming\":\"last-presented-frame\",\"imageMasterClock\":\"" << imageMarker[0]
      << "\",\"imageInstructionOrdinal\":\"" << imageMarker[1] << "\",\"position\":" << position() << "}\n";
    milestones.flush();
    if(!milestones) throw std::runtime_error("context-milestone");
    ++pictures;
  }
  auto service() -> void {
    graphicsFile.service(graphics);
    if(recording && (graphics.incomplete() || nall::GDB::server.capture.incomplete())) {
      recording = false;
      nall::GDB::server.capture.fail("context-incomplete");
      inputs.close(); milestones.close();
    }
  }
  // Called before reportPC acknowledges a CPU boundary, including WAI/STP idle.
  // Graphics shares the primary stop boundary; its bounded writer applies
  // backpressure throughout the scene instead of imposing an early frame limit.
  auto boundary() -> bool {
    if(replaying) {
      if(cpu.captureClock >= endClock || replayFailed) {
        replaying = false;
        bool consumed = replayInputs.peek() == std::char_traits<char>::eof();
        replayInputs.close();
        replayFailed |= cpu.captureClock != endClock || !consumed ||
          !std::equal(replayEndWram.begin(), replayEndWram.end(), reinterpret_cast<const u8*>(cpu.wram));
        return true;
      }
    }
    if(!recording) return false;
    if(cpu.captureClock >= nextScreenshot && pictures < 25) {
      try { screenshot("6175746f"); }
      catch(...) { nall::GDB::server.capture.fail("context-screenshot"); return true; }
      nextScreenshot = cpu.captureClock + 300 * frameClocks();
    }
    return false;
  }
  auto input(u8 port, u8 value) -> u8 {
    if(!recording && !replaying) return value;
    // Fixed 18-byte little-endian rows: absolute master clock, instruction
    // ordinal, port (0/1), and the two bits actually consumed by the console.
    std::array<u8, 18> row{};
    if(replaying) {
      replayInputs.read(reinterpret_cast<char*>(row.data()), row.size());
      u64 clock = 0;
      for(unsigned i = 0; i < 8; ++i) clock |= u64(row[i]) << (i * 8);
      if(!replayInputs || clock != cpu.captureClock || row[16] != port || row[17] > 3) {
        replayFailed = true;
        return 0;
      }
      return row[17];
    }
    u64 ordinal = nall::GDB::server.capture.instructionCount();
    for(unsigned i = 0; i < 8; ++i) {
      row[i] = cpu.captureClock >> (i * 8); row[i + 8] = ordinal >> (i * 8);
    }
    row[16] = port; row[17] = value;
    inputs.write(reinterpret_cast<char*>(row.data()), row.size());
    if(!inputs) nall::GDB::server.capture.fail("context-input-write");
    return value;
  }
  auto abort() -> void {
    nall::GDB::server.capture.graphics = nullptr;
    if(!graphics.sessionId().empty() && graphicsFile.owns())
      graphicsFile.command(graphics, "abort:" + graphics.sessionId(), true);
    inputs.close(); milestones.close(); replayInputs.close();
    if(ppu.implementation && ppu.screen()) ppu.screen()->setCaptureMarker({});
    prepared = recording = stopping = replaying = false;
  }
};
CaptureContext captureContext;

auto contextDecode(const string& value) -> std::string {
  if(value.size() % 2 || value.size() > 8192) throw std::runtime_error("context-text");
  std::string decoded;
  for(u32 i = 0; i < value.size(); i += 2) {
    auto nibble = [](char c) -> unsigned {
      if(c >= '0' && c <= '9') return c - '0';
      if(c >= 'a' && c <= 'f') return c - 'a' + 10;
      throw std::runtime_error("context-hex");
    };
    char byte = nibble(value[i]) * 16 + nibble(value[i + 1]);
    if(!byte) throw std::runtime_error("context-null");
    decoded.push_back(byte);
  }
  return decoded;
}
auto contextHex(const std::string& text) -> std::string {
  std::string result;
  for(unsigned char c : text) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
  return result;
}
auto captureContextCommand(const string& request) -> string {
  auto fields = nall::split(request, ":");
  auto& context = captureContext;
  auto& capture = nall::GDB::server.capture;
  std::string state;
  try {
    if(fields.size() == 1 && fields[0] == "abort") { context.abort(); state = "aborted"; }
    else if(fields.size() == 5 && (fields[0] == "prepare" || fields[0] == "replay" || fields[0] == "restore")) {
      if(capture.active() || capture.pendingInstruction() || context.recording || context.replaying || context.graphicsFile.owns()) return "E09";
      context.abort();
      context.graphics = {};
      for(auto* port : {&controllerPort1, &controllerPort2}) {
        if(port->device && port->device->node->name() != "Gamepad" && port->device->node->name() != "Rumble Gamepad") return "E11";
      }
      context.directory = std::filesystem::u8path(contextDecode(fields[1]));
      if(!context.directory.is_absolute()) return "E02";
      context.identity = contextDecode(fields[2]) + "\n" + contextDecode(fields[3]) + "\n" + contextDecode(fields[4]) + "\n";
      if(fields[0] == "prepare") {
        if(!std::filesystem::create_directory(context.directory)) return "E03";
        // Synchronize BEFORE arming either stream. The CPU remains at its stopped
        // instruction; snapshots and trace start describe the resulting safe state.
        auto state = system.serialize(true);
        if(!state.size()) throw std::runtime_error("context-state");
        CaptureContext::write(context.path("start.bst"), state.data(), state.size());
        context.writeText("identity.txt", context.identity);
        context.writeText("machine.json", context.machine());
        context.startClock = cpu.captureClock;
        context.writeText("start-clock.txt", std::to_string(context.startClock));
        context.wram("wram-start.bin");
        context.prepared = true;
        return "prepared";
      }
      std::ifstream identityFile(context.path("identity.txt"), std::ios::binary);
      std::string actual((std::istreambuf_iterator<char>(identityFile)), {});
      if(actual != context.identity) return "E04";
      std::ifstream machineFile(context.path("machine.json"), std::ios::binary);
      std::string machine((std::istreambuf_iterator<char>(machineFile)), {});
      if(machine != context.machine()) return "E04";
      auto stateSize = std::filesystem::file_size(context.path("start.bst"));
      if(stateSize < 538 || stateSize > 32 * 1024 * 1024) return "E05";
      auto bytes = file::read(context.path("start.bst").c_str());
      if(bytes.size() != stateSize) return "E05";
      std::ifstream start(context.path("start-clock.txt"));
      if(!(start >> context.startClock)) return "E07";
      std::ifstream end(context.path("end-clock.txt"));
      if(!(end >> context.endClock) || context.endClock < context.startClock) return "E07";
      if(std::filesystem::file_size(context.path("wram-end.bin")) != 131072) return "E07";
      auto endWram = file::read(context.path("wram-end.bin").c_str());
      context.replayEndWram.assign(endWram.begin(), endWram.end());
      serializer saved(bytes.data(), bytes.size());
      if(!system.unserialize(saved)) return "E06";
      // Capture clocks are debugger metadata, not part of ares's state schema.
      // Restore the retained epoch without changing the emulator save format.
      cpu.captureClock = context.startClock;
      if(fields[0] == "restore") return "restored";
      context.replayInputs.open(context.path("inputs.bin"), std::ios::binary);
      if(!context.replayInputs) return "E08";
      context.replaying = true; context.replayFailed = false;
      state = "replaying";
    } else if(fields.size() == 2 && fields[0] == "start") {
      if(!context.prepared || !capture.active() || capture.eventCount() || fields[1] != capture.sessionId().c_str() || cpu.captureClock != context.startClock) return "E09";
      context.session = capture.sessionId();
      context.startPosition = context.position();
      context.inputs.open(context.path("inputs.bin"), std::ios::binary);
      context.milestones.open(context.path("milestones.jsonl"));
      if(!context.inputs || !context.milestones) throw std::runtime_error("context-open");
      ppu.screen()->setCaptureMarker([] { return std::array<u64, 2>{cpu.captureClock, nall::GDB::server.capture.instructionCount()}; });
      context.pictures = 0;
      context.screenshot("7374617274");
      context.nextScreenshot = cpu.captureClock + 300 * context.frameClocks();
      if(!capture.busScope()) {
        auto clocks = capture.clockRatio(); context.graphics.clockRate(clocks.first, clocks.second);
        auto identity = nall::split(context.identity.c_str(), "\n");
        auto response = context.graphicsFile.command(context.graphics,
          "start:8388608:bus-ppu:" + contextHex(context.path("graphics.act2")) + ":" + std::string(identity[0].data()) +
          ":" + contextHex(identity[1].data()) + ":" + contextHex(identity[2].data()), true);
        if(response.empty() || response[0] != '{') throw std::runtime_error("context-graphics");
        capture.graphics = &context.graphics;
      }
      context.recording = true; context.prepared = false;
      state = "recording";
    } else if(fields.size() == 3 && fields[0] == "mark") {
      if(!context.recording || fields[1] != context.session.c_str()) return "E09";
      contextDecode(fields[2]);
      context.screenshot(fields[2].data()); state = "recording";
    } else if(fields.size() == 2 && fields[0] == "stop") {
      if((!context.recording && !context.stopping) || fields[1] != context.session.c_str()) return "E09";
      if(context.recording) {
        context.endClock = cpu.captureClock;
        context.endPosition = context.position();
        context.wram("wram-end.bin"); context.screenshot("656e64");
        context.inputs.close(); context.milestones.close();
        if(!context.inputs || !context.milestones) throw std::runtime_error("context-close");
        context.writeText("end-clock.txt", std::to_string(context.endClock));
        if(context.graphics.active()) context.graphicsFile.command(context.graphics, "stop:" + context.graphics.sessionId(), true);
        context.recording = false; context.stopping = true;
      }
      context.service();
      if(context.graphics.incomplete()) throw std::runtime_error("context-graphics-incomplete");
      if(context.graphicsFile.owns()) return "draining";
      context.writeText("boundaries.json", "{\"session\":\"" + context.session + "\",\"start\":" + context.startPosition +
        ",\"end\":" + context.endPosition + ",\"graphicsSession\":\"" + context.graphics.sessionId() +
        "\",\"graphicsWindowFrames\":null}\n");
      capture.graphics = nullptr;
      ppu.screen()->setCaptureMarker({});
      context.stopping = false;
      state = "complete";
    } else if(fields.size() == 1 && fields[0] == "status") {
      if(context.replayFailed) return "replay-diverged";
      state = context.replaying ? "replaying" : "idle";
    } else return "E01";
  } catch(...) {
    capture.fail("context-io-or-state"); context.abort(); return "E10";
  }
  return state.c_str();
}
}

auto captureControllerInput(u8 port, u8 value) -> u8 { return captureContext.input(port, value); }
