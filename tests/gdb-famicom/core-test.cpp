#include <fc/fc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <thread>
#include <fstream>
using namespace ares;
using namespace ares::Famicom;
using nall::GDB::server;

// Synthetic MMC3 cartridge; tests have no dependency on private game files.
struct Host : ares::Platform {
  std::shared_ptr<vfs::directory> cartridgePak = std::make_shared<vfs::directory>();
  unsigned polls = 0;
  auto input(Node::Input::Input input) -> void override {
    if(auto button = input->cast<Node::Input::Button>()) {
      if(input->name() == "Reset") { button->setValue(false); return; }
      button->setValue((++polls / 7) & 1);
    }
  }
  auto pak(Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "Famicom Cartridge") return cartridgePak;
    return std::make_shared<vfs::directory>();
  }
};
auto command(const string& packet) -> string { bool reply = true; return server.processCommand(packet, reply); }
auto boundary(Node::System root) -> void {
  for(unsigned i = 0; i < 10000; ++i) {
    root->run(); server.updateLoop(); if(server.isHalted()) return;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  assert(false && "Famicom did not reach an instruction boundary");
}
auto snapshot(Node::System root) -> std::vector<u8> {
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  return {state.data(), state.data() + state.size()};
}
auto little(const u8* bytes, u32 size) -> u64 {
  u64 value = 0;
  for(u32 i = 0; i < size; ++i) value |= u64(bytes[i]) << (8 * i);
  return value;
}
auto nall::main(Arguments) -> void {
  Host host; platform = &host;
  std::vector<u8> rom(0x20000, 0xea);
  // Fixed-bank loop and distinct banked code at the same CPU PC.
  u8 program[] = {0xe8, 0x86, 0, 0x4c, 0, 0xc0};
  std::copy(std::begin(program), std::end(program), rom.begin() + 0x1c000);
  rom[0] = 0xe8; rom[0x2000] = 0xca;
  rom[0x1fff] = 0xad; rom[0x6000] = 0; rom[0x6001] = 0x80;
  for(u32 at : {0x1fffa, 0x1fffc, 0x1fffe}) { rom[at] = 0; rom[at + 1] = 0xc0; }
  host.cartridgePak->setAttribute("board", "HVC-TLROM");
  host.cartridgePak->append("program.rom", {rom.data(), rom.size()});
  std::vector<u8> chr(131072); for(u32 i : range(chr.size())) chr[i] = i >> 10; host.cartridgePak->append("character.rom", {chr.data(), chr.size()});
  Node::System root;
  assert(load(root, "[Nintendo] Famicom (NTSC-J)"));
  auto port = root->find<Node::Port>("Cartridge Slot"); assert(port);
  port->allocate(); port->connect(); root->power(false);
  server.onConnect(); command("?"); boundary(root);
  assert(cpu.PC == 0xc000 && command("g").size() == 14);
  assert(server.capture.famicom && command("qSupported").find("uttoriCaptureV2+"));
  assert(!command("qSupported").find("uttoriSramMemory+"));
  auto initial = snapshot(root);
  for(unsigned i = 0; i < 1024; ++i) { command("s"); boundary(root); }
  auto reference = snapshot(root);
  serializer restore(initial.data(), initial.size()); restore.setReading();
  std::thread([&] { assert(root->unserialize(restore)); }).join();
  assert(server.capture.command("start:8388608")[0] == '{');
  for(unsigned i = 0; i < 1024; ++i) { command("s"); boundary(root); }
  assert(server.capture.produced == 1024);
  assert(snapshot(root) == reference && "Famicom capture changed machine state");
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());

  // Debug reads must not acknowledge PPU status or controller/APU registers.
  // Unsynchronized serialization observes the halted core without advancing the scheduler.
  auto beforeRead = root->serialize(false);
  assert(server.hooks.namedMemoryRead("nes.cpu", 0x2002, 1).size() == 0);
  auto afterRead = root->serialize(false);
  assert(beforeRead.size() == afterRead.size() && !memcmp(beforeRead.data(), afterRead.data(), beforeRead.size()));
  assert(server.hooks.namedMemoryRead("nes.prg-rom", 0x2000, 1) == "ca");
  assert(!server.hooks.breakpointMatches(0x100000000ull + (0x2000ull << 16) + 0x8000, 0x8000));
  cartridge.writePRG(0x8000, 6); cartridge.writePRG(0x8001, 1);
  assert(server.hooks.breakpointMatches(0x100000000ull + (0x2000ull << 16) + 0x8000, 0x8000));
  assert(!server.hooks.breakpointMatches(0x100000000ull + (0x2000ull << 16) + 0xa000, 0x8000));

  // A boundary-spanning LDA must retain both physical fetch spans, without speculative reads.
  cartridge.writePRG(0x8000, 6); cartridge.writePRG(0x8001, 0);
  cartridge.writePRG(0x8000, 7); cartridge.writePRG(0x8001, 3);
  cpu.PC = 0x9fff;
  assert(server.capture.command("start:8388608")[0] == '{');
  command("s"); boundary(root);
  server.capture.command("stop:" + server.capture.sessionId());
  auto& chunk = server.capture.queue[server.capture.head];
  auto* record = chunk.bytes.data() + 64;
  assert(record[48] == 3 && record[51] == 0xad && record[52] == 0 && record[53] == 0x80);
  assert(little(record + 79, 4) == 0x1fff);
  assert(little(record + 83, 4) == 0x6000);
  assert(little(record + 87, 4) == 0x6001);
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());
  // Stop can race a frame yield inside the opcode's real read; pending fetches still belong to it.
  server.capture.command("start:8388608");
  server.capture.beginNes(cpu.PC, {0,0,0,0,0,u8(cpu.PC),u8(cpu.PC >> 8)}, cartridge.board->debugMapper(), cpu.captureClock);
  server.capture.command("stop:" + server.capture.sessionId());
  cpu.fetched(cpu.PC, 0xea); server.capture.end();
  assert(server.capture.produced == 1 && !server.capture.incomplete());
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());

  // Every conditional branch must capture its displacement on both outcomes. Passive CPU
  // serialization includes IRQ flags, RAM and thread clocks without synchronizing other devices.
  auto branchRestore = snapshot(root);
  auto branchCpuState = [] {
    serializer state; cpu.serialize(state);
    return std::vector<u8>{state.data(), state.data() + state.size()};
  };
  for(u8 opcode : {0x10, 0x30, 0x50, 0x70, 0x90, 0xb0, 0xd0, 0xf0}) {
    for(bool take : {false, true}) {
      for(auto [pc, displacement] : {std::pair<u16,u8>{0x500,0x20}, {0x5fd,0x20}, {0x600,0xdc}}) {
        cpu.ram.write(pc, opcode); cpu.ram.write(pc + 1, displacement); cpu.PC = pc;
        // Bit 5 selects the branch pair's set/clear condition for each status flag.
        bool flag = take == bool(opcode & 0x20);
        cpu.P.n = flag; cpu.P.z = flag; cpu.P.v = flag; cpu.P.c = flag;
        auto start = snapshot(root);
        command("s"); boundary(root);
        auto reference = branchCpuState();
        serializer restore(start.data(), start.size()); restore.setReading();
        std::thread([&] { assert(root->unserialize(restore)); }).join();
        assert(server.capture.command("start:8388608")[0] == '{');
        command("s"); boundary(root);
        assert(cpu.PC == u16(take ? pc + 2 + i8(displacement) : pc + 2));
        assert(branchCpuState() == reference);
        server.capture.command("stop:" + server.capture.sessionId());
        auto* branch = server.capture.queue[server.capture.head].bytes.data() + 64;
        assert(branch[48] == 2 && branch[51] == opcode && branch[52] == displacement);
        server.capture.command("abort:" + server.capture.sessionId());
        server.capture.command("release:" + server.capture.sessionId());
      }
    }
  }
  serializer restoreBranches(branchRestore.data(), branchRestore.size()); restoreBranches.setReading();
  std::thread([&] { assert(root->unserialize(restoreBranches)); }).join();

  // A paused snapshot is a passive storage view even with pending NMI, buffered reads,
  // a set write latch and active MMC3 CHR switching. Repeated slices never touch A12.
  ppu.io.nmiFlag = 1; ppu.scroll.latch = 1; ppu.var.latchData = 0x7a;
  cartridge.writePRG(0x8000, 0); cartridge.writePRG(0x8001, 4);
  auto passiveBefore = root->serialize(false);
  auto graphic = server.hooks.namedMemoryRead("nes.ppu-snapshot", 0, 13056);
  assert(graphic.size() == 26112 && graphic.slice(0, 8) == "4e505031");
  assert(server.hooks.namedMemoryRead("nes.ppu-snapshot", 256, 1) == "04");
  auto passiveAfter = root->serialize(false);
  assert(passiveBefore.size() == passiveAfter.size() && !memcmp(passiveBefore.data(), passiveAfter.data(), passiveBefore.size()));

  // Run real PPU port writes, OAM DMA and a DMC fetch twice from the same state.
  // Capture may slow the host but must preserve every byte of emulated machine state.
  u8 graphicsProgram[] = {
    0xa9,0,0x8d,1,0x20, 0xa9,0x3f,0x8d,6,0x20, 0xa9,0,0x8d,6,0x20,
    0xa9,0x2a,0x8d,7,0x20, 0xa9,0x20,0x8d,6,0x20, 0xa9,0,0x8d,6,0x20,
    0xa9,0x43,0x8d,7,0x20, 0xa9,0,0x8d,3,0x20, 0xa9,3,0x8d,0x14,0x40,
    0xad,2,0x20, 0xad,7,0x20, 0x4c,0xb3,2
  };
  // Synchronizing a save state can leave auxiliary threads ahead until their next safe point.
  // Warm up with 128 NOPs before the port sequence; the self-loop then begins at $02B3.
  for(u32 i : range(128)) cpu.ram.write(0x200 + i, 0xea);
  std::copy(std::begin(graphicsProgram), std::end(graphicsProgram), cpu.ram.data() + 0x280);
  for(u32 i : range(256)) cpu.ram.write(0x300 + i, i);
  cpu.PC = 0x200; ppu.scroll.latch = 0;
  cpu.dmcDMAPending();
  auto graphicsStart = snapshot(root);
  for(unsigned i = 0; i < 256; ++i) { command("s"); boundary(root); }
  auto graphicsReference = snapshot(root);
  serializer graphicsRestore(graphicsStart.data(), graphicsStart.size()); graphicsRestore.setReading();
  std::thread([&] { assert(root->unserialize(graphicsRestore)); }).join();
  assert(server.capture.command("ppu-capabilities").find("nes.cpu-bus-ppu") != std::string::npos);
  assert(server.capture.command("start:8388608:bus-ppu")[0] == '{');
  for(unsigned i = 0; i < 256; ++i) { command("s"); boundary(root); }
  assert(snapshot(root) == graphicsReference && "PPU/DMA capture changed machine state");
  server.capture.command("stop:" + server.capture.sessionId());
  u32 oamWrites = 0, dmcReads = 0, paletteWrites = 0, ciramWrites = 0;
  for(size_t q = 0; q < server.capture.size; ++q) {
    auto& data = server.capture.queue[(server.capture.head + q) % server.capture.queue.size()];
    for(u32 at = 64; at < data.used; at += 128) {
      auto* event = data.bytes.data() + at;
      assert(little(event+4,4) == 128);
      if(little(event,2) == 5) continue;
      assert(little(event,2) == 4);
      if(event[46] == 1 && event[45]) { ++oamWrites; assert(little(event+42,2) == 0x2004); }
      if(event[46] == 2) { ++dmcReads; assert(!event[45]); }
      if(event[102] == 3) { ++paletteWrites; assert(event[104] == 2 && event[116] == 0x2a && event[117] == 0x2a); }
      if(event[102] == 2) { ++ciramWrites; assert(event[104] == 1 && event[116] == 0x43); }
    }
  }
  assert(oamWrites == 256 && dmcReads >= 1 && paletteWrites == 1 && ciramWrites == 1);
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());

  // Restriction decisions and palette aliases belong to the real PPU implementation.
  server.capture.command("start:8388608:bus-ppu");
  ppu.io.bgEnable = 1; ppu.io.ly = 0;
  cpu.captureWrite(0x2004, 0x12);
  ppu.io.bgEnable = 0; ppu.io.spriteEnable = 0; ppu.io.busAddress = 0x3f10;
  cpu.captureWrite(0x3fff, 0xff);
  ppu.io.busAddress = 0; cpu.captureWrite(0x2007, 0x42);
  server.capture.command("stop:" + server.capture.sessionId());
  auto& effects = server.capture.queue[server.capture.head];
  auto* blocked = effects.bytes.data() + 64;
  assert(blocked[102] == 4 && blocked[103] == 1 && !blocked[104]);
  auto* paletteEvent = blocked + 128;
  assert(paletteEvent[102] == 3 && paletteEvent[104] == 2 && paletteEvent[116] == 0x3f);
  assert(little(paletteEvent+108,4) == 0x10 && little(paletteEvent+112,4) == 0);
  assert(paletteEvent[128+102] == 5 && paletteEvent[128+103] == 1);
  // Optional synthetic wire fixtures exercise the TypeScript decoder against the native writer.
  if(auto directory = std::getenv("UTTORI_NES_PPU_FIXTURE")) {
    std::ofstream chunkFile(std::string(directory) + "/ppu-bus.chunk", std::ios::binary);
    chunkFile.write(reinterpret_cast<const char*>(effects.bytes.data()), effects.used);
    std::ofstream snapshotFile(std::string(directory) + "/ppu-snapshot.bin", std::ios::binary);
    for(u32 i = 0; i < graphic.size(); i += 2) { u8 byte = graphic.slice(i, 2).hex(); snapshotFile.put(byte); }
  }
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());

  // An actual rendering window must retain every fetch and native MMC3 A12/IRQ
  // transition. Muted capture and enabled capture produce identical save states.
  ppu.io.bgEnable = 1; ppu.io.spriteEnable = 1; ppu.io.bgAddress = 0x1000;
  cartridge.writePRG(0xc000, 0); cartridge.writePRG(0xe001, 0);
  auto renderStart = snapshot(root);
  for(unsigned i = 0; i < 512; ++i) { command("s"); boundary(root); }
  auto renderReference = snapshot(root);
  serializer renderRestore(renderStart.data(), renderStart.size()); renderRestore.setReading();
  std::thread([&] { assert(root->unserialize(renderRestore)); }).join();
  server.capture.command("start:8388608:bus-ppu");
  for(unsigned i = 0; i < 512; ++i) { command("s"); boundary(root); }
  assert(snapshot(root) == renderReference);
  server.capture.command("stop:" + server.capture.sessionId());
  u32 fetches = 0, edges = 0, pins = 0;
  u64 previous = 0;
  for(size_t q = 0; q < server.capture.size; ++q) {
    auto& data = server.capture.queue[(server.capture.head + q) % server.capture.queue.size()];
    for(u32 at = 64; at < data.used; at += 128) {
      auto* event = data.bytes.data() + at;
      assert(little(event + 24, 8) >= previous); previous = little(event + 24, 8);
      if(little(event, 2) != 5) continue;
      if(event[56] == 0) { ++fetches; assert(event[59] && event[57] == 1); }
      if(event[56] == 1) { ++edges; assert(event[68] != event[76]); }
      if(event[56] == 3) ++pins;
    }
  }
  assert(fetches > 0 && edges > 0);
  if(auto directory = std::getenv("UTTORI_NES_PPU_FIXTURE")) {
    auto& data = server.capture.queue[server.capture.head];
    std::ofstream fixture(std::string(directory) + "/ppu-timeline.chunk", std::ios::binary);
    fixture.write(reinterpret_cast<const char*>(data.bytes.data()), data.used);
  }
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());

  // A12 assertion belongs to the PPU-address callback; the CPU pin changes
  // later when the cartridge coroutine runs. Verify both edges independently.
  ppu.io.bgEnable = 0; ppu.io.spriteEnable = 0;
  cartridge.writePRG(0xe000, 0);
  for(unsigned i = 0; i < 8; ++i) { command("s"); boundary(root); }
  assert(!cpu.io.irqLine);
  server.capture.command("start:8388608:bus-ppu");
  cartridge.writePRG(0xc000, 0); cartridge.writePRG(0xc001, 0); cartridge.writePRG(0xe001, 0);
  cartridge.ppuAddressBus(0); cartridge.ppuAddressBus(0x1000);
  assert(cartridge.board->debugIRQ()[3] && !cpu.io.irqLine);
  command("s"); boundary(root);
  assert(cpu.io.irqLine);
  cartridge.writePRG(0xe000, 0);
  assert(!cartridge.board->debugIRQ()[3] && cpu.io.irqLine);
  command("s"); boundary(root);
  assert(!cpu.io.irqLine);
  server.capture.command("stop:" + server.capture.sessionId());
  auto& irqChunk = server.capture.queue[server.capture.head];
  u32 irqPins = 0;
  for(u32 at = 64; at < irqChunk.used; at += 128) {
    auto* event = irqChunk.bytes.data() + at;
    if(little(event, 2) == 5 && event[56] == 3) {
      assert(event[57] == 2 && event[98] == !irqPins);
      ++irqPins;
    }
  }
  assert(irqPins == 2);
  if(auto directory = std::getenv("UTTORI_NES_PPU_FIXTURE")) {
    std::ofstream fixture(std::string(directory) + "/ppu-irq.chunk", std::ios::binary);
    fixture.write(reinterpret_cast<const char*>(irqChunk.bytes.data()), irqChunk.used);
  }
  server.capture.command("abort:" + server.capture.sessionId());
  server.capture.command("release:" + server.capture.sessionId());
  ppu.io.bgEnable = 1; ppu.io.spriteEnable = 1;

  // Replay polls override changed host buttons before gamepad hold/shift logic.
  controllerPort1.allocate("Gamepad"); controllerPort2.allocate("Gamepad");
  // Debugger masks feed real latch/serial reads and are recorded at the same
  // sampling boundary as host input; malformed masks cannot alias valid values.
  assert(command("qUttoriContext:buttons:0:80") == "buttons-set");
  controllerPort1.device->latch(1); controllerPort1.device->latch(0);
  assert((controllerPort1.device->data() & 1) == 1);
  assert((controllerPort1.device->data() & 1) == 0);
  assert(command("qUttoriContext:buttons:0:40") == "buttons-set");
  controllerPort1.device->latch(1); controllerPort1.device->latch(0);
  assert((controllerPort1.device->data() & 1) == 0);
  assert((controllerPort1.device->data() & 1) == 1);
  assert(command("qUttoriContext:buttons:1:00") == "buttons-set");
  assert(command("qUttoriContext:buttons:2:80") == "E01");
  assert(command("qUttoriContext:buttons:0:100") == "E01");
  assert(command("qUttoriContext:buttons:0:zz") == "E01");
  assert(command("qUttoriContext:buttons:0:host") == "buttons-set");
  assert(command("qUttoriContext:buttons:1:host") == "buttons-set");
  assert(command("qUttoriContext:buttons:0:80") == "buttons-set");
  auto pollsBeforeReset = host.polls;
  ares::Famicom::system.power(true);
  controllerPort1.device->latch(1); controllerPort1.device->latch(0);
  assert(host.polls > pollsBeforeReset);
  // Finish the pending reset before installing the RAM program's entry point.
  // Otherwise the first CPU boundary replaces PC with the cartridge reset vector.
  command("s"); boundary(root);
  const u8 inputProgram[] = {0xa9,1,0x8d,0x16,0x40,0xa9,0,0x8d,0x16,0x40,
    0xad,0x16,0x40,0x85,0,0xad,0x17,0x40,0x85,2,0xe6,1,0x4c,0,4};
  std::copy(std::begin(inputProgram), std::end(inputProgram), cpu.ram.data() + 0x400);
  cpu.PC = 0x400;
  auto hexText = [](const std::string& text) -> string { string result; for(u8 c : text) result.append(hex(c, 2, '0')); return result; };
  auto directory = std::filesystem::temp_directory_path() / ("ares-nes-context-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  string identity = {":", hexText(std::string(64, 'a')), ":", hexText("ares"), ":", hexText("nes-test"), ":", hexText(std::string(64, 'b'))};
  auto prepare = command({"qUttoriContext:prepare:", hexText(directory.string()), identity});
  assert(prepare == "prepared");
  assert(std::filesystem::file_size(directory / "start-extra.bin") == 38);
  server.capture.command("start:8388608");
  auto session = server.capture.sessionId();
  assert(command({"qUttoriContext:start:", session.c_str()}) == "recording");
  auto drain = [] {
    auto& capture = server.capture;
    while(capture.size) {
      auto& front = capture.queue[capture.head];
      capture.command("read:" + capture.sessionId() + ":" + std::to_string(front.sequence));
      capture.command("ack:" + capture.sessionId() + ":" + std::to_string(front.sequence) + ":" + std::to_string(front.first + front.count - 1));
    }
  };
  for(unsigned i = 0; i < 24000; ++i) { drain(); command("s"); boundary(root); }
  auto endClock = cpu.captureClock; auto endPC = cpu.PC;
  assert(server.capture.graphics && server.capture.graphics->eventCount() > 10000);
  assert(server.capture.graphics->instructionCount() == server.capture.instructionCount());
  ppu.screen->refresh();
  string status;
  do {
    status = command({"qUttoriContext:stop:", session.c_str()});
    assert(status == "draining" || status == "complete");
    server.updateLoop(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(status != "complete");
  server.capture.command("stop:" + session); drain();
  assert(server.capture.complete());
  assert(std::filesystem::file_size(directory / "ram-end.bin") == 2048);
  assert(std::filesystem::file_size(directory / "inputs.bin") > 0);
  assert(std::filesystem::file_size(directory / "graphics.act2") > 0);
  host.polls += 4;
  assert(command({"qUttoriContext:replay:", hexText(directory.string()), identity}) == "replaying");
  command("c"); boundary(root);
  auto replayStatus = command("qUttoriContext:status");
  if(replayStatus != "idle") std::cerr << "Replay status: " << replayStatus << " clocks " << cpu.captureClock << "/" << endClock << " pc " << cpu.PC << "/" << endPC << " directory " << directory << "\n";
  assert(replayStatus == "idle");
  assert(cpu.captureClock == endClock && cpu.PC == endPC);
  // A single changed retained controller byte must fail even if RAM is unchanged.
  auto expected = file::read((directory / "end-state.bin").string().c_str());
  expected[expected.size() - 1] ^= 1;
  { std::ofstream out(directory / "end-state.bin", std::ios::binary); out.write(reinterpret_cast<const char*>(expected.data()), expected.size()); }
  assert(command({"qUttoriContext:replay:", hexText(directory.string()), identity}) == "replaying");
  command("c"); boundary(root);
  assert(command("qUttoriContext:status") == "replay-diverged");
  assert(command({"qUttoriContext:restore:", hexText(directory.string()), identity}) == "restored");
  command("qUttoriContext:abort");
  std::filesystem::remove_all(directory);
  server.onDisconnect(); root->unload();

  // NROM CHR-RAM uses the same passive mapping contract and reports actual RAM writes.
  host.cartridgePak = std::make_shared<vfs::directory>();
  host.cartridgePak->setAttribute("board", "HVC-NROM");
  host.cartridgePak->setAttribute("mirror", "vertical");
  host.cartridgePak->append("program.rom", {rom.data(), rom.size()});
  std::vector<u8> chrRam(8192); host.cartridgePak->append("character.ram", {chrRam.data(), chrRam.size()});
  assert(load(root, "[Nintendo] Famicom (NTSC-J)"));
  port = root->find<Node::Port>("Cartridge Slot"); port->allocate(); port->connect(); root->power(false);
  server.onConnect(); command("?"); boundary(root);
  auto ramMapping = cartridge.board->debugPPUAddress(0x1234);
  assert(ramMapping && ramMapping->kind == 2 && ramMapping->offset == 0x1234);
  assert(server.hooks.namedMemoryRead("nes.ppu-snapshot", 0, 13056).size() == 26112);
  server.capture.command("start:8388608:bus-ppu");
  ppu.io.busAddress = 0x1234; cpu.captureWrite(0x2007, 0x5a);
  server.capture.command("stop:" + server.capture.sessionId());
  auto* ramEvent = server.capture.queue[server.capture.head].bytes.data() + 64;
  assert(ramEvent[102] == 1 && ramEvent[104] == 1 && little(ramEvent+108,4) == 0x1234 && ramEvent[116] == 0x5a);
  assert(server.hooks.namedMemoryRead("nes.chr-ram", 0x1234, 1) == "5a");
  std::cout << "PASS NES PPU passive snapshots, mapper views, OAM/DMC, palette aliases, restricted writes, machine equivalence\n";
  std::cout << "PASS Famicom reset/step, state equivalence, safe inspection, bank+PC breakpoints, per-fetch provenance\n";
  server.onDisconnect(); root->unload();
}
