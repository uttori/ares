#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <thread>
using namespace ares;
using namespace ares::SuperFamicom;
using nall::GDB::server;

struct Host : ares::Platform {
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override { return std::make_shared<vfs::directory>(); }
};
auto command(const string& packet) -> string { bool reply = true; return server.processCommand(packet, reply); }
auto stopAtBoundary(Node::System root) -> void {
  for(unsigned i = 0; i < 64; ++i) { root->run(); if(server.isHalted()) return; }
  assert(false && "GDB stop did not reach a boundary");
}
auto drain() -> void {
  auto& c = server.capture;
  while(c.size) {
    auto& front = c.queue[c.head];
    auto chunk = c.command("read:" + c.sessionId() + ":" + std::to_string(front.sequence));
    assert(chunk.size() > 128);
    auto through = front.first + front.count - 1;
    assert(c.command("ack:" + c.sessionId() + ":" + std::to_string(front.sequence) + ":" + std::to_string(through))[0] == '{');
  }
}
auto snapshot(Node::System root) -> std::vector<u8> {
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  return {state.data(), state.data() + state.size()};
}
auto nall::main(Arguments args) -> void {
  Host host; platform = &host;
  ppu.setAccurate(args.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  u8 rom[0x8000]{};
  // INX; STX $0000; BRA back. Actual reads/fetches, no trace peeks.
  rom[0] = 0xe8; rom[1] = 0x86; rom[2] = 0; rom[3] = 0x80; rom[4] = 0xfb; rom[0x7ffd] = 0x80;
  bus.map([&](n24 address, n8) -> n8 { return rom[address & 0x7fff]; }, [](n24, n8) {}, "00:8000-ffff");
  auto boot = [&] {
    server.capture = {};
    root->power(false); server.onConnect(); command("?"); stopAtBoundary(root);
  };
  boot();
  assert(command("qSupported").find("uttoriCaptureV2+"));
  auto initial = snapshot(root);
  for(unsigned i = 0; i < 4096; ++i) { command("s"); stopAtBoundary(root); }
  auto reference = snapshot(root);
  serializer restored(initial.data(), initial.size());
  restored.setReading();
  std::thread([&] { assert(root->unserialize(restored)); }).join();
  assert(command("qUttoriCapture:start:4718656").beginsWith("J"));
  assert(server.capture.produced == 0);
  for(unsigned i = 0; i < 4096; ++i) {
    drain(); command("s"); stopAtBoundary(root);
  }
  server.capture.command("stop:" + server.capture.sessionId()); drain();
  assert(server.capture.produced == 4096 && server.capture.state == "complete");
  assert(snapshot(root) == reference && "capture changes serialized machine state");

  // Bus chunks can be acknowledged while an instruction is pending. Instruction
  // ordinals associate accesses without retaining an unbounded DMA instruction.
  serializer busInitial(initial.data(), initial.size());
  busInitial.setReading();
  std::thread([&] { assert(root->unserialize(busInitial)); }).join();
  assert(command("qUttoriCapture:start:4718656:bus").beginsWith("J"));
  for(unsigned i = 0; i < 4096; ++i) {
    drain(); command("s"); stopAtBoundary(root);
    assert(server.capture.size == 1);
    auto& tail = server.capture.queue[server.capture.head];
    for(u32 offset = 64; offset < tail.used; offset += 64) {
      auto* record = tail.bytes.data() + offset;
      uint64_t ordinal = 0;
      for(unsigned b = 0; b < 8; ++b) ordinal |= uint64_t(record[32 + b]) << (b * 8);
      assert(record[0] == 2 && ordinal == i + 1);
      assert(record[54] == 0 && record[55] == 255 && record[57] == 1);
    }
  }
  server.capture.command("stop:" + server.capture.sessionId()); drain();
  assert(server.capture.produced > 4096 && server.capture.state == "complete");
  assert(snapshot(root) == reference && "bus capture changes serialized machine state");

  // Hardware interrupt entry is outside the preceding instruction: its stack
  // and vector bus accesses need explicit context and an unavailable ordinal.
  for(bool nmi : {false, true}) {
    boot();
    cpu.status.interruptPending = true;
    cpu.status.nmiPending = nmi; cpu.status.irqPending = !nmi;
    server.capture.command("start:4718656:bus");
    command("s"); stopAtBoundary(root);
    unsigned interruptAccesses = 0;
    auto& tail = server.capture.queue[server.capture.head];
    for(u32 offset = 64; offset < tail.used; offset += 64) {
      auto* record = tail.bytes.data() + offset;
      if(record[57] != 2) continue;
      ++interruptAccesses;
      for(unsigned b = 32; b < 40; ++b) assert(record[b] == 0);
      assert(record[54] == 0 && record[55] == 255);
    }
    assert(interruptAccesses >= 5);
    server.capture.command("stop:" + server.capture.sessionId()); drain();
    assert(server.capture.state == "complete");
  }

  // Continuous execution fills tiny storage. Frontend calls keep returning,
  // and no clock or register advances while the consumer stalls.
  boot();
  server.capture.command("start:4718656"); command("c");
  for(unsigned i = 0; i < 8 && !server.capture.throttled; ++i) root->run();
  assert(server.capture.throttled);
  auto clock = cpu.captureClock; auto registers = command("g");
  auto produced = server.capture.produced;
  for(unsigned i = 0; i < 20; ++i) { root->run(); assert(cpu.captureClock == clock && command("g") == registers); }
  command("?"); stopAtBoundary(root); drain();
  assert(server.isHalted());
  for(unsigned i = 0; i < 3; ++i) { root->run(); assert(cpu.captureClock == clock); }
  command("s"); stopAtBoundary(root);
  assert(server.capture.produced == produced + 1);
  server.capture.command("stop:" + server.capture.sessionId()); drain();
  assert(server.capture.state == "complete");

  // Rewritten RAM instruction bytes at the same address remain two observations.
  server.capture.command("start:4718656");
  cpu.r.pc.d = 0x7e0100; cpu.wram[0x100] = 0xe8;
  command("s"); stopAtBoundary(root);
  cpu.r.pc.d = 0x7e0100; cpu.wram[0x100] = 0xca;
  command("s"); stopAtBoundary(root);
  assert(server.capture.produced == 2);
  auto& chunk = server.capture.queue[server.capture.head];
  assert(chunk.bytes[64 + 51] == 0xe8 && chunk.bytes[64 + 68 + 51] == 0xca);
  server.capture.command("stop:" + server.capture.sessionId()); drain();

  // WAI and STP are committed once at entry, and debugger control stays live.
  for(u8 opcode : {u8(0xcb), u8(0xdb)}) {
    boot(); rom[0] = opcode;
    server.capture.command("start:4718656"); command("s"); stopAtBoundary(root);
    assert(server.capture.produced == 1 && !server.capture.pendingInstruction());
    server.capture.command("stop:" + server.capture.sessionId()); drain();
    assert(server.capture.state == "complete");
  }
  // All eight DMA channels at their maximum transfer length still consume one
  // bounded instruction record. Frame exits cannot seal its reserved tail.
  boot(); rom[0] = 0xe8;
  for(auto& channel : cpu.channels) {
    channel.dmaEnable = true; channel.hdmaEnable = false;
    channel.direction = false; channel.fixedTransfer = true;
    channel.transferMode = 0; channel.sourceBank = 0x7e;
    channel.sourceAddress = 0; channel.targetAddress = 0x22;
    channel.transferSize = 0; // 65536 bytes
  }
  cpu.status.dmaPending = true; cpu.status.dmaActive = true;
  server.capture.command("start:4718656"); command("s");
  bool sawPending = false;
  for(unsigned i = 0; i < 64; ++i) {
    root->run();
    if(server.capture.pendingInstruction()) {
      sawPending = true;
      assert(server.capture.command("read:" + server.capture.sessionId() + ":1").empty());
    }
    if(server.isHalted()) break;
  }
  assert(server.isHalted() && sawPending && server.capture.produced == 1);
  for(auto& channel : cpu.channels) assert(!channel.dmaEnable && channel.transferSize == 0);
  server.capture.command("stop:" + server.capture.sessionId()); drain();
  assert(server.capture.state == "complete");

  // A real CPU instruction can stall inside maximum DMA. Its bus prefix must
  // drain before the instruction completes; saving there must not deadlock the
  // guarded UI handoff or clear the independent debugger stop.
  boot(); rom[0] = 0xe8;
  for(auto& channel : cpu.channels) {
    channel.dmaEnable = true; channel.hdmaEnable = false;
    channel.direction = false; channel.fixedTransfer = true;
    channel.transferMode = 0; channel.sourceBank = 0x7e;
    channel.sourceAddress = 0; channel.targetAddress = 0x22; channel.transferSize = 0;
  }
  cpu.status.dmaPending = true; cpu.status.dmaActive = true;
  server.capture.command("start:4718656:bus"); command("s");
  for(unsigned i = 0; !server.capture.throttled; ++i) { assert(i < 64); root->run(); }
  assert(server.capture.pendingInstruction());
  assert(!server.capture.command("read:" + server.capture.sessionId() + ":1").empty());
  auto busPrefix = server.capture.produced;
  assert(!snapshot(root).empty());
  assert(server.capture.state == "incomplete" && server.capture.error == "state-save");
  assert(server.capture.produced == busPrefix);
  stopAtBoundary(root); drain();
  server.capture.command("release:" + server.capture.sessionId());
  auto savedClock = cpu.captureClock;
  for(unsigned i = 0; i < 3; ++i) { root->run(); assert(server.isHalted() && cpu.captureClock == savedClock); }
  command("s"); stopAtBoundary(root);
  assert(cpu.captureClock > savedClock);

  // Disconnect and reset terminate evidence instead of silently resuming it.
  boot(); rom[0] = 0xe8;
  server.capture.command("start:4718656"); server.onDisconnect();
  assert(server.capture.state == "incomplete" && !server.capture.ready());
  server.capture.command("release:" + server.capture.sessionId());
  assert(server.capture.ready());
  server.capture.command("start:4718656"); root->power(false);
  assert(server.capture.state == "incomplete" && !server.capture.ready());
  server.capture = {}; server.reset(); root->unload();
  std::cout << "PASS: machine-state equivalence, slow consumer, independent pause/step, RAM variants, WAI/STP, maximum eight-channel DMA, disconnect/reset\n";
}
