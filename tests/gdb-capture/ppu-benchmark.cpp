#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <nall/main.hpp>
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>
using namespace ares;
using namespace ares::SuperFamicom;
using nall::GDB::server;

struct Host : ares::Platform {
  auto pak(Node::Object) -> std::shared_ptr<vfs::directory> override { return std::make_shared<vfs::directory>(); }
};

// Measure fixed work on the real CPU/PPU coroutine. Stream modes include native
// encoding/ack only; file mode also includes worker drain and publication. No
// TCP or consumer decoding is timed.
auto nall::main(Arguments args) -> void {
  Host host; platform = &host;
  bool accurate = args.find("accurate"), baseline = args.find("baseline"), files = args.find("files");
  ppu.setAccurate(accurate);
  option("Deterministic Entropy", "true");
  Node::System root;
  assert(load(root, "[Nintendo] Super Famicom (NTSC)"));
  std::ostringstream results;
  results << "BENCH_BEGIN\n{\"ppu\":\"" << (accurate ? "accurate" : "performance") << "\",\"iterations\":200000,\"runs\":[\n";
  auto outputDirectory = std::filesystem::temp_directory_path() / ("ares-file-bench-" + std::to_string(std::random_device{}()));
  if(files) std::filesystem::create_directory(outputDirectory);
  auto encode = [](const std::string& value) {
    const char* digits = "0123456789abcdef"; std::string out;
    for(unsigned char byte : value) { out += digits[byte >> 4]; out += digits[byte & 15]; }
    return out;
  };
  unsigned modes = baseline ? 2 : 3;
  if(files) modes = 4;
  bool comma = false;
  for(bool blank : {true, false}) for(unsigned mode = 0; mode < modes; ++mode) for(unsigned sample = 0; sample < 6; ++sample) {
    server.capture = {}; root->power(false);
    server.onConnect(); bool reply = true; server.processCommand("?", reply);
    for(unsigned i = 0; !server.isHalted(); ++i) { assert(i < 64); root->run(); }
    server.onDisconnect();
    bool done = false, setup = false;
    uint64_t beginClock = 0;
    cpu.create(ares::SuperFamicom::system.cpuFrequency(), [&] {
      if(!setup) {
        cpu.write(0x2100, blank ? 0x80 : 0);
        cpu.write(0x2115, 0x80); cpu.write(0x2116, 0); cpu.write(0x2117, 0);
        cpu.write(0x2121, 0); cpu.write(0x2102, 0); cpu.write(0x2103, 0);
        setup = true; scheduler.exit(Event::Step);
      }
      if(!done) {
        beginClock = cpu.captureClock;
        const uint16_t ports[] = {0x2118, 0x2119, 0x2122, 0x2122, 0x2104, 0x2104};
        for(unsigned i = 0; i < 200000; ++i) cpu.write(ports[i % 6], uint8_t(i));
        done = true;
      }
      scheduler.exit(Event::Step);
    });
    scheduler.power(cpu); root->run(); assert(setup && !done);
    auto output = outputDirectory / (std::to_string(blank) + "-" + std::to_string(sample) + ".act2");
    if(mode == 3) {
      auto request = "start:8388608:bus-ppu:" + encode(output.string()) + ":" + std::string(64, 'a') + ":61726573:62656e6368";
      if(args.find("profile")) request += ":profile";
      assert(server.captureFile.command(server.capture, request, true)[0] == '{');
    } else if(mode) assert(server.capture.command(mode == 1 ? "start:4718656:bus" : "start:4718656:bus-ppu")[0] == '{');
    uint64_t events = 0, bytes = 0;
    std::string filePosition;
    auto drain = [&] {
      while(server.capture.size) {
        auto& front = server.capture.queue[server.capture.head];
        auto seq = std::to_string(front.sequence), through = std::to_string(front.first + front.count - 1);
        auto encoded = server.capture.command("read:" + server.capture.sessionId() + ":" + seq);
        assert(!encoded.empty() && encoded[0] != 'E');
        bytes += encoded.size() / 2; events += front.count;
        assert(server.capture.command("ack:" + server.capture.sessionId() + ":" + seq + ":" + through)[0] == '{');
      }
    };
    const auto started = std::chrono::steady_clock::now();
    for(unsigned i = 0; !done; ++i) {
      assert(i < 1000000); root->run();
      if(mode == 3) {
        server.captureFile.service(server.capture);
        if(server.capture.incomplete()) std::cerr << server.capture.status() << '\n';
        assert(!server.capture.incomplete());
        // This harness has no frontend event loop. Let the OS schedule the I/O
        // worker during backpressure instead of exhausting a busy-spin guard.
        if(!server.capture.ready()) std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      else if(mode) drain();
    }
    if(mode == 3) {
      auto status = server.captureFile.command(server.capture, "stop:" + server.capture.sessionId(), true);
      for(unsigned i = 0; status.find("\"fileState\":\"complete\"") == std::string::npos; ++i) {
        assert(i < 100000 && !server.capture.incomplete());
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        status = server.captureFile.command(server.capture, "status:" + server.capture.sessionId(), true);
      }
      filePosition = status;
      events = server.capture.produced; bytes = std::filesystem::file_size(output);
      assert(events == 200000 && server.capture.complete());
    } else if(mode) {
      server.capture.command("stop:" + server.capture.sessionId()); drain();
      assert(server.capture.state == "complete" && events == 200000);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    // First sample warms each mode; keep it visible so the median calculation is auditable.
    if(comma) results << ",\n";
    comma = true;
    results << "{\"blank\":" << (blank ? "true" : "false") << ",\"mode\":" << mode << ",\"sample\":" << sample
      << ",\"ms\":" << ms << ",\"events\":" << events << ",\"bytes\":" << bytes << ",\"masterClocks\":" << cpu.captureClock - beginClock;
    if(!filePosition.empty()) results << ",\"position\":" << filePosition;
    results << '}';
  }
  results << "\n]}\nBENCH_END\n";
  std::cout << results.str();
  root->unload();
  if(files) std::filesystem::remove_all(outputDirectory);
}
