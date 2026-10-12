#pragma once

#include <nall/tcptext/tcptext-server.hpp>
#include <nall/gdb/watchpoint.hpp>
#include <array>
#include <nall/gdb/capture.hpp>
#include <nall/gdb/capture-file.hpp>
#include <functional>

namespace nall::GDB {

enum class Signal : u8 {
  HANGUP  = 1,
  INT     = 2,
  QUIT    = 3,
  ILL     = 4,
  TRAP    = 5,
  ABORT   = 6,
  EMT     = 7,
  FPE     = 8,
  KILL    = 9,
  BUS     = 10,
  SEGV    = 11,
  SYS     = 12,
  PIPE    = 13,
  ALRM    = 14,
  TERM    = 15,
  URG     = 16,
  STOP    = 17,
  TSTP    = 18,
  CONT    = 19,
  CHLD    = 20,
  TTIN    = 21,
  TTOU    = 22,
  IO      = 23,
  PROF    = 27,
  WINCH   = 28,
  LOST    = 29,
};

/**
 * This implements a GDB server to handle remote debugging via a GDB client.
 * It is both independent of ares itself and any specific system.
 * Functionality is added by providing system-specific callbacks, as well as using the API inside a system.
 * (See the Readme.md file for more information.)
 * 
 * NOTE:
 * Command handling and the overall logic was carefully designed to support as many IDEs and GDB versions as possible.
 * Things can break very easily (and the official documentation may lie), so be very sure of any changes made here.
 * If changes are necessary, please verify that the following gdb-versions / IDEs still work properly:
 * 
 * GDB:
 * - gdb-multiarch        (the plain vanilla version exists in most package managers, supports a lot of arches)
 * - mips64-ultra-elf-gdb (special MIPS build of gdb-multiarch, i do NOT recommend it, behaves strangely)
 * - mingw-w64-x86_64-gdb (vanilla build for Windows/MSYS)
 * 
 * IDEs/Tools:
 * - GDB's CLI
 * - VSCode
 * - CLion (with bundled gdb-multiarch)
 * 
 * For testing, please also check both linux and windows (WSL2).
 * With WSL2, windows-ares is started from within WSL, while the debugger runs in linux.
 * This can be easily tested with VSCode and it's debugger.
 */
class Server : public nall::TCPText::Server {
  public:

    // Lossless evidence queue, owned and advanced by the emulation thread.
    ares_capture::Capture capture;
    auto reset() -> void;

    struct {
      // Memory
      std::function<string(u64 address, u32 byteCount)> read{};
      std::function<string(const string& addressSpace, u64 address, u32 byteCount)> namedMemoryRead{};
      std::function<void(u64 address, std::vector<u8> value)> write{};
      // Optional atomic writer: validate the complete range before mutation and return OK or an RSP error.
      std::function<string(u64 address, const std::vector<u8>& value)> checkedWrite{};
      std::function<u64(u64 address)> normalizeAddress{};

      // Registers
      bool registersLittleEndian = false;
      std::function<string()> regReadGeneral{};
      std::function<void(const string &regData)> regWriteGeneral{};
      std::function<string(u32 regIdx)> regRead{};
      std::function<bool(u32 regIdx, u64 regValue)> regWrite{};

      // Emulator
      bool instructionBoundaryStop = false;
      std::function<void()> emuReset{};
      std::function<void(u64 address)> emuCacheInvalidate{};
      std::function<string()> targetXML{};
      bool instructionTrace{};
      bool instructionTraceDetails{};


    } hooks{};

    std::function<void()> onClientConnectCallback{};

    // Exception
    auto reportSignal(Signal sig, u64 originPC) -> bool;

    // PC / Memory State Updates
    // Report a coherent CPU boundary. Idle boundaries do not hit code breakpoints;
    // canExecute=false preserves a pending step and breakpoint bypass under capture backpressure.
    auto reportPC(u64 pc, bool instruction = true, bool canExecute = true) -> bool;
    auto reportInstruction(u64 pc) -> void;
    // Legacy bounded trace: latch pre-instruction registers and publish only at end.
    // Register order is A/X/Y/S/D/DB/PB/PC/P/E; byte and access overflow is counted.
    auto beginInstructionTrace(u64 pc, const std::array<u16, 10>& state) -> void;
    // Tag bytes obtained by real CPU fetches; never read the bus to synthesize evidence.
    auto traceFetch(u64 address, u8 value) -> void;
    auto traceMemory(u64 address, u8 value, u8 access, u8 actor = 0, u8 channel = 15) -> void;
    auto endInstructionTrace() -> void;
    auto detailedTraceActive() const -> bool { return instructionTraceActive && instructionTraceFull; }

    auto reportMemRead(u64 address, u32 size) -> void;
    auto reportMemWrite(u64 address, u32 size) -> void;

    // Breakpoints / Watchpoints
    auto isHalted() const { return forceHalt && haltSignalSent; }
    auto isStopPending() const { return forceHalt && !haltSignalSent; }
    auto hasBreakpoints() const { 
      return !breakpoints.empty() || singleStepActive || !watchpointRead.empty() || !watchpointWrite.empty();
    }
    auto hasBreakpointAt(u64 pc) const -> bool;
    auto hasWatchpoints() const -> bool;
    
    auto getPcOverride() const { return pcOverride; };

    auto updateLoop() -> void;
    auto getStatusText(u32 port, bool useIPv4) -> string;

  protected:
    auto onText(string_view text) -> void override;
    auto onConnect() -> void override;
    auto onDisconnect() -> void override;

  private:
    bool insideCommand{false};
    string cmdBuffer{""};

    bool haltSignalSent{false}; // marks if a signal as been sent for new halts (force-halt and breakpoints)
    bool forceHalt{false}; // forces a halt despite no breakpoints being hit
    bool singleStepActive{false};
    bool stoppedAtInstructionBoundary{false};
    bool skipBreakpointOnce{false};
    bool pendingResetReply{false};

    // The legacy ring overwrites oldest entries and counts loss; Capture uses backpressure.
    static constexpr u32 instructionTraceCapacity = 65536;
    std::array<u64, instructionTraceCapacity> instructionTraceAddresses{};
    u32 instructionTraceHead{0};
    u32 instructionTraceSize{0};
    u64 instructionTraceNextSequence{1};
    u64 instructionTraceDroppedEvents{0};
    bool instructionTraceActive{false};
    struct TraceMemory { u64 address{}; u8 value{}, access{}, actor{}, channel{}; };
    struct TraceDetail {
      std::array<u16, 10> state{};
      std::array<u8, 4> bytes{};
      std::array<TraceMemory, 32> memory{};
      u64 pc{}, dropped{};
      u8 byteCount{}, memoryCount{};
    };
    std::vector<TraceDetail> instructionTraceDetails;
    TraceDetail pendingTrace{};
    bool instructionTraceFull{false}, pendingTraceActive{false};
    u64 unattributedTraceMemory{0};

    bool noAckMode{false}; // gets set if lldb prefers no acknowledgements
    bool nonStopMode{false}; // (NOTE: Not working for now), gets set if gdb wants to switch over to async-messaging
    bool handshakeDone{false}; // set to true after a few handshake commands, used to prevent exception-reporting until client is ready
    bool requestDisconnect{false}; // set to true if the client decides it wants to disconnect

    bool hasActiveClient{false};
    u32 messageCount{0}; // message count per update loop
    s32 currentThreadC{-1}; // selected thread for the next 'c' command

    u64 currentPC{0};
    maybe<u64> pcOverride{0}; // temporary override to handle edge-cases for exceptions/watchpoints

    // client-state:
    std::vector<u64> breakpoints{};
    std::vector<Watchpoint> watchpointRead{};
    std::vector<Watchpoint> watchpointWrite{};
    std::array<bool, 256> passSignals{};

    auto processCommand(const string& cmd, bool &shouldReply) -> string;
    auto resetClientData() -> void;

    auto reportWatchpoint(const Watchpoint &wp, u64 address) -> void;

    auto sendPayload(const string& payload) -> void;
    auto sendSignal(Signal code) -> void;
    auto sendSignal(Signal code, const string& reason) -> void;

    auto haltProgram() -> void;
    auto resumeProgram() -> void;

  public:
    // Keep optional sink bookkeeping after existing debugger fields. Their
    // offsets are read on every CPU/bus access, even with capture disabled.
    ares_capture::FileCapture captureFile;
};

extern Server server;

}
