#include <nall/main.hpp>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace nall;
using std::max;
using std::min;
struct Size { f32 w, h; template<typename W, typename H> Size(W width, H height) : w(f32(width)), h(f32(height)) {} auto width() const { return w; } auto height() const { return h; } };
struct Monitor {
  static auto count() -> u32 { return 2; }
  static auto workspace(u32) -> Size { return {1920, 1080}; }
};
struct Screen {
  u32 w = 256, h = 224;
  auto width() { return w; } auto height() { return h; }
  auto scaleX() { return 1; } auto scaleY() { return 1; }
  auto aspectX() { return 1; } auto aspectY() { return 1; }
  auto rotation() { return 0; }
};
struct { bool kiosk = false; std::vector<std::shared_ptr<Screen>> screens; } program;
struct { struct { u32 windowWidth = 800, windowHeight = 576; bool adaptiveSizing = true, autoCentering = true; string aspectCorrection = "None"; } video; } settings;
bool emulator = true;
struct Alignment { static constexpr int Center = 0; };
auto setConfiguredWindowSize(u32 w, u32 h) -> void { settings.video.windowWidth = w; settings.video.windowHeight = h; }
auto minimumConfiguredWindowSize() -> Size { return {160, 144}; }
// Coordinates deliberately start on the secondary monitor. Centering models the primary workspace.
struct Presentation {
  enum : u32 { StatusHeight = 24 };
  int x = 2300, y = 100;
  bool fullscreen = false, isMaximized = false;
  Size size{800, 600};
  struct { auto checked() { return true; } } showStatusBarSetting;
  auto fullScreen() { return fullscreen; } auto maximized() { return isMaximized; }
  auto setFullScreen(bool value) -> void { fullscreen = value; }
  auto setSize(Size value) -> void { size = value; }
  auto setGeometry(int, Size value) -> void { x = 500; y = 200; size = value; }
  auto setMinimumSize(Size) -> void {}
  auto resizeWindow(bool preservePlacement = false) -> void;
};
#include "resize.inc"
auto require(bool condition, const char* message) -> void {
  if(!condition) throw std::runtime_error(message);
}
auto nall::main(Arguments) -> void {
  program.screens.push_back(std::make_shared<Screen>());
  for(bool autoCenter : {false, true}) {
    settings.video.autoCentering = autoCenter;
    Presentation window;
    window.resizeWindow(true);
    require(window.x == 2300 && window.y == 100, "First Continue moved the paused window");
    require(window.size.width() < 800, "Frame aspect adjustment was lost");
    program.screens.front()->h = 240;
    window.resizeWindow(true);
    require(window.x == 2300 && window.y == 100, "Resolution switch moved the window");
    window.fullscreen = true;
    window.resizeWindow(true);
    require(window.fullscreen, "Resolution switch exited fullscreen");
    window.fullscreen = false;
    window.resizeWindow();
    require((window.x == 500) == autoCenter, "Explicit window sizing ignored centering preference");
    program.screens.front()->h = 224;
  }
  std::cout << "PASS first-frame/resolution placement, fullscreen, and explicit sizing preferences\n";
}
