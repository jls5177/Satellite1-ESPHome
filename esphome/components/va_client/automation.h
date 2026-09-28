#pragma once

#include "esphome/core/automation.h"
#include "va_client.h"

#include <string>

namespace esphome {
namespace va_client {

class OnPhaseTrigger : public Trigger<std::string> {
 public:
  explicit OnPhaseTrigger(VaClient *parent) { parent->add_on_phase_trigger(this); }
};

class OnRepeatedFailureTrigger : public Trigger<> {
 public:
  explicit OnRepeatedFailureTrigger(VaClient *parent) {
    parent->add_on_repeated_failure_trigger(this);
  }
};

// Fires when the device opens a follow-up mic window (i.e. server's
// request_follow_up message landed and the audio buffer has drained).
// yaml uses this to play the wake chime + flip the LED to "listening"
// so the user knows the assistant is waiting for their answer.
class OnFollowupOpenedTrigger : public Trigger<> {
 public:
  explicit OnFollowupOpenedTrigger(VaClient *parent) {
    parent->add_on_followup_opened_trigger(this);
  }
};

class OnTimerStartedTrigger : public Trigger<> {
 public:
  explicit OnTimerStartedTrigger(VaClient *parent) { parent->add_on_timer_started_trigger(this); }
};

class OnTimerFinishedTrigger : public Trigger<std::string> {
 public:
  explicit OnTimerFinishedTrigger(VaClient *parent) { parent->add_on_timer_finished_trigger(this); }
};

class OnTimerCancelledTrigger : public Trigger<> {
 public:
  explicit OnTimerCancelledTrigger(VaClient *parent) { parent->add_on_timer_cancelled_trigger(this); }
};

class OnTimerTickTrigger : public Trigger<> {
 public:
  explicit OnTimerTickTrigger(VaClient *parent) { parent->add_on_timer_tick_trigger(this); }
};

class OnAnnouncementRequestTrigger : public Trigger<> {
 public:
  explicit OnAnnouncementRequestTrigger(VaClient *parent) {
    parent->add_on_announcement_request_trigger(this);
  }
};

}  // namespace va_client
}  // namespace esphome
