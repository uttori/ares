struct HVC_NROM : Interface {
  static auto create(string id) -> Interface* {
    if(id == "HVC-NROM"    ) return new HVC_NROM;
    if(id == "HVC-NROM-128") return new HVC_NROM;
    if(id == "HVC-NROM-256") return new HVC_NROM;
    return nullptr;
  }

  Memory::Readable<n8> programROM;
  Memory::Writable<n8> programRAM;
  Memory::Readable<n8> characterROM;
  Memory::Writable<n8> characterRAM;

  auto load() -> void override {
    Interface::load(programROM, "program.rom");
    Interface::load(programRAM, "save.ram");
    Interface::load(characterROM, "character.rom");
    Interface::load(characterRAM, "character.ram");
    mirror = pak->attribute("mirror") == "vertical";
  }

  auto save() -> void override {
    Interface::save(programRAM, "save.ram");
    Interface::save(characterRAM, "character.ram");
  }

  auto readPRG(n32 address, n8 data) -> n8 override {
    if(address < 0x6000) return data;
    if(address < 0x8000 && !programRAM) return data;
    if(address < 0x8000) return programRAM.read(address);
    return programROM.read(address);
  }

  auto debugPRGOffset(n16 address) -> maybe<u32> override {
    if(address < 0x8000 || !programROM) return {};
    return u32(address) & (programROM.size() - 1);
  }

  auto debugMapper() -> std::array<u8, 18> override {
    std::array<u8, 18> state{};
    state[0] = 0; state[13] = bool(programRAM); state[14] = 1; state[15] = !mirror;
    return state;
  }

  auto debugRead(const string& space, u32 address) -> maybe<n8> override {
    if(space == "nes.prg-rom" && address < programROM.size()) return programROM.read(address);
    if(space == "nes.chr-rom" && address < characterROM.size()) return characterROM.read(address);
    if(space == "nes.chr-ram" && address < characterRAM.size()) return characterRAM.read(address);
    if(space == "nes.prg-ram" && address < programRAM.size()) return programRAM.read(address);
    if(space == "nes.cpu") {
      if(auto offset = debugPRGOffset(address)) return programROM.read(*offset);
      if(address >= 0x6000 && address < 0x8000 && programRAM) { return programRAM.read(address & 0x1fff); }
    }
    return {};
  }

  auto debugPPUAddress(n14 address) -> maybe<DebugPPUAddress> override {
    if(address & 0x2000) return DebugPPUAddress{3, u32(address >> !mirror & 0x0400 | (n10)address), true};
    if(characterROM) return DebugPPUAddress{1, u32(address) & (characterROM.size() - 1), false};
    if(characterRAM) return DebugPPUAddress{2, u32(address) & (characterRAM.size() - 1), true};
    return {};
  }

  auto writePRG(n32 address, n8 data) -> void override {
    if(address < 0x6000) return;
    if(address < 0x8000 && !programRAM) return;
    if(address < 0x8000) return programRAM.write(address, data);
  }

  auto readCHR(n32 address, n8 data) -> n8 override {
    if(address & 0x2000) {
      address = address >> !mirror & 0x0400 | (n10)address;
      return ppu.readCIRAM(address);
    }
    if(characterROM) return characterROM.read(address);
    if(characterRAM) return characterRAM.read(address);
    return data;
  }

  auto writeCHR(n32 address, n8 data) -> void override {
    if(address & 0x2000) {
      address = address >> !mirror & 0x0400 | (n10)address;
      return ppu.writeCIRAM(address, data);
    }
    if(characterRAM) {
      cpu.capturePPUWrite(1, u32(address) & (characterRAM.size() - 1), data);
      return characterRAM.write(address, data);
    }
  }

  auto power() -> void override {
  }

  auto serialize(serializer& s) -> void override {
    s(programRAM);
    s(characterRAM);
    s(mirror);
  }

  n1 mirror;  //0 = horizontal, 1 = vertical
};
