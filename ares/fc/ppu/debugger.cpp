auto PPU::Debugger::load(Node::Object parent) -> void {
  memory.ciram = parent->append<Node::Debugger::Memory>("PPU CIRAM");
  memory.ciram->setSize(ppu.ciram.size());
  memory.ciram->setRead([&](u32 address) -> u8 {
    return ppu.ciram[address];
  });
  memory.ciram->setWrite([&](u32 address, u8 data) -> void {
    ppu.ciram[address] = data;
  });

  memory.cgram = parent->append<Node::Debugger::Memory>("PPU CGRAM");
  memory.cgram->setSize(ppu.cgram.size());
  memory.cgram->setRead([&](u32 address) -> u6 {
    return ppu.cgram[address];
  });
  memory.cgram->setWrite([&](u32 address, u6 data) -> void {
    ppu.cgram[address] = data;
  });

  memory.oam = parent->append<Node::Debugger::Memory>("PPU OAM");
  memory.oam->setSize(ppu.oam.size());
  memory.oam->setRead([&](u32 address) -> u8 {
    return ppu.oam[address];
  });
  memory.oam->setWrite([&](u32 address, u8 data) -> void {
    ppu.oam[address] = data;
  });

  memory.soam = parent->append<Node::Debugger::Memory>("PPU SOAM");
  memory.soam->setSize(ppu.soam.size());
  memory.soam->setRead([&](u32 address) -> u8 {
    return ppu.soam[address];
  });
  memory.soam->setWrite([&](u32 address, u8 data) -> void {
    ppu.soam[address] = data;
  });
}

auto PPU::Debugger::unload() -> void {
  memory.ciram.reset();
  memory.cgram.reset();
  memory.oam.reset();
}

// Effective control bits reflect the current temporary nametable selection; the PPU
// does not retain a separate historical copy of the last PPUCTRL write.
auto PPU::debugState() const -> std::array<u8, 13> {
  return {u8(var.data), u8(var.data >> 8), u8(scroll.data), u8(scroll.data >> 8),
    u8(io.busAddress), u8(io.busAddress >> 8), u8(scroll.fineX), u8(scroll.latch),
    u8(var.latchData), u8(sprite.oamAddress),
    u8(u8(scroll.nametable) | (io.vramIncrement == 32) << 2 | bool(io.spriteAddress) << 3 |
      bool(io.bgAddress) << 4 | (io.spriteHeight == 16) << 5 | u8(io.masterSelect) << 6 | u8(io.nmiEnable) << 7),
    u8(u8(io.grayscale) | u8(io.bgEdgeEnable) << 1 | u8(io.spriteEdgeEnable) << 2 |
      u8(io.bgEnable) << 3 | u8(io.spriteEnable) << 4 | u8(io.emphasis) << 5),
    u8(u8(sprite.spriteOverflow) << 5 | u8(io.spriteZeroHit) << 6 | u8(io.nmiFlag) << 7)};
}
