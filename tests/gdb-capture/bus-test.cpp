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
using Capture = ares_capture::Capture;

struct Host : ares::Platform {
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override { return std::make_shared<vfs::directory>(); }
};

// Independent wire reader: inspect emitted records, not producer scratch state.
auto le(const uint8_t* p, unsigned size) -> uint64_t {
  uint64_t value = 0;
  for(unsigned i = 0; i < size; ++i) value |= uint64_t(p[i]) << (i * 8);
  return value;
}
struct BusEvent {
  uint64_t sequence, clock, ordinal, address;
  uint32_t pc;
  uint8_t value, write, actor, channel, phase, context;
};
struct Boundary {
  uint64_t sequence, id;
  uint32_t transferred;
  uint8_t boundary, actor, channel;
  std::array<uint8_t, 12> configuration;
};
struct Reader {
  bool transfers = false, ppuEffects = false;
  std::function<void(const uint8_t*)> effect;
  std::function<void(const Boundary&)> boundary;
  uint64_t nextChunk{1}, nextEvent{1}, lastClock{};
  std::function<void(const BusEvent&)> observe;

  auto drain() -> void {
    auto& c = server.capture;
    for(;;) {
      auto request = "read:" + c.sessionId() + ":" + std::to_string(nextChunk);
      auto text = c.command(request);
      if(text.empty()) return;
      assert(text == c.command(request) && "retry changed delivered bytes");
      assert(text.size() >= 256 && text.size() <= Capture::chunkBytes * 2);
      std::vector<uint8_t> bytes(text.size() / 2);
      for(size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::stoul(text.substr(i * 2, 2), nullptr, 16);
      auto* p = bytes.data();
      assert(std::memcmp(p, "ACT2", 4) == 0 && le(p + 4, 2) == 2 && le(p + 6, 2) == 0);
      assert(le(p + 24, 8) == nextChunk && le(p + 32, 8) == nextEvent);
      auto count = le(p + 40, 4);
      assert(le(p + 44, 4) == 64 && count > 0 && count <= 4095);
      assert(le(p + 48, 4) == bytes.size() - 64 && le(p + 52, 4) == bytes.size() - 64);
      auto expectedCRC = le(p + 56, 4);
      std::fill(bytes.begin() + 56, bytes.begin() + 60, 0);
      uint32_t crc = ~uint32_t(0);
      for(auto byte : bytes) {
        crc ^= byte;
        for(unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
      }
      assert(uint32_t(~crc) == expectedCRC);
      size_t offset = 64;
      for(uint64_t i = 0; i < count; ++i) {
        assert(offset + 64 <= bytes.size());
        auto* r = p + offset;
        auto length = le(r + 4, 4);
        assert(offset + length <= bytes.size());
        offset += length;
        if(le(r, 2) == 3) {
          assert(transfers && length == 80 && le(r + 2, 2) == 1 && le(r + 76, 4) == 0);
          assert(le(r + 8, 8) == nextEvent++ && le(r + 24, 8) >= lastClock);
          lastClock = le(r + 24, 8);
          Boundary event{le(r + 8, 8), le(r + 40, 8), uint32_t(le(r + 72, 4)), r[52], r[53], r[54], {}};
          std::memcpy(event.configuration.data(), r + 60, 12);
          if(boundary) boundary(event);
          continue;
        }
        assert(le(r, 2) == 2);
        const bool extended = ppuEffects && le(r + 2, 2) == 2;
        assert(length == (extended ? 88 : 64) && le(r + 2, 2) == (extended ? 2 : 1));
        if(extended) {
          assert(r[64] >= 1 && r[64] <= 3 && r[65] <= 2 && r[66] <= 2 && r[67] < 16);
          assert(le(r + 68, 2) < 1368 && le(r + 70, 2) < 313 && le(r + 82, 6) == 0);
          if(effect) effect(r);
        }
        assert(le(r + 16, 4) == 0 && le(r + 20, 4) == 0 && le(r + 62, 2) == 0);
        BusEvent e{le(r + 8, 8), le(r + 24, 8), le(r + 32, 8), le(r + 40, 8), uint32_t(le(r + 48, 4)),
          r[52], r[53], r[54], r[55], r[56], r[57]};
        assert(e.sequence == nextEvent++ && e.clock >= lastClock && e.address <= 0xffffff);
        assert(e.write <= 1 && e.actor <= 2 && e.phase <= 4 && e.context <= 4);
        assert(le(r + 58, 2) < 1368 && le(r + 60, 2) < 313);
        lastClock = e.clock;
        if(observe) observe(e);
      }
      assert(offset == bytes.size());
      auto ack = "ack:" + c.sessionId() + ":" + std::to_string(nextChunk++) + ":" + std::to_string(nextEvent - 1);
      assert(c.command(ack)[0] == '{' && c.command(ack)[0] == '{');
    }
  }
};

auto snapshot(Node::System root) -> std::vector<u8> {
  serializer state;
  std::thread([&] { state = root->serialize(); }).join();
  return {state.data(), state.data() + state.size()};
}

// Execute real channel methods on the emulation coroutine. The one-shot entry
// remains safe to resume for ordinary synchronized serialization afterwards.
auto run(Node::System root, const std::function<void()>& setup, const std::function<void()>& work,
         Reader* reader, const std::function<void()>& onHold = {}, std::vector<u8>* initial = nullptr,
         const std::filesystem::path* nativeFile = nullptr) -> std::vector<u8> {
  server.capture = {};
  root->power(false);
  // Enter the powered CPU once before replacing its test entry point. Thread's
  // entry-point registry removes the power callback only on its first entry.
  auto prime = [&] {
    server.onConnect();
    bool reply = true;
    server.processCommand("?", reply);
    for(unsigned limit = 0; !server.isHalted(); ++limit) {
      assert(limit < 64);
      root->run();
    }
    server.onDisconnect();
  };
  prime();
  cpu.status.interruptPending = false;
  setup();
  // A power cycle does not reset every diagnostic counter (e.g. DSP samples).
  // Equivalence pairs must start from one exact serialized machine state.
  if(initial) {
    if(initial->empty()) *initial = snapshot(root);
    else {
      serializer state(initial->data(), initial->size());
      state.setReading();
      std::thread([&] { assert(root->unserialize(state)); }).join();
      prime();
    }
  }
  bool done = false, held = false;
  cpu.create(ares::SuperFamicom::system.cpuFrequency(), [&] {
    if(!done) { work(); done = true; }
    scheduler.exit(Event::Step);
  });
  scheduler.power(cpu);
  if(nativeFile) {
    auto path = nativeFile->string();
    auto encoded = ares_capture::Capture::hex(reinterpret_cast<const uint8_t*>(path.data()), path.size());
    assert(server.captureFile.command(server.capture, "start:4718656:bus-ppu:" + encoded + ":" + std::string(64, 'a') + ":61726573:73746174652d74657374", true)[0] == '{');
  }
  if(reader) {
    auto start = reader->transfers ? "start:4718656:bus-transfers" : "start:4718656:bus";
    if(reader->ppuEffects) start = "start:4718656:bus-ppu";
    auto result = server.capture.command(start);
    assert(result.find("snes.cpu-bus") != std::string::npos);
  }
  for(unsigned limit = 0; !done; ++limit) {
    assert(limit < 1000000 && "bus producer failed to yield or drain");
    root->run();
    if(reader && server.capture.throttled && !held) {
      held = true;
      auto clock = cpu.captureClock, produced = server.capture.produced;
      for(unsigned retry = 0; retry < 8; ++retry) {
        root->run();
        assert(cpu.captureClock == clock && server.capture.produced == produced);
      }
      assert(server.capture.allocatedBytes() <= 4718656);
      if(onHold) onHold();
    }
    if(reader) reader->drain();
    if(nativeFile) server.captureFile.service(server.capture);
  }
  if(reader) {
    server.capture.command("stop:" + server.capture.sessionId());
    reader->drain();
    assert(server.capture.produced == reader->nextEvent - 1);
    if(!server.capture.failed) assert(server.capture.state == "complete");
  }
  if(nativeFile) {
    auto status = server.captureFile.command(server.capture, "stop:" + server.capture.sessionId(), true);
    for(unsigned i = 0; status.find("\"fileState\":\"complete\"") == std::string::npos; ++i) {
      assert(i < 100000 && !server.capture.incomplete());
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      status = server.captureFile.command(server.capture, "status:" + server.capture.sessionId(), true);
    }
    assert(server.capture.produced == server.capture.accepted && server.capture.produced > 0);
  }
  if(onHold) assert(held);
  return snapshot(root);
}

auto nall::main(Arguments args) -> void {
  Host host; platform = &host;
  ppu.setAccurate(args.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  assert(server.capture.command("bus-capabilities").find("requiredReserveBytes\":64") != std::string::npos);
  assert(server.capture.command("start:4718656:unknown")[0] == 'E');

  // All DMA modes, directions and address-update policies, including bank-local
  // wrap. A test B-bus device counts real accesses and returns a known byte.
  uint64_t deviceReads = 0, deviceWrites = 0;
  bus.map([&](n24, n8) -> n8 { ++deviceReads; return 0x5a; },
    [&](n24, n8) { ++deviceWrites; }, "00:2120-212f");
  const unsigned offsets[8][4] = {{0,0,0,0}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}};
  for(unsigned mode = 0; mode < 8; ++mode) for(bool direction : {false, true}) for(unsigned policy = 0; policy < 3; ++policy) for(bool transfers : {false, true}) {
    std::vector<BusEvent> events;
    Reader reader; reader.transfers = transfers;
    std::vector<Boundary> boundaries; reader.boundary = [&](const Boundary& e) { boundaries.push_back(e); };
    reader.observe = [&](const BusEvent& e) { events.push_back(e); };
    auto setup = [&] {
      deviceReads = deviceWrites = 0;
      std::fill(std::begin(cpu.wram), std::end(cpu.wram), 0x5a);
      auto& c = cpu.channels[3]; c.dmaEnable = true; c.direction = direction;
      c.transferMode = mode; c.fixedTransfer = policy == 0; c.reverseTransfer = policy == 2;
      c.sourceBank = 0x7e; c.sourceAddress = policy == 2 ? 1 : 0xfffe;
      c.targetAddress = 0x20; c.transferSize = 8;
    };
    run(root, setup, [&] { cpu.dmaRun(); }, &reader);
    if(transfers) {
      assert(boundaries.size() == 2 && boundaries[0].boundary == 0 && boundaries[1].boundary == 1);
      assert(boundaries[0].id == boundaries[0].sequence && boundaries[1].id == boundaries[0].id);
      assert(boundaries[1].transferred == 8 && boundaries[0].actor == 1 && boundaries[0].channel == 3);
      assert((boundaries[0].configuration[0] & 7) == mode && (boundaries[0].configuration[0] >> 7) == direction);
      assert(boundaries[0].configuration[1] == 0x20 && boundaries[0].configuration[4] == 0x7e);
      assert(boundaries[0].configuration[6] == 8 && boundaries[1].configuration[6] == 0);
    }
    assert(events.size() == 16 && deviceReads == (direction ? 8 : 0) && deviceWrites == (direction ? 0 : 8));
    for(unsigned i = 0; i < 8; ++i) {
      uint16_t a = policy == 2 ? 1 - i : 0xfffe;
      if(policy == 1) a += i;
      auto addressA = 0x7e0000u | a, addressB = 0x2120u + offsets[mode][i % 4];
      auto& read = events[i * 2]; auto& write = events[i * 2 + 1];
      assert(read.address == (direction ? addressB : addressA) && write.address == (direction ? addressA : addressB));
      assert(read.write == 0 && write.write == 1);
      for(auto* e : {&read, &write}) assert(e->value == 0x5a && e->actor == 1 && e->channel == 3 && e->phase == 1 && e->ordinal == 0);
    }
  }

  // WRAM-to-WRAM DMA suppresses the B-bus operation. Synthetic zeroes from an
  // invalid read are not events, but a subsequent real A-bus write still is.
  for(bool direction : {false, true}) {
    std::vector<BusEvent> events;
    Reader invalid; invalid.observe = [&](const BusEvent& e) { events.push_back(e); };
    run(root, [&] {
      auto& c = cpu.channels[0]; c.dmaEnable = true; c.direction = direction;
      c.fixedTransfer = true; c.transferMode = 0; c.sourceBank = 0x7e;
      c.sourceAddress = 0; c.targetAddress = 0x80; c.transferSize = 4;
    }, [&] { cpu.dmaRun(); }, &invalid);
    assert(events.size() == 4);
    for(auto& e : events) {
      assert(e.address == 0x7e0000 && e.write == direction && e.actor == 1);
      if(direction) assert(e.value == 0);
    }
  }

  // Direct/indirect HDMA table reload, skipped repeat line, termination, and
  // indirect pointer fetches retain their own phase instead of looking like DMA.
  for(bool indirect : {false, true}) for(bool transfers : {false, true}) {
    std::vector<BusEvent> events;
    Reader reader; reader.transfers = transfers;
    std::vector<Boundary> boundaries; reader.boundary = [&](const Boundary& e) { boundaries.push_back(e); };
    reader.observe = [&](const BusEvent& e) { events.push_back(e); };
    auto setup = [&] {
      deviceReads = deviceWrites = 0;
      auto& c = cpu.channels[2]; c.hdmaEnable = true; c.indirect = indirect;
      c.sourceBank = 0x7e; c.sourceAddress = 0x100; c.indirectBank = 0x7e;
      c.targetAddress = 0x20; c.transferMode = 0; c.direction = false;
      cpu.wram[0x100] = 2; // transfer first line, skip second line
      cpu.wram[0x101] = 0x34; cpu.wram[0x102] = indirect ? 0x12 : 0;
      cpu.wram[0x103] = 0; cpu.wram[0x104] = 0;
      cpu.wram[0x1234] = 0x34;
      cpu.status.hdmaSetupTriggered = true;
    };
    run(root, setup, [&] {
      cpu.hdmaSetup(); cpu.hdmaRun(); cpu.hdmaRun();
      assert(cpu.channels[2].hdmaCompleted);
    }, &reader);
    if(transfers) {
      assert(boundaries.size() == 2 && boundaries[0].actor == 2 && boundaries[0].channel == 2);
      assert(boundaries[0].boundary == 0 && boundaries[1].boundary == 1 && boundaries[1].transferred == 1);
      assert(bool(boundaries[0].configuration[0] & 0x40) == indirect);
      assert(boundaries[0].configuration[10] == 2);
    }
    assert(deviceWrites == 1 && events.size() == (indirect ? 8 : 5));
    unsigned table = 0, pointer = 0, payload = 0;
    for(auto& e : events) {
      assert(e.actor == 2 && e.channel == 2);
      if(e.phase == 2) ++table;
      if(e.phase == 3) ++pointer;
      if(e.phase == 4) { ++payload; assert(e.value == 0x34); }
    }
    assert(table == 3 && pointer == (indirect ? 3 : 0) && payload == 2);
  }

  // Eight maximum-length channels produce >1M records through two bounded
  // slots. Compare complete serialized state to the same work with capture off.
  auto maximum = [&] {
    for(auto& c : cpu.channels) {
      c.dmaEnable = true; c.direction = false; c.fixedTransfer = true;
      c.transferMode = 0; c.sourceBank = 0x7e; c.sourceAddress = 0;
      c.targetAddress = 0x22; c.transferSize = 0;
    }
  };
  auto work = [&] { cpu.dmaRun(); };
  std::vector<u8> initial;
  auto reference = run(root, maximum, work, nullptr, {}, &initial);
  Reader reader;
  std::array<uint64_t, 8> counts{};
  reader.observe = [&](const BusEvent& e) {
    assert(e.actor == 1 && e.channel < 8 && e.phase == 1);
    ++counts[e.channel];
  };
  auto captured = run(root, maximum, work, &reader, [] {}, &initial);
  if(captured != reference) {
    unsigned differences = 0;
    for(size_t i = 0; i < std::min(captured.size(), reference.size()); ++i) if(captured[i] != reference[i]) {
      if(differences++ < 16) std::cerr << "state byte " << i << ": " << unsigned(reference[i]) << " -> " << unsigned(captured[i]) << '\n';
    }
    std::cerr << "state sizes " << reference.size() << '/' << captured.size() << ", differences " << differences << '\n';
  }
  assert(captured == reference);
  assert(reader.nextEvent - 1 == 1048576);
  for(auto count : counts) assert(count == 131072);

  // Stop fixes an exact prefix at a suspended access; abort preserves incomplete
  // integrity while allowing the coroutine to finish its current transfer.
  for(bool abort : {false, true}) {
    Reader prefix;
    run(root, maximum, work, &prefix, [&] {
      server.capture.command(std::string(abort ? "abort:" : "stop:") + server.capture.sessionId());
    });
    assert(prefix.nextEvent - 1 == 4095);
    assert(server.capture.state == (abort ? "incomplete" : "complete"));
  }

  // Synchronizing a save from a bus hold must finish at a normal core boundary,
  // invalidate the interrupted capture, and retain its exact readable prefix.
  Reader saved;
  run(root, maximum, work, &saved, [&] {
    assert(!snapshot(root).empty());
    assert(server.capture.state == "incomplete" && server.capture.error == "state-save");
  });
  assert(saved.nextEvent - 1 == 4095);
  // Invoke the real DMA edge with a pending HDMA burst on the same channel.
  // Its nested HDMA boundary must not overwrite the outer DMA run's identity.
  Reader preempted; preempted.transfers = true;
  std::vector<Boundary> interrupted;
  preempted.boundary = [&](const Boundary& e) { interrupted.push_back(e); };
  run(root, [&] {
    auto& c = cpu.channels[0]; c.dmaEnable = true; c.hdmaEnable = true;
    c.hdmaCompleted = false; c.hdmaDoTransfer = true; c.indirect = false;
    c.transferMode = 0; c.direction = false; c.fixedTransfer = true;
    c.sourceBank = 0x7e; c.sourceAddress = 0; c.hdmaAddress = 0x100;
    c.targetAddress = 0x20; c.transferSize = 8; c.lineCounter = 2;
    cpu.status.dmaActive = true; cpu.status.hdmaPending = true; cpu.status.hdmaMode = 1;
    cpu.status.clockCount = 8;
  }, [&] { cpu.channels[0].dmaRun(); }, &preempted);
  assert(interrupted.size() == 4);
  assert(interrupted[0].actor == 1 && interrupted[0].boundary == 0);
  assert(interrupted[1].actor == 2 && interrupted[2].actor == 2 && interrupted[2].boundary == 1);
  assert(interrupted[3].actor == 1 && interrupted[3].boundary == 2 && interrupted[3].transferred == 1);
  assert(interrupted[3].id == interrupted[0].id && interrupted[2].id == interrupted[1].id);

  Reader described; described.transfers = true;
  std::vector<Boundary> boundaries;
  described.boundary = [&](const Boundary& e) { boundaries.push_back(e); };
  auto transferState = run(root, maximum, work, &described, [] {}, &initial);
  assert(transferState == reference && described.nextEvent - 1 == 1048576 + 16);
  assert(boundaries.size() == 16);
  for(unsigned i = 0; i < 8; ++i) {
    assert(boundaries[i * 2].boundary == 0 && boundaries[i * 2 + 1].boundary == 1);
    assert(boundaries[i * 2 + 1].transferred == 65536);
    assert(boundaries[i * 2].id == boundaries[i * 2 + 1].id);
  }
  Reader partial; partial.transfers = true;
  boundaries.clear(); partial.boundary = [&](const Boundary& e) { boundaries.push_back(e); };
  run(root, maximum, work, &partial, [&] { server.capture.command("stop:" + server.capture.sessionId()); });
  assert(boundaries.size() == 1 && boundaries[0].boundary == 0);

  // A new session armed while the old coroutine is suspended must receive an
  // unpaired end (ID zero), never an ID belonging to the previous session.
  Reader rearmed; rearmed.transfers = true;
  boundaries.clear(); rearmed.boundary = [&](const Boundary& e) { boundaries.push_back(e); };
  run(root, maximum, work, &rearmed, [&] {
    server.capture.command("abort:" + server.capture.sessionId());
    assert(server.capture.command("start:4718656:bus-transfers")[0] == '{');
  });
  assert(boundaries.size() == 15 && boundaries[0].boundary == 1 && boundaries[0].id == 0);

  // Restore the PPU mapping after the synthetic B-bus device used above.
  ppu.map();
  // Replay native effects into the initial memory image. Rendering destinations
  // come from the PPU under test, independently of any reconstructed port model.
  for(bool blank : {true, false}) for(bool large : {false, true}) {
    if(args.find("accurate")) ppuImpl.vramSize->setValue(large ? 131072 : 65536);
    else ppuPerformanceImpl.vramSize->setValue(large ? 131072 : 65536);
    std::array<Node::Debugger::Memory, 3> memory{
      root->scan<Node::Debugger::Memory>("PPU VRAM"), root->scan<Node::Debugger::Memory>("PPU CGRAM"),
      root->scan<Node::Debugger::Memory>("PPU OAM")};
    std::array<std::vector<uint8_t>, 3> replay;
    auto setup = [&] {
      for(auto& c : cpu.channels) c.dmaEnable = c.hdmaEnable = false;
      cpu.status.dmaPending = cpu.status.hdmaPending = false;
    };
    auto graphicsWork = [&] {
      cpu.write(0x2100, blank ? 0x80 : 0);
      cpu.write(0x2115, 0x80); cpu.write(0x2116, 0xff); cpu.write(0x2117, 0xff);
      cpu.write(0x2121, 0xff); cpu.write(0x2102, 0); cpu.write(0x2103, 0);
      // Cross visible scanlines and queue boundaries, including writes before the
      // first observed data latch. Debugger reads here have no port side effects.
      while(cpu.vcounter() < 8 || cpu.hcounter() < 100) cpu.step(2);
      for(unsigned i = 0; i < 3; ++i) {
        auto bytes = i == 0 ? (large ? 131072u : 65536u) : memory[i]->size();
        replay[i].resize(bytes);
        for(unsigned a = 0; a < bytes; ++a) replay[i][a] = memory[i]->read(a);
      }
      const uint16_t ports[] = {0x2118, 0x2119, 0x2122, 0x2122, 0x2104, 0x2104};
      for(unsigned i = 0; i < 12000; ++i) cpu.write(0x800000 | ports[i % 6], uint8_t(i));
      for(auto target : {0x18, 0x22, 0x04}) {
        auto& c = cpu.channels[0]; c.dmaEnable = true; c.direction = false;
        c.fixedTransfer = true; c.reverseTransfer = false; c.transferMode = target == 0x18 ? 1 : 0;
        c.sourceBank = 0x7e; c.sourceAddress = 0; c.targetAddress = target; c.transferSize = 4096;
        cpu.dmaRun();
        // An explicit HDMA burst reaches the same native PPU hook with channel attribution.
        c.hdmaEnable = true; c.hdmaCompleted = false; c.hdmaDoTransfer = true;
        c.indirect = false; c.hdmaAddress = 0; c.lineCounter = 2;
        cpu.hdmaRun(); c.hdmaEnable = false;
      }
      // Vblank permits VRAM writes even with display enabled.
      while(cpu.vcounter() < 225) cpu.step(2);
      cpu.write(0x2118, 0x56); cpu.write(0x2119, 0x78);
    };
    std::vector<u8> startState;
    auto expected = run(root, setup, graphicsWork, nullptr, {}, &startState);
    Reader effects; effects.transfers = effects.ppuEffects = true;
    unsigned blocked = 0, latched = 0, written = 0, redirected = 0, doubled = 0;
    effects.effect = [&](const uint8_t* r) {
      const unsigned target = r[64] - 1, outcome = r[65], count = r[66], flags = r[67];
      assert(bool(flags & 4) == bool(args.find("accurate")));
      assert(bool(flags & 8) == (target == 0 && large));
      assert(bool(flags & 1) == blank);
      if(outcome == 1) { assert(target == 0 && !blank && count == 0 && (flags & 2)); ++blocked; }
      if(outcome == 2) { assert(target != 0 && count == 0 && !(flags & 2)); ++latched; }
      if(outcome == 0) {
        assert(count >= 1); ++written;
        if(flags & 2) ++redirected;
        if(count == 2 && le(r + 72, 4) == le(r + 76, 4)) ++doubled;
        for(unsigned i = 0; i < count; ++i) {
          auto address = le(r + 72 + i * 4, 4);
          assert(address < replay[target].size());
          replay[target][address] = r[80 + i];
        }
      }
    };
    auto actual = run(root, setup, graphicsWork, &effects, [] {}, &startState);
    assert(!server.capture.failed);
    assert(actual == expected && "PPU capture changed serialized emulator state");
    assert(written && latched && (blank ? blocked == 0 && redirected == 0 : blocked && redirected));
    if(!blank && !args.find("accurate")) assert(doubled && "performance PPU duplicate OAM writes lost");
    for(unsigned i = 0; i < 3; ++i) for(unsigned a = 0; a < replay[i].size(); ++a) {
      if(replay[i][a] != memory[i]->read(a)) std::cerr << "replay blank=" << blank << " large=" << large << " target=" << i << " address=" << a << " expected=" << unsigned(replay[i][a]) << " actual=" << unsigned(memory[i]->read(a)) << '\n';
      assert(replay[i][a] == memory[i]->read(a) && "native effects do not reproduce PPU memory");
    }
    // The writer must not advance any emulated processor while awaiting I/O.
    // Replay exactly the same starting state through native-file backpressure.
    auto path = std::filesystem::temp_directory_path() / ("ares-file-state-" + std::to_string(std::random_device{}()) + ".act2");
    auto fileActual = run(root, setup, graphicsWork, nullptr, {}, &startState, &path);
    assert(fileActual == expected && "native file capture changed serialized emulator state");
    assert(std::filesystem::file_size(path) > 0);
    std::filesystem::remove(path);

  }

  server.capture = {}; server.reset(); root->unload();
  std::cout << "PASS bus capture: DMA modes/directions/wrap, HDMA tables/indirect/repeat, 1048576-event bounded drain, machine-state equivalence, stop/abort, PPU rendering/blanking 64/128 KiB memory replay\n";
}
