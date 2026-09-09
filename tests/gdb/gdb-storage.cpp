#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
// nall defines NDEBUG from BUILD_RELEASE even when the compiler uses -UNDEBUG.
#undef NDEBUG
#include <cassert>
#include <iostream>
using namespace ares;
using namespace ares::SuperFamicom;
#include <sfc/system/gdb-memory.cpp>
auto nall::main(Arguments) -> void {
  ppu.setAccurate(false);
  bus.reset();
  cpu.wram[0] = 0x12; cpu.wram[0x1fff] = 0x34; cpu.wram[0x1ffff] = 0x56;
  for(u32 address : {0x000000, 0x800000, 0x7e0000}) assert(bus.peek(address).get() == 0x12);
  assert(bus.peek(0x801fff).get() == 0x34 && bus.peek(0x7fffff).get() == 0x56);
  unsigned reads = 0;
  bus.map([&](n24, n8) -> n8 { ++reads; return 0xff; }, [](n24, n8) {}, "00:2100-21ff");
  assert(!bus.peek(0x002100) && !reads);
  // A device mapped outside the CPU I/O area must not be called either.
  bus.map([&](n24, n8) -> n8 { ++reads; return 0xff; }, [](n24, n8) {}, "40:6000-60ff");
  assert(!bus.peek(0x406000) && !reads);
  auto storage = [](n24 offset) -> maybe<n8> { return n8(offset); };
  bus.map([](n24, n8) -> n8 { assert(false); return 0; }, [](n24, n8) {}, "70-71:0000-7fff", 0x8000, 0, 0, storage);
  assert(bus.peek(0x700001).get() == 1 && bus.peek(0x710001).get() == 1);
  bus.unmap("70-71:0000-7fff");
  assert(!bus.peek(0x700001));
  bus.map([&](n24, n8) -> n8 { ++reads; return 0xff; }, [](n24, n8) {}, "70:0000-7fff");
  assert(!bus.peek(0x700001) && !reads); // reused slot has no stale peeker
  installGdbMemoryHooks();
  auto& read = nall::GDB::server.hooks.read;
  assert(read(0x800000, 1) == "12");
  assert(read(0x7fffff, 1) == "56");
  assert(read(0x801fff, 2) == "E00"); // whole request is unavailable at MMIO
  assert(read(0xffffff, 2) == "E00");
  assert(read(0x1000000, 1) == "E00");
  assert(read(0x7e0000, 0x2001) == "E00");
  assert(!reads);
  auto parent = std::make_shared<ares::Core::Object>("storage tests");
  cpu.debugger.load(parent);
  dsp.debugger.load(parent);
  cartridge.rom.allocate(0x8000); cartridge.ram.allocate(0x2000);
  cartridge.rom.program(0x7fff, 0x78); cartridge.ram.write(0x1fff, 0x9a);
  dsp.apuram[0xffff] = 0xbc;
  cartridge.debugger.load(parent);
  unsigned nodes = 0;
  for(auto memory : parent->find<ares::Node::Debugger::Memory>()) {
    assert(!memory->peek(memory->size()));
    assert(!memory->peek(0x100000000ull));
    auto end = memory->peek(memory->size() - 1);
    assert(end);
    if(memory->name() == "CPU WRAM") { assert(end.get() == 0x56); ++nodes; }
    if(memory->name() == "APU RAM") { assert(end.get() == 0xbc); ++nodes; }
    if(memory->name() == "Cartridge ROM") { assert(end.get() == 0x78); ++nodes; }
    if(memory->name() == "Cartridge RAM") { assert(end.get() == 0x9a); ++nodes; }
  }
  assert(nodes == 4);
  nall::GDB::server.reset();
  std::cout << "PASS U4: actual SFC WRAM aliases, safe bus dispatch, mapped SRAM offsets, remap/unmap, exact RSP ranges, four physical storage boundaries\n";
}
