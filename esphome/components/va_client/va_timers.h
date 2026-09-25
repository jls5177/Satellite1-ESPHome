#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esphome {
namespace va_client {

struct TimerInfo {
  std::string id;
  std::string name;
  uint32_t total_seconds{0};
  uint32_t seconds_left{0};
  bool ringing{false};
};

class VaTimers {
 public:
  static constexpr size_t kMaxTimers = 8;
  enum class Result { OK, LIMIT, INVALID, NOT_FOUND };

  Result start(const std::string &id, const std::string &name, uint32_t seconds, uint32_t now_ms) {
    if (id.empty() || seconds == 0 || seconds > 86400)
      return Result::INVALID;
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [&id](const Entry &entry) { return entry.id == id; });
    if (it == timers_.end()) {
      if (timers_.size() >= kMaxTimers)
        return Result::LIMIT;
      timers_.push_back({id, name, seconds, now_ms, false});
    } else {
      *it = {id, name, seconds, now_ms, false};
    }
    return Result::OK;
  }

  Result cancel(const std::string &id) {
    if (id.empty())
      return Result::INVALID;
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [&id](const Entry &entry) { return entry.id == id; });
    if (it == timers_.end())
      return Result::NOT_FOUND;
    timers_.erase(it);
    return Result::OK;
  }

  bool cancel_all() {
    const bool had_timers = !timers_.empty();
    timers_.clear();
    return had_timers;
  }

  std::vector<TimerInfo> list(uint32_t now_ms) const {
    std::vector<TimerInfo> result;
    result.reserve(timers_.size());
    for (const auto &timer : timers_)
      result.push_back({timer.id, timer.name, timer.total_s, remaining_(timer, now_ms), timer.ringing});
    return result;
  }

  std::vector<TimerInfo> expire(uint32_t now_ms) {
    std::vector<TimerInfo> finished;
    for (auto &timer : timers_) {
      if (!timer.ringing && remaining_(timer, now_ms) == 0) {
        timer.ringing = true;
        finished.push_back({timer.id, timer.name, timer.total_s, 0, true});
      }
    }
    return finished;
  }

  bool has_active(uint32_t now_ms) const {
    for (const auto &timer : timers_)
      if (!timer.ringing && remaining_(timer, now_ms) != 0)
        return true;
    return false;
  }

  bool has_ringing() const {
    for (const auto &timer : timers_)
      if (timer.ringing)
        return true;
    return false;
  }

  TimerInfo first_active(uint32_t now_ms) const {
    TimerInfo soonest;
    for (const auto &timer : timers_) {
      if (timer.ringing)
        continue;
      const uint32_t remaining = remaining_(timer, now_ms);
      if (remaining != 0 && (soonest.seconds_left == 0 || remaining < soonest.seconds_left))
        soonest = {timer.id, timer.name, timer.total_s, remaining, false};
    }
    return soonest;
  }

  bool stop_ringing() {
    const size_t before = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [](const Entry &entry) { return entry.ringing; }),
                  timers_.end());
    return before != timers_.size();
  }

 private:
  struct Entry {
    std::string id;
    std::string name;
    uint32_t total_s;
    uint32_t started_ms;
    bool ringing;
  };
  static uint32_t remaining_(const Entry &timer, uint32_t now_ms) {
    if (timer.ringing)
      return 0;
    const uint32_t duration_ms = timer.total_s * 1000u;
    const uint32_t elapsed = now_ms - timer.started_ms;
    return elapsed >= duration_ms ? 0 : (duration_ms - elapsed + 999u) / 1000u;
  }
  std::vector<Entry> timers_;
};

struct TimerCommand {
  enum class Kind { START, CANCEL, LIST };
  Kind kind{Kind::LIST};
  std::string request_id;
  std::string id;
  std::string name;
  uint32_t duration_s{0};
  bool all{false};
  bool valid{false};
};

inline void append_utf8(std::string &out, uint32_t codepoint) {
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

inline bool parse_timer_command(std::string_view json, TimerCommand &out) {
  out = {};
  size_t pos = 0;
  const auto space = [&]() {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                                 json[pos] == '\r' || json[pos] == '\t'))
      ++pos;
  };
  const auto hex4 = [&](uint32_t &value) {
    if (json.size() - pos < 4)
      return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = json[pos++];
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0)
        return false;
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return true;
  };
  const auto string = [&](std::string &value) {
    if (pos >= json.size() || json[pos++] != '"')
      return false;
    value.clear();
    while (pos < json.size()) {
      const unsigned char c = static_cast<unsigned char>(json[pos++]);
      if (c == '"')
        return true;
      if (c < 0x20)
        return false;
      if (c != '\\') {
        value.push_back(static_cast<char>(c));
        continue;
      }
      if (pos == json.size())
        return false;
      const char escape = json[pos++];
      if (escape == 'u') {
        uint32_t codepoint;
        if (!hex4(codepoint))
          return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
          if (json.size() - pos < 2 || json[pos++] != '\\' || json[pos++] != 'u')
            return false;
          uint32_t low;
          if (!hex4(low) || low < 0xDC00 || low > 0xDFFF)
            return false;
          codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
          return false;
        }
        append_utf8(value, codepoint);
      } else if (escape == '"' || escape == '\\' || escape == '/') {
        value.push_back(escape);
      } else if (escape == 'b' || escape == 'f' || escape == 'n' ||
                 escape == 'r' || escape == 't') {
        value.push_back(escape == 'b' ? '\b' : escape == 'f' ? '\f' :
                        escape == 'n' ? '\n' : escape == 'r' ? '\r' : '\t');
      } else {
        return false;
      }
    }
    return false;
  };
  space();
  if (pos == json.size() || json[pos++] != '{')
    return false;
  unsigned seen = 0;
  std::string type;
  space();
  while (pos < json.size() && json[pos] != '}') {
    std::string key;
    if (!string(key))
      return false;
    space();
    if (pos == json.size() || json[pos++] != ':')
      return false;
    space();
    unsigned bit = key == "type" ? 1 : key == "request_id" ? 2 :
                   key == "id" ? 4 : key == "name" ? 8 :
                   key == "duration_s" ? 16 : key == "all" ? 32 : 0;
    if (bit && (seen & bit))
      return false;
    seen |= bit;
    if (pos < json.size() && json[pos] == '"') {
      std::string value;
      if (!string(value) || (bit && bit != 1 && bit != 2 && bit != 4 && bit != 8))
        return false;
      if (bit == 1) type = value;
      if (bit == 2) out.request_id = value;
      if (bit == 4) out.id = value;
      if (bit == 8) out.name = value;
    } else if (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
      uint32_t number = 0;
      do {
        const uint32_t digit = static_cast<uint32_t>(json[pos++] - '0');
        if (number > (UINT32_MAX - digit) / 10)
          return false;
        number = number * 10 + digit;
      } while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9');
      if (bit && bit != 16)
        return false;
      if (bit == 16) out.duration_s = number;
    } else if (json.substr(pos, 4) == "true" || json.substr(pos, 5) == "false") {
      const bool value = json.substr(pos, 4) == "true";
      pos += value ? 4 : 5;
      if (bit && bit != 32)
        return false;
      if (bit == 32) out.all = value;
    } else {
      return false;
    }
    space();
    if (pos < json.size() && json[pos] == ',') {
      ++pos;
      space();
      if (pos == json.size() || json[pos] == '}')
        return false;
    } else if (pos >= json.size() || json[pos] != '}') {
      return false;
    }
  }
  if (pos == json.size() || json[pos++] != '}')
    return false;
  space();
  if (pos != json.size() || !(seen & 2) || out.request_id.empty())
    return false;
  if (type == "timer_start") {
    out.kind = TimerCommand::Kind::START;
    out.valid = (seen & (4 | 8 | 16)) == (4 | 8 | 16) && !out.id.empty() &&
                out.duration_s >= 1 && out.duration_s <= 86400 && !(seen & 32);
  } else if (type == "timer_cancel") {
    out.kind = TimerCommand::Kind::CANCEL;
    out.valid = out.all ? !(seen & 4) : (seen & 4) && !out.id.empty();
  } else if (type == "timer_list") {
    out.kind = TimerCommand::Kind::LIST;
    out.valid = !(seen & (4 | 8 | 16 | 32));
  }
  return out.valid;
}

inline std::string timer_json_string(std::string_view value) {
  std::string output = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      output += "\\u00";
      output.push_back(hex[c >> 4]);
      output.push_back(hex[c & 15]);
    } else {
      output.push_back(static_cast<char>(c));
    }
  }
  output.push_back('"');
  return output;
}

inline std::string timer_list_json(const std::vector<TimerInfo> &timers) {
  std::string output = "[";
  for (const auto &timer : timers) {
    if (output.size() > 1)
      output += ",";
    output += "{\"id\":" + timer_json_string(timer.id) +
              ",\"name\":" + timer_json_string(timer.name) +
              ",\"total_s\":" + std::to_string(timer.total_seconds) +
              ",\"remaining_s\":" + std::to_string(timer.seconds_left) +
              ",\"ringing\":" + (timer.ringing ? "true" : "false") + "}";
  }
  return output + "]";
}

inline std::string timer_ack_json(const std::string &request_id, VaTimers::Result result,
                                  const std::vector<TimerInfo> &timers) {
  const char *error = result == VaTimers::Result::LIMIT ? "limit" :
                      result == VaTimers::Result::NOT_FOUND ? "not_found" : "invalid";
  return "{\"type\":\"timer_ack\",\"request_id\":" + timer_json_string(request_id) +
         ",\"ok\":" + (result == VaTimers::Result::OK ? "true" : "false") +
         (result == VaTimers::Result::OK ? "" : std::string(",\"error\":\"") + error + "\"") +
         ",\"timers\":" + timer_list_json(timers) + "}";
}

inline std::string timer_state_json(const std::vector<TimerInfo> &timers) {
  return "{\"type\":\"timer_state\",\"timers\":" + timer_list_json(timers) + "}";
}

inline std::string timer_finished_json(const TimerInfo &timer) {
  return "{\"type\":\"timer_finished\",\"id\":" + timer_json_string(timer.id) +
         ",\"name\":" + timer_json_string(timer.name) + "}";
}

}  // namespace va_client
}  // namespace esphome
