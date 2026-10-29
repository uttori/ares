#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
using namespace ares;
using namespace ares::SuperFamicom;
using nall::GDB::server;

struct Host : ares::Platform {
  unsigned polls = 0;
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override { return std::make_shared<vfs::directory>(); }
  auto input(Node::Input::Input input) -> void override {
    if(auto button = input->cast<Node::Input::Button>()) {
      if(input->name() == "Reset") { button->setValue(false); return; }
      button->setValue((++polls / 7) & 1);
    }
  }
};
auto command(const string& packet) -> string { bool reply = true; return server.processCommand(packet, reply); }
auto hexText(const std::string& value) -> string { string out; for(unsigned char c : value) out.append(hex(c, 2, '0')); return out; }
auto stopAtBoundary(Node::System root) -> void {
  for(unsigned i = 0; i < 10000; ++i) {
    root->run(); server.updateLoop(); if(server.isHalted()) return;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  std::cerr << server.capture.status() << "\n";
  if(server.capture.graphics) std::cerr << server.capture.graphics->status() << "\n";
  assert(false && "no stopped boundary");
}
auto drain() -> void {
  auto& c = server.capture;
  while(c.size) {
    auto& front = c.queue[c.head];
    c.command("read:" + c.sessionId() + ":" + std::to_string(front.sequence));
    auto through = front.first + front.count - 1;
    c.command("ack:" + c.sessionId() + ":" + std::to_string(front.sequence) + ":" + std::to_string(through));
  }
}
auto nall::main(Arguments args) -> void {
  Host host; platform = &host;
  ppu.setAccurate(args.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root; assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  controllerPort1.allocate("Gamepad"); controllerPort2.allocate("Gamepad");
  // Read serial controller bits into WRAM, increment a counter, and repeat.
  // Host button values differ during replay; only retained console inputs matter.
  u8 rom[0x8000]{};
  const u8 program[] = {0xa9,1,0x8d,0x16,0x40,0xa9,0,0x8d,0x16,0x40,0xad,0x16,0x40,0x85,0,0xe6,1,0x80,0xed};
  std::copy(std::begin(program), std::end(program), rom); rom[0x7ffd] = 0x80;
  bus.map([&](n24 address, n8) -> n8 { return rom[address & 0x7fff]; }, [](n24, n8) {}, "00:8000-ffff");
  root->power(false); server.onConnect(); command("?"); stopAtBoundary(root);
  auto directory = std::filesystem::temp_directory_path() / ("ares-context-" + server.capture.hex(reinterpret_cast<const u8*>(&host), sizeof(void*)) + (ppu.accurate ? "-accurate" : "-fast"));
  std::filesystem::remove_all(directory);
  string identity = {":", hexText(std::string(64, 'a')), ":", hexText("ares"), ":", hexText("test-build")};
  auto prepare = command({"qUttoriContext:prepare:", hexText(directory.string()), identity});
  assert(prepare == "prepared");
  assert(std::filesystem::file_size(directory / "wram-start.bin") == 131072);
  assert(command("qUttoriCapture:start:8388608").beginsWith("J"));
  auto session = server.capture.sessionId();
  assert(command({"qUttoriContext:start:", session.c_str()}) == "recording");
  auto startClock = cpu.captureClock;
  for(unsigned i = 0; i < 45000; ++i) { drain(); command("s"); stopAtBoundary(root); }
  // Regress the original two-frame cutoff using real CPU execution. The second
  // window must still produce graphics events and share the primary ordinal.
  assert(cpu.captureClock - startClock > 2 * 1364 * 262);
  auto* graphics = server.capture.graphics;
  assert(graphics && graphics->active());
  auto graphicsEvents = graphics->eventCount();
  for(unsigned i = 0; i < 1000; ++i) { drain(); command("s"); stopAtBoundary(root); }
  assert(graphics->eventCount() > graphicsEvents);
  assert(graphics->instructionCount() == server.capture.instructionCount());
  assert(graphics->allocatedBytes() <= 8388608);
  auto clock = cpu.captureClock; auto pc = cpu.r.pc.d;
  std::vector<u8> expected(reinterpret_cast<u8*>(cpu.wram), reinterpret_cast<u8*>(cpu.wram) + sizeof(cpu.wram));
  string status;
  do {
    status = command({"qUttoriContext:stop:", session.c_str()});
    assert(status == "draining" || status == "complete");
    server.updateLoop(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(status != "complete");
  assert(graphics->complete() && !graphics->incomplete());
  std::ifstream boundariesFile(directory / "boundaries.json");
  std::string boundaries((std::istreambuf_iterator<char>(boundariesFile)), {});
  assert(boundaries.find("\"graphicsWindowFrames\":null") != std::string::npos);
  assert(cpu.captureClock == clock);
  server.capture.command("stop:" + session); drain();
  assert(server.capture.complete());
  assert(std::filesystem::file_size(directory / "wram-end.bin") == 131072);
  assert(std::filesystem::file_size(directory / "inputs.bin") > 0);
  assert(std::filesystem::exists(directory / "graphics.act2"));
  assert(command({"qUttoriContext:replay:", hexText(directory.string()), identity}) == "replaying");
  command("c");
  for(unsigned i = 0; i < 64 && !server.isHalted(); ++i) { root->run(); server.updateLoop(); }
  assert(server.isHalted());
  assert(command("qUttoriContext:status") == "idle");
  assert(cpu.captureClock == clock && cpu.r.pc.d == pc);
  assert(std::equal(expected.begin(), expected.end(), reinterpret_cast<u8*>(cpu.wram)));
  assert(command({"qUttoriContext:restore:", hexText(directory.string()), identity, "00"}).beginsWith("E"));
  // The last viewport can outlive a capture. A new epoch keeps its pixels but
  // clears the old trace ordinal, including a frame pending on the video worker.
  auto screen = ppu.screen();
  screen->setCaptureMarker([] { return std::array<u64, 2>{1234, 99}; });
  screen->frame(); screen->refresh();
  u32 width = 0, height = 0;
  std::array<u64, 2> marker{};
  screen->capturePixels(width, height, marker);
  assert(marker[0] == 1234 && marker[1] == 99);
  screen->setCaptureMarker([] { return std::array<u64, 2>{5678, 1}; });
  screen->capturePixels(width, height, marker);
  assert(marker[0] == 0 && marker[1] == 0);
  root->unload(); server.onDisconnect();
  std::filesystem::remove_all(directory);
  std::cout << "context snapshots, independent graphics and controller replay passed\n";
}
