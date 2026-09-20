#include <nall/gdb/capture-file.hpp>
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iostream>
using namespace ares_capture;
namespace fs = std::filesystem;

auto hex(const std::string& value) -> std::string {
  const char* digits = "0123456789abcdef";
  std::string out;
  for(unsigned char byte : value) { out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
auto waitFor(FileCapture& sink, Capture& capture, const std::string& expected) -> std::string {
  for(unsigned i = 0; i < 10000; ++i) {
    auto status = sink.command(capture, "status:" + capture.sessionId(), true);
    if(status.find("\"fileState\":\"" + expected + "\"") != std::string::npos) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(false && "native sink timeout"); return "";
}
auto release(FileCapture& sink, Capture& capture) -> void {
  for(unsigned i = 0; i < 10000; ++i) {
    auto result = sink.command(capture, "release:" + capture.sessionId(), true);
    if(result != "E09") { assert(result[0] == '{' && !sink.owns() && capture.ready()); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(false && "native sink failed to release");
}
auto start(FileCapture& sink, Capture& capture, const fs::path& path, const std::string& scope = "instructions", bool profile = false) -> std::string {
  auto request = "start:8388608:" + scope + ":" + hex(path.string()) + ":" + std::string(64, 'a') + ":" + hex("test\"producer") + ":" + hex("native-file-test");
  if(profile) request += ":profile";
  assert(sink.command(capture, request, false) == "E09");
  auto result = sink.command(capture, request, true);
  assert(result[0] == '{');
  return result;
}
auto produce(Capture& capture, unsigned count) -> void {
  for(unsigned i = 0; i < count; ++i) {
    assert(capture.ready());
    capture.begin(0x808000, {}, i); capture.fetch(0xea); capture.end();
  }
}
auto read(const fs::path& path) -> std::vector<uint8_t> {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}
auto le(const std::vector<uint8_t>& bytes, size_t offset, unsigned count) -> uint64_t {
  uint64_t value = 0;
  for(unsigned i = 0; i < count; ++i) value |= uint64_t(bytes.at(offset + i)) << (8 * i);
  return value;
}
// Independent bitwise CRC reader: never trusts sink checkpoints without parsing
// preceding ACT2 envelopes and all event sequence numbers.
auto verify(const fs::path& path, uint64_t expected, bool complete) -> void {
  auto bytes = read(path);
  size_t offset = 0;
  uint64_t events = 0, sequence = 0;
  bool pending = false, terminal = false;
  while(offset < bytes.size()) {
    std::string magic(bytes.begin() + offset, bytes.begin() + offset + 4);
    const bool chunk = magic == "ACT2";
    size_t length = chunk ? 64 + le(bytes, offset + 48, 4) : 12 + le(bytes, offset + 4, 4);
    assert(offset + length <= bytes.size());
    uint32_t crc = ~uint32_t(0);
    for(size_t i = chunk ? 0 : 12; i < length; ++i) {
      uint8_t byte = bytes.at(offset + i);
      if(chunk && i >= 56 && i < 60) byte = 0;
      crc ^= byte;
      for(unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    }
    assert(~crc == le(bytes, offset + (chunk ? 56 : 8), 4));
    if(chunk) {
      assert(!pending && !terminal && ++sequence == le(bytes, offset + 24, 8));
      assert(events + 1 == le(bytes, offset + 32, 8));
      unsigned count = 0;
      for(size_t i = 64; i < length;) {
        assert(++events == le(bytes, offset + i + 8, 8)); ++count;
        i += le(bytes, offset + i + 4, 4);
      }
      assert(count == le(bytes, offset + 40, 4)); pending = true;
    } else if(magic == "ACK2") { assert(pending); pending = false; }
    else if(magic == "END2") { assert(!pending && offset + length == bytes.size()); terminal = true; }
    else assert(magic == "ACS2" && offset == 0);
    offset += length;
  }
  assert(events == expected && terminal == complete && !pending);
}
auto main(int argc, char** argv) -> int {
  assert(argc == 2);
  fs::path directory = fs::absolute(argv[1]); fs::create_directories(directory);
  {
    Capture capture; FileCapture sink;
    assert(sink.command(capture, "start:8388608:instructions:c080:" + std::string(64, 'a') + ":61:62", true) == "E02");
    assert(capture.sessionId().empty());
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "profile.act2";
    start(sink, capture, path, "instructions", true); waitFor(sink, capture, "writing");
    // All fetched lengths exercise different CRC word/tail boundaries. The
    // independent reader checks both compiler-selected and forced-portable CRC.
    for(unsigned i = 0; i < 8000; ++i) {
      assert(capture.ready()); capture.begin(0x808000, {}, i);
      for(unsigned n = 0; n <= i % 4; ++n) capture.fetch(uint8_t(n));
      capture.end();
    }
    sink.command(capture, "stop:" + capture.sessionId(), true);
    auto status = waitFor(sink, capture, "complete");
    assert(status.find("\"fileProfile\":") != std::string::npos && capture.sealNanoseconds > 0);
    verify(path, 8000, true);
    start(sink, capture, directory / "unprofiled.act2");
    assert(!capture.profileSeals && capture.sealNanoseconds == 0);
    sink.command(capture, "stop:" + capture.sessionId(), true);
    status = waitFor(sink, capture, "complete");
    assert(status.find("fileProfile") == std::string::npos);
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "complete.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    FileCapture::holdSync = true;
    produce(capture, 20000); // More than four chunks: proves batched release.
    sink.service(capture);
    assert(capture.status().find("\"acceptedThrough\":\"0\"") != std::string::npos);
    auto before = capture.occupiedBytes();
    for(unsigned i = 0; i < 100; ++i) sink.service(capture);
    assert(capture.occupiedBytes() == before); // Slow disk cannot release or lose data.
    assert(sink.command(capture, "start:8388608:instructions:00:" + std::string(64, 'a') + ":61:62", true) == "E09");
    FileCapture::holdSync = false;
    sink.command(capture, "stop:" + capture.sessionId(), true);
    auto status = waitFor(sink, capture, "complete");
    assert(capture.complete() && capture.occupiedBytes() == 0 && !sink.owns());
    assert(status.find("\"fileBatches\":\"2\"") != std::string::npos);
    assert(!fs::exists(path.string() + ".partial")); verify(path, 20000, true);
    // Empty rearm must publish a valid header/footer without inventing a chunk.
    auto empty = directory / "empty.act2";
    start(sink, capture, empty); waitFor(sink, capture, "writing");
    sink.command(capture, "stop:" + capture.sessionId(), true); waitFor(sink, capture, "complete");
    verify(empty, 0, true);
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "slow-full.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    FileCapture::holdSync = true;
    unsigned events = 0;
    while(capture.ready()) {
      capture.begin(0x808000, {}, events++); capture.fetch(0xea); capture.end();
      assert(capture.occupiedBytes() <= capture.allocatedBytes());
    }
    assert(events > 20000);
    sink.service(capture);
    assert(!capture.ready());
    FileCapture::holdSync = false;
    sink.command(capture, "stop:" + capture.sessionId(), true); waitFor(sink, capture, "complete");
    verify(path, events, true);
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "sync-failed.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    FileCapture::failSyncAt = FileCapture::syncCalls + 1;
    produce(capture, 100); sink.command(capture, "stop:" + capture.sessionId(), true);
    waitFor(sink, capture, "incomplete");
    assert(!capture.ready() && capture.occupiedBytes() > 0 && !fs::exists(path));
    assert(capture.status().find("\"acceptedThrough\":\"0\"") != std::string::npos);
    release(sink, capture);
    FileCapture::failSyncAt = -1;
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "collision.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    std::ofstream(path) << "preserve"; // Race the final publication after opening.
    produce(capture, 100); sink.command(capture, "stop:" + capture.sessionId(), true);
    auto status = waitFor(sink, capture, "incomplete");
    assert(status.find("\"state\":\"incomplete\"") != std::string::npos);
    assert(read(path) == std::vector<uint8_t>({'p','r','e','s','e','r','v','e'}));
  }
  for(const auto& name : {"existing", "missing/parent", "partial"}) {
    Capture capture; FileCapture sink;
    auto path = directory / name;
    if(std::string(name) == "existing") std::ofstream(path) << "preserve";
    if(std::string(name) == "partial") std::ofstream(path.string() + ".partial") << "preserve";
    start(sink, capture, path); waitFor(sink, capture, "incomplete");
    assert(capture.incomplete() && !capture.ready());
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "cancel-finalize.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    FileCapture::holdSync = true;
    sink.command(capture, "stop:" + capture.sessionId(), true);
    for(unsigned i = 0; !FileCapture::waitingSync; ++i) {
      assert(i < 10000); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sink.command(capture, "abort:" + capture.sessionId(), true);
    assert(sink.command(capture, "release:" + capture.sessionId(), true) == "E09");
    assert(!capture.ready() && sink.owns());
    FileCapture::holdSync = false;
    release(sink, capture);
    assert(!fs::exists(path));
  }
  {
    Capture capture; FileCapture sink;
    auto path = directory / "reset.act2";
    start(sink, capture, path); waitFor(sink, capture, "writing");
    produce(capture, 100); capture.fail("state-load"); sink.service(capture);
    waitFor(sink, capture, "incomplete"); assert(!fs::exists(path));
    release(sink, capture);
  }
  std::cout << "native file: batched ownership, slow sync, failure, no-clobber, empty rearm, cancellation passed\n";
}
