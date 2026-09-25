// SPDX-License-Identifier: GPL-3.0-only
#include "../../esphome/components/va_client/ws_frame_assembler.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using esphome::va_client::Pcm16FrameAssembler;
using esphome::va_client::WsTextAssembler;
using esphome::va_client::release_fade_tail;

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
  test_pcm_split_at_every_byte();
  test_pcm_continuations_and_flush();
  test_text_split_and_fragmented();
  test_text_bounds_and_out_of_order();
  test_tail_starvation();
  return 0;
}
