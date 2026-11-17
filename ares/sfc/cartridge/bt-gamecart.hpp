#pragma once

namespace ares::SuperFamicom {
  // Clock the local BT host once per emulated frame, including during debugger stepping.
  auto btGameCartFrame() -> void;
}
