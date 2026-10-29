// Bus observation surrounds the actual operation without advancing clocks or reading devices.
// A reserve wait can stop recording; re-check ownership after yielding before touching scratch.
auto CPU::captureBusBegin(n16 address, u8 actor, bool write) -> bool {
  auto& capture = nall::GDB::server.capture.busTarget();
  if(!capture.nesBusActive()) return false;
  while(capture.nesBusActive() && !capture.ready()) { scheduler.exit(Event::Step); scheduler.synchronize(); }
  if(!capture.nesBusActive()) return false;
  captureBusBody = {};
  auto put = [&](u32 offset, u64 value, u32 bytes) {
    for(u32 i : range(bytes)) captureBusBody[offset - 32 + i] = value >> (i * 8);
  };
  put(40, PC, 2); put(42, address, 2); put(45, write, 1); put(46, actor, 1);
  put(47, captureContext, 1);
  auto pcPhysical = cartridge.board->debugPRGOffset(PC);
  u32 owner = pcPhysical ? *pcPhysical : 0xffffffff;
  if(capture.pendingInstruction()) owner = capturePCPhysical;
  auto physical = cartridge.board->debugPRGOffset(address);
  put(48, owner, 4); put(52, physical ? *physical : 0xffffffff, 4);
  put(56, ppu.io.lx, 2); put(58, ppu.io.ly, 2);
  auto state = ppu.debugState();
  std::copy_n(state.begin(), 10, captureBusBody.begin() + 46);
  // This is an address translation at the access boundary, not an extra PPUDATA read.
  auto mapped = cartridge.board->debugPPUAddress(ppu.io.busAddress);
  if(mapped) { put(118, mapped->kind, 1); put(120, mapped->offset, 4); }
  return true;
}

auto CPU::captureBusEnd(n8 value) -> void {
  auto& capture = nall::GDB::server.capture.busTarget();
  captureBusBody[12] = value;
  auto mapper = cartridge.board->debugMapper();
  std::copy(mapper.begin(), mapper.end(), captureBusBody.begin() + 28);
  auto state = ppu.debugState();
  std::copy_n(state.begin(), 10, captureBusBody.begin() + 56);
  std::copy_n(state.begin() + 10, 3, captureBusBody.begin() + 66);
  captureBusBody[69] = u8(io.irqLine) | u8(io.nmiLine) << 1 | u8(io.apuLine) << 2;
  // Nested mapper events may have consumed the original reservation.
  while(capture.nesBusActive() && !capture.ready()) { scheduler.exit(Event::Step); scheduler.synchronize(); }
  capture.nesBus(captureBusBody, captureClock);
}

auto CPU::captureRead(n16 address, u8 actor) -> n8 {
  bool recording = captureBusBegin(address, actor, false);
  n8 value = readBus(address);
  if(recording) captureBusEnd(value);
  return value;
}

auto CPU::captureWrite(n16 address, n8 value, u8 actor) -> void {
  bool recording = captureBusBegin(address, actor, true);
  writeBus(address, value);
  if(recording) {
    // A CHR-ROM write reaches a real PPU port but has no writable storage destination.
    if(address >= 0x2000 && address < 0x4000 && (address & 7) == 7 && !captureBusBody[70] && captureBusBody[86] == 1)
      capturePPUBlocked(5);
    captureBusEnd(value);
  }
}

auto CPU::capturePPUWrite(u8 target, u32 offset, u8 value) -> void {
  auto& capture = nall::GDB::server.capture.busTarget();
  if(!capture.nesBusActive()) return;
  u8 count = captureBusBody[72];
  if(count >= 2 || (count && captureBusBody[70] != target)) { capture.fail("nes-ppu-effect-bound"); return; }
  captureBusBody[70] = target;
  captureBusBody[71] = 0;
  for(u32 i : range(4)) captureBusBody[76 + count * 4 + i] = offset >> (i * 8);
  captureBusBody[84 + count] = value;
  captureBusBody[72] = count + 1;
}

auto CPU::capturePPUBlocked(u8 target) -> void {
  if(!nall::GDB::server.capture.busTarget().nesBusActive()) return;
  captureBusBody[70] = target;
  captureBusBody[71] = 1;
}


// Frontend pressure pauses execution without advancing any emulated device.
auto captureNesReady() -> bool {
  auto& capture = nall::GDB::server.capture.busTarget();
  if(!capture.nesBusActive()) return false;
  while(capture.nesBusActive() && !capture.ready()) { scheduler.exit(Event::Step); scheduler.synchronize(); }
  return capture.nesBusActive();
}

// Kind 0 is an actual renderer read, 1 an A12 transition, 2 an MMC3 register
// write, and 3 the board output reaching the CPU IRQ pin. The observer timestamp
// preserves coroutine execution order; source time preserves hardware timing.
auto captureNesTimeline(u8 kind, u16 address, u8 value, const std::array<u8, 8>& before) -> void {
  auto& capture = nall::GDB::server.capture.busTarget();
  if(!captureNesReady()) return;
  std::array<u8, 96> body{};
  auto put = [&](u32 at, u64 value, u32 bytes) { for(u32 i : range(bytes)) body[at - 32 + i] = value >> (8 * i); };
  const Thread* device = &cpu;
  u8 source = 0;
  if(ppu.active()) { device = &ppu; source = 1; }
  else if(cartridge.active()) { device = &cartridge; source = 2; }
  // Thread IDs break ties by less than one scalar tick. Round the signed delta
  // to remove that tie-breaker; scheduler rebasing cancels in the subtraction.
  i64 delta = i64(device->clock()) - i64(cpu.clock());
  i64 scalar = cpu.scalar();
  i64 ticks = (delta + (delta < 0 ? -scalar / 2 : scalar / 2)) / scalar;
  put(32, cpu.captureClock + ticks, 8);
  put(40, capture.pendingInstruction() ? capture.instructionCount() : 0, 8);
  put(48, cpu.PC, 2); put(50, address, 2); put(52, ppu.io.lx, 2); put(54, ppu.io.ly, 2);
  put(56, kind, 1); put(57, source, 1); put(58, value, 1);
  if(kind == 0) {
    auto mapped = cartridge.board->debugPPUAddress(address);
    if(mapped) { put(59, mapped->kind, 1); put(60, mapped->offset, 4); }
  }
  std::copy(before.begin(), before.end(), body.begin() + 32);
  auto after = cartridge.board->debugIRQ();
  std::copy(after.begin(), after.end(), body.begin() + 40);
  auto mapper = cartridge.board->debugMapper();
  std::copy(mapper.begin(), mapper.end(), body.begin() + 48);
  put(98, cpu.io.irqLine, 1);
  capture.nesTimeline(body, cpu.captureClock);
}
