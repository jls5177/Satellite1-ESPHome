#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace esphome {
namespace va_client {

enum class WsMessageType {
  UNKNOWN, ERROR, HELLO, AUDIO_DONE, REQUEST_FOLLOW_UP, PHASE,
  TIMER_START, TIMER_CANCEL, TIMER_LIST, ANNOUNCE, ANNOUNCE_CANCEL, ANNOUNCE_RESULT
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
  if (type == "announce")
    return WsMessageType::ANNOUNCE;
  if (type == "announce_cancel")
    return WsMessageType::ANNOUNCE_CANCEL;
  if (type == "announce_result")
    return WsMessageType::ANNOUNCE_RESULT;
  return WsMessageType::UNKNOWN;
}

inline bool valid_announcement_id(std::string_view id) {
  if (id.empty() || id.size() > 32)
    return false;
  for (char c : id) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}

// The announcement frames contain only flat string/bool fields. Validate the
// complete object so an "id" in another field cannot masquerade as its id.
inline bool announcement_json_field(std::string_view json, std::string_view key,
                                    std::string_view &value, bool &quoted) {
  size_t pos = 0;
  auto space = [&]() {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == '\r' || json[pos] == '\n')) ++pos;
  };
  space();
  if (pos == json.size() || json[pos++] != '{') return false;
  bool found = false;
  std::string_view parsed_value;
  bool parsed_quoted = false;
  do {
    space();
    if (pos < json.size() && json[pos] == '}') {
      ++pos;
      space();
      if (pos != json.size() || !found) return false;
      value = parsed_value;
      quoted = parsed_quoted;
      return true;
    }
    if (pos == json.size() || json[pos++] != '"') return false;
    size_t begin = pos;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' || static_cast<unsigned char>(json[pos]) < 0x20) return false;
      ++pos;
    }
    if (pos == json.size()) return false;
    const auto field = json.substr(begin, pos++ - begin);
    space();
    if (pos == json.size() || json[pos++] != ':') return false;
    space();
    quoted = pos < json.size() && json[pos] == '"';
    if (quoted) {
      begin = ++pos;
      while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\') {
          if (++pos == json.size()) return false;
        } else if (static_cast<unsigned char>(json[pos]) < 0x20) {
          return false;
        }
        ++pos;
      }
      if (pos == json.size()) return false;
      value = json.substr(begin, pos++ - begin);
    } else {
      begin = pos;
      while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
             json[pos] != ' ' && json[pos] != '\n' &&
             json[pos] != '\r' && json[pos] != '\t') ++pos;
      value = json.substr(begin, pos - begin);
    }
    if (field == key) {
      if (found) return false;
      found = true;
      parsed_value = value;
      parsed_quoted = quoted;
    }
    space();
    if (pos == json.size()) return false;
    if (json[pos] == '}') {
      ++pos;
      space();
      if (pos != json.size() || !found) return false;
      value = parsed_value;
      quoted = parsed_quoted;
      return true;
    }
    if (json[pos++] != ',') return false;
    space();
    if (pos == json.size() || json[pos] == '}') return false;
  } while (true);
}

struct AnnouncementRequest {
  std::string id;
  bool chime{false};
  bool follow_up{false};
};

inline bool parse_announcement(std::string_view json, AnnouncementRequest &out) {
  std::string_view id, chime, follow_up;
  bool quoted = false;
  if (!announcement_json_field(json, "id", id, quoted) || !quoted ||
      !valid_announcement_id(id))
    return false;
  if (!announcement_json_field(json, "chime", chime, quoted) || quoted ||
      (chime != "true" && chime != "false"))
    return false;
  if (!announcement_json_field(json, "follow_up", follow_up, quoted) || quoted ||
      (follow_up != "true" && follow_up != "false"))
    return false;
  out = {std::string(id), chime == "true", follow_up == "true"};
  return true;
}

inline bool parse_announcement_cancel(std::string_view json, std::string &id) {
  std::string_view value;
  bool quoted = false;
  if (!announcement_json_field(json, "id", value, quoted) || !quoted ||
      !valid_announcement_id(value))
    return false;
  id = std::string(value);
  return true;
}

inline std::string json_escape_string(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (c < 0x20) {
          result += "\\u00";
          result += hex[c >> 4];
          result += hex[c & 15];
        } else {
          result += static_cast<char>(c);
        }
    }
  }
  result += '"';
  return result;
}

inline std::string_view truncate_utf8(std::string_view text, size_t max_chars) {
  size_t pos = 0, count = 0;
  while (pos < text.size() && count < max_chars) {
    const auto c = static_cast<unsigned char>(text[pos]);
    size_t width = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 :
                   (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (width == 0 || pos + width > text.size()) break;
    for (size_t i = 1; i < width; ++i)
      if ((static_cast<unsigned char>(text[pos + i]) & 0xC0) != 0x80)
        return text.substr(0, pos);
    if (width > 1) {
      const auto next = static_cast<unsigned char>(text[pos + 1]);
      if ((width == 2 && c < 0xC2) ||
          (width == 3 && ((c == 0xE0 && next < 0xA0) ||
                          (c == 0xED && next >= 0xA0))) ||
          (width == 4 && (c > 0xF4 || (c == 0xF0 && next < 0x90) ||
                          (c == 0xF4 && next >= 0x90))))
        break;
    }
    pos += width;
    ++count;
  }
  return text.substr(0, pos);
}

class AnnouncementReservation {
 public:
  enum class State : uint8_t { IDLE, RESERVED, READY, PLAYING, DONE, CANCELLED };
  bool reserve(std::string_view id, bool follow_up) {
    if (state_ != State::IDLE || !valid_announcement_id(id)) return false;
    std::memcpy(id_.data(), id.data(), id.size());
    id_[id.size()] = '\0';
    state_ = State::RESERVED;
    follow_up_ = follow_up;
    return true;
  }
  bool ready(std::string_view id) {
    if (state_ != State::RESERVED || this->id() != id) return false;
    state_ = State::READY;
    return true;
  }
  bool playing() {
    if (state_ != State::READY && state_ != State::PLAYING) return false;
    state_ = State::PLAYING;
    return true;
  }
  bool finish() {
    if (state_ != State::PLAYING) return false;
    state_ = State::DONE;
    return true;
  }
  bool cancel(std::string_view id) {
    if (!active() || this->id() != id) return false;
    state_ = State::CANCELLED;
    return true;
  }
  void reset() { state_ = State::IDLE; id_[0] = '\0'; follow_up_ = false; }
  bool active() const {
    return state_ == State::RESERVED || state_ == State::READY || state_ == State::PLAYING;
  }
  bool admits_pcm() const { return state_ == State::READY || state_ == State::PLAYING; }
  bool follow_up() const { return follow_up_; }
  State state() const { return state_; }
  std::string_view id() const { return id_.data(); }

 private:
  std::array<char, 33> id_{};
  State state_{State::IDLE};
  bool follow_up_{false};
};

inline const char *announcement_busy_reason(bool connected, bool session, bool followup,
                                            bool timer, bool reserved, bool phase_idle,
                                            bool muted) {
  if (reserved) return "reserved";
  if (!connected || session) return "session";
  if (followup) return "followup";
  if (timer) return "timer";
  if (!phase_idle) return "phase";
  if (muted) return "muted";
  return nullptr;
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
