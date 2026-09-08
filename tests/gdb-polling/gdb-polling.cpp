#include <nall/main.hpp>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace nall::GDB {
unsigned sleeps = 0;
auto usleep(unsigned) -> void { ++sleeps; }
struct Server {
  bool started = true, halted = false, requestDisconnect = false, noAckMode = false;
  bool disconnected = false;
  u32 messageCount = 0, polls = 0, delivered = 0, acknowledgements = 0;
  std::function<void(Server&)> next;
  auto isStarted() const -> bool { return started; }
  auto isHalted() const -> bool { return halted; }
  auto disconnectClient() -> void { disconnected = true; }
  auto resumeProgram() -> void { halted = false; }
  auto sendText(const char*) -> void { ++acknowledgements; }
  auto packet() -> void { ++messageCount; ++delivered; }
  auto update() -> void {
    if(++polls > 100000) throw std::runtime_error("polling exceeded bounded test limit");
    if(next) next(*this);
  }
  auto updateLoop() -> void;
};
#include "polling-loop.inc"

}
using nall::GDB::Server;
auto require(bool condition, const char* message) -> void {
  if(!condition) throw std::runtime_error(message);
}
auto nall::main(Arguments) -> void {
  unsigned failures = 0;
  auto test = [&](const char* name, auto scenario) {
    try { nall::GDB::sleeps = 0; scenario(); std::cout << "PASS " << name << '\n'; }
    catch(const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  };
  test("inactive server does not poll", [] {
    Server s; s.started = false; s.updateLoop();
    require(s.polls == 0, "inactive server polled");
    require(nall::GDB::sleeps == 0, "inactive server slept");
  });
  for(bool halted : {false, true}) {
    test(halted ? "halted idle budget" : "running idle budget", [=] {
      Server s; s.halted = halted; s.updateLoop();
      require(s.polls == (halted ? 10000u : 100u), "idle budget changed");
      require(nall::GDB::sleeps == (halted ? 20u : 0u), "idle sleep budget changed");
    });
    test(halted ? "halted queued messages" : "running queued messages", [=] {
      Server s; s.halted = halted;
      // A full initial budget of completed messages, followed by one more.
      // Exiting the inner loop after each message leaves this queue undrained.
      const u32 packets = halted ? 21u : 2u;
      s.next = [=](Server& server) { if(server.polls <= packets) server.packet(); };
      s.updateLoop();
      require(s.delivered == packets, "queued message deferred to another updateLoop call");
    });
    test(halted ? "halted continuous traffic is bounded" : "running continuous traffic is bounded", [=] {
      Server s; s.halted = halted;
      s.next = [](Server& server) { server.packet(); };
      s.updateLoop();
      require(s.polls <= (halted ? 20000u : 10100u), "reset budget exceeded");
      require(s.delivered >= 10000u, "polling exited before exercising the reset guard");
    });
  }
  test("message at idle boundary permits a following message", [] {
    Server s;
    s.next = [](Server& server) {
      if(server.polls == 100 || server.polls == 101) server.packet();
    };
    s.updateLoop();
    require(s.delivered == 2, "boundary packet did not extend the polling window");
  });
  test("resume immediately returns control for execution", [] {
    Server s; s.halted = true;
    s.next = [](Server& server) { server.packet(); server.resumeProgram(); };
    s.updateLoop();
    require(s.polls == 1 && !s.halted, "polled again after resume");
    require(nall::GDB::sleeps == 0, "slept after resume");
  });
  for(bool noAck : {false, true}) {
    test(noAck ? "disconnect without ack" : "disconnect with ack", [=] {
      Server s; s.halted = true; s.requestDisconnect = true; s.noAckMode = noAck;
      s.updateLoop();
      require(s.polls == 0 && s.disconnected && !s.halted && !s.requestDisconnect,
        "disconnect lifecycle changed");
      require(s.acknowledgements == (noAck ? 0u : 1u), "disconnect acknowledgement changed");
    });
  }
  std::cout.flush();
  std::cerr.flush();
  std::_Exit(failures ? 1 : 0);
}
