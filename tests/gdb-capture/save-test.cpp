#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace ares;
using namespace ares::SuperFamicom;
using nall::GDB::server;

// A bounded host catches synchronization loops without depending on CTest's timeout.
struct Host : ares::Platform {
  unsigned events = 0;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override {
    return std::make_shared<vfs::directory>();
  }
  auto event(Event) -> void override {
    if(++events % 1024 == 0 && std::chrono::steady_clock::now() > deadline) {
      std::cerr << "FAIL: save did not drain recording at a safe boundary\n";
      std::cerr << server.capture.status() << '\n' << server.capture.busTarget().status() << '\n';
      std::_Exit(EXIT_FAILURE);
    }
  }
};

auto command(const string& packet) -> string {
  bool reply = true;
  return server.processCommand(packet, reply);
}

auto hexText(const std::string& value) -> string {
  return ares_capture::Capture::hex(reinterpret_cast<const u8*>(value.data()), value.size()).c_str();
}

// Match the desktop's guarded handoff: serialization runs outside the emulation thread.
auto snapshot(Node::System root, Host& host) -> serializer {
  host.events = 0;
  host.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  assert(state);
  return state;
}

auto stopAtBoundary(Node::System root, Host& host) -> void {
  host.events = 0;
  host.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  for(unsigned i = 0; i < 128; ++i) {
    root->run();
    server.updateLoop();
    if(server.isHalted()) return;
  }
  assert(false && "debugger did not stop after saving");
}

// File completion must publish the final ACT2, with every produced event acknowledged.
auto finishFile() -> void {
  auto session = server.capture.sessionId();
  auto status = server.captureFile.command(server.capture, "stop:" + session, true);
  for(unsigned i = 0; status.find("\"fileState\":\"complete\"") == std::string::npos; ++i) {
    assert(i < 10000 && !server.capture.incomplete());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    status = server.captureFile.command(server.capture, "status:" + session, true);
  }
  assert(server.capture.complete() && server.capture.produced == server.capture.accepted);
}

auto nall::main(Arguments args) -> void {
  Host host;
  platform = &host;
  ppu.setAccurate(args.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  u8 rom[0x8000]{};
  // INX; BRA back. The first instruction also executes the pending maximum DMA.
  rom[0] = 0xe8; rom[1] = 0x80; rom[2] = 0xfd; rom[0x7ffd] = 0x80;
  bus.map([&](n24 address, n8) -> n8 { return rom[address & 0x7fff]; },
    [](n24, n8) {}, "00:8000-ffff");
  auto directory = std::filesystem::temp_directory_path() /
    ("ares-save-recording-" + std::to_string(reinterpret_cast<uintptr_t>(&host)));
  std::filesystem::create_directories(directory);
  string identity = {":", hexText(std::string(64, 'a')), ":", hexText("ares"), ":", hexText("save-test")};

  for(bool full : {true, false}) {
    server.capture = {};
    root->power(false);
    server.onConnect(); command("?"); stopAtBoundary(root, host);
    for(auto& channel : cpu.channels) {
      channel.dmaEnable = true; channel.hdmaEnable = false;
      channel.direction = false; channel.fixedTransfer = true;
      channel.transferMode = 0; channel.sourceBank = 0x7e;
      channel.sourceAddress = 0; channel.targetAddress = 0x22;
      channel.transferSize = 0; // The hardware interprets zero as 65536 bytes.
    }
    cpu.status.dmaPending = true; cpu.status.dmaActive = true;
    auto folder = directory / (full ? "full" : "bus");
    if(full) assert(command({"qUttoriContext:prepare:", hexText(folder.string()), identity}) == "prepared");
    auto file = folder / "recording.act2";
    std::filesystem::create_directories(folder);
    std::string scope = full ? "instructions" : "bus-ppu";
    auto start = "start:4718656:" + scope + ":" + std::string(hexText(file.string()).data()) +
      ":" + std::string(64, 'a') + ":61726573:736176652d74657374";
    assert(server.captureFile.command(server.capture, start, true)[0] == '{');
    auto session = server.capture.sessionId();
    if(full) assert(command({"qUttoriContext:start:", session.c_str()}) == "recording");
    auto& busCapture = server.capture.busTarget();
    auto busSession = busCapture.sessionId();
    command("s");
    // Withhold frontend service until the bus queue fills inside the instruction.
    // The save must service both writers itself while finishing the DMA coroutine.
    host.events = 0;
    for(unsigned i = 0; !busCapture.throttled; ++i) {
      assert(i < 128);
      root->run();
    }
    assert(server.capture.pendingInstruction());
    auto prefix = busCapture.eventCount();
    auto saved = snapshot(root, host);
    assert(server.capture.active() && busCapture.active());
    assert(server.capture.sessionId() == session && busCapture.sessionId() == busSession);
    assert(!server.capture.incomplete() && !busCapture.incomplete());
    assert(busCapture.eventCount() > prefix && busCapture.eventCount() >= 1048576);
    for(auto& channel : cpu.channels) assert(!channel.dmaEnable && channel.transferSize == 0);
    assert(!server.capture.pendingInstruction());
    stopAtBoundary(root, host);
    auto clock = cpu.captureClock;
    auto savedCycles = cpu.counter.cpu;
    auto registers = command("g");
    // Repeated saves while halted must neither advance the CPU nor stop recording.
    snapshot(root, host);
    snapshot(root, host);
    assert(clock == cpu.captureClock && registers == command("g"));
    command("s"); stopAtBoundary(root, host);
    assert(cpu.captureClock > clock && server.capture.active() && busCapture.active());
    if(full) {
      string status;
      do {
        status = command({"qUttoriContext:stop:", session.c_str()});
        assert(status == "draining" || status == "complete");
        server.updateLoop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while(status != "complete");
      assert(std::filesystem::exists(folder / "graphics.act2") && busCapture.complete());
    }
    finishFile();
    assert(std::filesystem::exists(file));
    assert(!std::filesystem::exists(file.string() + ".partial"));
    // Verify the saved state can be loaded after finalizing the recording.
    saved.setReading();
    std::thread([&] { assert(root->unserialize(saved)); }).join();
    assert(savedCycles == cpu.counter.cpu && registers == command("g"));
    auto restoredClock = cpu.captureClock;
    command("s"); stopAtBoundary(root, host);
    assert(cpu.captureClock > restoredClock);
    server.onDisconnect();
  }
  server.capture = {}; server.reset(); root->unload();
  std::filesystem::remove_all(directory);
  std::cout << "PASS: save during bus/full recording drains maximum DMA, preserves sessions and reloads\n";
}
