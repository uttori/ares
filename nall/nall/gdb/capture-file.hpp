#pragma once

#include <nall/gdb/capture.hpp>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <cerrno>
#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace ares_capture {
// Private downstream ACS2 file sink. Only service()/command() touch Capture, on
// its owning thread. The worker owns disk I/O and one copied four-chunk batch;
// it never touches emulation, debugger state, or producer queue storage.
class FileCapture {
public:
#if defined(ARES_CAPTURE_FILE_TEST)
  // Fault injection is compiled only into the standalone native sink tests.
  static inline std::atomic<int> syncCalls{0}, failSyncAt{-1};
  static inline std::atomic<bool> holdSync{false}, waitingSync{false};
#endif
private:
  enum class Phase { Preparing, Idle, Writing, Complete, Incomplete };
  static constexpr size_t batchChunks = 4;
  // Four raw chunks plus bounded ACK2 frames fit inside Capture::overheadBytes.
  static constexpr size_t batchBytes = batchChunks * (Capture::chunkBytes + 256);
  struct Writer {
    std::string path, partial, header, session, terminal;
    std::vector<uint8_t> bytes;
    uint64_t sequence{}, event{}, writtenBytes{}, batches{};
    // Worker counters are read only after acquiring Idle/Complete, just like bytes.
    bool profile{};
    uint64_t writeNs{}, syncNs{};
    std::atomic<Phase> phase{Phase::Preparing};
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable wake;
    bool job{}, quit{}, finish{}, cancel{};
    std::thread thread;

    Writer(std::string path, std::string header, std::string session, bool profile)
    : path(std::move(path)), partial(this->path + ".partial"), header(std::move(header)), session(std::move(session)), profile(profile) {
      bytes.reserve(batchBytes);
      thread = std::thread([this] { run(); });
    }
    ~Writer() {
      // Destruction cannot free a buffer still used by write(). Cancellation
      // leaves the exclusive partial artifact available for prefix recovery.
      { std::lock_guard<std::mutex> lock(mutex); quit = true; }
      wake.notify_one();
      if(thread.joinable()) thread.join();
    }
    static auto sync(int fd) -> bool {
#if defined(ARES_CAPTURE_FILE_TEST)
      waitingSync = true;
      while(holdSync.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      waitingSync = false;
      if(++syncCalls == failSyncAt.load()) return false;
#endif
#if defined(_WIN32)
      return _commit(fd) == 0;
#else
      int result;
      do { result = fsync(fd); } while(result < 0 && errno == EINTR);
      return result == 0;
#endif
    }
    static auto closeFile(int fd) -> void {
#if defined(_WIN32)
      _close(fd);
#else
      close(fd);
#endif
    }
    auto writeAll(int fd, const uint8_t* data, size_t count) -> bool {
      while(count) {
#if defined(_WIN32)
        auto amount = _write(fd, data, unsigned(count));
#else
        auto amount = write(fd, data, count);
#endif
        if(amount < 0 && errno == EINTR) continue;
        if(amount <= 0) return false;
        data += amount; count -= amount; writtenBytes += amount;
      }
      return true;
    }
    auto writeFrame(int fd, const char* magic, const std::string& json) -> bool {
      auto frame = FileCapture::frame(magic, json);
      return writeAll(fd, frame.data(), frame.size());
    }
    auto writeBatch(int fd) -> bool {
      auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if(!writeAll(fd, bytes.data(), bytes.size())) return false;
      auto written = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if(!sync(fd)) return false;
      if(profile) {
        writeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(written - started).count();
        syncNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - written).count();
      }
      return true;
    }
    auto run() -> void {
      int fd = -1;
      try {
        // Never truncate an existing destination, partial artifact, or symlink.
        if(std::filesystem::symlink_status(std::filesystem::u8path(path)).type() != std::filesystem::file_type::not_found)
          throw 0;
#if defined(_WIN32)
        fd = _wopen(std::filesystem::u8path(partial).c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
        if(fd < 0 || !writeFrame(fd, "ACS2", header) || !sync(fd)) throw 0;
        phase.store(Phase::Idle, std::memory_order_release);
        for(;;) {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [this] { return job || quit; });
          if(quit || cancel) { phase.store(Phase::Incomplete, std::memory_order_release); break; }
          job = false;
          lock.unlock();
          if(finish) {
            if(cancelled.load()) throw 0;
            if(!writeFrame(fd, "END2", terminal) || !sync(fd)) throw 0;
            closeFile(fd); fd = -1;
            // Hard-link publication is no-clobber even if another process races
            // us after start. No directory power-loss durability is claimed.
            if(cancelled.load()) throw 0;
            std::filesystem::create_hard_link(std::filesystem::u8path(partial), std::filesystem::u8path(path));
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::u8path(partial), ignored);
            phase.store(Phase::Complete, std::memory_order_release);
            return;
          }
          // ACT2/ACK2 pairs keep the existing bounded recovery format. One fsync
          // covers all preceding bytes; only its success permits batch release.
          // Recovery still validates every CRC and contiguous checkpoint, so a
          // torn/reordered unsynced tail never skips a missing earlier chunk.
          if(!writeBatch(fd)) throw 0;
          ++batches;
          phase.store(Phase::Idle, std::memory_order_release);
        }
      } catch(...) { phase.store(Phase::Incomplete, std::memory_order_release); }
      if(fd >= 0) closeFile(fd);
    }
  };
  std::unique_ptr<Writer> writer;
  uint64_t committedBytes{}, committedBatches{}, copyNs{}, committedWriteNs{}, committedSyncNs{};

public:
  // A live file session owns delivery/acknowledgement exclusively. Streamed
  // reads or a new start must not discard its retained queue or pending I/O.
  auto owns() const -> bool {
    return writer && writer->phase.load(std::memory_order_acquire) != Phase::Complete;
  }
  auto service(Capture& capture) -> void {
    if(!writer) return;
    auto& w = *writer;
    auto phase = w.phase.load(std::memory_order_acquire);
    if(phase == Phase::Incomplete) { capture.failNative("native-file-io-or-cancel"); return; }
    if(phase != Phase::Idle) return;
    // Acquire of Idle publishes the completed batch and worker counters. The
    // next job's release through the mutex hands buffer ownership back.
    committedBytes = w.writtenBytes; committedBatches = w.batches;
    committedWriteNs = w.writeNs; committedSyncNs = w.syncNs;
    if(w.sequence) {
      if(!capture.acknowledgeNative(w.sequence, w.event)) { capture.fail("native-file-ack"); }
      w.sequence = w.event = 0;
    }
    w.bytes.clear();
    if(capture.incomplete()) {
      submit(w, false, true);
      return;
    }
    auto started = w.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto sealsBefore = capture.sealNanoseconds;
    for(size_t i = 0; i < batchChunks; ++i) {
      auto* chunk = capture.nativeChunk(i);
      if(!chunk) break;
      w.bytes.insert(w.bytes.end(), chunk->bytes.begin(), chunk->bytes.begin() + chunk->used);
      w.sequence = chunk->sequence; w.event = chunk->first + chunk->count - 1;
      auto ack = frame("ACK2", "{\"session\":" + quote(w.session) + ",\"chunkThrough\":" + quote(std::to_string(w.sequence)) + ",\"eventThrough\":" + quote(std::to_string(w.event)) + "}");
      w.bytes.insert(w.bytes.end(), ack.begin(), ack.end());
    }
    // Exclude seals triggered while collecting a partial tail: these already
    // belong to the producer checksum counter. Copy includes ACK frame encoding.
    if(w.profile) copyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count() - (capture.sealNanoseconds - sealsBefore);
    if(!w.bytes.empty()) submit(w, false, false);
    else if(capture.complete()) {
      w.terminal = capture.status();
      submit(w, true, false);
    }
  }

  auto command(Capture& capture, const std::string& request, bool halted) -> std::string {
    auto fields = split(request);
    if(fields.empty()) return "E01";
    if(fields[0] == "capabilities" && fields.size() == 2) {
      auto cap = capture.command(capability(fields[1]));
      if(cap.empty() || cap[0] != '{') return "E01";
      cap.pop_back(); return cap + ",\"nativeFile\":true}";
    }
    if(fields[0] == "start" && (fields.size() == 7 || (fields.size() == 8 && fields[7] == "profile"))) {
      if(!halted || owns()) return "E09";
      std::string path, id, revision;
      if(!unhex(fields[3], path, 4096) || !unhex(fields[5], id, 256) || !unhex(fields[6], revision, 1024)
        || fields[4].size() != 64 || fields[4].find_first_not_of("0123456789abcdef") != std::string::npos
        || !std::filesystem::u8path(path).is_absolute() || capability(fields[2]).empty()) return "E02";
      writer.reset(); committedBytes = committedBatches = copyNs = committedWriteNs = committedSyncNs = 0;
      auto start = "start:" + fields[1];
      if(fields[2] != "instructions") start += ":" + fields[2];
      auto result = capture.command(start);
      if(result.empty() || result[0] != '{') return result;
      capture.profileSeals = fields.size() == 8; capture.sealNanoseconds = 0;
      auto clock = capture.clockRatio();
      auto header = "{\"format\":\"uttori-ares-capture\",\"version\":2,\"session\":" + quote(capture.sessionId()) +
        ",\"scope\":" + quote(capture.scopeName()) + ",\"ownership\":\"durable-prefix\",\"compression\":\"none\",\"checksum\":\"crc32c\",\"maxChunkBytes\":262144,\"allocatedBytes\":" + std::to_string(capture.allocatedBytes()) +
        ",\"clock\":{\"id\":\"snes.master\",\"numerator\":" + quote(std::to_string(clock.first)) + ",\"denominator\":" + quote(std::to_string(clock.second)) +
        "},\"architecture\":\"snes.65816\",\"addressSpace\":\"snes.cpu\",\"romContentSha256\":" + quote(fields[4]) +
        ",\"producer\":{\"id\":" + quote(id) + ",\"revision\":" + quote(revision) + "}}";
      try { writer = std::make_unique<Writer>(path, header, capture.sessionId(), capture.profileSeals); }
      catch(...) { capture.fail("native-file-start"); return "E04"; }
      return status(capture);
    }
    if(!writer || fields.size() != 2 || fields[1] != writer->session || fields[1] != capture.sessionId()) return "E05";
    if(fields[0] != "status" && fields[0] != "stop" && fields[0] != "abort" && fields[0] != "release") return "E08";
    if(fields[0] == "release") {
      service(capture);
      // A cancelled in-flight batch still owns storage. Do not report release
      // until the worker has stopped; callers can retry while control stays live.
      if(writer->phase.load(std::memory_order_acquire) != Phase::Incomplete) return "E09";
      writer.reset();
      return capture.command("release:" + fields[1]);
    }
    if(fields[0] == "abort" && writer->phase.load(std::memory_order_acquire) != Phase::Complete) {
      writer->cancelled.store(true);
      capture.failNative("consumer-aborted");
    } else if(fields[0] != "status") capture.command(fields[0] + ":" + fields[1]);
    service(capture);
    return status(capture);
  }

private:
  static auto submit(Writer& w, bool finish, bool cancel) -> void {
    { std::lock_guard<std::mutex> lock(w.mutex);
      w.finish = finish; w.cancel = cancel; w.job = true;
      w.phase.store(Phase::Writing, std::memory_order_release);
    }
    w.wake.notify_one();
  }
  auto status(const Capture& capture) const -> std::string {
    auto phase = writer->phase.load(std::memory_order_acquire);
    auto result = capture.status();
    // Producer drain alone is not file completion: END2 must be synced and the
    // destination published before clients may import the archive.
    if(phase != Phase::Complete && capture.complete()) {
      auto at = result.find("\"state\":\"complete\"");
      auto state = phase == Phase::Incomplete ? "\"state\":\"incomplete\"" : "\"state\":\"draining\"";
      result.replace(at, 18, state);
    }
    const char* state = "writing";
    if(phase == Phase::Preparing) state = "preparing";
    if(phase == Phase::Complete) state = "complete";
    if(phase == Phase::Incomplete || capture.incomplete()) state = "incomplete";
    result.pop_back();
    if(writer->profile) {
      // Parallel writer spans overlap emulation; they are not additive wall shares.
      result += ",\"fileProfile\":{\"sealNs\":" + quote(std::to_string(capture.sealNanoseconds)) +
        ",\"copyNs\":" + quote(std::to_string(copyNs)) + ",\"writeNs\":" + quote(std::to_string(committedWriteNs)) +
        ",\"syncNs\":" + quote(std::to_string(committedSyncNs)) + "}";
    }
    return result + ",\"fileState\":" + quote(state) + ",\"fileBytes\":" + quote(std::to_string(phase == Phase::Complete ? writer->writtenBytes : committedBytes)) + ",\"fileBatches\":" + quote(std::to_string(committedBatches)) + "}";
  }
  static auto capability(const std::string& scope) -> std::string {
    if(scope == "instructions") return "capabilities";
    if(scope == "bus") return "bus-capabilities";
    if(scope == "bus-transfers") return "transfer-capabilities";
    if(scope == "bus-ppu") return "ppu-capabilities";
    return "";
  }
  static auto split(const std::string& value) -> std::vector<std::string> {
    if(value.size() > 16384) return {};
    std::vector<std::string> fields;
    size_t start = 0;
    for(size_t i = 0; i <= value.size(); ++i) if(i == value.size() || value[i] == ':') {
      fields.push_back(value.substr(start, i - start)); start = i + 1;
      if(fields.size() > 8) return {};
    }
    return fields;
  }
  static auto unhex(const std::string& value, std::string& out, size_t max) -> bool {
    if(value.empty() || value.size() % 2 || value.size() > max * 2) return false;
    const std::string digits = "0123456789abcdef";
    for(size_t i = 0; i < value.size(); i += 2) {
      auto hi = digits.find(value[i]), lo = digits.find(value[i + 1]);
      if(hi == std::string::npos || lo == std::string::npos || !(hi || lo)) return false;
      out += char(hi * 16 + lo);
    }
    // Metadata is emitted as UTF-8 JSON and paths cross the same wire encoding.
    // Reject overlong forms, surrogates and truncated sequences before arming.
    for(size_t i = 0; i < out.size();) {
      uint32_t code = uint8_t(out[i++]), minimum = 0;
      unsigned remaining = 0;
      if(code < 0x80) continue;
      if(code >= 0xc2 && code <= 0xdf) { code &= 0x1f; remaining = 1; minimum = 0x80; }
      else if(code >= 0xe0 && code <= 0xef) { code &= 0x0f; remaining = 2; minimum = 0x800; }
      else if(code >= 0xf0 && code <= 0xf4) { code &= 7; remaining = 3; minimum = 0x10000; }
      else return false;
      if(i + remaining > out.size()) return false;
      while(remaining--) {
        auto byte = uint8_t(out[i++]);
        if((byte & 0xc0) != 0x80) return false;
        code = (code << 6) | (byte & 0x3f);
      }
      if(code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
  }
  static auto quote(const std::string& value) -> std::string {
    std::string out = "\"";
    const char* digits = "0123456789abcdef";
    for(unsigned char c : value) {
      if(c == '"' || c == '\\') { out += '\\'; out += c; }
      else if(c < 32) { out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15]; }
      else out += c;
    }
    return out + '"';
  }
  static auto frame(const char* magic, const std::string& json) -> std::vector<uint8_t> {
    std::vector<uint8_t> out(12 + json.size());
    std::memcpy(out.data(), magic, 4);
    uint32_t crc = ~uint32_t(0);
    for(unsigned char byte : json) {
      crc ^= byte;
      for(unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    }
    crc = ~crc;
    for(unsigned i = 0; i < 4; ++i) { out[4 + i] = json.size() >> (8 * i); out[8 + i] = crc >> (8 * i); }
    std::memcpy(out.data() + 12, json.data(), json.size());
    return out;
  }
};
}
