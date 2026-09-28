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
using esphome::va_client::scale_sample;
using esphome::va_client::barge_in_trim;
using esphome::va_client::barge_in_mic_allowed;
using esphome::va_client::AnnouncementRequest;
using esphome::va_client::AnnouncementReservation;
using esphome::va_client::parse_announcement;
using esphome::va_client::parse_announcement_cancel;
using esphome::va_client::json_escape_string;
using esphome::va_client::truncate_utf8;
using esphome::va_client::announcement_busy_reason;

static void test_scale_sample_negative() {
  const size_t num = 238, den = 239;  // size_t operands, as in the ring fades
  assert(scale_sample(-1000, num, den) == -995);
  assert(scale_sample(-32768, 1, den) == -137);
  assert(scale_sample(-5, 0, den) == 0);
  std::vector<int16_t> s(4, -1000);
  fade_in_samples(s.data(), s.size(), 4, 0);
  assert(s[0] == 0 && s[1] == -250 && s[3] == -750);
}

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
  assert(classify_ws_message("{\"type\":\"announce\"}") == WsMessageType::ANNOUNCE);
  assert(classify_ws_message("{\"type\":\"announce_cancel\"}") == WsMessageType::ANNOUNCE_CANCEL);
  assert(classify_ws_message("{\"type\":\"announce_result\"}") == WsMessageType::ANNOUNCE_RESULT);
  assert(classify_ws_message("{\"type\":\"announce_extra\"}") == WsMessageType::UNKNOWN);
  assert(classify_ws_message("{\"type\":\"invalid}") == WsMessageType::UNKNOWN);
}

static void test_announcement_protocol() {
  AnnouncementRequest request;
  assert(parse_announcement("{\"type\":\"announce\",\"id\":\"Ab_9-\",\"chime\":true,\"follow_up\":false}", request));
  assert(request.id == "Ab_9-" && request.chime && !request.follow_up);
  assert(parse_announcement("{ \"follow_up\" : true, \"chime\":false,\"id\":\"abc\" }", request));
  assert(request.id == "abc" && !request.chime && request.follow_up);
  assert(!parse_announcement("{\"id\":\"bad space\",\"chime\":true,\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"a.b\",\"chime\":true,\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"\",\"chime\":true,\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"" + std::string(33, 'x') +
                             "\",\"chime\":true,\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"ok\",\"chime\":\"true\",\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"ok\",\"chime\":true,\"follow_up\":null}", request));
  assert(!parse_announcement("{\"id\":\"ok\",\"id\":\"again\",\"chime\":true,\"follow_up\":false}", request));
  assert(!parse_announcement("{\"id\":\"ok\",\"chime\":true,\"follow_up\":false,}", request));
  assert(!parse_announcement("{\"note\":\"\\\"id\\\":\\\"fake\\\"\",\"chime\":true,\"follow_up\":false}", request));
  std::string id;
  assert(parse_announcement_cancel("{\"type\":\"announce_cancel\",\"id\":\"good-1\"}", id));
  assert(id == "good-1");
  assert(!parse_announcement_cancel("{\"type\":\"announce_cancel\",\"id\":\"bad\\\"\"}", id));
  assert(!parse_announcement_cancel("{\"id\":\"bad!\"}", id));
}

static void test_announcement_reservation() {
  using State = AnnouncementReservation::State;
  AnnouncementReservation r;
  assert(r.state() == State::IDLE && !r.active());
  assert(r.reserve("first_1", true));
  assert(r.active() && r.follow_up() && r.id() == "first_1");
  assert(!r.reserve("second", false));
  assert(!r.ready("wrong") && !r.playing() && !r.finish());
  assert(r.ready("first_1") && r.admits_pcm() && r.state() == State::READY);
  assert(!r.ready("first_1") && r.playing() && r.state() == State::PLAYING);
  assert(r.finish() && r.state() == State::DONE && !r.active() && !r.finish());
  r.reset();
  assert(r.reserve("second", false) && !r.follow_up());
  assert(!r.cancel("first_1") && r.cancel("second"));
  assert(r.state() == State::CANCELLED && !r.active());
  r.reset();
  assert(r.state() == State::IDLE && r.id().empty());
  assert(announcement_busy_reason(true, false, false, false, false, true, false) == nullptr);
  assert(std::string(announcement_busy_reason(true, true, false, false, false, true, false)) == "session");
  assert(std::string(announcement_busy_reason(true, false, true, false, false, true, false)) == "followup");
  assert(std::string(announcement_busy_reason(true, false, false, true, false, true, false)) == "timer");
  assert(std::string(announcement_busy_reason(true, false, false, false, true, true, false)) == "reserved");
  assert(std::string(announcement_busy_reason(true, false, false, false, false, false, false)) == "phase");
  assert(std::string(announcement_busy_reason(true, false, false, false, false, true, true)) == "muted");
  assert(std::string(announcement_busy_reason(false, false, false, false, false, true, false)) == "session");
}

static void test_announcement_text_encoding() {
  assert(json_escape_string("a\"b\\c\n\t\r") == "\"a\\\"b\\\\c\\n\\t\\r\"");
  assert(json_escape_string(std::string(1, '\x01')) == "\"\\u0001\"");
  assert(json_escape_string("é") == "\"é\"");
  assert(truncate_utf8("abc", 2) == "ab");
  assert(truncate_utf8("aé雪z", 3) == "aé雪");
  assert(truncate_utf8("aé雪z", 500) == "aé雪z");
  assert(truncate_utf8(std::string(499, 'x') + "éz", 500) ==
         std::string(499, 'x') + "é");
  assert(truncate_utf8("a\xe2\x82", 5) == "a");
  assert(truncate_utf8("a\xc0\x80", 5) == "a");
  assert(truncate_utf8("a\xed\xa0\x80", 5) == "a");
  assert(truncate_utf8("a\xf4\x90\x80\x80", 5) == "a");
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

static void test_barge_in_trim() {
  constexpr size_t fade_bytes = 480;
  assert(barge_in_trim(0, fade_bytes) == 0);
  assert(barge_in_trim(2, fade_bytes) == 2);
  assert(barge_in_trim(fade_bytes - 2, fade_bytes) == fade_bytes - 2);
  assert(barge_in_trim(fade_bytes, fade_bytes) == fade_bytes);
  assert(barge_in_trim(fade_bytes + 2, fade_bytes) == fade_bytes);
  assert(barge_in_trim(2000000, fade_bytes) == fade_bytes);
  assert(barge_in_trim(100, 0) == 0);
}

static void test_barge_in_mic_holdoff() {
  assert(barge_in_mic_allowed(false, true, 0, 1000, 400));
  assert(barge_in_mic_allowed(true, false, 0, 1000, 400));
  assert(!barge_in_mic_allowed(true, true, 0, 1000, 400));
  assert(!barge_in_mic_allowed(true, true, 0, 1000, 0));
  assert(!barge_in_mic_allowed(true, true, 1000, 999, 0));
  assert(!barge_in_mic_allowed(true, true, 1000, 999, 400));
  assert(!barge_in_mic_allowed(true, true, 1000, 1399, 400));
  assert(barge_in_mic_allowed(true, true, 1000, 1400, 400));
  assert(barge_in_mic_allowed(true, true, 1000, 1000, 0));
  assert(!barge_in_mic_allowed(true, true, 1000, 2999, 2000));
  assert(barge_in_mic_allowed(true, true, 1000, 3000, 2000));
  assert(!barge_in_mic_allowed(true, true, UINT32_MAX - 100, 298, 400));
  assert(barge_in_mic_allowed(true, true, UINT32_MAX - 100, 299, 400));
}

int main() {
  test_playback_clock();
  test_scale_sample_negative();
  test_fade_in_samples();
  test_pcm_split_at_every_byte();
  test_pcm_continuations_and_flush();
  test_text_split_and_fragmented();
  test_message_types();
  test_announcement_protocol();
  test_announcement_reservation();
  test_announcement_text_encoding();
  test_text_bounds_and_out_of_order();
  test_tail_starvation();
  test_barge_in_trim();
  test_barge_in_mic_holdoff();
  return 0;
}
