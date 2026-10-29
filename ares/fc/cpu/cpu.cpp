#include <fc/fc.hpp>
#include <nall/gdb/server.hpp>

namespace ares::Famicom {

CPU cpu;
#include "memory.cpp"
#include "timing.cpp"
#include "capture.cpp"
#include "debugger.cpp"
#include "serialization.cpp"

auto CPU::load(Node::Object parent) -> void {
  ram.allocate(2_KiB);

  node = parent->append<Node::Object>("CPU");

  debugger.load(node);
}

auto CPU::unload() -> void {
  ram.reset();
  debugger = {};
  node = {};
}

auto CPU::main() -> void {
  captureContext = 2;
  if(io.interruptPending) {
    if(io.resetPending) {
      debugger.interrupt("Reset");
      reset();
      io.resetPending = 0;
    } else if(io.nmiPending) {
      debugger.interrupt("NMI");
      interrupt();
    } else {
      debugger.interrupt("IRQ");
      interrupt();
    }
  }

  captureContext = 0;
  auto& trace = nall::GDB::server;
  while(true) {
    bool room = trace.capture.ready();
    if(trace.reportPC(PC, true, room) && room) break;
    scheduler.exit(Event::Step);
    scheduler.synchronize();
  }
  if(trace.capture.active()) {
    auto mapper = cartridge.board->debugMapper();
    if(mapper[0] == 255) { trace.capture.fail("unsupported-nes-mapper"); return; }
    if(trace.capture.busTarget().nesBusActive() && !cartridge.board->debugPPUAddress(0)) {
      trace.capture.fail("unsupported-nes-ppu-mapping"); return;
    }
    auto physical = cartridge.board->debugPRGOffset(PC);
    capturePCPhysical = physical ? *physical : 0xffffffff;
    trace.capture.beginNes(PC, {u8(A), u8(X), u8(Y), u8(S), u8(u8(P) | 0x20), u8(PC), u8(PC >> 8)}, mapper, captureClock);
  }
  trace.reportInstruction(PC);
  debugger.instruction();
  instruction();
  trace.capture.end();
}

auto CPU::fetched(n16 address, n8 value) -> void {
  auto& capture = nall::GDB::server.capture;
  // A stop can arrive while a bus cycle yielded a frame. Finish the already reserved instruction
  // even after recording was disabled, otherwise its final real fetches would be lost.
  if(!capture.pendingInstruction()) return;
  auto offset = cartridge.board->debugPRGOffset(address);
  capture.fetchNes(value, offset ? *offset : 0xffffffff);
}

auto CPU::step(u32 clocks) -> void {
  assert(clocks == rate());
  captureClock += clocks;
  io.oddCycle ^= 1;
  Thread::step(clocks);
  Thread::synchronize();
}

auto CPU::power(bool reset) -> void {
  nall::GDB::server.capture.fail("power-reset");
  captureClock = 0;
  MOS6502::BCD = 0;
  if(!reset) MOS6502::power();
  Thread::create(system.frequency(), std::bind_front(&CPU::main, this));

  if(!reset) {
    ram.fill(0xff);
  }

  io = {};
  io.resetPending = 1;
  io.interruptPending = 1;
}

}
