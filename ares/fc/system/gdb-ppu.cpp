// NPP1 is an immutable view of the halted core. Mapped reads inspect backing storage
// directly: no PPUDATA buffering, palette masking, mapper A12 edges, or scheduler work.
auto gdbPPUSnapshot() -> std::vector<u8> {
  if(!nall::GDB::server.isHalted() || !cartridge.board) return {};
  std::vector<u8> result(13056);
  auto put = [&](u32 offset, u64 value, u32 bytes) {
    for(u32 i : range(bytes)) result[offset + i] = value >> (i * 8);
  };
  std::memcpy(result.data(), "NPP1", 4);
  put(4, 1, 2); put(6, 256, 2); put(8, result.size(), 4);
  put(16, cpu.captureClock, 8); put(24, cpu.PC, 2);
  put(26, ppu.io.lx, 2); put(28, ppu.io.ly, 2); put(30, ppu.vlines(), 2);
  auto state = ppu.debugState();
  std::copy(state.begin(), state.end(), result.begin() + 32);
  put(45, ppu.io.field, 1); put(46, cpu.rate(), 1); put(47, ppu.rate(), 1);
  auto mapper = cartridge.board->debugMapper();
  if(mapper[0] == 255) return {};
  std::copy(mapper.begin(), mapper.end(), result.begin() + 64);
  auto pc = cartridge.board->debugPRGOffset(cpu.PC);
  put(84, pc ? *pc : 0xffffffff, 4);
  auto clock = nall::GDB::server.capture.clockRatio();
  put(96, clock.first, 8); put(104, clock.second, 8);
  for(u32 slot : range(12)) {
    auto mapped = cartridge.board->debugPPUAddress(slot * 1024);
    if(!mapped) return {};
    put(128 + slot * 8, mapped->kind, 1);
    put(129 + slot * 8, mapped->writable, 1);
    put(132 + slot * 8, mapped->offset, 4);
  }
  for(u32 address : range(12288)) {
    auto mapped = cartridge.board->debugPPUAddress(address);
    if(!mapped) return {};
    maybe<n8> value;
    if(mapped->kind == 3) value = ppu.ciram.read(mapped->offset);
    else value = cartridge.board->debugRead(mapped->kind == 1 ? "nes.chr-rom" : "nes.chr-ram", mapped->offset);
    if(!value) return {};
    result[256 + address] = *value;
  }
  for(u32 i : range(32)) result[12544 + i] = ppu.cgram[i];
  for(u32 i : range(256)) result[12576 + i] = ppu.oam[i];
  for(u32 i : range(32)) result[12832 + i] = ppu.soam[i];
  // RGB table comes from the active PPU model, including its current emphasis bits.
  for(u32 i : range(64)) {
    u64 rgb = ppu.color(i | u32(ppu.io.emphasis) << 6);
    result[12864 + i * 3] = rgb >> 40;
    result[12865 + i * 3] = rgb >> 24;
    result[12866 + i * 3] = rgb >> 8;
  }
  return result;
}
