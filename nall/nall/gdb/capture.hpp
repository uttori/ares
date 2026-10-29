#pragma once

// Downstream ACT2 producer with separately negotiated instruction and bus scopes.
// No ares or transport dependencies; callers serialize access through the
// emulation thread or the frontend's guarded handoff.
#include <array>
#include <chrono>
#include <algorithm>
#include <utility>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>
#if defined(__ARM_FEATURE_CRC32) && !defined(ARES_CAPTURE_PORTABLE_CRC)
#include <arm_acle.h>
#elif defined(__SSE4_2__) && !defined(ARES_CAPTURE_PORTABLE_CRC)
#include <nmmintrin.h>
#endif

namespace ares_capture {
class Capture {
public:
  // Caller zeroes any checksum field first. Works on physical raw or compressed
  // bytes, using bounded scratch owned by the caller and no allocations.
  static auto crc32c(const uint8_t* p, size_t length) -> uint32_t {
    // Sealing is paid on every byte, including the PPU extension. A fixed 1 KiB
    // Castagnoli table replaces eight polynomial steps per byte without changing
    // the checksum or allocating per chunk. The independent test reader stays bitwise.
    static constexpr auto crcTable = [] {
      std::array<uint32_t, 256> table{};
      for(unsigned i = 0; i < table.size(); ++i) {
        uint32_t crc = i;
        for(unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
        table[i] = crc;
      }
      return table;
    }();
    uint32_t crc = ~uint32_t(0);
    uint32_t i = 0;
    // Intrinsics are used only when the compiler target promises CRC support;
    // portable builds retain the exact Castagnoli fallback. memcpy permits
    // unaligned records without aliasing undefined behavior. Wire bytes are LE.
#if (defined(__ARM_FEATURE_CRC32) || (defined(__SSE4_2__) && defined(__x86_64__))) && !defined(ARES_CAPTURE_PORTABLE_CRC) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for(; i + 8 <= length; i += 8) {
      uint64_t word; std::memcpy(&word, p + i, 8);
#if defined(__ARM_FEATURE_CRC32)
      crc = __crc32cd(crc, word);
#else
      crc = uint32_t(_mm_crc32_u64(crc, word));
#endif
    }
#endif
    for(; i < length; ++i) crc = (crc >> 8) ^ crcTable[(crc ^ p[i]) & 255];
    return ~crc;
  }
  static constexpr uint32_t chunkBytes = 262144;
  static constexpr uint32_t recordReserve = 71; // 32 common + 19 identity + 4 fetched + 16 state
  static constexpr uint32_t busRecordBytes = 64;
  static constexpr uint32_t transferRecordBytes = 80;
  static constexpr uint32_t ppuRecordBytes = 88;
  // Session bytes plus begin-event ID live on the channel coroutine's stack. A
  // stop/rearm cannot attach a later end marker to the previous capture session.
  using TransferToken = std::array<uint64_t, 3>;
  // These values are the private bus record's wire dictionary. A phase describes
  // the actual operation, independent of mutable DMA enable bits.
  enum class BusPhase : uint8_t { CPU, DMA, HDMATable, HDMAIndirect, HDMATransfer };
  enum class BusContext : uint8_t { Unattributed, Instruction, Interrupt, Wait, Stop };
  // Covers the fixed record scratch, queue bookkeeping and bounded hex/JSON
  // response temporaries. TCP socket buffers belong to the transport budget.
  static constexpr uint32_t overheadBytes = 4194304;
  struct Chunk {
    std::array<uint8_t, chunkBytes> bytes{};
    uint64_t sequence{}, first{};
    uint32_t used{64}, count{};
    bool sealed{};
  };

  // Chunk-level timing avoids clock reads on individual CPU/PPU accesses.
  // One producer owns each emulation thread. Thread-local diagnostics preserve
  // the Capture/Server object layout and never add work to disabled bus hooks.
  static inline thread_local bool profileSeals{};
  static inline thread_local uint64_t sealNanoseconds{};
  // Selected by the system hook installation, never inferred from register values.
  bool famicom = false;
  static constexpr uint32_t nesRecordReserve = 91; // common + identity + three bytes + 37-byte state
  auto architectureName() const -> const char* { return famicom ? "65xx.2a03" : "snes.65816"; }
  auto addressSpaceName() const -> const char* { return famicom ? "nes.cpu" : "snes.cpu"; }
  auto clockName() const -> const char* { return famicom ? "nes.master" : "snes.master"; }
  auto beginNes(uint16_t pc, const std::array<uint8_t, 7>& registers,
                const std::array<uint8_t, 18>& mapper, uint64_t cycle) -> void {
    begin(pc, {}, cycle);
    std::copy(registers.begin(), registers.end(), pendingNes.begin());
    std::copy(mapper.begin(), mapper.end(), pendingNes.begin() + 7);
    std::fill(pendingNes.begin() + 25, pendingNes.end(), 255);
  }
  auto fetchNes(uint8_t value, uint32_t physical) -> void {
    if(!pending || busOnly) return;
    if(fetchedCount >= 3) { fail("nes-fetched-byte-bound"); return; }
    put(pendingNes.data() + 25 + fetchedCount * 4, physical, 4);
    fetch(value);
  }
  // NES bus records have a separate dictionary; no SNES DMA or PPU fields are reused.
  static constexpr uint32_t nesBusRecordBytes = 128;
  auto nesBusActive() const -> bool { return famicom && busOnly && recording; }
  auto nesBus(const std::array<uint8_t, 96>& body, uint64_t cycle) -> void {
    if(!nesBusActive()) return;
    auto* out = append(4, nesBusRecordBytes, cycle);
    if(!out) return;
    std::memcpy(out + 32, body.data(), body.size());
    put(out + 32, pending ? ordinal : 0, 8);
    if(pending) { put(out + 40, pendingPC, 2); out[47] = 1; }
  }
  // PPU/mapper records share the observer's CPU epoch; body[0..7] retains the
  // exact source-device master tick because auxiliary coroutines can run ahead.
  auto nesTimeline(const std::array<uint8_t, 96>& body, uint64_t cycle) -> void {
    if(!nesBusActive()) return;
    auto* out = append(5, nesBusRecordBytes, cycle);
    if(out) std::memcpy(out + 32, body.data(), body.size());
  }
  auto active() const -> bool { return recording; }
  auto busScope() const -> bool { return busOnly; }
  // Optional full-capture graphics companion. It has its own session, scope and sink;
  // instruction records and their ownership counters never contain bus events.
  Capture* graphics = nullptr;
  auto busTarget() -> Capture& { if(graphics) return *graphics; return *this; }
  auto eventCount() const -> uint64_t { return produced; }
  auto instructionCount() const -> uint64_t { return ordinal; }
  auto busActive() const -> bool { return busOnly && recording; }
  auto transferActive() const -> bool { return transfers && recording; }
  auto ppuActive() const -> bool { return ppuEffects && recording; }

  // Scratch belongs to exactly one CPU-bus operation. Reset after reservation
  // waits, before the access; PPU hooks fill it after synchronization, without
  // yielding or inspecting memory. Disabled capture never clears this scratch.
  auto ppuReset() -> void { if(ppuActive()) ppu = {}; }
  auto ppuBegin(uint8_t target, uint8_t flags, uint16_t h, uint16_t v) -> void {
    ppu = {}; ppu.target = target; ppu.outcome = 2;
    ppu.flags = flags; ppu.h = h; ppu.v = v;
  }
  auto ppuRestricted() -> void { ppu.flags |= 2; }
  auto ppuBlocked() -> void { ppuRestricted(); ppu.outcome = 1; }
  // Preserve write order, including two OAM writes redirected to the same byte.
  auto ppuWrite(uint32_t address, uint8_t value) -> void {
    if(!ppu.target || ppu.count == 2) { fail("ppu-effect-bound"); return; }
    ppu.outcome = 0;
    ppu.address[ppu.count] = address; ppu.value[ppu.count++] = value;
  }
  auto pendingInstruction() const -> bool { return pending; }
  auto occupiedBytes() const -> uint64_t { return uint64_t(size) * sizeof(Chunk); }
  auto allocatedBytes() const -> uint64_t { return uint64_t(queue.size()) * sizeof(Chunk) + overheadBytes; }
  auto sessionId() const -> const std::string& { return session; }
  auto clockRate(uint64_t numerator, uint64_t denominator) -> void { clockNumerator = numerator; clockDenominator = denominator; }

  // Called BEFORE an instruction or bus operation begins. A false return
  // requires yielding to the frontend without performing that operation.
  auto ready() -> bool {
    if(graphics && !graphics->ready()) return false;
    if(failed) return false;
    if(!recording) return true;
    bool tailRoom = false;
    if(size) {
      auto& tail = queue[(head + size - 1) % queue.size()];
      tailRoom = !tail.sealed && tail.used + reserveBytes() <= chunkBytes;
    }
    bool room = tailRoom || size < queue.size();
    if(throttled && (size > queue.size() / 4 || !room)) return false;
    if(!room || (!throttled && !tailRoom && size >= highWater)) {
      if(!throttled) { throttled = true; ++throttleCount; }
      return false;
    }
    throttled = false;
    state = "recording";
    return true;
  }

  auto begin(uint64_t pc, const std::array<uint16_t, 10>& registers, uint64_t cycle) -> void {
    if(graphics && graphics->active()) graphics->begin(pc, registers, cycle);
    if(!recording) return;
    if(pending || !ready() || produced == UINT64_MAX || ordinal == UINT64_MAX) { fail("instruction-reserve"); return; }
    ++ordinal;
    pending = true;
    pendingPC = pc; pendingState = registers; pendingCycle = cycle; fetchedCount = 0;
  }
  auto fetch(uint8_t byte) -> void {
    if(!pending || busOnly) return;
    if(fetchedCount == fetched.size()) { fail("fetched-byte-bound"); return; }
    fetched[fetchedCount++] = byte;
  }
  // WAI/STP commit on their first idle boundary. Subsequent idle cycles and
  // interrupt entry are excluded by this instruction-only scope.
  auto end() -> void {
    if(graphics) graphics->end();
    if(!pending) return;
    pending = false;
    if(busOnly) return;
    if(!fetchedCount) { fail("missing-fetched-bytes"); return; }
    uint32_t stateBytes = famicom ? 37 : 16;
    uint32_t length = 51 + stateBytes + fetchedCount;
    auto* out = append(1, length, pendingCycle);
    if(!out) return;
    put(out + 32, ordinal, 8);
    put(out + 40, pendingPC, 8); out[48] = fetchedCount; put(out + 49, stateBytes, 2);
    std::memcpy(out + 51, fetched.data(), fetchedCount);
    auto* regs = out + 51 + fetchedCount;
    if(famicom) {
      std::memcpy(regs, pendingNes.data(), pendingNes.size());
    } else {
    for(unsigned i = 0; i < 5; ++i) put(regs + i * 2, pendingState[i], 2);
    regs[10] = pendingState[5]; regs[11] = pendingState[6];
    put(regs + 12, pendingState[7], 2); regs[14] = pendingState[8]; regs[15] = pendingState[9];
    }
    auto& tail = queue[(head + size - 1) % queue.size()];
    if(!recording) { seal(tail); state = "draining"; }
  }

  // ready() must succeed immediately before the real bus operation. Append the
  // observed value immediately afterwards, before clocks or another bus access.
  // Ordinal zero explicitly means no captured instruction owns this access.
  auto bus(uint32_t address, uint8_t value, bool write, BusPhase phase, uint8_t channel,
           BusContext context, uint32_t pc, uint64_t cycle, uint16_t h, uint16_t v) -> void {
    if(!busActive()) return;
    bool effect = false;
    if(ppuEffects) {
      const auto port = address & 0xffff;
      effect = write && !(address & 0x400000) &&
        (port == 0x2104 || port == 0x2118 || port == 0x2119 || port == 0x2122);
      // Missing hooks must not silently downgrade a verified-effects session.
      if(bool(ppu.target) != effect) { fail("ppu-effect-missing"); return; }
    }
    auto* out = append(2, effect ? ppuRecordBytes : busRecordBytes, cycle);
    if(!out) return;
    put(out + 32, pending ? ordinal : 0, 8);
    put(out + 40, address, 8);
    put(out + 48, pending ? pendingPC : pc, 4);
    out[52] = value; out[53] = write;
    out[54] = 2;
    if(phase == BusPhase::CPU) out[54] = 0;
    if(phase == BusPhase::DMA) out[54] = 1;
    out[55] = channel; out[56] = uint8_t(phase);
    out[57] = uint8_t(pending ? BusContext::Instruction : context);
    put(out + 58, h, 2); put(out + 60, v, 2); put(out + 62, 0, 2);
    if(effect) {
      put(out + 2, 2, 2);
      out[64] = ppu.target; out[65] = ppu.outcome; out[66] = ppu.count; out[67] = ppu.flags;
      put(out + 68, ppu.h, 2); put(out + 70, ppu.v, 2);
      put(out + 72, ppu.address[0], 4); put(out + 76, ppu.address[1], 4);
      out[80] = ppu.value[0]; out[81] = ppu.value[1]; put(out + 82, 0, 6);
      ppu = {};
    }
  }

  // Mark one whole DMA channel run or one HDMA payload burst. End markers carry
  // attempted byte slots, including hardware-suppressed accesses; bus records
  // remain the authority for which reads/writes actually occurred.
  auto transfer(uint8_t boundary, bool hdma, uint8_t channel, uint32_t transferred,
                const std::array<uint8_t, 12>& configuration, TransferToken token,
                BusContext context, uint32_t pc, uint64_t cycle, uint16_t h, uint16_t v) -> TransferToken {
    if(!transferActive()) return {};
    if(boundary == 0) {
      std::memcpy(token.data(), sessionBytes.data(), 16);
      token[2] = produced + 1;
    } else if(std::memcmp(token.data(), sessionBytes.data(), 16) != 0) {
      // Capture may begin inside a transfer. ID zero denotes an end whose begin
      // was not observed, even if an earlier capture saw that begin.
      token = {};
    }
    auto* out = append(3, transferRecordBytes, cycle);
    if(!out) return {};
    put(out + 32, pending ? ordinal : 0, 8); put(out + 40, token[2], 8);
    put(out + 48, pending ? pendingPC : pc, 4);
    out[52] = boundary; out[53] = hdma ? 2 : 1; out[54] = channel;
    out[55] = uint8_t(pending ? BusContext::Instruction : context);
    put(out + 56, h, 2); put(out + 58, v, 2);
    std::memcpy(out + 60, configuration.data(), configuration.size());
    put(out + 72, transferred, 4); put(out + 76, 0, 4);
    return token;
  }

  auto fail(const std::string& reason) -> void {
    if(graphics) graphics->fail(reason);
    if(session.empty() || state == "complete" || state == "incomplete") return;
    recording = pending = false; failed = true; state = "incomplete"; error = reason;
    if(size) seal(queue[(head + size - 1) % queue.size()]);
  }

  auto command(const std::string& request) -> std::string {
    auto fields = split(request);
    if(fields.empty()) return "E01";
    if(famicom && (fields[0] == "bus-capabilities" || fields[0] == "transfer-capabilities" ||
      (fields[0] == "start" && fields.size() != 2 && !(fields.size() == 3 && fields[2] == "bus-ppu")))) return "E09";
    if(famicom && fields[0] == "ppu-capabilities" && fields.size() == 1)
      return "{\"version\":2,\"scope\":\"nes.cpu-bus-ppu\",\"mode\":\"lossless\",\"ownership\":\"durable-prefix\",\"maxChunkBytes\":262144,\"requiredReserveBytes\":128,\"overheadBytes\":4194304,\"resumeSession\":false}";
    if(fields[0] == "capabilities" && fields.size() == 1)
      return "{\"version\":2,\"scope\":\"" + std::string(famicom ? "nes.2a03.instructions" : "snes.65816.instructions") + "\",\"mode\":\"lossless\",\"ownership\":\"durable-prefix\",\"maxChunkBytes\":262144,\"requiredReserveBytes\":" + std::to_string(famicom ? nesRecordReserve : recordReserve) + ",\"overheadBytes\":4194304,\"resumeSession\":false}";
    if(fields[0] == "bus-capabilities" && fields.size() == 1)
      return "{\"version\":2,\"scope\":\"snes.cpu-bus\",\"mode\":\"lossless\",\"ownership\":\"durable-prefix\",\"maxChunkBytes\":262144,\"requiredReserveBytes\":64,\"overheadBytes\":4194304,\"resumeSession\":false}";
    if(fields[0] == "transfer-capabilities" && fields.size() == 1)
      return "{\"version\":2,\"scope\":\"snes.cpu-bus-transfers\",\"mode\":\"lossless\",\"ownership\":\"durable-prefix\",\"maxChunkBytes\":262144,\"requiredReserveBytes\":80,\"overheadBytes\":4194304,\"resumeSession\":false}";
    if(fields[0] == "ppu-capabilities" && fields.size() == 1)
      return "{\"version\":2,\"scope\":\"snes.cpu-bus-ppu\",\"mode\":\"lossless\",\"ownership\":\"durable-prefix\",\"maxChunkBytes\":262144,\"requiredReserveBytes\":88,\"overheadBytes\":4194304,\"resumeSession\":false}";
    if(fields[0] == "start" && (fields.size() == 2 || (fields.size() == 3 && (fields[2] == "bus" || fields[2] == "bus-transfers" || fields[2] == "bus-ppu")))) {
      uint64_t budget;
      if(!number(fields[1], budget) || budget > 64 * 1024 * 1024 || budget < overheadBytes + 2 * sizeof(Chunk)) return "E02";
      if(!session.empty() && state != "complete" && state != "incomplete") return "E03";
      std::array<uint8_t, 16> id{};
      try { std::random_device rng; for(auto& byte : id) byte = rng(); }
      catch(...) { return "E04"; }
      // An explicit new session discards the previous stopped/incomplete queue.
      // Release it before allocating, so rearming cannot double the byte budget.
      std::vector<Chunk>().swap(queue);
      head = size = 0;
      std::vector<Chunk> fresh;
      try { fresh.resize((budget - overheadBytes) / sizeof(Chunk)); }
      catch(...) { return "E04"; }
      auto numerator = clockNumerator, denominator = clockDenominator;
      bool selectedFamicom = famicom;
      *this = Capture{};
      famicom = selectedFamicom;
      profileSeals = false; sealNanoseconds = 0;
      busOnly = fields.size() == 3;
      ppuEffects = busOnly && fields[2] == "bus-ppu";
      transfers = ppuEffects || (busOnly && fields[2] == "bus-transfers");
      clockRate(numerator, denominator);
      queue = std::move(fresh); sessionBytes = id; session = hex(id.data(), id.size());
      highWater = std::max<size_t>(1, queue.size() * 3 / 4);
      recording = true; state = "armed";
      return status();
    }
    if(fields.size() < 2 || fields[1] != session || session.empty()) return "E05";
    if(fields[0] == "status" && fields.size() == 2) return status();
    if(fields[0] == "stop" && fields.size() == 2) {
      recording = false;
      // Bus records commit independently; stopping fixes the last completed bus
      // event even if the containing instruction/DMA transfer has not finished.
      if(busOnly) pending = false;
      if(!failed && state != "complete") {
        state = pending ? "stopping" : "draining";
        if(!pending && size) seal(queue[(head + size - 1) % queue.size()]);
        completeIfDrained();
      }
      return status();
    }
    if(fields[0] == "abort" && fields.size() == 2) { fail("consumer-aborted"); return status(); }
    // Explicit recovery of debugger control after an incomplete capture. It
    // never changes integrity or starts/resumes a capture.
    if(fields[0] == "release" && fields.size() == 2 && state == "incomplete") { failed = false; return status(); }
    uint64_t seq;
    if(fields[0] == "read" && fields.size() == 3 && number(fields[2], seq)) {
      if(seq != acceptedChunk + 1) return "E06";
      if(!size) return "";
      auto& front = queue[head];
      // A pending instruction reserved the current tail. Do not seal that
      // reservation out from under it (e.g. a frame exit inside DMA).
      if(!front.sealed && pending && !busOnly) return "";
      seal(front); delivered = front.first + front.count - 1;
      return hex(front.bytes.data(), front.used);
    }
    uint64_t event;
    if(fields[0] == "ack" && fields.size() == 4 && number(fields[2], seq) && number(fields[3], event)) {
      if(seq == acceptedChunk && event == accepted) return status();
      if(!size) return "E07";
      auto& front = queue[head];
      if(!front.sealed || seq != front.sequence || event != front.first + front.count - 1 || event != delivered) return "E07";
      acceptedChunk = seq; accepted = event;
      head = (head + 1) % queue.size(); --size;
      completeIfDrained();
      return status();
    }
    return "E08";
  }

  // Native sinks copy at most one bounded batch on the emulation thread. Returned
  // storage remains producer-owned until acknowledgeNative(), never writer-owned.
  auto nativeChunk(size_t index) -> const Chunk* {
    if(index >= size) return nullptr;
    auto& chunk = queue[(head + index) % queue.size()];
    if(!chunk.sealed) {
      if(pending && !busOnly) return nullptr;
      seal(chunk);
    }
    delivered = std::max(delivered, chunk.first + chunk.count - 1);
    return &chunk;
  }

  // Release exactly the copied contiguous batch, only after the sink's fsync.
  // A failed session can retain a durable prefix, but can never become complete.
  auto acknowledgeNative(uint64_t sequence, uint64_t event) -> bool {
    size_t count = 0;
    while(count < size) {
      const auto& chunk = queue[(head + count) % queue.size()];
      if(!chunk.sealed || chunk.sequence > sequence) return false;
      ++count;
      if(chunk.sequence == sequence) {
        if(event != chunk.first + chunk.count - 1 || event > delivered) return false;
        acceptedChunk = sequence; accepted = event;
        head = (head + count) % queue.size(); size -= count;
        completeIfDrained();
        return true;
      }
    }
    return false;
  }
  // File publication is still part of the session after the producer drains.
  // A sink failure at that boundary must keep execution fail-closed as well.
  auto failNative(const std::string& reason) -> void {
    if(state == "complete") state = "draining";
    fail(reason);
  }
  auto incomplete() const -> bool { return state == "incomplete"; }
  auto complete() const -> bool { return state == "complete"; }
  auto scopeName() const -> const char* {
    if(famicom) return busOnly ? "nes.cpu-bus-ppu" : "nes.2a03.instructions";
    if(ppuEffects) return "snes.cpu-bus-ppu";
    if(transfers) return "snes.cpu-bus-transfers";
    return busOnly ? "snes.cpu-bus" : "snes.65816.instructions";
  }
  auto clockRatio() const -> std::pair<uint64_t, uint64_t> { return {clockNumerator, clockDenominator}; }

  auto status() const -> std::string {
    auto q = [](uint64_t v) { return "\"" + std::to_string(v) + "\""; };
    auto scope = scopeName();
    return "{\"scope\":\"" + std::string(scope) +
      "\",\"session\":\"" + session + "\",\"state\":\"" + (throttled && recording ? "throttled" : state) +
      "\",\"producedThrough\":" + q(produced) + ",\"deliveredThrough\":" + q(delivered) +
      ",\"acceptedThrough\":" + q(accepted) + ",\"durableThrough\":" + q(accepted) +
      ",\"releasedThrough\":" + q(accepted) + ",\"chunkThrough\":" + q(acceptedChunk) +
      ",\"producedChunks\":" + q(chunks) + ",\"bufferedBytes\":" + std::to_string(occupiedBytes()) +
      ",\"clockNumerator\":" + q(clockNumerator) + ",\"clockDenominator\":" + q(clockDenominator) +
      ",\"allocatedBytes\":" + std::to_string(allocatedBytes()) +
      ",\"throttleCount\":" + q(throttleCount) + ",\"droppedInScope\":\"0\",\"error\":\"" + error + "\"}";
  }

private:
  std::array<uint8_t, 37> pendingNes{};
  std::vector<Chunk> queue;
  size_t head{}, size{}, highWater{};
  std::string session, state{"idle"}, error;
  std::array<uint8_t, 16> sessionBytes{};
  uint64_t produced{}, chunks{}, delivered{}, accepted{}, acceptedChunk{}, throttleCount{};
  uint64_t clockNumerator{236250000}, clockDenominator{11};
  bool recording{}, throttled{}, pending{}, failed{}, busOnly{}, transfers{}, ppuEffects{};
  // Fixed 24-byte wire extension; never part of emulator save-state serialization.
  struct PPUEffect {
    uint8_t target{}, outcome{}, count{}, flags{};
    uint16_t h{}, v{};
    std::array<uint32_t, 2> address{};
    std::array<uint8_t, 2> value{};
  } ppu;
  uint64_t ordinal{};
  uint64_t pendingPC{}, pendingCycle{};
  std::array<uint16_t, 10> pendingState{};
  std::array<uint8_t, 4> fetched{};
  uint8_t fetchedCount{};

  auto reserveBytes() const -> uint32_t {
    if(famicom && busOnly) return nesBusRecordBytes;
    if(ppuEffects) return ppuRecordBytes;
    if(transfers) return transferRecordBytes;
    if(busOnly) return busRecordBytes;
    return famicom ? nesRecordReserve : recordReserve;
  }

  // Commit one record into already reserved queue capacity. Both scopes share
  // chunk ownership, retry/ack semantics, and the same bounded allocation.
  auto append(uint16_t kind, uint32_t length, uint64_t cycle) -> uint8_t* {
    if(produced == UINT64_MAX) { fail("event-overflow"); return nullptr; }
    if(size) {
      auto& tail = queue[(head + size - 1) % queue.size()];
      if(tail.used + reserveBytes() > chunkBytes) seal(tail);
    }
    if(!size || queue[(head + size - 1) % queue.size()].sealed) {
      if(size == queue.size() || chunks == UINT64_MAX) { fail("reserve-violation"); return nullptr; }
      auto& fresh = queue[(head + size++) % queue.size()];
      fresh = {}; fresh.sequence = ++chunks; fresh.first = produced + 1;
    }
    auto& tail = queue[(head + size - 1) % queue.size()];
    auto* out = tail.bytes.data() + tail.used;
    put(out, kind, 2); put(out + 2, 1, 2); put(out + 4, length, 4);
    put(out + 8, ++produced, 8); put(out + 16, famicom ? 1 : 0, 4); put(out + 20, famicom ? 1 : 0, 4);
    put(out + 24, cycle, 8);
    tail.used += length; ++tail.count;
    return out;
  }

  auto completeIfDrained() -> void { if(!failed && !recording && !pending && !size) state = "complete"; }
  static auto put(uint8_t* out, uint64_t value, unsigned bytes) -> void {
    for(unsigned i = 0; i < bytes; ++i) out[i] = value >> (8 * i);
  }
  static auto hex(const uint8_t* bytes, size_t count) -> std::string {
    const char* digits = "0123456789abcdef";
    std::string out(count * 2, '0');
    for(size_t i = 0; i < count; ++i) { out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15]; }
    return out;
  }
  static auto number(const std::string& s, uint64_t& out) -> bool {
    if(s.empty() || (s.size() > 1 && s[0] == '0')) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
  }
  static auto split(const std::string& value) -> std::vector<std::string> {
    std::vector<std::string> parts;
    size_t begin = 0;
    for(size_t i = 0; i <= value.size(); ++i) if(i == value.size() || value[i] == ':') {
      parts.push_back(value.substr(begin, i - begin)); begin = i + 1;
      if(parts.size() > 4) return {};
    }
    return parts;
  }
  auto seal(Chunk& chunk) -> void {
    if(chunk.sealed) return;
    auto started = profileSeals ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto* p = chunk.bytes.data();
    std::memcpy(p, "ACT2", 4); put(p + 4, 2, 2); put(p + 6, 0, 2);
    std::memcpy(p + 8, sessionBytes.data(), 16);
    put(p + 24, chunk.sequence, 8); put(p + 32, chunk.first, 8);
    put(p + 40, chunk.count, 4); put(p + 44, 64, 4);
    put(p + 48, chunk.used - 64, 4); put(p + 52, chunk.used - 64, 4);
    put(p + 56, 0, 8);
    put(p + 56, crc32c(p, chunk.used), 4); chunk.sealed = true;
    if(profileSeals) sealNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
  }
};
}
