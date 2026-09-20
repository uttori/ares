inline auto CPU::dmaEnable() -> bool {
  for(auto& channel : channels) if(channel.dmaEnable) return true;
  return false;
}

inline auto CPU::hdmaEnable() -> bool {
  for(auto& channel : channels) if(channel.hdmaEnable) return true;
  return false;
}

inline auto CPU::hdmaActive() -> bool {
  for(auto& channel : channels) if(channel.hdmaActive()) return true;
  return false;
}

auto CPU::dmaRun() -> void {
  counter.dma += 8;
  step(8);
  dmaEdge();
  for(auto& channel : channels) channel.dmaRun();
  status.irqLock = true;
}

auto CPU::hdmaReset() -> void {
  for(auto& channel : channels) channel.hdmaReset();
}

auto CPU::hdmaSetup() -> void {
  counter.dma += 8;
  step(8);
  for(auto& channel : channels) channel.hdmaSetup();
  status.irqLock = true;
}

auto CPU::hdmaRun() -> void {
  counter.dma += 8;
  step(8);
  for(auto& channel : channels) channel.hdmaTransfer();
  for(auto& channel : channels) channel.hdmaAdvance();
  status.irqLock = true;
}

//

inline auto CPU::Channel::step(u32 clocks) -> void {
  cpu.counter.dma += clocks;
  cpu.step(clocks);
}

inline auto CPU::Channel::edge() -> void {
  cpu.dmaEdge();
}

inline auto CPU::Channel::validA(n24 address) -> bool {
  //A-bus cannot access the B-bus or CPU I/O registers
  if((address & 0x40ff00) == 0x2100) return false;  //00-3f,80-bf:2100-21ff
  if((address & 0x40fe00) == 0x4000) return false;  //00-3f,80-bf:4000-41ff
  if((address & 0x40ffe0) == 0x4200) return false;  //00-3f,80-bf:4200-421f
  if((address & 0x40ff80) == 0x4300) return false;  //00-3f,80-bf:4300-437f
  return true;
}

inline auto CPU::Channel::readA(n24 address, u8 phase) -> n8 {
  step(4);
  if(validA(address)) cpu.captureBusReady();
  cpu.r.mdr = validA(address) ? bus.read(address, cpu.r.mdr) : (n8)0x00;
  if(validA(address)) cpu.captureBus(address, cpu.r.mdr, false, phase, id);
  step(4);
  if(validA(address)) nall::GDB::server.traceMemory(address, cpu.r.mdr, 0, phase == 1 ? 1 : 2, id);
  return cpu.r.mdr;
}

inline auto CPU::Channel::readB(n8 address, bool valid, u8 phase) -> n8 {
  step(4);
  if(valid) cpu.captureBusReady();
  cpu.r.mdr = valid ? bus.read(0x2100 | address, cpu.r.mdr) : (n8)0x00;
  if(valid) cpu.captureBus(0x2100 | address, cpu.r.mdr, false, phase, id);
  step(4);
  if(valid) nall::GDB::server.traceMemory(0x2100 | address, cpu.r.mdr, 0, phase == 1 ? 1 : 2, id);
  return cpu.r.mdr;
}

inline auto CPU::Channel::writeA(n24 address, n8 data, u8 phase) -> void {
  if(validA(address)) {
    cpu.captureBusReady();
    bus.write(address, data);
    cpu.captureBus(address, data, true, phase, id);
    nall::GDB::server.traceMemory(address, data, 1, phase == 1 ? 1 : 2, id);
  }
}

inline auto CPU::Channel::writeB(n8 address, n8 data, bool valid, u8 phase) -> void {
  if(valid) {
    cpu.captureBusReady();
    bus.write(0x2100 | address, data);
    cpu.captureBus(0x2100 | address, data, true, phase, id);
    nall::GDB::server.traceMemory(0x2100 | address, data, 1, phase == 1 ? 1 : 2, id);
  }
}

inline auto CPU::Channel::transfer(n24 addressA, n2 index, bool hdma) -> void {
  u8 phase = hdma ? 4 : 1;
  n8 addressB = targetAddress;
  switch(transferMode) {
  case 1: case 5: addressB += index.bit(0); break;
  case 3: case 7: addressB += index.bit(1); break;
  case 4: addressB += index; break;
  }

  //transfers from WRAM to WRAM are invalid
  bool valid = addressB != 0x80 || ((addressA & 0xfe0000) != 0x7e0000 && (addressA & 0x40e000) != 0x0000);

  cpu.r.mar = addressA;
  if(direction == 0) {
    auto data = readA(addressA, phase);
    writeB(addressB, data, valid, phase);
    if(valid) cpu.debugger.dma(id, addressA, addressB, data);
  } else {
    auto data = readB(addressB, valid, phase);
    writeA(addressA, data, phase);
    if(valid) cpu.debugger.dma(id, addressA, addressB, data);
  }
}

auto CPU::Channel::captureTransfer(u8 boundary, bool hdma, u32 transferred, std::array<uint64_t, 3> token) -> std::array<uint64_t, 3> {
  auto& capture = nall::GDB::server.capture;
  if(!capture.transferActive()) return {};
  // Reserve before reading the snapshot: a frontend handoff can stop or replace
  // the session. The marker and its configuration must describe the same instant.
  cpu.captureBusReady();
  if(!capture.transferActive()) return {};
  using Context = ares_capture::Capture::BusContext;
  auto context = Context::Unattributed;
  if(cpu.status.interruptPending) context = Context::Interrupt;
  if(cpu.r.wai) context = Context::Wait;
  if(cpu.r.stp) context = Context::Stop;
  std::array<uint8_t, 12> configuration{
    uint8_t(transferMode | fixedTransfer << 3 | reverseTransfer << 4 | unused << 5 | indirect << 6 | direction << 7),
    uint8_t(targetAddress), uint8_t(sourceAddress), uint8_t(sourceAddress >> 8),
    uint8_t(sourceBank), uint8_t(indirectBank), uint8_t(transferSize), uint8_t(transferSize >> 8),
    uint8_t(hdmaAddress), uint8_t(hdmaAddress >> 8), uint8_t(lineCounter),
    uint8_t(dmaEnable | hdmaEnable << 1 | hdmaCompleted << 2 | hdmaDoTransfer << 3)
  };
  return capture.transfer(boundary, hdma, id, transferred, configuration, token,
    context, cpu.r.pc.d, cpu.captureClock, cpu.hcounter(), cpu.vcounter());
}

inline auto CPU::Channel::dmaRun() -> void {
  if(!dmaEnable) return;
  auto token = captureTransfer(0, false, 0);
  u32 transferred = 0;

  step(8);
  edge();

  n2 index = 0;
  do {
    transfer(sourceBank << 16 | sourceAddress, index++);
    ++transferred;
    if(!fixedTransfer) !reverseTransfer ? sourceAddress++ : sourceAddress--;
    edge();
  } while(dmaEnable && --transferSize);

  // HDMA may clear dmaEnable at an edge; that is an interrupted run, not a
  // successful transfer of the original DAS length.
  captureTransfer(dmaEnable ? 1 : 2, false, transferred, token);
  dmaEnable = false;
}

inline auto CPU::Channel::hdmaActive() -> bool {
  return hdmaEnable && !hdmaCompleted;
}

inline auto CPU::Channel::hdmaFinished() -> bool {
  auto channel = next;
  while(channel) {
    if(channel->hdmaActive()) return false;
    channel = channel->next;
  }
  return true;
}

inline auto CPU::Channel::hdmaReset() -> void {
  hdmaCompleted = false;
  hdmaDoTransfer = false;
}

inline auto CPU::Channel::hdmaSetup() -> void {
  hdmaDoTransfer = true;  //note: needs hardware verification
  if(!hdmaEnable) return;

  dmaEnable = false;  //HDMA will stop active DMA mid-transfer
  hdmaAddress = sourceAddress;
  lineCounter = 0;
  hdmaReload();
}

inline auto CPU::Channel::hdmaReload() -> void {
  auto data = readA(cpu.r.mar = sourceBank << 16 | hdmaAddress, 2);

  if((n7)lineCounter == 0) {
    lineCounter = data;
    hdmaAddress++;

    hdmaCompleted = lineCounter == 0;
    hdmaDoTransfer = !hdmaCompleted;

    if(indirect) {
      data = readA(cpu.r.mar = sourceBank << 16 | hdmaAddress++, 3);
      indirectAddress = data << 8 | 0x00;  //todo: should 0x00 be indirectAddress >> 8 ?
      if(hdmaCompleted && hdmaFinished()) return;

      data = readA(cpu.r.mar = sourceBank << 16 | hdmaAddress++, 3);
      indirectAddress = data << 8 | indirectAddress >> 8;
    }
  }
}

inline auto CPU::Channel::hdmaTransfer() -> void {
  if(!hdmaActive()) return;
  dmaEnable = false;  //HDMA will stop active DMA mid-transfer
  if(!hdmaDoTransfer) return;

  auto token = captureTransfer(0, true, 0);
  static constexpr u32 lengths[8] = {1, 2, 2, 4, 4, 4, 2, 4};
  for(n2 index : range(lengths[transferMode])) {
    n24 address = !indirect ? sourceBank << 16 | hdmaAddress++ : indirectBank << 16 | indirectAddress++;
    transfer(address, index, true);
  }
  captureTransfer(1, true, lengths[transferMode], token);
}

inline auto CPU::Channel::hdmaAdvance() -> void {
  if(!hdmaActive()) return;
  lineCounter--;
  hdmaDoTransfer = lineCounter.bit(7);
  hdmaReload();
}
