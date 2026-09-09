#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
// nall defines NDEBUG from BUILD_RELEASE even when the compiler uses -UNDEBUG.
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
  const char* operation = "attach";

  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override {
    return std::make_shared<vfs::directory>();
  }

  auto event(Event event) -> void override {
    // Fail deterministically if serialization keeps yielding the same GDB stop.
    if(event == Event::Step && ++steps > 8) {
      std::cerr << "FAIL: GDB pause loop blocked " << operation << '\n';
      std::_Exit(EXIT_FAILURE);
    }
  }

  auto begin(const char* name) -> void {
    operation = name;
    steps = 0;
  }
};

auto command(const string& packet) -> string {
  bool reply = true;
  return server.processCommand(packet, reply);
}

auto runToStop(TestPlatform& host) -> void {
  host.begin("run to stop");
  for(unsigned frames = 0; frames < 4; ++frames) {
    SuperFamicom::system.run();
    if(server.isHalted()) return;
  }
  assert(false && "core did not stop");
}

auto snapshot(Node::System root, TestPlatform& host) -> serializer {
  host.begin("synchronized save/undo snapshot");
  auto pc = cpu.r.pc.d;
  auto clocks = cpu.counter.cpu;
  auto registers = command("g");
  server.sendBuffer.clear();
  // The desktop transfers control from its guarded worker to the UI thread.
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  assert(state && server.isHalted());
  assert(cpu.r.pc.d == pc && cpu.counter.cpu == clocks);
  assert(command("g") == registers && server.sendBuffer.empty());
  return state;
}

auto restore(Node::System root, serializer& state) -> void {
  state.setReading();
  std::thread([&] { assert(root->unserialize(state)); }).join();
  assert(server.isHalted() && server.sendBuffer.empty());
  assert(command("?") == "T05");
}

auto nall::main(Arguments arguments) -> void {
  TestPlatform host;
  platform = &host;
  ppu.setAccurate(arguments.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));

  // Tiny in-memory program: INX; INX; BRA back. No ROM or firmware files.
  u8 rom[0x8000]{};
  rom[0] = 0xe8; rom[1] = 0xe8; rom[2] = 0x80; rom[3] = 0xfc;
  rom[0x7ffd] = 0x80;
  bool wai = arguments.find("wai"), stp = arguments.find("stp");
  if(wai || stp) rom[0] = wai ? 0xcb : 0xdb;
  bus.map([&](n24 address, n8) -> n8 { return rom[address & 0x7fff]; },
    [](n24, n8) {}, "00:8000-ffff");
  root->power(false);
  server.onConnect();
  command("?");
  runToStop(host);
  assert(cpu.r.pc.d == 0x8000);
  if(wai || stp) {
    command("s");
    runToStop(host);
    assert(cpu.r.pc.d == 0x8001 && cpu.r.wai == wai && cpu.r.stp == stp);
  }

  cpu.wram[0] = 0x12;
  auto savedPC = cpu.r.pc.d;
  auto savedX = cpu.r.x.w;
  auto saved = snapshot(root, host);
  // Repeated save must work even when the scheduler last synchronized an auxiliary thread.
  snapshot(root, host);
  if(!wai && !stp) {
    command("s");
    runToStop(host);
    assert(cpu.r.pc.d == 0x8001 && cpu.r.x.w == savedX + 1);
  }
  cpu.wram[0] = 0x34;
  auto undoPC = cpu.r.pc.d;
  auto undoX = cpu.r.x.w;
  auto undo = snapshot(root, host); // Program::stateLoad saves this before loading.
  assert(command({"Z0,", hex(savedPC), ",1"}) == "OK");
  restore(root, saved);
  assert(cpu.r.pc.d == savedPC && cpu.r.x.w == savedX && cpu.wram[0] == 0x12);
  assert(cpu.r.wai == wai && cpu.r.stp == stp);
  snapshot(root, host);
  restore(root, undo); // Program::undoStateLoad uses the same unserialize callback.
  assert(cpu.r.pc.d == undoPC && cpu.r.x.w == undoX && cpu.wram[0] == 0x34);
  restore(root, saved);

  command("s");
  runToStop(host);
  if(wai || stp) {
    assert(cpu.r.pc.d == savedPC && cpu.r.x.w == savedX);
    assert(cpu.r.wai == wai && cpu.r.stp == stp);
  } else {
    // The restored breakpoint is skipped once, so one INX actually executes.
    assert(cpu.r.pc.d == savedPC + 1 && cpu.r.x.w == savedX + 1);
    command("c");
    runToStop(host);
    assert(cpu.r.pc.d == savedPC); // breakpoint still fires on the next loop
  }

  server.onDisconnect();
  server.reset();
  root->unload();
  std::cout << "PASS: paused save/load/undo, stable registers and CPU clocks, restored breakpoint and step\n";
}
