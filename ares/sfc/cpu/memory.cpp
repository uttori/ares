auto CPU::captureBusReady() -> void {
  auto& capture = nall::GDB::server.capture;
  // Keep disabled and instruction-only capture out of the per-access PPU path.
  if(!capture.busActive()) return;
  // The coroutine retains the exact transfer continuation. Do not acknowledge a
  // debugger stop or synchronize a save here: this may be mid-instruction/DMA.
  // Stop/abort releases this reservation wait; failed capture holds at CPU::main.
  while(capture.busActive() && !capture.ready()) scheduler.exit(Event::Step);
  capture.ppuReset();
}

auto CPU::captureBus(n24 address, n8 data, bool write, u8 phase, u8 channel) -> void {
  auto& capture = nall::GDB::server.capture;
  if(!capture.busActive()) return;
  using Context = ares_capture::Capture::BusContext;
  auto context = Context::Unattributed;
  if(status.interruptPending) context = Context::Interrupt;
  if(r.wai) context = Context::Wait;
  if(r.stp) context = Context::Stop;
  capture.bus(address, data, write, ares_capture::Capture::BusPhase(phase), channel,
    context, r.pc.d, captureClock, hcounter(), vcounter());
}

auto CPU::idle() -> void {
  // WAI/STP may never return to CPU::main; finish evidence and service stops here.
  if(r.wai || r.stp) {
    nall::GDB::server.capture.end();
    while(!nall::GDB::server.reportPC(r.pc.d, false) || !nall::GDB::server.capture.ready()) {
      scheduler.exit(Event::Step);
      scheduler.synchronize();
    }
  }
  status.clockCount = 6;
  dmaEdge();
  step(6);
  aluEdge();
}

auto CPU::read(n24 address) -> n8 {
  status.clockCount = wait(address);
  dmaEdge();
  r.mar = address;
  step(status.clockCount - 4);
  captureBusReady();
  auto data = bus.read(address, r.mdr);
  captureBus(address, data, false);
  step(4);
  aluEdge();
  //$00-3f,80-bf:4000-43ff reads are internal to CPU, and do not update the MDR
  if((address & 0x40fc00) != 0x4000) r.mdr = data;
  nall::GDB::server.traceMemory(address, data, 0);
  return data;
}

auto CPU::write(n24 address, n8 data) -> void {
  aluEdge();
  status.clockCount = wait(address);
  dmaEdge();
  r.mar = address;
  step(status.clockCount);
  captureBusReady();
  bus.write(address, r.mdr = data);
  captureBus(address, data, true);
  nall::GDB::server.traceMemory(address, data, 1);
}

alwaysinline auto CPU::wait(n24 address) const -> u32 {
  //00-3f,80-bf:8000-ffff; 40-7f,c0-ff:0000-ffff
  if(address & 0x408000) return address & 0x800000 ? io.romSpeed : 8;

  //00-3f,80-bf:0000-1fff,6000-7fff
  if(address + 0x6000 & 0x4000) return 8;

  //00-3f,80-bf:2000-3fff,4200-5fff
  if(address - 0x4000 & 0x7e00) return 6;

  //00-3f,80-bf:4000-41ff
  return 12;
}

auto CPU::readDisassembler(n24 address) -> n8 {
  return bus.read(address, r.mdr);
}

auto CPU::instructionFetch(n24 address, n8 value) -> void {
  nall::GDB::server.traceFetch(address, value);
  nall::GDB::server.capture.fetch(value);
}
