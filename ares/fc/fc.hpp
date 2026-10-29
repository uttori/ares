#pragma once
//started: 2011-09-05

#include <ares/ares.hpp>
#include <vector>

#include <component/processor/mos6502/mos6502.hpp>
#include <component/audio/ym2149/ym2149.hpp>
#include <component/audio/ym2413/ym2413.hpp>
#include <component/eeprom/m24c/m24c.hpp>
#include <component/flash/sst39sf0x0/sst39sf0x0.hpp>
#include <component/arcade/dip-switches.hpp>
#include "ymfm_opn.h"

namespace ares::Famicom {
  #include <ares/inline.hpp>
  auto enumerate() -> std::vector<string>;
  auto load(Node::System& node, string name) -> bool;

  struct Region {
    static inline auto NTSCJ() -> bool;
    static inline auto NTSCU() -> bool;
    static inline auto PAL()   -> bool;
    static inline auto Dendy() -> bool;
  };

  #include <fc/controller/controller.hpp>
  #include <fc/expansion/expansion.hpp>
  #include <fc/vs/unisystem.hpp>
  #include <fc/system/system.hpp>
  #include <fc/cartridge/cartridge.hpp>
  #include <fc/cpu/cpu.hpp>
  #include <fc/apu/apu.hpp>
  #include <fc/ppu/ppu.hpp>
  #include <fc/fds/fds.hpp>

  auto captureControllerInput(u8 slot, u8 value) -> u8;
  auto captureControllerPoll(Node::Input::Button button, u8 slot) -> void;
  // A reservation can yield only to the frontend; re-check recording before the observed operation.
  auto captureNesReady() -> bool;
  // Source 0 CPU, 1 PPU, 2 cartridge. Device time is reconstructed from scheduler-relative clocks.
  auto captureNesTimeline(u8 kind, u16 address, u8 value, const std::array<u8, 8>& before) -> void;
}
