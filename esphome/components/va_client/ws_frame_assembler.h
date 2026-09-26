#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace esphome {
namespace va_client {

enum class WsMessageType {
  UNKNOWN, ERROR, HELLO, AUDIO_DONE, REQUEST_FOLLOW_UP, PHASE,
  TIMER_START, TIMER_CANCEL, TIMER_LIST
};

inline WsMessageType classify_ws_message(std::string_view message) {
  constexpr std::string_view key = "\"type\"";
  const size_t key_pos = message.find(key);
  if (key_pos == std::string_view::npos)
    return WsMessageType::UNKNOWN;
  size_t start = key_pos + key.size();
  const auto skip_space = [&]() {
    while (start < message.size() && (message[start] == ' ' || message[start] == '\t' ||
                                      message[start] == '\n' || message[start] == '\r'))
      ++start;
  };
  skip_space();
  if (start == message.size() || message[start++] != ':')
    return WsMessageType::UNKNOWN;
  skip_space();
  if (start == message.size() || message[start++] != '"')
    return WsMessageType::UNKNOWN;
  const size_t end = message.find('"', start);
  if (end == std::string_view::npos)
    return WsMessageType::UNKNOWN;
  const auto type = message.substr(start, end - start);
  if (type == "error")
    return WsMessageType::ERROR;
  if (type == "hello")
    return WsMessageType::HELLO;
  if (type == "audio_done")
    return WsMessageType::AUDIO_DONE;
  if (type == "request_follow_up")
    return WsMessageType::REQUEST_FOLLOW_UP;
  if (type == "phase")
    return WsMessageType::PHASE;
  if (type == "timer_start")
    return WsMessageType::TIMER_START;
  if (type == "timer_cancel")
    return WsMessageType::TIMER_CANCEL;
  if (type == "timer_list")
    return WsMessageType::TIMER_LIST;
  return WsMessageType::UNKNOWN;
}

// esp_websocket_client reports offsets within a frame, not within a whole
// fragmented message. A continuation starts a new frame at offset zero.
class WsTextAssembler {
 public:
  enum class Result { INCOMPLETE, COMPLETE, DROPPED };
  static constexpr size_t kMaxBytes = 4096;

  void reset() {
    size_ = 0;
    frame_offset_ = 0;
    frame_length_ = 0;
    active_ = false;
  }

  Result append(bool continuation, bool fin, const char *data, size_t length,
                size_t offset, size_t payload_length) {
    if (!continuation && offset == 0) {
      reset();
      active_ = true;
    }
    if (!active_ || (offset == 0 && continuation && frame_offset_ != frame_length_) ||
        (offset != 0 && (offset != frame_offset_ || payload_length != frame_length_)) ||
        offset > payload_length || length > payload_length - offset ||
        length > kMaxBytes - size_ ||
        (length > 0 && data == nullptr)) {
      reset();
      return Result::DROPPED;
    }
    if (offset == 0)
      frame_length_ = payload_length;
    if (length > 0)
      std::memcpy(bytes_.data() + size_, data, length);
    size_ += length;
    frame_offset_ = offset + length;
    if (frame_offset_ != frame_length_ || !fin)
      return Result::INCOMPLETE;
    active_ = false;
    return Result::COMPLETE;
  }

  const char *data() const { return bytes_.data(); }
  size_t size() const { return size_; }

 private:
  std::array<char, kMaxBytes> bytes_{};
  size_t size_{0};
  size_t frame_offset_{0};
  size_t frame_length_{0};
  bool active_{false};
};

class Pcm16FrameAssembler {
 public:
  void reset() {
    pending_ = false;
    active_ = false;
    frame_offset_ = 0;
    frame_length_ = 0;
  }

  // Produces only whole little-endian PCM16 samples. A new binary message
  // discards an incomplete sample from the previous message.
  bool append(bool continuation, const uint8_t *data, size_t length, size_t offset,
              size_t payload_length, std::vector<uint8_t> &output) {
    output.clear();
    if (!continuation && offset == 0) {
      reset();
      active_ = true;
    }
    if (!active_ || (offset == 0 && continuation && frame_offset_ != frame_length_) ||
        (offset != 0 && (offset != frame_offset_ || payload_length != frame_length_)) ||
        offset > payload_length || length > payload_length - offset ||
        (length > 0 && data == nullptr)) {
      reset();
      return false;
    }
    if (offset == 0)
      frame_length_ = payload_length;
    frame_offset_ = offset + length;
    size_t i = 0;
    if (pending_ && length > 0) {
      output.push_back(pending_byte_);
      output.push_back(data[i++]);
      pending_ = false;
    }
    const size_t pairs_bytes = (length - i) & ~size_t{1};
    if (pairs_bytes > 0)
      output.insert(output.end(), data + i, data + i + pairs_bytes);
    i += pairs_bytes;
    if (i < length) {
      pending_byte_ = data[i];
      pending_ = true;
    }
    return true;
  }

 private:
  size_t frame_offset_{0};
  size_t frame_length_{0};
  uint8_t pending_byte_{0};
  bool pending_{false};
  bool active_{false};
};

// Estimates how much audio is still queued downstream (resampler, mixer and
// I2S buffers) from what was fed and the wall clock. Downstream playback
// starts no earlier than the feed, so the real queue is never shorter.
class PlaybackClock {
 public:
  void reset() { this->valid_ = false; }
  void fed(int64_t now_us, size_t bytes, uint32_t bytes_per_second) {
    if (this->remaining_us(now_us) == 0) {
      this->until_us_ = now_us;
      this->valid_ = true;
    }
    this->until_us_ += static_cast<int64_t>(static_cast<uint64_t>(bytes) * 1000000u / bytes_per_second);
  }
  uint32_t remaining_us(int64_t now_us) const {
    if (!this->valid_ || this->until_us_ <= now_us)
      return 0;
    return static_cast<uint32_t>(this->until_us_ - now_us);
  }

 private:
  int64_t until_us_{0};
  bool valid_{false};
};

// Scales a sample by num/den with signed math (size_t operands would make a
// negative sample wrap to a large unsigned value).
inline int16_t scale_sample(int16_t sample, size_t num, size_t den) {
  return static_cast<int16_t>((static_cast<int32_t>(sample) * static_cast<int32_t>(num)) /
                              static_cast<int32_t>(den));
}

// Continues a linear fade-in of fade_samples samples that is already at
// position pos (samples faded so far), so a fade can span several chunks.
// Returns the new position; pos >= fade_samples means no fade is active.
inline size_t fade_in_samples(int16_t *samples, size_t count, size_t fade_samples, size_t pos) {
  for (size_t i = 0; i < count && pos < fade_samples; i++, pos++)
    samples[i] = scale_sample(samples[i], pos, fade_samples);
  return pos;
}

inline size_t barge_in_trim(size_t fill, size_t fade_bytes) {
  return fill < fade_bytes ? fill : fade_bytes;
}

inline bool barge_in_mic_allowed(bool phase_is_replying, bool barge_in_effective,
                                 uint32_t playback_started_ms, uint32_t now_ms, uint32_t holdoff_ms) {
  // A frame captured just before playback starts must not look 49 days old.
  return !phase_is_replying || !barge_in_effective ||
         (playback_started_ms != 0 &&
          static_cast<int32_t>(now_ms - playback_started_ms) >= static_cast<int32_t>(holdoff_ms));
}

inline bool release_fade_tail(size_t bytes, size_t fade_bytes, bool done, bool speaker_dry,
                              uint32_t now_ms, uint32_t last_audio_ms,
                              uint32_t starvation_ms) {
  return bytes > 0 && (done ||
      (speaker_dry && bytes <= fade_bytes && last_audio_ms != 0 &&
       now_ms - last_audio_ms >= starvation_ms));
}

}  // namespace va_client
}  // namespace esphome
