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

struct TestPlatform : ares::Platform {
  unsigned steps = 0;
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override {
    return std::make_shared<vfs::directory>();
  }
  auto event(Event event) -> void override {
    if(event == Event::Step && ++steps > 8) {
      std::cerr << "FAIL: paused serialization cannot reach its safe point\n";
      std::_Exit(EXIT_FAILURE);
    }
  }
};

auto command(const string& packet) -> string {
  bool reply = true;
  return server.processCommand(packet, reply);
}

auto runToStop(Node::System root, TestPlatform& host) -> void {
  host.steps = 0;
  for(unsigned frames = 0; frames < 4; ++frames) {
    root->run();
    if(server.isHalted()) return;
  }
  assert(false && "core did not stop");
}

auto snapshot(Node::System root, TestPlatform& host) -> serializer {
  host.steps = 0;
  auto registers = command("g");
  auto clocks = cpu.counter.cpu;
  auto events = server.instructionTraceNextSequence;
  server.sendBuffer.clear();
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  assert(state && server.isHalted() && server.hasActiveClient);
  assert(command("g") == registers && cpu.counter.cpu == clocks);
  assert(server.sendBuffer.empty() && server.instructionTraceNextSequence == events);
  assert(server.detailedTraceActive());
  return state;
}

auto restore(Node::System root, serializer& state) -> void {
  state.setReading();
  std::thread([&] { assert(root->unserialize(state)); }).join();
  assert(server.isHalted() && server.hasActiveClient && server.sendBuffer.empty());
  assert(server.detailedTraceActive());
}

auto nall::main(Arguments arguments) -> void {
  TestPlatform host;
  platform = &host;
  ppu.setAccurate(arguments.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  // INX; INX; BRA back. In-memory bus fixture; no ROM or firmware files.
  u8 rom[0x8000]{};
  rom[0] = 0xe8; rom[1] = 0xe8; rom[2] = 0x80; rom[3] = 0xfc;
  rom[0x7ffd] = 0x80;
  bus.map([&](n24 address, n8) -> n8 { return rom[address & 0x7fff]; },
    [](n24, n8) {}, "00:8000-ffff");
  root->power(false);
  server.onConnect();
  command("?");
  runToStop(root, host);
  assert(cpu.r.pc.d == 0x8000);
  assert(command("QUttoriTrace:2") == "OK");
  auto firstSequence = server.instructionTraceNextSequence;
  cpu.wram[0] = 0x12;
  auto initialX = cpu.r.x.w;
  auto saved = snapshot(root, host);
  snapshot(root, host);
  command("s");
  runToStop(root, host);
  assert(cpu.r.pc.d == 0x8001 && cpu.r.x.w == initialX + 1);
  cpu.wram[0] = 0x34;
  auto undo = snapshot(root, host);
  restore(root, saved);
  assert(cpu.r.pc.d == 0x8000 && cpu.r.x.w == initialX && cpu.wram[0] == 0x12);
  restore(root, undo);
  assert(cpu.r.pc.d == 0x8001 && cpu.r.x.w == initialX + 1 && cpu.wram[0] == 0x34);
  restore(root, saved);
  command("s");
  runToStop(root, host);
  assert(cpu.r.pc.d == 0x8001 && cpu.r.x.w == initialX + 1);
  assert(command("Z0,8002,1") == "OK");
  command("c");
  runToStop(root, host);
  assert(cpu.r.pc.d == 0x8002 && cpu.r.x.w == initialX + 2);
  assert(server.instructionTraceNextSequence == firstSequence + 3);
  server.onDisconnect();
  server.reset();
  root->unload();
  std::cout << "PASS: save/load/undo preserve pause, registers, clocks, rich trace, step and continue\n";
}
