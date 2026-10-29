#include <fc/fc.hpp>
#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace ares;
using nall::GDB::server;

// Synthetic cartridge and scripted CPU accesses exercise real device coroutines,
// rendering, DMA, mapper callbacks and the durable writer without private ROMs.
struct Host : ares::Platform {
  std::shared_ptr<vfs::directory> cartridge = std::make_shared<vfs::directory>();
  auto pak(Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "Famicom Cartridge") return cartridge;
    return std::make_shared<vfs::directory>();
  }
};

auto hexText(const std::string& text) -> std::string {
  const char* digits = "0123456789abcdef";
  std::string out;
  for(unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}

// Compare machine fields, excluding host coroutine stacks and clock rebasing.
// Setting the serialization mode directly avoids advancing devices to a new boundary.
template<typename Scheduler, typename System>
auto state(Scheduler& scheduler, System& system) -> std::vector<uint8_t> {
  std::vector<std::pair<decltype(&*scheduler.thread(0)), u64>> clocks;
  auto base = scheduler.minimum();
  for(u32 id = 0, found = 0; found < scheduler.threads(); ++id) {
    if(auto thread = scheduler.thread(id)) {
      clocks.emplace_back(&*thread, thread->clock());
      thread->setClock(thread->clock() - base);
      ++found;
    }
  }
  bool synchronized = scheduler.getSynchronize();
  serializer bytes;
  system.serialize(bytes, true);
  scheduler.setSynchronize(synchronized);
  for(auto& entry : clocks) entry.first->setClock(entry.second);
  return {bytes.data(), bytes.data() + bytes.size()};
}

auto nall::main(Arguments args) -> void {
  bool nes = args.find("nes"), accurate = args.find("accurate"), quick = args.find("quick");
  auto destination = std::getenv("UTTORI_CAPTURE_BENCH_OUTPUT");
  assert(destination && "Set UTTORI_CAPTURE_BENCH_OUTPUT to a new output directory");
  std::filesystem::path directory(destination);
  assert(std::filesystem::create_directory(directory));
  Host host; platform = &host;
  Node::System root;
  if(nes) {
    std::vector<u8> rom(0x20000, 0xea), chr(0x20000);
    rom[0x1c000] = 0x4c; rom[0x1c001] = 0; rom[0x1c002] = 0xc0;
    for(unsigned at : {0x1fffa, 0x1fffc, 0x1fffe}) { rom[at] = 0; rom[at + 1] = 0xc0; }
    for(unsigned i = 0; i < chr.size(); ++i) chr[i] = i ^ (i >> 8);
    host.cartridge->setAttribute("board", "HVC-TLROM");
    host.cartridge->append("program.rom", {rom.data(), rom.size()});
    host.cartridge->append("character.rom", {chr.data(), chr.size()});
    assert(Famicom::load(root, "[Nintendo] Famicom (NTSC-J)"));
    auto port = root->find<Node::Port>("Cartridge Slot"); port->allocate(); port->connect();
  } else {
    SuperFamicom::ppu.setAccurate(accurate);
    SuperFamicom::option("Deterministic Entropy", "true");
    assert(SuperFamicom::load(root, "[Nintendo] Super Famicom (NTSC)"));
  }
  root->power(false);
  server.onConnect(); bool reply = true; server.processCommand("?", reply);
  while(!server.isHalted()) root->run();
  server.onDisconnect();
  // Enter every auxiliary coroutine before snapshotting its stack. An untouched
  // startup stack would otherwise need a one-shot host entry registration on restore.
  root->serialize(true);
  auto initial = root->serialize(false);
  auto initialClock = nes ? Famicom::cpu.captureClock : SuperFamicom::cpu.captureClock;
  std::vector<uint8_t> reference;
  unsigned iterations = nes ? 4500000 : 9000000;
  if(quick) iterations = 10000;
  for(const std::string mode : {"off", "none", "zstd"}) {
    serializer restore(initial.data(), initial.size()); restore.setReading();
    std::thread([&] { assert(root->unserialize(restore)); }).join();
    // Capture clocks are observational fields omitted from upstream save states.
    if(nes) Famicom::cpu.captureClock = initialClock;
    else SuperFamicom::cpu.captureClock = initialClock;
    server.onDisconnect();
    bool setup = false, done = false;
    uint64_t begin = 0;
    if(nes) {
      using namespace Famicom;
      cpu.create(Famicom::system.frequency(), [&] {
        if(!setup) {
          cpu.write(0x2000, 0x08); cpu.write(0x2001, 0x1e);
          cpu.write(0xc000, 3); cpu.write(0xe001, 0);
          setup = true; scheduler.exit(Event::Step);
        }
        if(!done) {
          begin = cpu.captureClock;
          for(unsigned i = 0; i < iterations; ++i) {
            cpu.write(0x2005, uint8_t(i)); cpu.read(i & 0x7ff);
            if((i & 4095) == 0) { cpu.write(0x4014, 0); cpu.read(0); }
            if((i & 8191) == 0) { cpu.write(0x8000, 0); cpu.write(0x8001, uint8_t(i >> 13)); }
          }
          done = true;
        }
        scheduler.exit(Event::Step);
      });
      scheduler.power(cpu);
    } else {
      using namespace SuperFamicom;
      cpu.create(SuperFamicom::system.cpuFrequency(), [&] {
        if(!setup) {
          cpu.write(0x2100, 0); cpu.write(0x2115, 0x80);
          setup = true; scheduler.exit(Event::Step);
        }
        if(!done) {
          begin = cpu.captureClock;
          const uint16_t ports[] = {0x2118, 0x2119, 0x2122, 0x2122, 0x2104, 0x2104};
          for(unsigned i = 0; i < iterations; ++i) {
            cpu.write(ports[i % 6], uint8_t(i)); cpu.read(0x7e0000 | (i & 65535));
          }
          done = true;
        }
        scheduler.exit(Event::Step);
      });
      scheduler.power(cpu);
    }
    root->run(); assert(setup && !done);
    auto file = directory / (mode + ".act2");
    if(mode != "off") {
      std::string request = "start:8388608:bus-ppu:" + hexText(file.string()) + ":" + std::string(64, 'a') + ":61726573:7375737461696e6564";
      if(nes) request += ":" + std::string(64, 'b');
      request += ":" + mode + ":profile";
      assert(server.captureFile.command(server.capture, request, true)[0] == '{');
    }
    const auto started = std::chrono::steady_clock::now();
    while(!done) {
      root->run();
      if(mode != "off") {
        server.captureFile.service(server.capture);
        assert(!server.capture.incomplete());
        if(!server.capture.ready()) std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
    std::string position = "null";
    if(mode != "off") {
      position = server.captureFile.command(server.capture, "stop:" + server.capture.sessionId(), true);
      while(position.find("\"fileState\":\"complete\"") == std::string::npos) {
        assert(!server.capture.incomplete());
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        position = server.captureFile.command(server.capture, "status:" + server.capture.sessionId(), true);
      }
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto end = nes ? Famicom::cpu.captureClock : SuperFamicom::cpu.captureClock;
    auto snapshot = nes ? state(Famicom::scheduler, Famicom::system) : state(SuperFamicom::scheduler, SuperFamicom::system);
    if(mode == "off") reference = snapshot;
    assert(snapshot == reference && "Storage mode changed emulated machine state");
    uint64_t fileBytes = mode == "off" ? 0 : std::filesystem::file_size(file);
    std::cout << "RESULT {\"system\":\"" << (nes ? "nes" : "snes") << "\",\"accurate\":" << accurate
      << ",\"mode\":\"" << mode << "\",\"iterations\":" << iterations << ",\"seconds\":" << seconds
      << ",\"masterClocks\":" << end - begin << ",\"fileBytes\":" << fileBytes
      << ",\"stateEqual\":true,\"position\":" << position << "}\n" << std::flush;
  }
  root->unload();
}
