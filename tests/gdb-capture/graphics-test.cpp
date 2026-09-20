#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <iostream>
using namespace ares;
using namespace ares::SuperFamicom;
using nall::GDB::server;

struct Host : ares::Platform {
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override { return std::make_shared<vfs::directory>(); }
};

// Script actual CPU port accesses; expected destinations come only from PPU memory readback.
struct Operation { uint32_t address; uint8_t value; bool read = false; };
struct Scenario {
  const char* name;
  std::vector<Operation> operations;
  bool dma = false;
  uint8_t target = 0, mode = 0;
};

auto le(const uint8_t* p, unsigned size) -> uint64_t {
  uint64_t value = 0;
  for(unsigned i = 0; i < size; ++i) value |= uint64_t(p[i]) << (i * 8);
  return value;
}

auto nall::main(Arguments args) -> void {
  Host host; platform = &host;
  ppu.setAccurate(args.find("accurate"));
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  root->power(false);
  // Enter the powered CPU before replacing its callback, matching the existing capture tests.
  server.onConnect();
  bool reply = true;
  server.processCommand("?", reply);
  for(unsigned limit = 0; !server.isHalted(); ++limit) { assert(limit < 64); root->run(); }
  server.onDisconnect();
  const char* targets[] = {"vram", "cgram", "oam"};
  Node::Debugger::Memory memories[] = {
    root->scan<Node::Debugger::Memory>("PPU VRAM"),
    root->scan<Node::Debugger::Memory>("PPU CGRAM"),
    root->scan<Node::Debugger::Memory>("PPU OAM"),
  };
  assert(memories[0] && memories[1] && memories[2]);
  assert(memories[0]->size() == 65536);
  std::vector<Scenario> scenarios;
  for(unsigned mapping = 0; mapping < 4; ++mapping) for(unsigned high = 0; high < 2; ++high) {
    scenarios.push_back({"vram-mapping", {{0x2100, 0x80}, {0x2115, uint8_t(high << 7 | mapping << 2)},
      {0x2116, 0xa5}, {0x2117, 0x83}, {0x2118, 0x12}, {0x2119, 0x34}}});
  }
  for(unsigned size = 0; size < 4; ++size) {
    scenarios.push_back({"vram-wrap", {{0x2100, 0x80}, {0x2115, uint8_t(0x80 | size)},
      {0x2116, 0xff}, {0x2117, 0xff}, {0x2119, 0x55}, {0x2118, 0xaa}}});
  }
  scenarios.push_back({"vram-read-mirrors", {{0x2100, 0x80}, {0xbf2115, 0x80}, {0x802116, 0},
    {0x3f2117, 0x20}, {0x7e2117, 0xff}, {0x402115, 0}, {0xc02116, 0xff}, {0x2117, 0, true},
    {0x2139, 0, true}, {0x2118, 1}, {0x213a, 0, true}, {0xbf2118, 2}, {0xff2118, 0x34}}});
  scenarios.push_back({"cgram-latches", {{0x2100, 0x80}, {0x2121, 0xff}, {0x2122, 0xab},
    {0x2122, 0xff}, {0x213b, 0, true}, {0x2122, 0x81}, {0x2122, 0x12},
    {0x2121, 8}, {0x213b, 0, true}, {0x2122, 0x34}}});
  scenarios.push_back({"oam-latches-mirrors", {{0x2100, 0x80}, {0x2102, 0xff}, {0x2103, 0},
    {0x2104, 0xab}, {0x2104, 0xcd}, {0x2104, 0xef}, {0x2102, 0xff}, {0x2103, 1},
    {0x2104, 0x56}, {0x2104, 0x78}, {0x2138, 0, true}, {0x2104, 0x9a}}});
  scenarios.push_back({"dma-vram", {{0x2100, 0x80}, {0x2115, 0x80}, {0x2116, 0xff}, {0x2117, 0x7f}}, true, 0x18, 1});
  scenarios.push_back({"dma-cgram", {{0x2100, 0x80}, {0x2121, 0xff}}, true, 0x22, 0});
  scenarios.push_back({"dma-oam", {{0x2100, 0x80}, {0x2102, 0xff}, {0x2103, 0}}, true, 0x04, 0});

  std::cout << "{\"vramBytes\":65536,\"scenarios\":[\n";
  bool firstScenario = true;
  for(const auto& scenario : scenarios) {
    // Only the test baseline uses debugger writes; they happen before arming capture and
    // are not presented as CPU activity. Each scenario explicitly establishes its port state.
    for(auto memory : memories) for(uint32_t address = 0; address < memory->size(); ++address) memory->write(address, 0);
    server.capture = {};
    cpu.status.dmaPending = cpu.status.hdmaPending = false;
    for(auto& channel : cpu.channels) channel.dmaEnable = channel.hdmaEnable = false;
    for(unsigned i = 0; i < 8; ++i) cpu.wram[i] = 0xa0 + i;
    bool done = false;
    cpu.create(ares::SuperFamicom::system.cpuFrequency(), [&] {
      if(!done) {
        for(auto op : scenario.operations) {
          if(op.read) cpu.read(op.address);
          else cpu.write(op.address, op.value);
        }
        if(scenario.dma) {
          auto& c = cpu.channels[0]; c.dmaEnable = true; c.direction = false;
          c.fixedTransfer = c.reverseTransfer = false; c.transferMode = scenario.mode;
          c.sourceBank = 0x7e; c.sourceAddress = 0; c.targetAddress = scenario.target; c.transferSize = 8;
          cpu.dmaRun();
        }
        done = true;
      }
      scheduler.exit(Event::Step);
    });
    scheduler.power(cpu);
    assert(server.capture.command("start:4718656:bus")[0] == '{');
    for(unsigned limit = 0; !done; ++limit) { assert(limit < 64); root->run(); }
    server.capture.command("stop:" + server.capture.sessionId());
    assert(server.capture.size == 1 && !server.capture.failed);
    // Establish delivered ownership before acknowledging the inspected queue slot.
    assert(!server.capture.command("read:" + server.capture.sessionId() + ":1").empty());
    auto& chunk = server.capture.queue[server.capture.head];
    if(!firstScenario) std::cout << ",\n";
    firstScenario = false;
    std::cout << "{\"name\":\"" << scenario.name << "\",\"accesses\":[";
    for(unsigned offset = 64; offset < chunk.used; offset += 64) {
      auto* r = chunk.bytes.data() + offset;
      assert(le(r, 2) == 2 && le(r + 4, 4) == 64);
      if(offset > 64) std::cout << ',';
      std::cout << "{\"address\":" << le(r + 40, 8) << ",\"value\":" << unsigned(r[52])
        << ",\"direction\":\"" << (r[53] ? "write" : "read") << "\",\"actor\":" << unsigned(r[54]) << '}';
    }
    std::cout << "],\"memory\":[";
    bool firstByte = true;
    for(unsigned target = 0; target < 3; ++target) {
      for(uint32_t address = 0; address < memories[target]->size(); ++address) {
        auto value = memories[target]->read(address);
        if(!value) continue;
        if(!firstByte) std::cout << ',';
        firstByte = false;
        std::cout << "{\"target\":\"" << targets[target] << "\",\"address\":" << address << ",\"value\":" << unsigned(value) << '}';
      }
    }
    std::cout << "]}";
    auto ack = "ack:" + server.capture.sessionId() + ":1:" + std::to_string(server.capture.produced);
    assert(server.capture.command(ack)[0] == '{' && server.capture.state == "complete");
  }
  std::cout << "\n]}\n";
  root->unload();
}
