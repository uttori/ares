#include <fc/fc.hpp>
#include <nall/gdb/server.hpp>

namespace ares::Famicom {

Cartridge& cartridge = cartridgeSlot.cartridge;
#include "slot.cpp"
#include "board/board.cpp"
#include "serialization.cpp"

auto Cartridge::allocate(Node::Port parent) -> Node::Peripheral {
  return node = parent->append<Node::Peripheral>(string{system.name(), " Cartridge"});
}

auto Cartridge::connect() -> void {
  if(!node->setPak(pak = platform->pak(node))) return;

  information = {};
  information.title  = pak->attribute("title");
  information.region = pak->attribute("region");

  board.reset(Board::Interface::create(pak->attribute("board")));
  board->pak = pak;
  board->load();

  power();
  if(fds.present) {
    fds.load(node);
  }
}

auto Cartridge::disconnect() -> void {
  if(!node) return;
  if(fds.present) {
    fds.unload();
    fds.present = 0;
  }
  board->unload();
  board->pak.reset();
  board.reset();
  pak.reset();
  information = {};
  node.reset();
}

auto Cartridge::save() -> void {
  if(!node) return;
  board->save();
}

auto Cartridge::power() -> void {
  Thread::create(system.frequency(), std::bind_front(&Cartridge::main, this));
  board->power();
}

auto Cartridge::main() -> void {
  board->main();
}

auto Cartridge::readPRG(n32 address, n8 data) -> n8 {
  return board->readPRG(address, data);
}

auto Cartridge::writePRG(n32 address, n8 data) -> void {
  bool recording = address >= 0xc000 && nall::GDB::server.capture.busTarget().nesBusActive()
    && board->debugMapper()[0] == 4 && captureNesReady();
  std::array<u8, 8> before{};
  if(recording) before = board->debugIRQ();
  board->writePRG(address, data);
  if(recording) captureNesTimeline(2, address, data, before);
}

auto Cartridge::readCHR(n32 address, n8 data) -> n8 {
  return board->readCHR(address, data);
}

auto Cartridge::writeCHR(n32 address, n8 data) -> void {
  return board->writeCHR(address, data);
}

auto Cartridge::ppuAddressBus(n14 address) -> void {
  if(!nall::GDB::server.capture.busTarget().nesBusActive()) return board->ppuAddressBus(address);
  auto before = board->debugIRQ();
  bool edge = before[4] != bool(address & 0x1000) && board->debugMapper()[0] == 4;
  bool recording = edge && captureNesReady();
  board->ppuAddressBus(address);
  if(recording) captureNesTimeline(1, address, 0, before);
}

auto Cartridge::scanline(n32 y) -> void {
  return board->scanline(y);
}

}
