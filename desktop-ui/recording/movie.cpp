#include "movie.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

namespace recording {
namespace fs = std::filesystem;
using ares::SuperFamicom::NativeVideoFrame;

static auto check(int result, const char* operation) -> void {
  if(result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}

struct Movie::Part {
  AVFormatContext* format = nullptr;
  AVCodecContext* encoder = nullptr;
  AVFrame* picture = nullptr;
  AVPacket* packet = nullptr;
  AVStream *video = nullptr, *audio = nullptr;
  fs::path partial;
  bool hasField = false;

  ~Part() {
    if(format && format->pb) avio_closep(&format->pb);
    av_packet_free(&packet);
    av_frame_free(&picture);
    avcodec_free_context(&encoder);
    avformat_free_context(format);
  }

  auto open(const NativeVideoFrame& src, unsigned rate) -> void {
    check(avformat_alloc_output_context2(&format, nullptr, "matroska", nullptr), "Matroska");
    auto codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if(!codec) throw std::runtime_error("FFmpeg has no FFV1 encoder");
    encoder = avcodec_alloc_context3(codec);
    video = avformat_new_stream(format, nullptr);
    audio = avformat_new_stream(format, nullptr);
    picture = av_frame_alloc();
    packet = av_packet_alloc();
    if(!encoder || !video || !audio || !picture || !packet) throw std::bad_alloc();
    encoder->width = src.width;
    encoder->height = src.lines * (src.interlace ? 2 : 1);
    encoder->pix_fmt = AV_PIX_FMT_BGR0;
    encoder->time_base = av_d2q(1.0 / src.frequency, INT_MAX);
    // A physical dot occupies half as much horizontal space in hires, and half a line in interlace.
    AVRational aspect = src.pal ? AVRational{55,43} : AVRational{8,7};
    aspect = av_mul_q(aspect, {int(src.interlace ? 512 : 256), int(src.width)});
    video->sample_aspect_ratio = encoder->sample_aspect_ratio = aspect;
    encoder->level = 3;
    encoder->thread_count = 1;
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(encoder, codec, nullptr), "Open FFV1");
    check(avcodec_parameters_from_context(video->codecpar, encoder), "Video stream");
    video->time_base = encoder->time_base;
    // DefaultDuration helps editors identify the nominal field rate. Actual timestamps use PPU clocks.
    double lines = src.pal ? 312 : 262;
    if(src.interlace) lines += 0.5;
    double ticks = 1364 * lines;
    if(!src.pal && !src.interlace) ticks -= 2;
    if(src.pal && src.interlace) ticks += 2;
    video->avg_frame_rate = av_d2q(src.frequency / ticks, 1000000);
    audio->time_base = {1, int(rate)};
    auto pcm = audio->codecpar;
    pcm->codec_type = AVMEDIA_TYPE_AUDIO;
    pcm->codec_id = AV_CODEC_ID_PCM_S16LE;
    pcm->format = AV_SAMPLE_FMT_S16;
    pcm->sample_rate = rate;
    pcm->bits_per_coded_sample = 16;
    pcm->block_align = 4;
    av_channel_layout_default(&pcm->ch_layout, 2);
    picture->format = encoder->pix_fmt;
    picture->width = encoder->width;
    picture->height = encoder->height;
    check(av_frame_get_buffer(picture, 0), "Allocate video frame");
    auto filename = partial.u8string();
    check(avio_open(&format->pb, reinterpret_cast<const char*>(filename.c_str()), AVIO_FLAG_WRITE), "Open output");
    check(avformat_write_header(format, nullptr), "Write Matroska header");
  }

  auto encode(AVFrame* frame) -> void {
    check(avcodec_send_frame(encoder, frame), "Encode FFV1");
    for(;;) {
      int result = avcodec_receive_packet(encoder, packet);
      if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      check(result, "Read FFV1 packet");
      if(frame) packet->duration = frame->duration;
      av_packet_rescale_ts(packet, encoder->time_base, video->time_base);
      packet->stream_index = video->index;
      check(av_interleaved_write_frame(format, packet), "Write video");
    }
  }
};

// Neutral SNES luminance, using the same 5-to-16-bit expansion as the core's RGB palette.
static auto channel(unsigned value, unsigned luma) -> uint8_t {
  unsigned expanded = (value << 11) | (value << 6) | (value << 1) | (value >> 4);
  return (expanded * (luma + 1) / (luma ? 16 : 64)) >> 8;
}

Movie::Movie(fs::path path, unsigned sampleRate) : base(std::move(path)), rate(sampleRate) {
  if(!rate) throw std::runtime_error("Audio mixer has no sample rate");
  pending.reserve(2 * rate);
}

Movie::~Movie() = default;

auto Movie::path() const -> std::string {
  const auto& current = part && !part->partial.empty() ? part->partial : output;
  auto utf8 = current.u8string();
  return {utf8.begin(), utf8.end()};
}

auto Movie::audio(double left, double right) -> void {
  // A missing video callback must not turn a failed recording into an unbounded audio spool.
  if(pending.size() >= 2 * rate) throw std::runtime_error("No native video for one second of audio");
  for(auto value : {left, right}) pending.push_back(int16_t(std::clamp(value, -1.0, 1.0) * 32767.0));
}

auto Movie::writeAudio(size_t frames) -> void {
  if(!frames) return;
  auto pkt = part->packet;
  check(av_new_packet(pkt, int(frames * 4)), "Allocate PCM packet");
  for(size_t i = 0; i < frames * 2; i++) {
    uint16_t sample = pending[i];
    pkt->data[2*i] = uint8_t(sample);
    pkt->data[2*i+1] = uint8_t(sample >> 8);
  }
  pkt->pts = pkt->dts = samples - partSample;
  pkt->duration = frames;
  pkt->stream_index = part->audio->index;
  av_packet_rescale_ts(pkt, {1, int(rate)}, part->audio->time_base);
  check(av_interleaved_write_frame(part->format, pkt), "Write audio");
  pending.erase(pending.begin(), pending.begin() + frames * 2);
  samples += frames;
}

auto Movie::video(const NativeVideoFrame& src) -> void {
  if(part) {
    // At the end of this field all sound for the preceding field is available, even if the DSP trails the PPU.
    auto boundary = av_rescale_q(clocks, part->encoder->time_base, {1, int(rate)});
    auto available = int64_t(pending.size() / 2);
    writeAudio(std::clamp<int64_t>(boundary - samples, 0, available));
    if(src.width != part->picture->width || src.lines * (src.interlace ? 2 : 1) != part->picture->height) closePart();
  }
  if(!part) {
    part = std::make_unique<Part>();
    // Exclusive partials coordinate concurrent recorders. A completed take also owns its name.
    for(;;) {
      output = base;
      if(++number > 1) {
        auto name = base.stem();
        name += "-" + std::to_string(number) + ".mkv";
        output.replace_filename(name);
      }
      if(fs::exists(output)) continue;
      part->partial = output;
      part->partial += ".partial";
#ifdef _WIN32
      auto fp = _wfopen(part->partial.c_str(), L"wbx");
#else
      auto fp = std::fopen(part->partial.c_str(), "wbx");
#endif
      if(!fp && errno == EEXIST) continue;
      if(!fp) throw std::system_error(errno, std::generic_category(), "Reserve movie file");
      std::fclose(fp);
      if(!fs::exists(output)) break;
      fs::remove(part->partial);
    }
    partClock = clocks;
    partSample = samples;
    part->open(src, rate);
  }

  auto frame = part->picture;
  check(av_frame_make_writable(frame), "Acquire video frame");
  for(unsigned y = 0; y < src.lines; y++) {
    auto row = src.pixels + y * src.pitch;
    unsigned line = y;
    if(src.interlace) line = 2*y + unsigned(src.odd);
    auto dst = frame->data[0] + line * frame->linesize[0];
    for(unsigned x = 0; x < src.width; x++) {
      auto color = row[x * src.step];
      auto brightness = (color >> 15) & 15;
      dst[4*x] = channel((color >> 10) & 31, brightness);
      dst[4*x+1] = channel((color >> 5) & 31, brightness);
      dst[4*x+2] = channel(color & 31, brightness);
      dst[4*x+3] = 0;
    }
    // Seed the missing parity once; subsequent interlaced frames retain the preceding field.
    if(src.interlace && !part->hasField) std::memcpy(frame->data[0] + (line ^ 1) * frame->linesize[0], dst, src.width * 4);
  }
  part->hasField = true;
  frame->pts = clocks - partClock;
  frame->duration = src.clocks;
  part->encode(frame);
  clocks += src.clocks;
}

auto Movie::closePart() -> void {
  part->encode(nullptr);
  check(av_write_trailer(part->format), "Finalize Matroska");
  check(avio_closep(&part->format->pb), "Close recording");
#if defined(__APPLE__)
  if(renamex_np(part->partial.c_str(), output.c_str(), RENAME_EXCL) < 0)
    throw std::system_error(errno, std::generic_category(), "Publish recording");
#elif defined(_WIN32)
  if(!MoveFileW(part->partial.c_str(), output.c_str()))
    throw std::system_error(GetLastError(), std::system_category(), "Publish recording");
#else
  // A hard link publishes the completed file atomically and fails if its name is already taken.
  fs::create_hard_link(part->partial, output);
  fs::remove(part->partial);
#endif
  part.reset();
}

auto Movie::finish() -> void {
  if(!part) return;
  writeAudio(pending.size() / 2);
  closePart();
}

}
