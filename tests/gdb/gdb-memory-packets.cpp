#include <nall/nall.hpp>
#include <nall/main.hpp>
#include <nall/gdb/server.cpp>
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
  server.reset();
  std::cout << "PASS U4 packets: invalid hex, missing fields, excess fields, overflow and oversized reads never invoke storage\n";
}
