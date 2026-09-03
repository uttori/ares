#include <sfc/sfc.hpp>
#include <nall/gdb/server.hpp>
#include <algorithm>

namespace ares::SuperFamicom {

auto enumerate() -> std::vector<string> {
  return {
    "[Nintendo] Super Famicom (NTSC)",
    "[Nintendo] Super Famicom (PAL)",
  };
}

auto load(Node::System& node, string name) -> bool {
  auto list = enumerate();
  if(std::find(list.begin(), list.end(), name) == list.end()) return false;
  return system.load(node, name);
}

auto option(string name, string value) -> bool {
  if(name == "Pixel Accuracy") ppu.setAccurate(value.boolean());
  if(name == "Deterministic Entropy") system.deterministicEntropy = value.boolean();
  return true;
}

Random random;
Scheduler scheduler;
System system;
#include "controls.cpp"
#include "serialization.cpp"

namespace {

auto gdbHexLittleEndian(u64 value, u32 byteCount) -> string {
  string result;
  for(u32 byte : range(byteCount)) {
    result.append(hex(value >> byte * 8 & 0xff, 2, '0'));
  }
  return result;
}

auto installGdbHooks() -> void {
  using nall::GDB::server;

  server.hooks.instructionBoundaryStop = true;
  server.hooks.instructionTrace = true;

  server.hooks.targetXML = []() -> string {
    return "<target version=\"1.0\">"
      "<feature name=\"org.uttori.wdc65816.core\">"
        "<reg name=\"a\" bitsize=\"16\" regnum=\"0\"/>"
        "<reg name=\"x\" bitsize=\"16\" regnum=\"1\"/>"
        "<reg name=\"y\" bitsize=\"16\" regnum=\"2\"/>"
        "<reg name=\"s\" bitsize=\"16\" regnum=\"3\"/>"
        "<reg name=\"d\" bitsize=\"16\" regnum=\"4\"/>"
        "<reg name=\"db\" bitsize=\"8\" regnum=\"5\"/>"
        "<reg name=\"pb\" bitsize=\"8\" regnum=\"6\"/>"
        "<reg name=\"pc\" bitsize=\"16\" regnum=\"7\" type=\"code_ptr\"/>"
        "<reg name=\"p\" bitsize=\"8\" regnum=\"8\"/>"
        "<reg name=\"e\" bitsize=\"8\" regnum=\"9\"/>"
      "</feature>"
    "</target>";
  };

  server.hooks.read = [](u64 address, u32 byteCount) -> string {
    string result;
    result.reserve(byteCount * 2);
    for(u32 byte : range(byteCount)) {
      n24 current = address + byte & 0xff'ffff;
      u24 busAddress = current;
      u24 deviceAddress = busAddress & 0x40'ffff;
      n8 value = 0;
      if(deviceAddress < 0x20'00 || deviceAddress > 0x5f'ff) {
        value = cpu.readDisassembler(current);
      }
      result.append(hex(value, 2, '0'));
    }
    return result;
  };

  server.hooks.namedMemoryRead = [](const string& addressSpace, u64 address, u32 byteCount) -> string {
    if(address > 0xff'ffff) return "E00";
    u64 memorySize = 0;
    if(addressSpace == "snes.wram") memorySize = 128_KiB;
    else if(addressSpace == "snes.cartridge-rom") memorySize = cartridge.rom.size();
    else return "E00";

    if(address >= memorySize) return {};
    u32 readable = min<u64>(byteCount, memorySize - address);
    string result;
    result.reserve(readable * 2);
    for(u32 byte : range(readable)) {
      n8 value = addressSpace == "snes.wram"
        ? cpu.wram[address + byte]
        : cartridge.rom.read(address + byte);
      result.append(hex(value, 2, '0'));
    }
    return result;
  };

  server.hooks.regRead = [](u32 registerIndex) -> string {
    switch(registerIndex) {
      case 0: return gdbHexLittleEndian(cpu.r.a.w, 2);
      case 1: return gdbHexLittleEndian(cpu.r.x.w, 2);
      case 2: return gdbHexLittleEndian(cpu.r.y.w, 2);
      case 3: return gdbHexLittleEndian(cpu.r.s.w, 2);
      case 4: return gdbHexLittleEndian(cpu.r.d.w, 2);
      case 5: return gdbHexLittleEndian(cpu.r.b, 1);
      case 6: return gdbHexLittleEndian(cpu.r.pc.b, 1);
      case 7: return gdbHexLittleEndian(cpu.r.pc.w, 2);
      case 8: return gdbHexLittleEndian(cpu.r.p, 1);
      case 9: return gdbHexLittleEndian(cpu.r.e, 1);
    }
    return {};
  };

  server.hooks.regReadGeneral = []() -> string {
    string result;
    for(u32 registerIndex : range(10)) {
      result.append(server.hooks.regRead(registerIndex));
    }
    return result;
  };
}

}

auto System::game() -> string {
  #if defined(CORE_GB)
  if(cartridge.has.GameBoySlot && GameBoy::cartridge.node) {
    return GameBoy::cartridge.title();
  }
  #endif

  if(bsmemory.node) {
    return {cartridge.title(), " + ", bsmemory.title()};
  }

  if(sufamiturboA.node && sufamiturboB.node) {
    return {sufamiturboA.title(), " + ", sufamiturboB.title()};
  }

  if(sufamiturboA.node) {
    return sufamiturboA.title();
  }

  if(sufamiturboB.node) {
    return sufamiturboB.title();
  }

  if(cartridge.node) {
    return cartridge.title();
  }

  return "(no cartridge connected)";
}

auto System::run() -> void {
  scheduler.enter();
  auto reset = controls.reset->value();
  controls.poll();
  if(!reset && controls.reset->value()) power(true);
}

auto System::load(Node::System& root, string name) -> bool {
  if(node) unload();

  information = {};
  if(name.find("Super Famicom")) {
    information.name = "Super Famicom";
  }
  if(name.find("NTSC")) {
    information.region = Region::NTSC;
    information.cpuFrequency = Constants::Colorburst::NTSC * 6.0;
  }
  if(name.find("PAL")) {
    information.region = Region::PAL;
    information.cpuFrequency = Constants::Colorburst::PAL * 4.8;
  }

  node = std::make_shared<Core::System>(information.name);
  node->setAttribute("configuration", name);
  node->setGame(std::bind_front(&System::game, this));
  node->setRun(std::bind_front(&System::run, this));
  node->setPower(std::bind_front(&System::power, this));
  node->setSave(std::bind_front(&System::save, this));
  node->setUnload(std::bind_front(&System::unload, this));
  node->setSerialize([this](bool s){ return this->serialize(s); });
  node->setUnserialize([this](serializer& s){ return this->unserialize(s); });
  root = node;
  if(!node->setPak(pak = platform->pak(node))) return false;

  scheduler.reset();
  bus.reset();
  controls.load(node);
  cpu.load(node);
  installGdbHooks();
  smp.load(node);
  dsp.load(node);
  ppu.load(node);
  cartridgeSlot.load(node);
  controllerPort1.load(node);
  controllerPort2.load(node);
  expansionPort.load(node);
  return true;
}

auto System::unload() -> void {
  if(!node) return;
  save();
  cpu.unload();
  smp.unload();
  dsp.unload();
  ppu.unload();
  cartridgeSlot.unload();
  controllerPort1.unload();
  controllerPort2.unload();
  expansionPort.unload();
  pak.reset();
  node.reset();
}

auto System::save() -> void {
  if(!node) return;
  cartridge.save();
}

auto System::power(bool reset) -> void {
  for(auto& setting : node->find<Node::Setting::Setting>()) setting->setLatch();

  random.entropy(Random::Entropy::Low);
  if(deterministicEntropy) random.seed((n64)0);

  cpu.power(reset);
  smp.power(reset);
  dsp.power(reset);
  ppu.power(reset);
  cartridge.power(reset);
  scheduler.power(cpu);
}

}
