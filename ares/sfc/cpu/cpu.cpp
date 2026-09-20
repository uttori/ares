#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>

namespace ares::SuperFamicom {

CPU cpu;
#include "dma.cpp"
#include "memory.cpp"
#include "io.cpp"
#include "timing.cpp"
#include "irq.cpp"
#include "debugger.cpp"
#include "serialization.cpp"

auto CPU::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("CPU");

  version = parent->append<Node::Setting::Natural>("Version", 2);
  version->setAllowedValues({1, 2});

  debugger.load(node);
}

auto CPU::unload() -> void {
  nall::GDB::server.capture.fail("unload");
  version = {};
  debugger = {};
  node = {};
}

auto CPU::main() -> void {
  if(r.wai) return instructionWait();
  if(r.stp) return instructionStop();

  if(!status.interruptPending) {
    auto runnable = [&] {
      bool room = nall::GDB::server.capture.ready();
      return nall::GDB::server.reportPC(r.pc.d, true, room) && room;
    };
    while(!runnable()) {
      // Preserve the instruction boundary, but let the frontend service its
      // UI/quit/settings guard while stopped. It owns the RSP polling loop.
      scheduler.exit(Event::Step);
      // Save/load enters here to create a synchronized undo snapshot. Stay at
      // the stopped instruction instead of blocking until the client times out.
      scheduler.synchronize();
    }
    auto& trace = nall::GDB::server;
    if(trace.capture.active()) {
      trace.capture.begin(r.pc.d, {u16(r.a.w), u16(r.x.w), u16(r.y.w), u16(r.s.w), u16(r.d.w), u16(r.b), u16(r.pc.b), u16(r.pc.w), u16(r.p), u16(r.e)}, captureClock);
      debugger.instruction();
      instruction();
      trace.capture.end();
      return;
    }
    if(trace.detailedTraceActive()) {
      trace.beginInstructionTrace(r.pc.d, {u16(r.a.w), u16(r.x.w), u16(r.y.w), u16(r.s.w), u16(r.d.w), u16(r.b), u16(r.pc.b), u16(r.pc.w), u16(r.p), u16(r.e)});
      debugger.instruction();
      instruction();
      trace.endInstructionTrace();
      return;
    }
    trace.reportInstruction(r.pc.d);
    debugger.instruction();
    return instruction();
  }

  if(status.nmiPending) {
    status.nmiPending = 0;
    r.vector = r.e ? 0xfffa : 0xffea;
    debugger.interrupt("NMI");
    return interrupt();
  }

  if(status.irqPending) {
    status.irqPending = 0;
    r.vector = r.e ? 0xfffe : 0xffee;
    debugger.interrupt("IRQ");
    return interrupt();
  }

  if(status.resetPending) {
    status.resetPending = 0;
    step(132);
    r.vector = 0xfffc;
    debugger.interrupt("Reset");
    return interrupt();  //H=186
  }

  status.interruptPending = 0;
}

auto CPU::map() -> void {
  std::function<n8   (n24, n8)> reader;
  std::function<void (n24, n8)> writer;

  reader = std::bind_front(&CPU::readRAM, this);
  writer = std::bind_front(&CPU::writeRAM, this);
  auto peek = [this](n24 address) -> maybe<n8> {
    if(address >= 128_KiB) return {};
    return wram[address];
  };
  bus.map(reader, writer, "00-3f,80-bf:0000-1fff", 0x2000, 0, 0, peek);
  bus.map(reader, writer, "7e-7f:0000-ffff", 0x20000, 0, 0, peek);

  reader = std::bind_front(&CPU::readAPU, this);
  writer = std::bind_front(&CPU::writeAPU, this);
  bus.map(reader, writer, "00-3f,80-bf:2140-217f");

  reader = std::bind_front(&CPU::readCPU, this);
  writer = std::bind_front(&CPU::writeCPU, this);
  bus.map(reader, writer, "00-3f,80-bf:2180-2183,4016-4017,4200-421f");

  reader = std::bind_front(&CPU::readDMA, this);
  writer = std::bind_front(&CPU::writeDMA, this);
  bus.map(reader, writer, "00-3f,80-bf:4300-437f");
}

auto CPU::power(bool reset) -> void {
  nall::GDB::server.capture.fail("power-reset");
  captureClock = 0;
  WDC65816::power();
  create(system.cpuFrequency(), std::bind_front(&CPU::main, this));
  coprocessors.clear();
  PPUcounter::reset();
  PPUcounter::scanline = std::bind_front(&CPU::scanline, this);

  if(!reset) random.array({(u8*)wram, sizeof(wram)});

  for(u32 id : range(8)) {
    channels[id] = {};
    channels[id].id = id;
    if(id != 7) channels[id].next = channels[id + 1];
  }

  counter = {};

  io = {};
  io.version = version->value();

  alu = {};

  status = {};
  status.dramRefreshPosition = (io.version == 1 ? 530 : 538);
  status.hdmaSetupPosition = (io.version == 1 ? 12 + 8 - dmaCounter() : 12 + dmaCounter());
  status.hdmaPosition = 1104;
  status.resetPending = 1;
  status.interruptPending = 1;
}

}
