#include <nall/nall.hpp>
#include <nall/main.hpp>
#include <nall/gdb/server.cpp>
// nall defines NDEBUG from BUILD_RELEASE even when the compiler uses -UNDEBUG.
#undef NDEBUG
#include <cassert>
#include <iostream>
using namespace nall;
auto nall::main(Arguments) -> void {
  auto& server = GDB::server;
  unsigned reads = 0;
  server.hooks.read = [&](u64 address, u32 count) -> string {
    ++reads;
    assert(address == 0x7e0000 && count == 1);
    return "12";
  };
  auto command = [&](const string& packet) {
    bool reply = true;
    return server.processCommand(packet, reply);
  };
  for(auto invalid : {"m", "m0", "m,1", "m0,", "mxyz,1", "m0,xyz", "m0,1,2",
      "m100000000007e0000,1", "m10000000000000000,1", "m7e0000,10000000000000001",
      "m7e0000,100000000", "m7e0000,100000", "m7e0000,1:extra"}) {
    assert(command(invalid) == "E00");
    assert(reads == 0);
  }
  assert(command("m7e0000,1") == "12" && reads == 1);
  unsigned writes = 0;
  server.hooks.checkedWrite = [&](u64 address, const std::vector<u8>& bytes) -> string {
    ++writes;
    assert(address == 0x7e0000);
    assert(bytes.size() == 16 && bytes.front() == 0 && bytes.back() == 15);
    return "OK";
  };
  for(auto invalid : {"M", "M0", "M,1:00", "M0,:00", "Mxyz,1:00", "M0,xyz:00", "M0,1,2:00",
      "M10000000000000000,1:00", "M0,10000000000000001:00", "M0,1:0", "M0,1:0000",
      "M0,1:gg", "M0,1:00:00", "M0,100000:00"}) {
    assert(command(invalid) == "E00");
    assert(writes == 0);
  }
  assert(command("M7e0000,10:000102030405060708090a0b0c0d0e0f") == "OK" && writes == 1);
  server.reset();
  assert(!server.hooks.checkedWrite);
  std::cout << "PASS U4 packets: invalid hex, missing fields, excess fields, overflow and oversized reads never invoke storage\n";
}
