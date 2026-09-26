// SPDX-License-Identifier: GPL-3.0-only
#include "../../esphome/components/va_client/ws_frame_assembler.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using esphome::va_client::Pcm16FrameAssembler;
using esphome::va_client::WsTextAssembler;
using esphome::va_client::WsMessageType;
using esphome::va_client::classify_ws_message;
using esphome::va_client::release_fade_tail;
using esphome::va_client::PlaybackClock;
using esphome::va_client::fade_in_samples;

static void test_playback_clock() {
  PlaybackClock clock;
  assert(clock.remaining_us(1000) == 0);
  clock.fed(1000, 4800, 48000);  // 100 ms
  assert(clock.remaining_us(1000) == 100000);
  assert(clock.remaining_us(51000) == 50000);
  clock.fed(51000, 960, 48000);  // queued: extends from the current end
  assert(clock.remaining_us(51000) == 70000);
  assert(clock.remaining_us(200000) == 0);
  clock.fed(300000, 480, 48000);  // after an underrun, restarts from now
  assert(clock.remaining_us(300000) == 10000);
  const int64_t later = int64_t{1} << 33;  // past a 32-bit micros() wrap
  assert(clock.remaining_us(later) == 0);
  clock.fed(later, 4800, 48000);
  assert(clock.remaining_us(later + 20000) == 80000);
  clock.reset();
  assert(clock.remaining_us(10000) == 0);
}

static void test_fade_in_samples() {
  std::vector<int16_t> s(8, 1000);
  assert(fade_in_samples(s.data(), s.size(), 4, 0) == 4);
  assert(s[0] == 0 && s[1] == 250 && s[3] == 750 && s[4] == 1000 && s[7] == 1000);
  // A fade continues across fragments, including one-sample ones.
  std::vector<int16_t> a(1, 1000), b(6, 1000);
  size_t pos = fade_in_samples(a.data(), a.size(), 4, 0);
  assert(pos == 1 && a[0] == 0);
  pos = fade_in_samples(b.data(), b.size(), 4, pos);
  assert(pos == 4 && b[0] == 250 && b[2] == 750 && b[3] == 1000);
  // Inactive fade leaves samples untouched.
  assert(fade_in_samples(b.data(), b.size(), 4, 4) == 4 && b[0] == 250);
}

static void test_pcm_split_at_every_byte() {
  const std::array<uint8_t, 7> bytes{{1, 2, 3, 4, 5, 6, 7}};
  Pcm16FrameAssembler assembler;
  std::vector<uint8_t> output;
  std::vector<uint8_t> collected;
  for (size_t i = 0; i < bytes.size(); ++i) {
    assert(assembler.append(false, &bytes[i], 1, i, bytes.size(), output));
    collected.insert(collected.end(), output.begin(), output.end());
  }
  assert((collected == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
  const uint8_t next[] = {8, 9};
  assert(assembler.append(false, next, sizeof(next), 0, sizeof(next), output));
  assert((output == std::vector<uint8_t>{8, 9}));
}

static void test_pcm_continuations_and_flush() {
  Pcm16FrameAssembler assembler;
  std::vector<uint8_t> output;
  const uint8_t first[] = {0x11};
  const uint8_t second[] = {0x22, 0x33};
  const uint8_t third[] = {0x44};
  assert(assembler.append(false, first, 1, 0, 1, output));
  assert(output.empty());
  assert(assembler.append(true, second, 2, 0, 2, output));
  assert((output == std::vector<uint8_t>{0x11, 0x22}));
  assert(assembler.append(true, third, 1, 0, 1, output));
  assert((output == std::vector<uint8_t>{0x33, 0x44}));

  assembler.reset();  // ring epoch changed mid-frame
  assert(!assembler.append(false, second, 1, 1, 2, output));
  assert(output.empty());
  assert(assembler.append(false, third, 1, 0, 1, output));
  assert(output.empty());
  assert(assembler.append(false, second, 2, 0, 2, output));
  assert((output == std::vector<uint8_t>{0x22, 0x33}));
}

static void test_text_split_and_fragmented() {
  WsTextAssembler assembler;
  using Result = WsTextAssembler::Result;
  assert(assembler.append(false, false, "{\"type\":", 8, 0, 15) == Result::INCOMPLETE);
  assert(assembler.append(false, false, "\"hello\"", 7, 8, 15) == Result::INCOMPLETE);
  assert(assembler.append(true, true, "}", 1, 0, 1) == Result::COMPLETE);
  assert(std::string(assembler.data(), assembler.size()) == "{\"type\":\"hello\"}");
  const char *msg = "{\"type\":\"hello\",\"interrupt_response\":true}";
  const std::string message(msg);
  assembler.reset();
  for (size_t i = 0; i < message.size(); ++i) {
    const auto result = assembler.append(false, true, &msg[i], 1, i, message.size());
    assert(result == (i + 1 == message.size() ? Result::COMPLETE : Result::INCOMPLETE));
  }
  assert(std::string(assembler.data(), assembler.size()) == message);
  assert(classify_ws_message(std::string_view(assembler.data(), assembler.size())) ==
         WsMessageType::HELLO);
  constexpr char done[] = "{\"type\":\"audio_done\"}";
  assembler.reset();
  assert(assembler.append(false, true, done, 7, 0, sizeof(done) - 1) == Result::INCOMPLETE);
  assert(assembler.append(false, true, done + 7, sizeof(done) - 8, 7, sizeof(done) - 1) ==
         Result::COMPLETE);
  assert(classify_ws_message(std::string_view(assembler.data(), assembler.size())) ==
         WsMessageType::AUDIO_DONE);
}

static void test_message_types() {
  assert(classify_ws_message("{\"type\":\"audio_done\"}") == WsMessageType::AUDIO_DONE);
  assert(classify_ws_message("{\"type\":\"audio_done\",\"value\":\"idle\"}") ==
         WsMessageType::AUDIO_DONE);
  assert(classify_ws_message("{\"type\":\"audio_done_extra\"}") == WsMessageType::UNKNOWN);
  assert(classify_ws_message("{\"type\":\"new_type\",\"value\":\"idle\"}") ==
         WsMessageType::UNKNOWN);
  assert(classify_ws_message("{\"type\":\"phase\",\"value\":\"idle\"}") ==
         WsMessageType::PHASE);
  assert(classify_ws_message("{\"type\":\"request_follow_up\"}") ==
         WsMessageType::REQUEST_FOLLOW_UP);
  assert(classify_ws_message("{\"type\":\"error\"}") == WsMessageType::ERROR);
  assert(classify_ws_message("{\"type\":\"timer_start\"}") == WsMessageType::TIMER_START);
  assert(classify_ws_message("{\"type\":\"timer_cancel\"}") == WsMessageType::TIMER_CANCEL);
  assert(classify_ws_message("{\"type\":\"timer_list\"}") == WsMessageType::TIMER_LIST);
  assert(classify_ws_message("{ \"type\" : \"timer_start\" }") == WsMessageType::TIMER_START);
  assert(classify_ws_message("{\"type\":\"timer_list_extra\"}") == WsMessageType::UNKNOWN);
  assert(classify_ws_message("{\"type\":\"invalid}") == WsMessageType::UNKNOWN);
}

static void test_text_bounds_and_out_of_order() {
  WsTextAssembler assembler;
  using Result = WsTextAssembler::Result;
  std::string large(WsTextAssembler::kMaxBytes + 1, 'a');
  assert(assembler.append(false, true, large.data(), large.size(), 0, large.size()) ==
         Result::DROPPED);
  assert(assembler.append(false, true, large.data() + 1, 1, 1, large.size()) ==
         Result::DROPPED);
  assert(assembler.append(false, true, "ok", 2, 0, 2) == Result::COMPLETE);
  assert(std::string(assembler.data(), assembler.size()) == "ok");
  assert(assembler.append(false, true, "a", 1, 0, 2) == Result::INCOMPLETE);
  assert(assembler.append(false, true, "b", 1, 2, 2) == Result::DROPPED);
  assert(assembler.append(false, true, "new", 3, 0, 3) == Result::COMPLETE);
}

static void test_tail_starvation() {
  constexpr size_t fade = 480;
  assert(!release_fade_tail(480, fade, false, true, 1079, 1000, 80));
  assert(!release_fade_tail(480, fade, false, false, 1200, 1000, 80));
  assert(release_fade_tail(480, fade, false, true, 1080, 1000, 80));
  assert(!release_fade_tail(482, fade, false, true, 1200, 1000, 80));
  assert(release_fade_tail(482, fade, true, false, 1001, 1000, 80));
  assert(!release_fade_tail(0, fade, true, true, 1200, 1000, 80));
  assert(release_fade_tail(480, fade, false, true, 20, UINT32_MAX - 70, 80));
}

int main() {
  test_playback_clock();
  test_fade_in_samples();
  test_pcm_split_at_every_byte();
  test_pcm_continuations_and_flush();
  test_text_split_and_fragmented();
  test_message_types();
  test_text_bounds_and_out_of_order();
  test_tail_starvation();
  return 0;
}
