// SPDX-License-Identifier: GPL-3.0-only
#include "../../esphome/components/va_client/va_timers.h"

#include <cassert>
#include <cstdint>
#include <string>

using esphome::va_client::TimerCommand;
using esphome::va_client::VaTimers;
using esphome::va_client::parse_timer_command;
using esphome::va_client::timer_ack_json;
using esphome::va_client::timer_finished_json;
using esphome::va_client::timer_json_string;
using esphome::va_client::timer_list_json;
using esphome::va_client::timer_state_json;

static void test_lifecycle_and_wraparound() {
  VaTimers timers;
  constexpr uint32_t start = UINT32_MAX - 1499;
  assert(timers.start("t1", "Tea", 2, start) == VaTimers::Result::OK);
  assert(timers.has_active(start));
  assert(timers.first_active(start).seconds_left == 2);
  assert(timers.list(start + 999)[0].seconds_left == 2);
  assert(timers.list(start + 1000)[0].seconds_left == 1);
  assert(timers.list(start + 1999)[0].seconds_left == 1);
  assert(timers.expire(start + 1999).empty());
  const auto finished = timers.expire(start + 2000);
  assert(finished.size() == 1 && finished[0].name == "Tea");
  assert(timers.expire(start + 3000).empty());
  assert(!timers.has_active(start + 2000));
  assert(timers.has_ringing());
  assert(timers.list(start + 2000)[0].ringing);
  assert(timers.stop_ringing());
  assert(!timers.stop_ringing() && timers.list(0).empty());
}

static void test_limit_replace_cancel_and_soonest() {
  VaTimers timers;
  assert(timers.start("", "", 5, 0) == VaTimers::Result::INVALID);
  assert(timers.start("x", "", 0, 0) == VaTimers::Result::INVALID);
  assert(timers.start("x", "", 86401, 0) == VaTimers::Result::INVALID);
  for (int i = 0; i < 8; ++i)
    assert(timers.start("t" + std::to_string(i), "", 86400, 0) == VaTimers::Result::OK);
  assert(timers.start("ninth", "", 1, 0) == VaTimers::Result::LIMIT);
  assert(timers.start("t3", "replaced", 5, 1000) == VaTimers::Result::OK);
  assert(timers.first_active(1000).id == "t3");
  assert(timers.first_active(1000).total_seconds == 5);
  assert(timers.cancel("nope") == VaTimers::Result::NOT_FOUND);
  assert(timers.cancel("") == VaTimers::Result::INVALID);
  assert(timers.cancel("t3") == VaTimers::Result::OK);
  assert(timers.start("ninth", "", 1, 1000) == VaTimers::Result::OK);
  assert(timers.cancel_all());
  assert(!timers.cancel_all());
  assert(!timers.has_active(1000));
}

static void test_commands_and_json() {
  TimerCommand cmd;
  assert(parse_timer_command(
      R"({"type":"timer_start","request_id":"r1","id":"t1","name":"Tea \"time\" \u2615","duration_s":65})",
      cmd));
  assert(cmd.kind == TimerCommand::Kind::START && cmd.name == "Tea \"time\" \xe2\x98\x95");
  assert(cmd.duration_s == 65 && cmd.request_id == "r1");
  assert(parse_timer_command(
      R"({ "type" : "timer_cancel", "all" : true, "request_id" : "r2" })", cmd));
  assert(cmd.kind == TimerCommand::Kind::CANCEL && cmd.all);
  assert(parse_timer_command(R"({"type":"timer_list","request_id":"r3"})", cmd));
  assert(cmd.kind == TimerCommand::Kind::LIST);
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"","duration_s":0})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"","duration_s":86401})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"","duration_s":4294967296})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"","duration_s":-1})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_cancel","request_id":"r","all":true,"id":"t"})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_list","request_id":"r","id":"t"})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_list","request_id":"r","request_id":"r2"})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_list","request_id":"r",})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"\uD800","duration_s":1})", cmd));
  assert(!parse_timer_command(R"({"type":"timer_start","request_id":"r","id":"t","name":"bad\q","duration_s":1})", cmd));
  assert(parse_timer_command(
      R"({"type":"timer_start","request_id":"r","id":"t","name":"\uD83D\uDD14","duration_s":1})",
      cmd));
  assert(cmd.name == "\xf0\x9f\x94\x94");

  VaTimers timers;
  assert(timers.start("a\"\\", "A\nB", 1, 0) == VaTimers::Result::OK);
  assert(timer_json_string("\x01\"\\") == R"("\u0001\"\\")");
  assert(timer_list_json(timers.list(1000)) ==
         R"([{"id":"a\"\\","name":"A\u000aB","total_s":1,"remaining_s":0,"ringing":false}])");
  const std::string list = timer_list_json(timers.list(0));
  assert(timer_ack_json("r", VaTimers::Result::OK, timers.list(0)) ==
         "{\"type\":\"timer_ack\",\"request_id\":\"r\",\"ok\":true,\"timers\":" + list + "}");
  assert(timer_ack_json("r", VaTimers::Result::LIMIT, timers.list(0)) ==
         "{\"type\":\"timer_ack\",\"request_id\":\"r\",\"ok\":false,\"error\":\"limit\",\"timers\":" + list + "}");
  assert(timer_ack_json("r", VaTimers::Result::INVALID, {}) ==
         R"({"type":"timer_ack","request_id":"r","ok":false,"error":"invalid","timers":[]})");
  assert(timer_ack_json("r", VaTimers::Result::NOT_FOUND, {}) ==
         R"({"type":"timer_ack","request_id":"r","ok":false,"error":"not_found","timers":[]})");
  assert(timer_state_json({}) == R"({"type":"timer_state","timers":[]})");
  assert(timer_finished_json(timers.expire(1000).front()) ==
         R"({"type":"timer_finished","id":"a\"\\","name":"A\u000aB"})");
}

int main() {
  test_lifecycle_and_wraparound();
  test_limit_replace_cancel_and_soonest();
  test_commands_and_json();
}
