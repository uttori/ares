namespace Board {
#include "ic/jv001.hpp"
#include "ic/txc05-00002-010.hpp"

struct Interface {
  VFS::Pak pak;

  static auto create(string manifest) -> Interface*;

  virtual ~Interface() = default;

  virtual auto load() -> void {}
  virtual auto save() -> void {}
  virtual auto unload() -> void {}

  virtual auto main() -> void;
  virtual auto tick() -> void;

  // Debug inspection never routes through device reads. Unsupported boards fail closed.
  virtual auto debugPRGOffset(n16 address) -> maybe<u32> { return {}; }
  virtual auto debugRead(const string& space, u32 address) -> maybe<n8> { return {}; }
  // Physical PPU storage: 1 CHR-ROM, 2 CHR-RAM, 3 CIRAM. No reads or A12/IRQ callbacks.
  struct DebugPPUAddress { u8 kind; u32 offset; bool writable; };
  virtual auto debugPPUAddress(n14 address) -> maybe<DebugPPUAddress> { return {}; }
  // Mapper ID, selector, PRG/CHR modes, eight registers, RAM flags, mirroring and IRQ latch/enable.
  // MMC3 counter, latch, enable, board output, A12 level, filter delay, pulse counter, revision.
  virtual auto debugIRQ() -> std::array<u8, 8> { return {}; }
  virtual auto debugMapper() -> std::array<u8, 18> { std::array<u8, 18> state{}; state[0] = 255; return state; }

  virtual auto readPRG(n32 address, n8 data) -> n8 { return data; }
  virtual auto writePRG(n32 address, n8 data) -> void {}

  virtual auto readCHR(n32 address, n8 data) -> n8 { return data; }
  virtual auto writeCHR(n32 address, n8 data) -> void {}

  virtual auto scanline(n32 y) -> void {}

  virtual auto power() -> void {}

  virtual auto ppuAddressBus(n14 address) -> void {}

  virtual auto serialize(serializer&) -> void {}

protected:
  auto load(Memory::Readable<n8>& memory, string name) -> bool;
  auto load(Memory::Writable<n8>& memory, string name) -> bool;
  auto save(Memory::Writable<n8>& memory, string name) -> bool;
};

}
