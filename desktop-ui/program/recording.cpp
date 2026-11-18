#if defined(ARES_ENABLE_FFV1)
#include "../recording/movie.hpp"

namespace {
std::unique_ptr<recording::Movie> movie;
// A hotkey can arrive while the debugger is stopped partway through a scanline.
bool waitForMovieBoundary = false;

auto movieFailed(const std::exception& error) -> void {
  ares::SuperFamicom::nativeVideo = {};
  program.movieActive = false;
  program.runAheadUpdate();
  program.showMessage({"Recording failed: ", error.what(), " (", movie->path().c_str(), ")"});
  movie.reset();
}

auto captureMovieField(const ares::SuperFamicom::NativeVideoFrame& frame) -> void {
  if(waitForMovieBoundary) {
    waitForMovieBoundary = false;
    return;
  }
  try {
    movie->video(frame);
  } catch(const std::exception& error) {
    movieFailed(error);
  }
}

auto captureMovieAudio(const double samples[2]) -> void {
  if(!movie || waitForMovieBoundary) return;
  try {
    movie->audio(samples[0], samples[1]);
  } catch(const std::exception& error) {
    movieFailed(error);
  }
}
}

auto Program::startRecording(bool fromBoot) -> void {
  if(!emulator || emulator->name != "Super Famicom" || rewinding) {
    showMessage("Native recording requires a forward-running Super Famicom game");
    return;
  }
  string filename = emulator->locate({Location::notsuffix(emulator->game->location), " ",
    chrono::local::datetime().transform(":", "-"), ".mkv"}, ".mkv", settings.paths.screenshots);
  try {
    movie = std::make_unique<recording::Movie>(std::filesystem::u8path(filename.data()), ruby::audio.frequency());
  } catch(const std::exception& error) {
    showMessage({"Cannot start recording: ", error.what()});
    return;
  }
  waitForMovieBoundary = !fromBoot;
  ares::SuperFamicom::nativeVideo = {captureMovieField, [] { program.stopRecording(); }};
  movieActive = true;
  runAheadUpdate();
  if(fromBoot) recordNextGame = false;
  showMessage("Recording native video and audio");
}

auto Program::stopRecording() -> void {
  if(!movie) return;
  ares::SuperFamicom::nativeVideo = {};
  movieActive = false;
  runAheadUpdate();
  try {
    movie->finish();
    if(movie->path().empty()) showMessage("Recording stopped before the first frame");
    else showMessage({"Saved recording: ", movie->path().c_str()});
    movie.reset();
  } catch(const std::exception& error) {
    movieFailed(error);
  }
}

auto Program::toggleRecording() -> void {
  Program::Guard guard;
  if(movie) stopRecording();
  else if(emulator) startRecording(false);
  else recordNextGame = !recordNextGame;
}
#endif
