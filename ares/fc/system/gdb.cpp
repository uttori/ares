// Famicom hooks use the existing RSP transport. Device I/O is never read for debugger inspection.
namespace {
#include "gdb-ppu.cpp"
auto gdbBytes(u64 value, u32 size) -> string {
  string result;
  for(u32 byte : range(size)) result.append(hex(value >> (byte * 8) & 255, 2, '0'));
  return result;
}

auto installGdbHooks() -> void {
  using nall::GDB::server;
  server.hooks.instructionBoundaryStop = true;
  server.hooks.instructionTrace = true;
  server.hooks.instructionTraceDetails = true;
  server.capture.famicom = true;
  server.hooks.emuReset = [] { system.power(true); };
  server.hooks.registersLittleEndian = true;
  server.hooks.targetXML = []() -> string {
    return "<target version=\"1.0\"><feature name=\"dev.ares.ricoh2a03.core\">"
      "<reg name=\"a\" bitsize=\"8\" regnum=\"0\"/>"
      "<reg name=\"x\" bitsize=\"8\" regnum=\"1\"/>"
      "<reg name=\"y\" bitsize=\"8\" regnum=\"2\"/>"
      "<reg name=\"s\" bitsize=\"8\" regnum=\"3\"/>"
      "<reg name=\"p\" bitsize=\"8\" regnum=\"4\"/>"
      "<reg name=\"pc\" bitsize=\"16\" regnum=\"5\"/>"
      "</feature></target>";
  };
  server.hooks.regRead = [](u32 index) -> string {
    switch(index) {
    case 0: return gdbBytes(cpu.A, 1);
    case 1: return gdbBytes(cpu.X, 1);
    case 2: return gdbBytes(cpu.Y, 1);
    case 3: return gdbBytes(cpu.S, 1);
    case 4: return gdbBytes(u8(cpu.P) | 0x20, 1);
    case 5: return gdbBytes(cpu.PC, 2);
    }
    return {};
  };
  server.hooks.regReadGeneral = []() -> string {
    string result;
    for(u32 index : range(6)) result.append(server.hooks.regRead(index));
    return result;
  };
  server.hooks.regWrite = [](u32 index, u64 value) -> bool {
    switch(index) {
    case 0: cpu.A = value; break;
    case 1: cpu.X = value; break;
    case 2: cpu.Y = value; break;
    case 3: cpu.S = value; break;
    case 4: cpu.P = value; break;
    case 5: cpu.PC = value; break;
    default: return false;
    }
    return true;
  };
  server.hooks.regWriteGeneral = [](const string& data) {
    for(u32 index : range(5)) server.hooks.regWrite(index, data.slice(index * 2, 2).hex());
    cpu.PC = data.slice(10, 2).hex() | data.slice(12, 2).hex() << 8;
  };
  server.hooks.namedMemoryRead = [](const string& space, u64 address, u32 count) -> string {
    string result;
    if(space == "nes.ppu-snapshot") {
      auto bytes = gdbPPUSnapshot();
      if(bytes.empty()) return "E09";
      for(u64 at = address; at < bytes.size() && at - address < count; ++at) result.append(gdbBytes(bytes[at], 1));
      return result;
    }
    if(space == "nes.mapping") {
      if(!cartridge.board || address || count != 22) return "E01";
      auto state = cartridge.board->debugMapper();
      for(auto byte : state) result.append(gdbBytes(byte, 1));
      auto offset = cartridge.board->debugPRGOffset(cpu.PC);
      result.append(gdbBytes(offset ? *offset : 0xffffffff, 4));
      return result;
    }
    for(u32 index : range(count)) {
      u64 at = address + index;
      maybe<n8> value;
      if(space == "nes.ram" && at < 0x800) value = cpu.ram.read(at);
      else if(space == "nes.cpu" && at < 0x2000) value = cpu.ram.read(at & 0x7ff);
      else if(cartridge.board && at <= 0xffffffff) value = cartridge.board->debugRead(space, at);
      if(!value) break;
      result.append(gdbBytes(*value, 1));
    }
    return result;
  };
  server.hooks.read = [](u64 address, u32 count) { return server.hooks.namedMemoryRead("nes.cpu", address, count); };
  // RSP execute locations above 16 bits explicitly select physical PRG bytes.
  // CPU-only addresses remain useful for RAM code and intentional any-bank stops.
  server.hooks.breakpointMatches = [](u64 address, u64 pc) -> bool {
    if(address <= 0xffff) return address == pc;
    if(address < 0x100000000ull || !cartridge.board || (address & 0xffff) != pc) return false;
    auto offset = cartridge.board->debugPRGOffset(pc);
    return offset && ((address - 0x100000000ull) >> 16) == *offset;
  };
}
}
