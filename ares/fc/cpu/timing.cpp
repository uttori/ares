auto CPU::read(n16 address) -> n8 {
  if(io.oamDMAPending || io.dmcDMAPending) {
    dma(address);
  }

  n8 data = captureRead(address);
  if(address != 0x4015) io.openBus = data;
  step(rate());
  return data;
}

auto CPU::write(n16 address, n8 data) -> void {
  io.rwLine = 1;
  captureWrite(address, data);
  io.openBus = data;
  step(rate());
  io.rwLine = 0;
}

auto CPU::lastCycle() -> void {
  io.interruptPending = irqPending() | io.nmiPending;
}

auto CPU::cancelNmi() -> void {
  io.interruptPending = irqPending();
}

auto CPU::delayIrq() -> void {
  io.interruptPending = io.nmiPending;
}

auto CPU::irqPending() -> bool {
  return (io.irqLine | io.apuLine) & !P.i;
}

auto CPU::nmi(n16& vector) -> void {
  if(io.nmiPending) {
    if(irqPending()) debugger.interrupt("Haijack IRQ by NMI");
    io.nmiPending = false;
    vector = 0xfffa;
  }
}

auto CPU::dmcDMAPending() -> void {
  io.dmcDMAPending = 1;
  io.dmcDummyRead = 0;
}

auto CPU::dma(n16 address) -> void {
  bool oamWriteReady = false;
  n8   oamCounter = 0;
  bool skipDummyReads = (address == 0x4016 || address == 0x4017);

  // halt read
  io.openBus = captureRead(address, 3);
  step(rate());

  while (io.dmcDMAPending || io.oamDMAPending) {
    if (io.oddCycle) {
      // put_cycle
      if (io.oamDMAPending && oamWriteReady) {
        // oam write
        captureWrite(0x2004, io.openBus, 1);
        step(rate());

        oamWriteReady = false;
        if (++oamCounter == 0)
          io.oamDMAPending = 0;
      } else {
        // oam (re)alignment cycle
        // dmc alignment cycle
        io.dmcDummyRead = 1;

        if (!skipDummyReads)
          io.openBus = captureRead(address, 3);
        step(rate());
      }
    } else {
      // get_cycle
      if (io.dmcDMAPending && io.dmcDummyRead) {
        // dmc read
        io.openBus = captureRead(apu.dmc.dmaAddress(), 2);
        step(rate());

        apu.dmc.setDMABuffer(io.openBus);
        io.dmcDMAPending = 0;
      } else if (io.oamDMAPending) {
        if (io.dmcDMAPending)
          io.dmcDummyRead = 1;

        // oam read
        io.openBus = captureRead(io.oamDMAPage << 8 | oamCounter, 1);
        step(rate());

        oamWriteReady = true;
      } else {
        io.dmcDummyRead = 1;

        // dmc dummy read cycle
        if (!skipDummyReads)
          io.openBus = captureRead(address, 3);
        step(rate());
      }
    }
  }
}

auto CPU::nmiLine(bool line) -> void {
  //edge-sensitive (0->1)
  if(!io.nmiLine && line) io.nmiPending = true;
  io.nmiLine = line;
}

auto CPU::irqLine(bool line) -> void {
  // Board output can change in the PPU coroutine before the cartridge thread
  // drives this pin. Retain that propagation separately from the A12 event.
  bool recording = io.irqLine != line && captureNesReady();
  std::array<u8, 8> before{};
  if(recording) before = cartridge.board->debugIRQ();
  io.irqLine = line;
  if(recording) captureNesTimeline(3, 0, line, before);
}

auto CPU::apuLine(bool line) -> void {
  //level-sensitive
  io.apuLine = line;
}
