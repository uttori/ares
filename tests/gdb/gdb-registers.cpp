#include <nall/nall.hpp>
#include <nall/main.hpp>
#include <nall/gdb/server.cpp>
using namespace nall;
#include <ares/types.hpp>
#include <component/processor/wdc65816/wdc65816.hpp>
// nall defines NDEBUG from BUILD_RELEASE even when the compiler uses -UNDEBUG.
#undef NDEBUG
#include <cassert>
#include <iostream>
using namespace nall;
namespace ares::SuperFamicom {
struct { WDC65816::Registers r; } cpu;
struct { auto power(bool) -> void {} } system;
#include <sfc/system/gdb.cpp>
}
using namespace ares::SuperFamicom;
auto command(const string& packet) -> string {
  bool reply = true;
  return nall::GDB::server.processCommand(packet, reply);
}
auto nall::main(Arguments) -> void {
  using nall::GDB::server;
  installGdbHooks();
  cpu.r.p = 0; cpu.r.e = 0;
  assert(command("P0=3412") == "OK" && cpu.r.a.w == 0x1234);
  assert(command("p0") == "3412");
  assert(command("P5=ab") == "OK" && cpu.r.b == 0xab);
  assert(command("P6=7e") == "OK");
  assert(command("P7=0080") == "OK" && cpu.r.pc.d == 0x7e8000);
  for(auto invalid : {"P0=12", "P0=zzzz", "P0=123456", "P0", "Pa=0000", "Pz=3412", "P100000000=3412"}) assert(command(invalid) == "E00");
  assert(cpu.r.a.w == 0x1234);
  assert(command("P1=cdab") == "OK");
  assert(command("P2=efbe") == "OK");
  assert(command("P3=4523") == "OK");
  assert(command("P9=01") == "OK");
  assert(cpu.r.p.m && cpu.r.p.x && cpu.r.s.w == 0x0145);
  assert(cpu.r.x.w == 0xcd && cpu.r.y.w == 0xef);
  assert(cpu.r.a.w == 0x1234); // 8-bit accumulator mode preserves B.
  auto original = command("g");
  assert(original.size() == 32);
  assert(command("G00") == "E00" && command("g") == original);
  assert(command("Gzzzz0000000000000000000000000000") == "E00" && command("g") == original);
  assert(command("G3412cdabefbe45236745017e00800000") == "OK");
  assert(cpu.r.x.w == 0xabcd && cpu.r.y.w == 0xbeef && cpu.r.s.w == 0x2345 && !cpu.r.e);
  assert(command("g") == "3412cdabefbe45236745017e00800000");
  assert(command("G3412cdabefbe45236745017e00800001") == "OK");
  assert(cpu.r.x.w == 0xcd && cpu.r.y.w == 0xef && cpu.r.s.w == 0x145 && cpu.r.e);
  auto xml = server.hooks.targetXML();
  string combined;
  for(u32 offset = 0; offset < xml.size(); offset += 7) {
    auto part = command({"qXfer:features:read:target.xml:", hex(offset), ",7"});
    assert(part.size() <= 8);
    assert(part(0) == (offset + 7 < xml.size() ? 'm' : 'l'));
    combined.append(part.slice(1));
  }
  assert(combined == xml && !xml.find("uttori"));
  assert(command("qXfer:features:read:target.xml:ffffffffffffffff,7") == "l");
  assert(command("qXfer:features:read:target.xml:0,0") == "E00");
  assert(command("qXfer:features:read:target.xml:no,7") == "E00");
  assert(command("qXfer:features:read:missing.xml:0,7") == "");
  server.reset();
  assert(!server.hooks.registersLittleEndian && !server.hooks.regRead);
  // The existing big-endian numeric write callback contract remains intact.
  u64 seen = 0;
  server.hooks.regWrite = [&](u32, u64 value) { seen = value; return true; };
  assert(command("P0=12345678") == "OK" && seen == 0x12345678);
  std::cout << "PASS U3: little-endian p/P/g/G, banked PC, E/M/X invariants, atomic validation, chunked XML, reset, big-endian compatibility\n";
}
