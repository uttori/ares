// BT Interactive TV V1.0-573, full-bank and upper-half flash configurations. The local host
// delivers complete SDLC frames to the SCC-facing FIFO at cartridge IRQ time.
struct BTGameCart {
  // $3D doubles as the count high byte and error status. A 1 KiB block
  // keeps its frame length clear of the firmware's $08 trailer-error bit.
  static constexpr u32 BlockSize = 1024;
  enum class Request : u8 { Query = 0x00, Setup = 0x10, Block = 0x20, Finish = 0x30, Done, Failed, Abort = 0x40 };

  std::vector<u8> media, flash;
  u16 checksum = 0;
  Request request;
  bool loRom, abortDownload;
  u8 ramConfig;
  u32 offset;
  u8 rx[4 + 1 + 5 + BlockSize + 2], tx[12];
  u16 received, rxSize, sent;
  u8 waitFrames, selectedRegister, timer;
  bool interrupts, endOfFrame, handoff, playing;
  // Each one-MiB flash device has an independent command latch.
  enum class FlashMode : u8 { Array, Status, EraseSetup, Program };
  FlashMode mode[4];

  auto active() const -> bool { return !media.empty(); }

  auto connect(VFS::File fp) -> void {
    media.resize(fp->size());
    fp->read(media.data(), media.size());
    checksum = 0;
    for(auto byte : media) checksum += byte;
    loRom = cartridge.pak->attribute("btLoRom").boolean();
    u8 scenario = cartridge.pak->attribute("btScenario").natural();
    abortDownload = scenario == 2;
    // Alter only the advertised sum. The received game bytes stay identical.
    if(scenario == 1) checksum ^= 1;
    flash.resize(4_MiB, 0xff);
    if(auto saved = cartridge.pak->read("bt.save.ram")) {
      cartridge.ram.allocate(saved->size());
      cartridge.ram.load(saved);
    }

    auto read = std::bind_front(&BTGameCart::read, this);
    auto write = std::bind_front(&BTGameCart::write, this);
    bus.map(read, write, "00-3f,80-bf:8000-ffff");
    bus.map(read, write, "40-7d,c0-ff:0000-ffff");
    bus.map(std::bind_front(&BTGameCart::readIO, this), std::bind_front(&BTGameCart::writeIO, this), "70:0001-0005,6000");
  }

  auto disconnect() -> void {
    if(!active()) return;
    cpu.irq(0);
    *this = {};
  }

  auto power() -> void {
    // Reset restarts the local delivery session. Programming persists until erase.
    request = Request::Query;
    offset = received = rxSize = sent = 0;
    waitFrames = 60;
    selectedRegister = timer = ramConfig = 0;
    interrupts = endOfFrame = handoff = playing = false;
    for(auto& device : mode) device = FlashMode::Array;
    cpu.irq(0);
  }

  auto frame() -> void {
    if(!active() || !interrupts || playing || rxSize || request == Request::Done || request == Request::Failed) return;
    if(waitFrames) { waitFrames--; return; }

    // The four-byte big-endian length includes the command but excludes the
    // two link trailer bytes drained by the firmware before it checks RR1.
    rxSize = 4;
    rx[rxSize++] = (u8)request;
    if(request == Request::Query) rx[rxSize++] = 1;
    if(request == Request::Setup) {
      // The size is advertised in supported 512 KiB increments. Only actual
      // file bytes are transmitted and included in the additive checksum.
      u32 size = (media.size() + 0x7ffff) & ~0x7ffff;
      rx[rxSize++] = size >> 16;
      rx[rxSize++] = size >> 8;
      rx[rxSize++] = size;
      rx[rxSize++] = loRom ? 0 : 1;
      rx[rxSize++] = checksum >> 8;
      rx[rxSize++] = checksum;
      rx[rxSize++] = 0xff;  //complemented by the loader to start a valid rental
    }
    if(request == Request::Block) {
      rx[rxSize++] = 0;
      rx[rxSize++] = 0;
      rx[rxSize++] = offset >> 16;
      rx[rxSize++] = offset >> 8;
      rx[rxSize++] = offset;
      u32 count = std::min<u32>(BlockSize, media.size() - offset);
      memory::copy(rx + rxSize, media.data() + offset, count);
      rxSize += count;
    }
    rx[0] = rx[1] = 0;
    rx[2] = rxSize >> 8;
    rx[3] = rxSize;
    rx[rxSize++] = 0;
    rx[rxSize++] = 0;
    received = sent = 0;
    endOfFrame = false;
    cpu.irq(1);
  }

  // Advance only on the expected reply to the fully consumed frame. A short
  // error reply or a nonzero firmware result stops the local host.
  auto acceptReply(u8 byte) -> void {
    if(!rxSize || sent >= sizeof(tx)) return;
    tx[sent++] = byte;
    if(sent < 4) return;
    if(tx[0] || tx[1] || tx[2] || tx[3] < 5 || tx[3] > sizeof(tx)) {
      request = Request::Failed;
    } else if(sent < tx[3]) {
      return;
    } else if(tx[4] != (u8)request + 1 || received != rxSize) {
      request = Request::Failed;
    } else if(request == Request::Query) {
      request = sent == 12 && tx[6] == 0 && tx[11] == 1 ? Request::Setup : Request::Failed;
    } else if(request == Request::Abort) {
      // The $41 acknowledgement is five bytes and carries no status byte.
      request = sent == 5 ? Request::Done : Request::Failed;
    } else if(sent != 6 || tx[5] != 0) {
      request = Request::Failed;
    } else if(request == Request::Setup) {
      request = Request::Block;
    } else if(request == Request::Block) {
      offset += std::min<u32>(BlockSize, media.size() - offset);
      // Abort only after the first real block has been programmed and acknowledged.
      if(abortDownload) request = Request::Abort;
      else if(offset == media.size()) request = Request::Finish;
    } else if(request == Request::Finish) {
      request = Request::Done;
    } else {
      request = Request::Failed;
    }
    rxSize = sent = 0;
    cpu.irq(0);
  }

  auto readIO(n24 address, n8 openBus) -> n8 {
    if(playing && loRom) return read(address, openBus);
    switch((u16)address) {
    case 1: return (loRom ? 0 : 0x20) | (timer == 0xff ? 0x80 : 0);  //healthy battery
    case 2: {
      auto reg = selectedRegister;
      selectedRegister = 0;
      if(reg == 1) return endOfFrame ? 0x80 : 0;  //RR1: complete local frame with good CRC
      return 0x04 | (received < rxSize ? 1 : 0);  //RR0: transmit space and receive available
    }
    case 3:
      if(received < rxSize) {
        u8 data = rx[received++];
        if(received == rxSize) { endOfFrame = true; cpu.irq(0); }
        return data;
      }
    }
    return openBus;
  }

  auto writeIO(n24 address, n8 data) -> void {
    // After the handoff the adapter's private ports overlap LoROM save RAM.
    if(playing && loRom) return write(address, data);
    switch((u16)address) {
    case 1: timer = data; break;
    case 2:
      if(selectedRegister) {
        if(selectedRegister == 9) {
          interrupts = !(data & 0xc0) && (data & 8);
          cpu.irq(interrupts && received < rxSize);
        }
        selectedRegister = 0;
      } else if(data < 16) {
        selectedRegister = data;
      }
      break;
    case 3: acceptReply(data); break;
    case 4: ramConfig = data; break;
    case 5: break;  //write strobe, not a second SCC data byte
    case 0x6000: handoff = data == 0; break;
    }
  }

  // A LoROM bank exposes 32 KiB of flash. Its upper bank mirror is the
  // firmware's flash read port while the lower banks still execute the BIOS.
  auto flashOffset(n24 address) const -> u32 {
    if(loRom) return ((address & 0x7f0000) >> 1) | (address & 0x7fff);
    return address & 0x3fffff;
  }

  auto isSaveRam(n24 address) const -> bool {
    return playing && loRom && cartridge.ram.size() && (address & 0x708000) == 0x700000
      && (ramConfig == 0x7e || ramConfig == 0x72 || ramConfig == 0x42);
  }

  auto readMapped(n24 address) const -> n8 {
    u32 index = flashOffset(address);
    if(isSaveRam(address)) return cartridge.ram.read(index % cartridge.ram.size());
    if(playing || address >= (loRom ? 0x800000 : 0xc00000)) {
      if(mode[index >> 20] == FlashMode::Status) return n8(0x80);
      return n8(flash[index]);
    }
    u32 bootOffset = ((address >> 16 & 1) << 15) | (address & 0x7fff);
    return cartridge.rom.read(bootOffset);
  }

  auto read(n24 address, n8 openBus) -> n8 {
    // The write at $8ECB precedes XCE and JMP ($FFFC), both still in the BIOS.
    // Latch at the vector fetch so these final instruction bytes remain visible.
    if(handoff && address == 0x00fffc) { playing = true; handoff = false; }
    return readMapped(address);
  }

  auto write(n24 address, n8 data) -> void {
    u32 index = flashOffset(address);
    if(isSaveRam(address)) return cartridge.ram.write(index % cartridge.ram.size(), data);
    if(playing) return;
    if(loRom ? !(address & 0x8000) : address < 0xc00000) return;
    auto& device = mode[index >> 20];
    if(device == FlashMode::Program) {
      flash[index] &= data;
      device = FlashMode::Array;
      return;
    }
    if(data == 0xd0 && device == FlashMode::EraseSetup) {
      std::fill_n(flash.begin() + (index & 0x300000), 1_MiB, 0xff);
      device = FlashMode::Status;
      return;
    }
    switch(data) {
    case 0x40: device = FlashMode::Program; break;
    case 0xa7: device = FlashMode::EraseSetup; break;
    case 0x50: case 0x71: device = FlashMode::Status; break;
    case 0xff: device = FlashMode::Array; break;
    }
  }

  auto serialize(serializer& s) -> void {
    s(std::span<u8>{flash}); s(mode);
    s(request); s(offset); s(rx); s(tx); s(received); s(rxSize); s(sent);
    s(waitFrames); s(selectedRegister); s(timer);
    s(interrupts); s(endOfFrame); s(handoff); s(playing);
    if(loRom) s(ramConfig);
    // A damaged state must never index past its bound download or FIFO.
    if(s.reading() && (rxSize > sizeof(rx) || (rxSize && received > rxSize)
    || sent > sizeof(tx) || offset > media.size())) {
      request = Request::Failed;
      rxSize = received = sent = 0;
    }
  }
};

static BTGameCart btGameCart{};
auto btGameCartFrame() -> void { btGameCart.frame(); }
