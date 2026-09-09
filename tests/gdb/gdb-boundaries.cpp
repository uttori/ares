#include <nall/nall.hpp>
#include <nall/main.hpp>
#include <nall/gdb/server.cpp>
// nall defines NDEBUG from BUILD_RELEASE even when the compiler uses -UNDEBUG.
#undef NDEBUG
#include <cassert>
#include <iostream>
using namespace nall;
using nall::GDB::server;
bool reply;
auto command(const string& data) -> string {
  reply = true;
  return server.processCommand(data, reply);
}
auto output() -> string {
  string result;
  for(auto byte : server.sendBuffer) result.append(char(byte));
  server.sendBuffer.clear();
  return result;
}
auto nall::main(Arguments) -> void {
  server.hooks.instructionBoundaryStop = true;
  server.onConnect();
  assert(command("?") == "" && !reply && server.isStopPending() && !server.isHalted());
  assert(output() == "");
  assert(command("g") == "E00");
  assert(!server.reportPC(0x808000) && server.isHalted());
  assert(output().find("$S05#"));
  assert(command("?") == "T05" && reply && server.isHalted());
  assert(!server.reportPC(0x808000) && output() == "");
  assert(command("Z0,808000,1") == "OK");
  command("s"); assert(!reply);
  assert(server.reportPC(0x808000));
  assert(!server.reportPC(0x808001) && server.isHalted());
  output();
  command("c"); assert(!reply);
  assert(server.reportPC(0x808001));
  assert(!server.reportPC(0x808000));
  output();
  command("c");
  assert(server.reportPC(0x808000)); // step off the breakpoint once
  assert(!server.reportPC(0x808000)); // hit it on the next loop
  output();
  command("c");
  assert(server.reportPC(0x808000, false)); // WAI next-PC is not a code breakpoint.
  server.haltProgram();
  assert(!server.reportPC(0x808000, false));
  output();
  unsigned resets = 0;
  server.hooks.emuReset = [&] { ++resets; };
  assert(command("qRcmd,zz") == "E00" && !resets);
  assert(command("qRcmd,726573657420626164") == "E00" && !resets);
  assert(command("qRcmd,72657365742068616c74") == "" && !reply && resets == 1);
  assert(!server.isHalted() && server.isStopPending() && output() == "");
  assert(command("g") == "E00");
  assert(!server.reportPC(0x008000) && server.isHalted());
  auto resetReply = output();
  assert(resetReply.find("$OK#") && !resetReply.find("$S05#"));
  assert(command("qRcmd,72657365742072756e") == "OK" && reply && resets == 2);
  assert(server.reportPC(0x008000));
  string interrupt{char(3)};
  server.onText(interrupt);
  assert(!server.isHalted() && server.isStopPending());
  assert(!server.reportPC(0x008005));
  output();
  server.onDisconnect();
  assert(server.reportPC(0x008005) && !server.isHalted() && !server.pendingResetReply);
  server.onConnect();
  assert(server.breakpoints.empty() && !server.singleStepActive);
  command("?"); assert(!reply);
  assert(!server.reportPC(0x008005));
  server.reset();
  assert(!server.hooks.emuReset && !server.hooks.instructionBoundaryStop);
  server.onConnect();
  assert(command("?") == "T05" && reply && server.isHalted()); // existing core behavior
  std::cout << "PASS U5: deferred attach, stable repeated stop, step/continue at breakpoint, reset halt/run, Ctrl-C, disconnect/reconnect, existing-core compatibility\n";
}
