#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <algorithm>
#include <cstring>

#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_heap_caps.h>

namespace esphome {
namespace va_client {

static const char *const TAG = "va_client";

// Free-function trampoline. esp-idf event registration takes a C function
// pointer; we recover the VaClient* from the user_data slot.
static void va_ws_event_handler(void *handler_args, esp_event_base_t /*base*/, int32_t event_id, void *event_data) {
  auto *self = static_cast<VaClient *>(handler_args);
  if (self == nullptr)
    return;
  self->on_ws_event(event_id, event_data);
}

// Parse the unsigned integer immediately following `key` in `msg` (key includes
// the quotes + colon, e.g. "\"follow_up_ms\":"). Returns true and sets `out` if a
// run of digits was found right after the key. Keeps us out of a JSON parser for
// the few small ints the backend sends in `hello`.
static bool parse_uint_after_key(const std::string &msg, const char *key, uint32_t &out) {
  size_t p = msg.find(key);
  if (p == std::string::npos)
    return false;
  p += std::strlen(key);
  uint32_t v = 0;
  bool any = false;
  while (p < msg.size() && msg[p] >= '0' && msg[p] <= '9') {
    v = v * 10u + static_cast<uint32_t>(msg[p] - '0');
    p++;
    any = true;
  }
  if (!any)
    return false;
  out = v;
  return true;
}

static bool parse_bool_after_key(const std::string &msg, const char *key, bool &out) {
  size_t p = msg.find(key);
  if (p == std::string::npos)
    return false;
  p += std::strlen(key);
  while (p < msg.size() && (msg[p] == ' ' || msg[p] == '\t'))
    ++p;
  if (p >= msg.size() || msg[p++] != ':')
    return false;
  while (p < msg.size() && (msg[p] == ' ' || msg[p] == '\t'))
    ++p;
  if (msg.compare(p, 4, "true") == 0) {
    p += 4;
    out = true;
  } else if (msg.compare(p, 5, "false") == 0) {
    p += 5;
    out = false;
  } else {
    return false;
  }
  return p == msg.size() || msg[p] == ',' || msg[p] == '}' || msg[p] == ' ';
}

void VaClient::set_barge_in(bool enabled) {
  this->barge_in_ = enabled;
  if (enabled && this->ws_connected_ && this->server_hello_received_ &&
      !this->server_barge_in_ &&
      !this->server_barge_warning_logged_.exchange(true)) {
    ESP_LOGW(TAG, "hands-free interrupt is on but add-on interrupt_response is off");
  }
  if (static_cast<Phase>(this->current_phase_.load()) != Phase::REPLYING ||
      !this->ws_connected_ || this->session_starting_)
    return;
  this->streaming_ = false;
  this->clear_mic_tx_();
  if (this->effective_barge_in_() && !this->post_stop_guard_ && !this->control_channel_dirty_) {
    this->streaming_ = true;
    const Phase phase = static_cast<Phase>(this->current_phase_.load());
    if (phase != Phase::REPLYING && phase != Phase::LISTENING)
      this->streaming_ = false;
  }
}

void VaClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up VA Client...");

  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  } else {
    ESP_LOGE(TAG, "Microphone not configured");
  }

  // Allocate the audio ring buffer in PSRAM (8 MB available, internal RAM
  // is only 320 KB and we don't want to starve wifi/mww).
  this->audio_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->audio_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte audio buffer in PSRAM", (unsigned) kAudioBufBytes);
    this->mark_failed();
    return;
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u-byte audio ring buffer in PSRAM", (unsigned) kAudioBufBytes);
  }
  this->audio_drain_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioDrainChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->audio_drain_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate playback drain buffer in PSRAM");
    heap_caps_free(this->audio_buf_);
    this->audio_buf_ = nullptr;
    this->mark_failed();
    return;
  }

  // Allocate the mic pre-roll ring in PSRAM (kPreRollMs of 16 kHz int16 mono).
  this->preroll_capacity_samples_ = (size_t) kPreRollMs * (kMicSampleRate / 1000);
  this->preroll_buf_ = static_cast<int16_t *>(
      heap_caps_malloc(this->preroll_capacity_samples_ * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->preroll_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample mic pre-roll buffer in PSRAM",
             (unsigned) this->preroll_capacity_samples_);
    this->preroll_capacity_samples_ = 0;
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u ms mic pre-roll buffer in PSRAM (%u samples)",
                  (unsigned) kPreRollMs, (unsigned) this->preroll_capacity_samples_);
  }

  this->mic_tx_storage_ = static_cast<uint8_t *>(
      heap_caps_malloc(kMicTxBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->ws_send_mutex_ = xSemaphoreCreateMutex();
  if (this->mic_tx_storage_ == nullptr || this->ws_send_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate microphone TX queue or WebSocket send mutex");
    if (this->mic_tx_storage_ != nullptr) {
      heap_caps_free(this->mic_tx_storage_);
      this->mic_tx_storage_ = nullptr;
    }
    if (this->ws_send_mutex_ != nullptr) {
      vSemaphoreDelete(this->ws_send_mutex_);
      this->ws_send_mutex_ = nullptr;
    }
    this->mark_failed();
    return;
  }
  this->mic_tx_ring_.set_storage(this->mic_tx_storage_, kMicTxBufferBytes);
  if (xTaskCreate(mic_sender_task_trampoline_, "va_mic_tx", 4096, this, 5,
                  &this->mic_sender_task_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create microphone sender task");
    heap_caps_free(this->mic_tx_storage_);
    this->mic_tx_storage_ = nullptr;
    this->mic_tx_ring_.set_storage(nullptr, 0);
    vSemaphoreDelete(this->ws_send_mutex_);
    this->ws_send_mutex_ = nullptr;
    this->mark_failed();
    return;
  }
  if (xTaskCreate(ws_teardown_task_trampoline_, "va_ws_stop", 4096, this, 5,
                  &this->ws_teardown_task_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create WebSocket teardown task");
    this->mark_failed();
    return;
  }

  // Tell the resampler what format we'll feed it. The resampler converts to
  // its yaml-configured output format (48k 16-bit) before passing to the
  // mixer → i2s leaf. Start the speaker task once so play() calls just push
  // into its ring buffer instead of racing to re-create the i2s channel.
  if (this->speaker_ != nullptr) {
    audio::AudioStreamInfo info(/*bits_per_sample=*/16, /*channels=*/1, /*sample_rate=*/24000);
    this->speaker_->set_audio_stream_info(info);
    this->speaker_->start();
  }

  this->connect_();
}

void VaClient::loop() {
  const uint32_t timer_now = millis();
  for (const auto &finished : this->timers_.expire(timer_now)) {
    ESP_LOGI(TAG, "Timer finished: %s", finished.id.c_str());
    for (auto *t : this->timer_finished_triggers_)
      t->trigger(finished.name);
    if (this->ws_connected_ && !this->control_channel_dirty_) {
      const std::string message = timer_finished_json(finished);
      this->send_control_(message.c_str(), message.size(), "timer_finished");
    }
  }
  if (this->timers_.has_active(timer_now) &&
      timer_now - this->last_timer_tick_ms_ >= 1000u) {
    this->last_timer_tick_ms_ = timer_now;
    for (auto *t : this->timer_tick_triggers_)
      t->trigger();
  }
  if (this->reconnect_requested_.exchange(false)) {
    this->ws_stop_completed_ = false;
    xTaskNotifyGive(this->ws_teardown_task_);
    this->schedule_reconnect_();
  }
  if (this->reconnect_start_pending_ && !this->reconnect_pending_ &&
      this->ws_task_finished_ && this->ws_stop_completed_) {
    this->reconnect_start_pending_ = false;
    this->connect_();
  }
  this->log_mic_health_();
  // Drain the audio ring buffer into the speaker. speaker.play() accepts
  // only what fits in its own ring (returns the count actually queued).
  if (this->speaker_ != nullptr && this->is_audio_ready()) {
    const bool speaker_dry = !this->speaker_->has_buffered_data();
    // Snapshot ring state under the lock — head/tail/fill are all
    // mutated from the WS task on the other core.
    portENTER_CRITICAL(&this->ring_mux_);
    const uint32_t now_ms = millis();
    const bool starved_tail = !this->reply_audio_done_ &&
        release_fade_tail(this->audio_fill_, kFadeSamples * sizeof(int16_t),
                          false, speaker_dry, now_ms, this->last_binary_ms_, kWsGapWarnMs);
    if (starved_tail) {
      this->reply_audio_done_ = true;
      this->fade_out_pending_ = true;
    }
    if (this->fade_out_pending_.exchange(false))
      this->fade_ring_tail_();
    size_t head = this->audio_head_;
    size_t tail = this->audio_tail_;
    size_t fill = this->audio_fill_;
    uint32_t epoch = this->audio_ring_epoch_;
    portEXIT_CRITICAL(&this->ring_mux_);
    // Retain the fade window until the reply ends or the downstream runs dry.
    const size_t playable = this->reply_audio_done_.load() ? fill :
        (fill > kFadeSamples * sizeof(int16_t) ? fill - kFadeSamples * sizeof(int16_t) : 0);
    if (playable > 0) {
      // Resampler cold-start SILENCE-PRIME (crackle fix). The resampler does NOT
      // idle-timeout (verified vs ESPHome source): resample(stop_gracefully=false)
      // never returns FINISHED, and its output mixer-source is timeout:never, so the
      // chain stays WARM between normal replies. It goes COLD only after an explicit
      // `speaker.stop: media_resampling_speaker` (yaml interrupt / "stop" word / wake /
      // follow-up), which tears the task down to STATE_STOPPED. The next reply then
      // cold-starts a fresh AudioResampler whose windowed-sinc FIR begins from a zero
      // state → a startup-transient click. A PSRAM prebuffer can't fix it (the transient
      // is downstream of the ring). Fix: when cold, feed kChainPrimeMs of silence BEFORE
      // the first real sample so the FIR settles to a clean zero output first. We detect
      // "cold" two ways: the resampler actually reporting is_stopped() (true exactly
      // post-speaker.stop — the precise signal) OR, as a backup, nothing fed for
      // > kChainColdMs. is_stopped() closes the window the timer alone misses: a reply
      // whose audio lands < kChainColdMs after a speaker.stop (the residual click). A
      // needless prime on an already-warm chain is harmless (60ms of silence); both
      // signals are only ever true at a real cold reply-start, never mid-speech. The
      // real audio waits safely in PSRAM (and builds a small cushion) until priming done.
      {
        const uint32_t now_ms = millis();
        const bool resampler_cold = this->speaker_->is_stopped() ||
                                    this->last_fed_ms_ == 0 ||
                                    (now_ms - this->last_fed_ms_) > kChainColdMs;
        if (this->chain_prime_remaining_ == 0 && resampler_cold) {
          this->chain_prime_remaining_ =
              (size_t) kChainPrimeMs * (kPlaybackSampleRate / 1000) * 2;  // ms→bytes (mono 16-bit)
          ESP_LOGD(TAG, "resampler cold — priming %u bytes of silence before reply",
                   (unsigned) this->chain_prime_remaining_);
        }
        if (this->chain_prime_remaining_ > 0) {
          static const uint8_t kSilence[480] = {0};  // 10ms @24k mono16; fed in chunks
          size_t want = std::min(this->chain_prime_remaining_.load(), sizeof(kSilence));
          size_t fed = this->speaker_->play(kSilence, want);
          if (fed > 0) {
            size_t remaining = this->chain_prime_remaining_.load();
            while (remaining > 0 && !this->chain_prime_remaining_.compare_exchange_weak(
                remaining, remaining > fed ? remaining - fed : 0)) {}
            this->last_fed_ms_ = now_ms;  // count silence as "fed" so cold-check clears
          }
          // Hold real-audio drain until the chain is warmed. Real audio stays in
          // PSRAM. Re-enter loop() next tick to continue/finish priming.
          return;
        }
      }
      // Jitter buffer priming gate. After the ring was empty (reply start or a
      // post-underflow gap) hold playback until either the prebuffer cushion has
      // accumulated (fill >= target) or a deadline elapses (so real-time, non-
      // burst audio still starts promptly). Holding here lets the downstream
      // chain start with a cushion so a network gap doesn't dry it out → no
      // crackle. Skipped entirely when playback_prebuffer_ms_ == 0 (disabled).
      if (this->playback_priming_) {
        const size_t target =
            (size_t) this->playback_prebuffer_ms_ * (kPlaybackSampleRate / 1000) * 2;
        if (fill >= target ||
            (millis() - this->prime_started_ms_) >= this->playback_prebuffer_ms_) {
          this->playback_priming_ = false;
          ESP_LOGD(TAG, "prebuffer ready (%u bytes) — playback start", (unsigned) fill);
        } else {
          return;  // keep accumulating; don't drain (and don't false-flag underrun)
        }
      }
      if (this->fade_in_pending_) {
        portENTER_CRITICAL(&this->ring_mux_);
        this->fade_ring_head_();
        portEXIT_CRITICAL(&this->ring_mux_);
        this->fade_in_pending_ = false;
      }
      // Detector 3: downstream underrun. If the resampler/mixer/i2s chain
      // ran out of bytes to play while we *still* have PSRAM queued,
      // something hiccupped downstream — the user hears silence or a
      // brief stuck-sample glitch. Log the first occurrence per reply so
      // we know whether bad audio in a turn correlates with this.
      if (!this->underrun_logged_this_turn_ && !this->speaker_->has_buffered_data()) {
        ESP_LOGW(TAG, "downstream underrun: %u bytes queued in PSRAM but speaker chain is dry",
                 (unsigned) fill);
        this->underrun_logged_this_turn_ = true;
      }
      // Copy a bounded contiguous slice before play(): a disconnect can
      // reset and reuse the ring while the speaker is still consuming it.
      size_t contiguous = (head < tail) ? (tail - head) : (kAudioBufBytes - head);
      contiguous = std::min({contiguous, playable, kAudioDrainChunkBytes});
      portENTER_CRITICAL(&this->ring_mux_);
      if (this->audio_ring_epoch_ != epoch || this->audio_head_ != head) {
        portEXIT_CRITICAL(&this->ring_mux_);
        return;
      }
      std::memcpy(this->audio_drain_buf_, this->audio_buf_ + head, contiguous);
      portEXIT_CRITICAL(&this->ring_mux_);
      // play() runs OUTSIDE the critical section: it may block on the
      // resampler, while the scratch copy stays valid across ring resets.
      size_t accepted = this->speaker_->play(this->audio_drain_buf_, contiguous);
      if (accepted > 0) {
        this->last_fed_ms_ = millis();  // keep the chain "warm" for cold-detection
        portENTER_CRITICAL(&this->ring_mux_);
        if (this->audio_ring_epoch_ == epoch && accepted <= this->audio_fill_) {
          this->audio_head_ = (this->audio_head_ + accepted) % kAudioBufBytes;
          this->audio_fill_ -= accepted;
        }
        portEXIT_CRITICAL(&this->ring_mux_);
        static uint32_t dbg_last = 0;
        uint32_t now = millis();
        if (now - dbg_last >= 500) {
          ESP_LOGD(TAG, "drained %u bytes (%u still queued)", (unsigned) accepted,
                   (unsigned) (fill - accepted));
          dbg_last = now;
        }
      }
    }
  }
  // If a follow-up window was deferred while audio was draining, wait for
  // the downstream chain (resampler + mixer + i2s + DAC tail) to actually
  // finish playing before firing the deferred LED-idle / chime trigger.
  // Just because our PSRAM queue is empty doesn't mean the user has heard
  // the audio yet — and an "сейчас посмотрю" preamble before a tool call
  // would drain the ring mid-turn, so we can't act on audio_fill_==0
  // alone.
  //
  // Primary signal: speaker_->is_stopped(). The resampling speaker
  // transitions to STOPPED only after every byte we wrote has actually
  // gone out through the i2s pipeline.
  //
  // Fallback: kSpeakerStopTimeoutMs (3 s). If something wedges and the
  // speaker never reports STOPPED, we still progress so the LED doesn't
  // lock in `replying`.
  if (this->followup_pending_ && this->audio_fill_snapshot_() == 0 &&
      !this->waiting_for_speaker_stop_) {
    this->waiting_for_speaker_stop_ = true;
    this->speaker_stop_wait_started_ms_ = millis();
  }
  if (this->waiting_for_speaker_stop_) {
    // Use has_buffered_data() instead of is_stopped(): the resampler only
    // transitions to STATE_STOPPED once its downstream (mixer source)
    // reports stopped, but our mixer sources are configured `timeout:
    // never` and stay RUNNING forever, so is_stopped() would never fire
    // and we'd always hit the fallback. has_buffered_data() walks the
    // chain (resampler ring + mixer source ring) and returns false as
    // soon as both have drained — exactly what we want.
    //
    // Note: this does *not* cover the i2s 500ms ring + ~100ms DAC tail
    // downstream of the mixer. We fire ~500ms before true silence. For
    // the LED that's imperceptible; for the request_follow_up chime,
    // yaml's wait_until !is_announcing + i2s tail delay already absorbs
    // any small overlap with the fading TTS tail.
    const bool speaker_drained =
        (this->speaker_ != nullptr) && !this->speaker_->has_buffered_data();
    const bool timed_out =
        (millis() - this->speaker_stop_wait_started_ms_) >= kSpeakerStopTimeoutMs;
    if (speaker_drained || timed_out) {
      if (timed_out && !speaker_drained) {
        ESP_LOGW(TAG,
                 "speaker still had buffered data after %u ms — "
                 "proceeding anyway (fallback)",
                 (unsigned) kSpeakerStopTimeoutMs);
      }
      this->waiting_for_speaker_stop_ = false;
      const bool was_request = this->request_follow_up_pending_;
      this->followup_pending_ = false;
      this->request_follow_up_pending_ = false;
      if (was_request) {
        // Request-driven path: speaker has drained, now hand off to yaml
        // for the chime → wait_until !is_announcing → commit_followup_mic
        // sequence (announcement lane is separate from the TTS lane we
        // just waited on, so the chime won't collide with our tail).
        this->open_followup_window_(0);  // emit deferred LED idle + latency log; no mic
        this->followup_armed_ = true;
        for (auto *t : this->followup_opened_triggers_) {
          t->trigger();
        }
      } else {
        // Natural-idle path: emit the deferred LED idle, and — if the backend
        // configured a follow-up window (followup_ms_ > 0) — open the mic for
        // that long so the user can answer back without a wake word.
        this->open_followup_window_(this->followup_ms_);
      }
    }
  }
}

void VaClient::clear_mic_tx_() {
  portENTER_CRITICAL(&this->mic_tx_mux_);
  this->mic_tx_ring_.clear();
  this->mic_tx_generation_.fetch_add(1, std::memory_order_relaxed);
  portEXIT_CRITICAL(&this->mic_tx_mux_);
}

size_t VaClient::audio_fill_snapshot_() {
  portENTER_CRITICAL(&this->ring_mux_);
  const size_t fill = this->audio_fill_;
  portEXIT_CRITICAL(&this->ring_mux_);
  return fill;
}

void VaClient::mic_sender_task_trampoline_(void *arg) {
  static_cast<VaClient *>(arg)->mic_sender_loop_();
}

void VaClient::ws_teardown_task_trampoline_(void *arg) {
  static_cast<VaClient *>(arg)->ws_teardown_loop_();
}

void VaClient::ws_teardown_loop_() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    if (this->ws_destroy_requested_.exchange(false)) {
      if (handle != nullptr) {
        const esp_err_t err = esp_websocket_client_destroy(handle);
        if (err != ESP_OK)
          ESP_LOGE(TAG, "WebSocket destroy after registration failure: %d", (int) err);
        this->ws_handle_ = nullptr;
      }
      this->ws_stop_completed_ = true;
      continue;
    }
    if (handle != nullptr) {
      const esp_err_t err = esp_websocket_client_stop(handle);
      // ESP_FAIL also means the WS task already stopped on its own.
      if (err != ESP_OK && !this->ws_task_finished_)
        ESP_LOGE(TAG, "WebSocket stop failed before FINISH: %d", (int) err);
    }
    this->ws_stop_completed_ = this->ws_task_finished_.load();
  }
}

void VaClient::mic_sender_loop_() {
  uint8_t packet[kMicTxPacketBytes];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    if (!this->streaming_.load() || !this->ws_connected_.load() ||
        this->session_starting_.load() || this->control_channel_dirty_.load())
      continue;
    uint32_t generation;
    size_t length;
    portENTER_CRITICAL(&this->mic_tx_mux_);
    generation = this->mic_tx_generation_.load(std::memory_order_relaxed);
    length = this->mic_tx_ring_.peek(packet, sizeof(packet));
    portEXIT_CRITICAL(&this->mic_tx_mux_);
    if (length == 0)
      continue;

    // The control sender holds this mutex through wake, so a copied packet
    // can either go out before wake or be rejected here, never after it.
    if (xSemaphoreTake(this->ws_send_mutex_, pdMS_TO_TICKS(kWsControlSendTimeoutMs)) != pdTRUE)
      continue;
    bool valid = mic_tx_packet_ready(generation, this->mic_tx_generation_.load(),
                                     this->streaming_.load(), this->session_starting_.load(),
                                     this->ws_connected_.load()) &&
                 !this->control_channel_dirty_.load();
    int sent = 0;
    if (valid) {
      auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
      sent = esp_websocket_client_send_bin(handle, reinterpret_cast<const char *>(packet),
                                            static_cast<int>(length),
                                            pdMS_TO_TICKS(kWsControlSendTimeoutMs));
    }
    xSemaphoreGive(this->ws_send_mutex_);
    if (!valid)
      continue;
    if (sent <= 0 || static_cast<size_t>(sent) != length) {
      this->mic_tx_send_failures_.fetch_add(1);
      if (sent > 0)
        this->fail_control_channel_("partial microphone PCM send");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    portENTER_CRITICAL(&this->mic_tx_mux_);
    if (generation == this->mic_tx_generation_.load(std::memory_order_relaxed))
      this->mic_tx_ring_.consume(length);
    const bool more = this->mic_tx_ring_.size() > 0;
    portEXIT_CRITICAL(&this->mic_tx_mux_);
    if (more)
      xTaskNotifyGive(this->mic_sender_task_);
  }
}

void VaClient::log_mic_health_() {
  const uint32_t now = millis();
  if (now - this->last_mic_health_ms_ < 5000)
    return;
  this->last_mic_health_ms_ = now;
  size_t queued;
  portENTER_CRITICAL(&this->mic_tx_mux_);
  queued = this->mic_tx_ring_.size();
  portEXIT_CRITICAL(&this->mic_tx_mux_);
  ESP_LOGI(TAG, "mic health: callbacks=%u last=%ums max=%uus peak=%u zero=%u dropped=%u send_fail=%u queued=%u",
           (unsigned) this->mic_callback_count_.exchange(0),
           (unsigned) (now - this->last_mic_callback_ms_.load()),
           (unsigned) this->mic_callback_max_us_.exchange(0),
           (unsigned) this->mic_signal_peak_.exchange(0),
           (unsigned) this->mic_zero_frame_count_.exchange(0),
           (unsigned) this->mic_tx_dropped_frames_.exchange(0),
           (unsigned) this->mic_tx_send_failures_.exchange(0), (unsigned) queued);
}

void VaClient::fade_ring_head_() {
  const size_t count = std::min(kFadeSamples, this->audio_fill_ / sizeof(int16_t));
  if (count < 2)
    return;
  for (size_t i = 0; i < count; i++) {
    const size_t offset = (this->audio_head_ + i * 2) % kAudioBufBytes;
    int16_t sample;
    std::memcpy(&sample, this->audio_buf_ + offset, sizeof(sample));
    sample = static_cast<int16_t>((static_cast<int32_t>(sample) * i) / (count - 1));
    std::memcpy(this->audio_buf_ + offset, &sample, sizeof(sample));
  }
}

void VaClient::fade_ring_tail_() {
  const size_t count = std::min(kFadeSamples, this->audio_fill_ / sizeof(int16_t));
  if (count < 2)
    return;
  for (size_t i = 0; i < count; i++) {
    const size_t offset = (this->audio_tail_ + kAudioBufBytes - count * 2 + i * 2) % kAudioBufBytes;
    int16_t sample;
    std::memcpy(&sample, this->audio_buf_ + offset, sizeof(sample));
    sample = static_cast<int16_t>((static_cast<int32_t>(sample) * (count - 1 - i)) / (count - 1));
    std::memcpy(this->audio_buf_ + offset, &sample, sizeof(sample));
  }
}

void VaClient::connect_() {
  if (!this->ws_task_finished_ || !this->ws_stop_completed_) {
    this->reconnect_start_pending_ = true;
    return;
  }
  if (this->ws_handle_ != nullptr) {
    // Already initialised; just (re)start. A synchronous start failure must
    // reschedule — otherwise the reconnect chain stalls silently and the
    // device stays offline until a reboot.
    this->ws_task_finished_ = false;
    this->ws_stop_completed_ = false;
    esp_err_t err = esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(this->ws_handle_));
    if (err != ESP_OK) {
      this->ws_task_finished_ = true;
      this->ws_stop_completed_ = true;
      ESP_LOGE(TAG, "esp_websocket_client_start (restart) failed: %d — rescheduling", (int) err);
      this->schedule_reconnect_();
    }
    return;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
  cfg.disable_auto_reconnect = true;  // we drive reconnects ourselves with exponential backoff
  cfg.reconnect_timeout_ms = 5000;    // ignored because disable_auto_reconnect=true
  cfg.ping_interval_sec = 30;
  cfg.pingpong_timeout_sec = 60;
  cfg.disable_pingpong_discon = false;
  cfg.network_timeout_ms = 2000;

  esp_websocket_client_handle_t handle = esp_websocket_client_init(&cfg);
  if (handle == nullptr) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    this->schedule_reconnect_();
    return;
  }
  this->ws_handle_ = handle;

  esp_err_t err = esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, va_ws_event_handler, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_register_events failed: %d", (int) err);
    this->ws_stop_completed_ = false;
    this->ws_destroy_requested_ = true;
    xTaskNotifyGive(this->ws_teardown_task_);
    this->schedule_reconnect_();
    return;
  }

  ESP_LOGI(TAG, "Connecting to %s", this->url_.c_str());
  this->ws_task_finished_ = false;
  this->ws_stop_completed_ = false;
  err = esp_websocket_client_start(handle);
  if (err != ESP_OK) {
    this->ws_task_finished_ = true;
    this->ws_stop_completed_ = true;
    ESP_LOGE(TAG, "esp_websocket_client_start failed: %d", (int) err);
    this->schedule_reconnect_();
  }
}

void VaClient::schedule_reconnect_() {
  // esp_websocket_client emits multiple events per failure (DISCONNECTED,
  // CLOSED, sometimes ERROR). Coalesce them into a single reconnect.
  if (this->reconnect_pending_.exchange(true)) {
    return;
  }

  // One per *failure* (coalesced), not per individual WS event. Once we
  // cross the threshold fire the audible-error trigger exactly once until
  // a successful connect resets the counter.
  this->consecutive_failures_++;
  if (this->consecutive_failures_ >= kRepeatedFailureThreshold &&
      !this->repeated_failure_fired_) {
    this->repeated_failure_fired_ = true;
    ESP_LOGW(TAG, "%u consecutive reconnect failures — firing on_repeated_failure",
             (unsigned) this->consecutive_failures_);
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
  }

  uint32_t delay = this->reconnect_delay_ms_;
  ESP_LOGI(TAG, "Scheduling reconnect in %u ms", (unsigned) delay);
  // Backoff schedule: 1s -> 2s -> 5s -> 10s (capped).
  if (this->reconnect_delay_ms_ < 2000) {
    this->reconnect_delay_ms_ = 2000;
  } else if (this->reconnect_delay_ms_ < 5000) {
    this->reconnect_delay_ms_ = 5000;
  } else {
    this->reconnect_delay_ms_ = 10000;
  }
  this->set_timeout("va_reconnect", delay, [this]() {
    this->reconnect_pending_ = false;
    this->connect_();
  });
}

void VaClient::reset_connection_state_() {
  this->ws_connected_ = false;
  this->server_barge_in_ = false;
  this->server_hello_received_ = false;
  this->server_barge_warning_logged_ = false;
  this->streaming_ = false;
  this->session_starting_ = false;
  this->clear_mic_tx_();
  portENTER_CRITICAL(&this->ring_mux_);
  this->audio_head_ = this->audio_tail_ = this->audio_fill_ = 0;
  ++this->audio_ring_epoch_;
  ++this->pcm_flush_epoch_;
  portEXIT_CRITICAL(&this->ring_mux_);
  this->followup_pending_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->idle_emit_pending_ = false;
  this->playback_priming_ = false;
  this->prime_armed_ = true;
  this->fade_in_pending_ = true;
  this->fade_out_pending_ = false;
  this->thinking_tail_pending_ = false;
  this->reply_audio_done_ = false;
  this->thinking_tail_pending_ = false;
  this->chain_prime_remaining_ = 0;
  this->suppress_followup_ = true;
  this->suppress_incoming_audio_ = false;
  this->post_stop_guard_ = false;
  this->last_binary_ms_ = 0;
  this->turn_t_wake_ = 0;
  this->turn_t_first_audio_out_ = 0;
  this->current_phase_.store(static_cast<uint8_t>(Phase::IDLE));
  const uint32_t epoch = ++this->connection_epoch_;
  this->defer([this, epoch]() {
    if (this->connection_epoch_.load() != epoch)
      return;
    this->cancel_timeout("va_no_speech");
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    this->cancel_timeout("va_tts_tail");
    this->cancel_timeout("va_stable_connection");
    this->fire_phase_led_("idle");
  });
}

void VaClient::fail_control_channel_(const char *operation) {
  if (!this->control_channel_dirty_.exchange(true))
    ESP_LOGE(TAG, "%s failed; forcing a WebSocket reconnect", operation);
  else
    ESP_LOGE(TAG, "%s failed during recovery; retrying reconnect", operation);
  this->reset_connection_state_();
  this->reconnect_requested_ = true;
}

bool VaClient::send_control_(const char *message, size_t length, const char *operation,
                             bool start_marker) {
  if ((!start_marker && (!this->ws_connected_ || this->control_channel_dirty_)) ||
      this->ws_handle_ == nullptr)
    return false;
  const TickType_t started = xTaskGetTickCount();
  const TickType_t budget = pdMS_TO_TICKS(kWsControlSendTimeoutMs);
  if (xSemaphoreTake(this->ws_send_mutex_, budget) != pdTRUE) {
    this->fail_control_channel_(operation);
    return false;
  }
  int sent = -1;
  const TickType_t elapsed = xTaskGetTickCount() - started;
  // esp_websocket_client uses the supplied timeout for up to three internal
  // waits (two locks and transport); divide the remaining total budget.
  const TickType_t per_wait = elapsed < budget ? (budget - elapsed) / 3 : 0;
  if (per_wait > 0 && (start_marker || (this->ws_connected_ && !this->control_channel_dirty_))) {
    sent = esp_websocket_client_send_text(
        static_cast<esp_websocket_client_handle_t>(this->ws_handle_), message,
        static_cast<int>(length), per_wait);
  }
  xSemaphoreGive(this->ws_send_mutex_);
  if (sent != static_cast<int>(length)) {
    this->fail_control_channel_(operation);
    return false;
  }
  return true;
}

void VaClient::on_ws_event(int32_t event_id, void *event_data) {
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      ESP_LOGI(TAG, "WS connected");
      this->ws_connected_ = false;
      this->clear_mic_tx_();
      const char start_msg[] = "{\"type\":\"start\"}";
      auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
      if (!this->send_control_(start_msg, sizeof(start_msg) - 1, "start marker", true))
        break;
      if (!esp_websocket_client_is_connected(handle)) {
        this->fail_control_channel_("start socket check");
        break;
      }
      this->control_channel_dirty_ = false;
      ++this->connection_epoch_;
      this->ws_connected_ = true;
      const uint32_t timer_epoch = this->connection_epoch_.load();
      this->defer([this, timer_epoch]() {
        if (this->connection_epoch_.load() == timer_epoch && this->ws_connected_)
          this->send_timer_state_();
      });
      this->reconnect_delay_ms_ = 1000;  // reset backoff on a clean open
      // Don't reset the failure counter / fired flag yet — a flap-and-die
      // link would spam chimes. Only re-arm after the connection has held
      // for kStableConnectionMs without a disconnect.
      this->set_timeout("va_stable_connection", kStableConnectionMs, [this]() {
        if (this->ws_connected_) {
          this->consecutive_failures_ = 0;
          this->repeated_failure_fired_ = false;
          ESP_LOGD(TAG, "WS stable for %u ms — error chime re-armed",
                   (unsigned) kStableConnectionMs);
        }
      });

      this->set_phase_("idle");
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      if (!this->ws_connected_ || this->control_channel_dirty_)
        break;
      if (data == nullptr || data->data_len < 0 || data->payload_offset < 0 ||
          data->payload_len < 0 || (data->data_len > 0 && data->data_ptr == nullptr))
        break;
      if (this->frame_connection_epoch_ != this->connection_epoch_) {
        this->frame_connection_epoch_ = this->connection_epoch_;
        this->text_assembler_.reset();
        this->pcm_assembler_.reset();
        this->last_data_was_binary_ = false;
      }
      const size_t offset = static_cast<size_t>(data->payload_offset);
      const size_t length = static_cast<size_t>(data->data_len);
      const size_t payload_length = static_cast<size_t>(data->payload_len);
      const bool continuation = data->op_code == 0x00;
      if (data->op_code == 0x01 && offset == 0)
        this->last_data_was_binary_ = false;
      else if (data->op_code == 0x02 && offset == 0)
        this->last_data_was_binary_ = true;
      if (data->op_code != 0x01 && data->op_code != 0x02 && !continuation)
        break;
      if (this->last_data_was_binary_) {
        // Only flushes (interrupt, barge-in, error, disconnect) discard the rest
        // of an in-flight frame; tail trims keep it so audio isn't dropped.
        uint32_t epoch;
        portENTER_CRITICAL(&this->ring_mux_);
        epoch = this->pcm_flush_epoch_;
        portEXIT_CRITICAL(&this->ring_mux_);
        if (epoch != this->pcm_epoch_) {
          this->pcm_epoch_ = epoch;
          this->pcm_assembler_.reset();
        }
        if (!this->pcm_assembler_.append(continuation,
                reinterpret_cast<const uint8_t *>(data->data_ptr), length, offset,
                payload_length, this->pcm_chunk_)) {
          ESP_LOGW(TAG, "dropping out-of-order or interrupted PCM frame");
        } else if (!this->pcm_chunk_.empty()) {
          this->handle_binary_(this->pcm_chunk_.data(), this->pcm_chunk_.size());
        }
      } else {
        const auto result = this->text_assembler_.append(
            continuation, data->fin, data->data_ptr, length, offset, payload_length);
        if (result == WsTextAssembler::Result::COMPLETE)
          this->handle_text_(this->text_assembler_.data(), this->text_assembler_.size());
        else if (result == WsTextAssembler::Result::DROPPED)
          ESP_LOGW(TAG, "dropping oversized or out-of-order WS text message");
      }
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR: {
      if (this->ws_connected_) {
        ESP_LOGW(TAG, "WS disconnected (event %d)", (int) event_id);
      }
      this->reset_connection_state_();
      this->reconnect_requested_ = true;
      break;
    }
    case WEBSOCKET_EVENT_FINISH:
      this->ws_task_finished_ = true;
      break;
    default:
      break;
  }
}

void VaClient::handle_text_(const char *data, size_t len) {
  std::string msg(data, len);
  ESP_LOGD(TAG, "WS text: %s", msg.c_str());
  const auto type = classify_ws_message(msg);

  if (type == WsMessageType::TIMER_START || type == WsMessageType::TIMER_CANCEL ||
      type == WsMessageType::TIMER_LIST) {
    TimerCommand command;
    const bool valid = parse_timer_command(msg, command);
    const uint32_t epoch = this->connection_epoch_.load();
    this->defer([this, command, valid, epoch]() {
      if (this->connection_epoch_.load() != epoch || !this->ws_connected_)
        return;
      if (!valid) {
        ESP_LOGW(TAG, "Invalid timer request");
        this->send_timer_ack_(command.request_id, VaTimers::Result::INVALID);
      } else {
        this->handle_timer_command_(command);
      }
    });
    return;
  }

  if (type == WsMessageType::ERROR) {
    ESP_LOGW(TAG, "Server reported error: %s", msg.c_str());
    this->streaming_ = false;
    this->session_starting_ = false;
    this->clear_mic_tx_();
    this->suppress_followup_ = true;
    this->suppress_incoming_audio_ = true;
    portENTER_CRITICAL(&this->ring_mux_);
    this->audio_head_ = this->audio_tail_ = this->audio_fill_ = 0;
    ++this->audio_ring_epoch_;
    ++this->pcm_flush_epoch_;
    portEXIT_CRITICAL(&this->ring_mux_);
    // Without an audible cue the user just sees the LED go idle and
    // assumes the assistant ignored them. Reuse the on_repeated_failure
    // trigger — it already plays error_cloud_expired and the failure
    // mode is identical from the user's perspective ("something went
    // wrong, try again"). We deliberately don't bump consecutive_failures_
    // here; that counter is for WS reachability, not server-side errors.
    // Marshalled via defer(): handle_text_ runs on the WS task and ESPHome
    // triggers (→ the yaml play_sound script) are not thread-safe.
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
    this->set_phase_("idle");
    return;
  }

  if (type == WsMessageType::HELLO) {
    // Handshake ack from the backend. It may carry follow-up tuning so the
    // device behaviour is configurable from the add-on (no reflash): a reconnect
    // after an add-on restart re-reads these.
    //   "follow_up_ms":N            — how long the mic stays open after a reply
    //                                 so the user can answer without a wake word.
    //   "follow_up_open_delay_ms":N — delay before that mic opens, to let the
    //                                 reply's i2s/DAC tail finish playing out.
    uint32_t v = 0;
    if (parse_uint_after_key(msg, "\"follow_up_ms\":", v)) {
      if (v > kFollowupMsMax)
        v = kFollowupMsMax;
      this->followup_ms_ = v;
      ESP_LOGI(TAG, "hello: follow-up window = %u ms (%s)", (unsigned) v,
               v == 0 ? "disabled, turn-based" : "mic stays open after replies");
    }
    if (parse_uint_after_key(msg, "\"follow_up_open_delay_ms\":", v)) {
      if (v > kFollowupOpenDelayMaxMs)
        v = kFollowupOpenDelayMaxMs;
      this->followup_open_delay_ms_ = v;
      ESP_LOGI(TAG, "hello: follow-up mic-open delay = %u ms", (unsigned) v);
    }
    if (parse_uint_after_key(msg, "\"wake_open_delay_ms\":", v)) {
      if (v > kFollowupOpenDelayMaxMs)  // reuse the same 5 s ceiling
        v = kFollowupOpenDelayMaxMs;
      this->wake_open_delay_ms_ = v;
      ESP_LOGI(TAG, "hello: wake mic-open delay = %u ms", (unsigned) v);
    }
    if (parse_uint_after_key(msg, "\"playback_prebuffer_ms\":", v)) {
      if (v > kPlaybackPrebufferMaxMs)
        v = kPlaybackPrebufferMaxMs;
      this->playback_prebuffer_ms_ = v;
      ESP_LOGI(TAG, "hello: playback prebuffer (jitter buffer) = %u ms (%s)", (unsigned) v,
               v == 0 ? "disabled" : "cushion before playback");
    }
    bool interrupt_response = false;
    if (!parse_bool_after_key(msg, "\"interrupt_response\"", interrupt_response) &&
        msg.find("\"interrupt_response\"") != std::string::npos)
      ESP_LOGW(TAG, "hello: invalid interrupt_response; hands-free interrupt disabled");
    this->server_barge_in_ = interrupt_response;
    this->server_hello_received_ = true;
    this->server_barge_warning_logged_ = false;
    this->set_barge_in(this->barge_in_);
    return;
  }

  if (this->session_starting_ || this->control_channel_dirty_)
    return;

  if (type == WsMessageType::AUDIO_DONE) {
    if (!this->reply_audio_done_) {
      portENTER_CRITICAL(&this->ring_mux_);
      this->fade_ring_tail_();
      this->fade_out_pending_ = false;
      this->reply_audio_done_ = true;
      portEXIT_CRITICAL(&this->ring_mux_);
    }
    this->prime_armed_ = true;
    return;
  }

  if (type == WsMessageType::REQUEST_FOLLOW_UP) {
    // Server's model called the request_follow_up tool — it asked a
    // question and wants the user to answer without saying a wake word.
    // Defer to loop()'s waiting_for_speaker_stop_ logic so we only fire
    // the chime + arm the mic after speaker_->is_stopped() returns true
    // (i.e. the i2s pipeline has finished playing the question's audio).
    // Setting both pending flags is idempotent — loop() handles both the
    // "already drained" and "still queued" cases uniformly via the
    // speaker-state poll.
    ESP_LOGI(TAG, "request_follow_up — waiting for speaker drain (%u bytes queued)",
             (unsigned) this->audio_fill_snapshot_());
    this->followup_pending_ = true;
    this->request_follow_up_pending_ = true;
    return;
  }

  if (type != WsMessageType::PHASE)
    return;
  // Substring match on `"value":"<phase>"` — keeps us out of a JSON parser
  // until M3 needs richer payloads.
  static const char *const kPhases[] = {"listening", "thinking", "replying", "idle"};
  for (const char *p : kPhases) {
    std::string needle = std::string("\"value\":\"") + p + "\"";
    if (msg.find(needle) != std::string::npos) {
      this->set_phase_(p);
      return;
    }
  }
}

void VaClient::handle_timer_command_(const TimerCommand &command) {
  VaTimers::Result result = VaTimers::Result::OK;
  if (command.kind == TimerCommand::Kind::START) {
    result = this->timers_.start(command.id, command.name, command.duration_s, millis());
    if (result == VaTimers::Result::OK) {
      this->last_timer_tick_ms_ = millis();
      for (auto *t : this->timer_started_triggers_)
        t->trigger();
    }
  } else if (command.kind == TimerCommand::Kind::CANCEL) {
    if (command.all) {
      if (this->timers_.cancel_all())
        for (auto *t : this->timer_cancelled_triggers_)
          t->trigger();
    } else {
      result = this->timers_.cancel(command.id);
      if (result == VaTimers::Result::OK)
        for (auto *t : this->timer_cancelled_triggers_)
          t->trigger();
    }
  }
  this->send_timer_ack_(command.request_id, result);
}

void VaClient::send_timer_ack_(const std::string &request_id, VaTimers::Result result) {
  const std::string message = timer_ack_json(request_id, result, this->timers_.list(millis()));
  this->send_control_(message.c_str(), message.size(), "timer_ack");
}

void VaClient::send_timer_state_() {
  const std::string message = timer_state_json(this->timers_.list(millis()));
  this->send_control_(message.c_str(), message.size(), "timer_state");
}

void VaClient::stop_ringing() {
  if (this->timers_.stop_ringing() && this->ws_connected_ && !this->control_channel_dirty_)
    this->send_timer_state_();
}

void VaClient::handle_binary_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || len < 2 || this->audio_buf_ == nullptr)
    return;
  if (this->session_starting_ || this->control_channel_dirty_)
    return;
  // After a "stop"/barge-in (send_interrupt) the backend is still streaming the
  // rest of the already-generated reply. Drop it so the cancelled reply goes
  // silent instead of refilling the PSRAM ring we just flushed. Cleared on the
  // next "idle"/"listening" phase (see set_phase_).
  if (this->suppress_incoming_audio_)
    return;
  if (this->reply_audio_done_ &&
      static_cast<Phase>(this->current_phase_.load()) == Phase::REPLYING) {
    bool empty;
    portENTER_CRITICAL(&this->ring_mux_);
    if (this->audio_fill_ > 0 && this->audio_fill_ <= kFadeSamples * sizeof(int16_t)) {
      this->audio_head_ = this->audio_tail_ = this->audio_fill_ = 0;
      ++this->audio_ring_epoch_;
    }
    empty = this->audio_fill_ == 0;
    portEXIT_CRITICAL(&this->ring_mux_);
    this->reply_audio_done_ = false;
    if (empty)
      this->fade_in_pending_ = true;
  }
  const uint32_t now_ms = millis();
  if (this->turn_t_first_audio_out_ == 0 && this->turn_t_wake_ != 0) {
    this->turn_t_first_audio_out_ = now_ms;
  }
  // Detector 1: WS frame inter-arrival jitter. Normal cadence is ~20 ms
  // per frame (OpenAI streams realtime). A gap > kWsGapWarnMs means the
  // bridge stalled, network blip, or OpenAI burst late — anything that
  // could starve the downstream chain. Log immediately so the gap is
  // adjacent to whatever the user reports hearing.
  if (this->last_binary_ms_ != 0) {
    const uint32_t gap = now_ms - this->last_binary_ms_;
    if (gap > kWsGapWarnMs) {
      this->ws_gap_count_++;
      if (gap > this->ws_gap_max_ms_) this->ws_gap_max_ms_ = gap;
      ESP_LOGW(TAG, "ws audio gap: %u ms (ring fill %u bytes)",
               (unsigned) gap, (unsigned) this->audio_fill_snapshot_());
    }
  }
  this->last_binary_ms_ = now_ms;
  // PCM16 mono @ 24 kHz, append to ring buffer. loop() drains.
  // Snapshot audio_fill_ under the lock — it's modified by loop() on the
  // other core and we can't trust a torn read.
  size_t free_space;
  uint32_t audio_epoch;
  portENTER_CRITICAL(&this->ring_mux_);
  free_space = kAudioBufBytes - this->audio_fill_;
  audio_epoch = this->audio_ring_epoch_;
  portEXIT_CRITICAL(&this->ring_mux_);
  if (len > free_space) {
    ESP_LOGW(TAG, "audio buffer overflow: dropping %u bytes (have %u free of %u total)",
             (unsigned) (len - free_space), (unsigned) free_space, (unsigned) kAudioBufBytes);
    len = free_space;
    if (len == 0)
      return;
  }
  // Apply user-controlled volume from external_media_player before writing to
  // the ring. volume_ is set from yaml on every media_player volume / mute
  // change. With vol ≤ 1 there is no mathematical way to overflow int16, but
  // we keep a defensive saturation + clipped_samples_ counter so any future
  // gain re-introduction shows up in the per-turn summary instead of silently
  // distorting.
  size_t pairs = len / 2;
  if (pairs > 0) {
    auto *in = reinterpret_cast<const int16_t *>(data);
    // Scale into tts_buf_ — NOT mono_buf_: that vector is owned by the mic
    // task (on_mic_data_ refills it on every mic frame, and the mic never
    // stops while mWW runs), whereas we're on the WS task. Sharing one vector
    // raced the two tasks (concurrent resize/realloc + interleaved writes),
    // putting mic samples / freed memory into the TTS ring — audible as hiss.
    this->tts_buf_.resize(pairs);
    float vol = this->volume_;
    if (vol < 0.0f) vol = 0.0f;
    else if (vol > 1.0f) vol = 1.0f;
    // Q15 fixed point so the inner loop stays integer-only.
    int32_t scale = static_cast<int32_t>(vol * 32768.0f);
    uint32_t clipped = 0;
    for (size_t i = 0; i < pairs; i++) {
      int32_t v = (static_cast<int32_t>(in[i]) * scale) >> 15;
      if (v > 32767) { v = 32767; clipped++; }
      else if (v < -32768) { v = -32768; clipped++; }
      this->tts_buf_[i] = static_cast<int16_t>(v);
    }
    this->clipped_samples_ += clipped;
    data = reinterpret_cast<const uint8_t *>(this->tts_buf_.data());
    // WsFrameAssembler supplied only complete PCM16 samples.
    len = pairs * 2;
  }
  // Two-part write: from tail to end, then wrap to start.
  // We need a stable snapshot of audio_tail_ for the memcpy destination,
  // then commit tail + fill atomically with the writes so the reader on
  // the other core never sees a new tail before the memcpy completed.
  // Doing the memcpy *inside* the critical section is the simplest way
  // to guarantee that ordering — len is at most a few KB per WS frame
  // and PSRAM memcpy is ~10–20 µs, well under any audio deadline.
  portENTER_CRITICAL(&this->ring_mux_);
  if (audio_epoch != this->audio_ring_epoch_ || !this->ws_connected_ ||
      this->suppress_incoming_audio_) {
    portEXIT_CRITICAL(&this->ring_mux_);
    return;
  }
  const bool was_empty = (this->audio_fill_ == 0);
  size_t tail = this->audio_tail_;
  size_t first = std::min(len, kAudioBufBytes - tail);
  std::memcpy(this->audio_buf_ + tail, data, first);
  if (first < len) {
    std::memcpy(this->audio_buf_, data + first, len - first);
  }
  this->audio_tail_ = (tail + len) % kAudioBufBytes;
  this->audio_fill_ += len;
  portEXIT_CRITICAL(&this->ring_mux_);
  // The first PCM of a segment consumes the arm; it primes only if it lands in
  // an empty ring (old tails keep draining, and later mid-reply gaps don't re-prime).
  const bool armed = this->prime_armed_.exchange(false);
  if (armed && was_empty && this->playback_prebuffer_ms_ > 0 && !this->playback_priming_) {
    this->prime_started_ms_ = now_ms;
    this->playback_priming_ = true;
  }
  // No per-chunk log — fires 50+ times per reply at DEBUG and drowns the
  // log. The throttled drain log in loop() gives enough visibility into
  // queue depth.
}

void VaClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (this->is_failed())
    return;
  const uint32_t generation = this->mic_tx_generation_.load();
  const uint32_t started_us = micros();
  this->mic_callback_count_.fetch_add(1);
  this->last_mic_callback_ms_ = millis();
  // i2s_mics yields interleaved stereo int32 frames: [L0_low,L0_high, R0_low,R0_high, L1..].
  // Each frame = 8 bytes (2ch × 4 bytes). We want one channel converted to
  // int16 mono. Real audio sits in the high 16 bits (ADC pads up to int32).
  if (samples.size() < 8)
    return;

  const auto *in32 = reinterpret_cast<const int32_t *>(samples.data());
  size_t total_int32 = samples.size() / 4;
  size_t mono_samples = total_int32 / 2;  // half belong to this channel
  size_t offset = this->mic_channel_ & 0x1;

  this->mono_buf_.resize(mono_samples);
  const int32_t gain_q8 = this->mic_gain_q8_.load();
  uint32_t peak = 0;
  for (size_t i = 0; i < mono_samples; i++) {
    int32_t s = in32[i * 2 + offset];
    int16_t mono;
    if (gain_q8 == 256) {
      mono = static_cast<int16_t>(s >> 16);
    } else {
      // Keep 8 extra fraction bits from the 32-bit sample before gain.
      int64_t v = (static_cast<int64_t>(s >> 8) * gain_q8) >> 16;
      mono = static_cast<int16_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, v)));
    }
    this->mono_buf_[i] = mono;
    peak = std::max(peak, static_cast<uint32_t>(
        mono < 0 ? -static_cast<int32_t>(mono) : static_cast<int32_t>(mono)));
  }
  uint32_t old_peak = this->mic_signal_peak_.load();
  while (peak > old_peak && !this->mic_signal_peak_.compare_exchange_weak(old_peak, peak)) {}
  if (peak == 0)
    this->mic_zero_frame_count_.fetch_add(1);

  // Streaming gate. When no session is active we don't forward frames to the
  // server (otherwise OpenAI's VAD would respond to any room speech — the wake
  // word would be decoration). We keep a rolling pre-roll ring while closed,
  // but it is DISCARDED on session open (see below), so this is just a cheap
  // rolling buffer kept around for a possible future capture-gating approach.
  // The session opens via start_session() (wake handler) and closes on
  // "phase":"idle" from the server (response.done).
  if (!this->streaming_ || !this->ws_connected_ || this->session_starting_ ||
      this->control_channel_dirty_) {
    this->preroll_push_(this->mono_buf_.data(), this->mono_buf_.size());
    uint32_t elapsed = micros() - started_us;
    uint32_t prev = this->mic_callback_max_us_.load();
    while (elapsed > prev && !this->mic_callback_max_us_.compare_exchange_weak(prev, elapsed)) {}
    return;
  }

  // First frame of a fresh session: DISCARD the pre-roll instead of replaying
  // it. The ring caught the wake chime leaking through the mic (XMOS AEC leaves
  // ~10x) during the chime + tail-delay window; replaying it fed the chime back
  // to OpenAI as a phantom utterance ("Au!"). Resetting here (the ring's only
  // owner is this mic task) avoids any cross-task race — the main loop just sets
  // the flag. Matches marcinnowak79 gemini_proxy's ring_buffer_->reset() on
  // start. Trade-off: a word spoken *during* the chime is lost (the user speaks
  // after the listening ring lights up).
  bool queued = false;
  portENTER_CRITICAL(&this->mic_tx_mux_);
  if (mic_tx_packet_ready(generation, this->mic_tx_generation_.load(std::memory_order_relaxed),
                          this->streaming_.load(), this->session_starting_.load(),
                          this->ws_connected_.load()) && !this->control_channel_dirty_.load()) {
    if (this->preroll_discard_pending_.exchange(false)) {
      this->preroll_count_ = 0;
      this->preroll_head_ = 0;
    }
    queued = this->mic_tx_ring_.push_all(reinterpret_cast<const uint8_t *>(this->mono_buf_.data()),
                                        this->mono_buf_.size() * sizeof(int16_t));
  }
  portEXIT_CRITICAL(&this->mic_tx_mux_);
  if (queued) {
    xTaskNotifyGive(this->mic_sender_task_);
  } else {
    this->mic_tx_dropped_frames_.fetch_add(1);
  }
  uint32_t elapsed = micros() - started_us;
  uint32_t prev = this->mic_callback_max_us_.load();
  while (elapsed > prev && !this->mic_callback_max_us_.compare_exchange_weak(prev, elapsed)) {}
}

void VaClient::preroll_push_(const int16_t *data, size_t n) {
  if (this->preroll_buf_ == nullptr || this->preroll_capacity_samples_ == 0)
    return;
  const size_t cap = this->preroll_capacity_samples_;
  for (size_t i = 0; i < n; i++) {
    this->preroll_buf_[this->preroll_head_] = data[i];
    if (++this->preroll_head_ >= cap)
      this->preroll_head_ = 0;
    if (this->preroll_count_ < cap)
      this->preroll_count_++;
  }
}

VaClient::Phase VaClient::phase_from_string_(const std::string &phase) {
  if (phase == "listening")
    return Phase::LISTENING;
  if (phase == "thinking")
    return Phase::THINKING;
  if (phase == "replying")
    return Phase::REPLYING;
  return Phase::IDLE;
}

const char *VaClient::phase_name_(Phase p) {
  switch (p) {
    case Phase::LISTENING:
      return "listening";
    case Phase::THINKING:
      return "thinking";
    case Phase::REPLYING:
      return "replying";
    default:
      return "idle";
  }
}

void VaClient::set_phase_(const std::string &phase) {
  if (this->session_starting_ || this->control_channel_dirty_)
    return;
  // Don't dedupe — we want yaml-side control_leds to re-render even on
  // identical phase if other inputs (e.g. va WS connection state) have
  // changed since the last emission.
  const Phase prev = static_cast<Phase>(this->current_phase_.load());
  this->current_phase_.store(static_cast<uint8_t>(phase_from_string_(phase)));
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());

  // Post-stop `thinking` guard. After a local "stop" the mic gate is closed
  // (send_interrupt set post_stop_guard_), so no new turn can begin until a
  // wake (start_session clears it). A `thinking` arriving here is the server
  // VAD's end-of-turn for the utterance we just cancelled — acting on it
  // strands the LED in `thinking` until the backend's 15 s watchdog (observed
  // 2026-06-14: "stop" mid-question -> 15 s stuck thinking). Ignore it, keeping
  // the prior phase. Scoped to `thinking`: a web search's replying->thinking
  // has no stop (guard stays false), and a reply-drain emits no `thinking`.
  if (this->post_stop_guard_ && phase == "thinking") {
    this->current_phase_.store(static_cast<uint8_t>(prev));  // don't advance to the stale value
    ESP_LOGI(TAG, "ignoring stale 'thinking' after stop (no wake since)");
    return;
  }

  // Lift the post-"stop" incoming-audio suppression ONLY on "listening" — a
  // genuine fresh user turn whose reply is legitimate. We deliberately do NOT
  // lift on "idle": the backend keeps streaming the cancelled reply's audio in
  // real-time (OpenAI generated it faster than playback; a `response.cancel`
  // after the response already finished is a no-op), so it can drain for many
  // seconds AFTER the stop. An "idle" can arrive mid-drain — notably the
  // backend's thinking-watchdog force-idle — and lifting suppression there
  // un-mutes the still-arriving tail, which then plays as an "answer out of
  // nowhere" (observed live 2026-06-13 10:11). A real next reply is always
  // preceded by "listening" (the user speaks), so that stays the only safe
  // gate; until then the tail keeps being dropped. We also do NOT clear on
  // "thinking"/"replying" — those can still belong to the reply we cancelled.
  if (this->suppress_incoming_audio_ && phase == "listening") {
    this->suppress_incoming_audio_ = false;
    ESP_LOGI(TAG, "incoming-audio suppression lifted on phase=listening");
  }

  // Streaming gate state machine:
  //   listening  → mic on (user is being heard)
  //   thinking   → mic stays on. `thinking` only means "the VAD thinks the user
  //                stopped", but with semantic_vad it can flap listening↔thinking
  //                at the start of a turn while the user is still talking. No bot
  //                audio plays during thinking (no echo risk), so keep streaming
  //                until the reply genuinely begins — otherwise a spurious
  //                `thinking` cuts the mic, the backend's input watchdog
  //                force-ends the turn with no transcript, and the turn hangs.
  //   replying   → mic off (bot is speaking; gate to avoid picking up our own
  //                TTS in case the XMOS AEC isn't perfect). barge_in keeps it on.
  //   idle       → mic on for kFollowupMs so the user can answer a question
  //                without re-triggering the wake word. Timer expiry closes
  //                the session.
  if (phase == "listening") {
    this->reply_audio_done_ = false;
    this->prime_armed_ = true;
    if (prev == Phase::IDLE && this->audio_fill_snapshot_() == 0)
      this->fade_in_pending_ = true;
    if (!this->streaming_) {
      ESP_LOGI(TAG, "phase=listening — mic streaming on");
      this->streaming_ = true;
    }
    // Handsfree barge-in cut-over: a `listening` arriving while we still have
    // TTS queued means the backend's server VAD heard the user talk over the
    // reply and already cancelled the OpenAI response. Drop the audio still in
    // our PSRAM ring so playback stops immediately instead of finishing the
    // now-cancelled sentence. We do NOT send a WS interrupt here — the backend
    // initiated this — we just stop local playback.
    if (this->effective_barge_in_() && this->audio_fill_snapshot_() > 0) {
      portENTER_CRITICAL(&this->ring_mux_);
      this->audio_head_ = 0;
      this->audio_tail_ = 0;
      this->audio_fill_ = 0;
      ++this->audio_ring_epoch_;
      ++this->pcm_flush_epoch_;
      portEXIT_CRITICAL(&this->ring_mux_);
      this->idle_emit_pending_ = false;
      this->fade_in_pending_ = true;
      ESP_LOGI(TAG, "phase=listening during reply — barge-in, flushed TTS queue");
    }
    if (this->turn_t_listening_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_listening_ = millis();
    }
    // Server heard us — watchdog no longer needed.
    this->cancel_timeout("va_no_speech");
    this->cancel_timeout("va_followup");
  } else if (phase == "thinking" || phase == "replying") {
    if (phase == "thinking")
      this->prime_armed_ = true;
    if (phase == "thinking" && prev == Phase::REPLYING) {
      portENTER_CRITICAL(&this->ring_mux_);
      if (this->audio_fill_ > 0) {
        if (!this->reply_audio_done_)
          this->fade_ring_tail_();
        this->thinking_tail_pending_ = true;
      }
      portEXIT_CRITICAL(&this->ring_mux_);
      this->fade_out_pending_ = false;
      this->reply_audio_done_ = true;
    } else if (phase == "replying") {
      bool old_reply_remaining = false;
      if ((prev == Phase::THINKING && this->thinking_tail_pending_) ||
          (prev == Phase::REPLYING && this->reply_audio_done_)) {
        portENTER_CRITICAL(&this->ring_mux_);
        if (this->audio_fill_ > 0 && this->audio_fill_ <= kFadeSamples * sizeof(int16_t)) {
          this->audio_head_ = this->audio_tail_ = this->audio_fill_ = 0;
          ++this->audio_ring_epoch_;
        }
        old_reply_remaining = this->audio_fill_ > 0;
        portEXIT_CRITICAL(&this->ring_mux_);
      }
      this->thinking_tail_pending_ = false;
      // Keep the previous reply drainable until the first new PCM arrives.
      this->reply_audio_done_ = old_reply_remaining;
    }
    // Gate the mic off only once the bot actually starts speaking (`replying`).
    // With handsfree barge-in we don't gate at all (the server VAD + XMOS AEC
    // arbitrate talk-over). Crucially we do NOT gate on `thinking`: semantic_vad
    // can flap listening↔thinking at the start of a turn while the user is still
    // speaking, and cutting the mic on those spurious flaps starves the backend
    // of audio → its input watchdog force-ends the turn with no transcript → the
    // turn hangs in thinking. No bot audio plays during thinking, so there's no
    // echo cost to keeping the mic open until the reply genuinely starts.
    if (phase == "replying" && this->streaming_ && !this->effective_barge_in_()) {
      ESP_LOGI(TAG, "phase=replying — mic streaming off");
      this->streaming_ = false;
      this->clear_mic_tx_();
    }
    if (phase == "thinking" && this->turn_t_thinking_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_thinking_ = millis();
    }
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    this->cancel_timeout("va_tts_tail");
    this->cancel_timeout("va_no_speech");
    this->followup_pending_ = false;
    this->waiting_for_speaker_stop_ = false;
    this->request_follow_up_pending_ = false;
    this->followup_armed_ = false;
    this->idle_emit_pending_ = false;  // new turn began, drop any held idle
  } else if (phase == "idle") {
    this->thinking_tail_pending_ = false;
    this->playback_priming_ = false;
    this->prime_armed_ = true;
    if (!this->reply_audio_done_)
      this->fade_out_pending_ = true;
    this->reply_audio_done_ = true;
    if (!(prev == Phase::REPLYING && this->turn_t_first_audio_out_ == 0 &&
          this->streaming_)) {
      this->streaming_ = false;
      this->clear_mic_tx_();
      this->cancel_timeout("va_no_speech");
    }
    // Turn boundary: reset the WS-gap reference so the silence between THIS
    // reply and the NEXT turn's reply (~7 s across a follow-up exchange, where
    // start_session() — the other reset point — is never called) isn't logged
    // as a bogus "ws audio gap". Only intra-reply gaps are real signal.
    this->last_binary_ms_ = 0;
    // Only open a follow-up window if we just finished a real turn —
    // i.e. the previous phase was thinking or replying. Otherwise we'd
    // open the window for every spurious idle (initial WS hello, post-
    // disconnect idle, etc), spamming "follow-up window open" logs and
    // opening the mic for 5s every time the device just reconnects.
    const bool turn_just_ended = prev == Phase::THINKING || prev == Phase::REPLYING;
    if (!turn_just_ended) {
      // Plain idle (boot, reconnect, etc) — no follow-up. Fall through to
      // the regular trigger fire so the LED updates.
      //
      // An idle that arrives while the mic gate is still OPEN means an orphaned
      // stream that nothing else will close — shut it. Two real ways to reach
      // here with streaming_ still true:
      //   • idle straight from `listening`: the turn died without a reply (a
      //     backend force-idle on rate-limit / thinking-watchdog — the backend
      //     suppresses `thinking` after declaring a turn dead, so `listening`
      //     is exactly where the device sits — or a WS drop mid-listening).
      //   • idle from `idle` (prev==IDLE) with the mic open: the brief post-wake
      //     "waiting for the first phase" window, or an open follow-up window,
      //     when the WS drops (on_ws_event fires set_phase_("idle") on
      //     disconnect). Without this the gate stays open and the mic resumes
      //     streaming the room the instant we reconnect — and the backend's
      //     mic-resume buffer clear can't help because the stream never paused.
      // Closing here can't cut a LIVE follow-up window short: the only idle that
      // reaches an open window is exactly such a disconnect — the backend sends
      // no idle while it's waiting for the user to answer.
      if (this->streaming_) {
        ESP_LOGI(TAG, "idle while mic open (prev=%s) — closing orphaned mic gate",
                 phase_name_(prev));
        this->streaming_ = false;
        this->cancel_timeout("va_no_speech");
      }
    } else if (this->suppress_followup_) {
      // send_interrupt() set this — user explicitly asked us to stop.
      // Close the session cleanly: streaming off, no follow-up, fall through
      // to the regular trigger fire so the LED goes idle.
      this->suppress_followup_ = false;
      this->streaming_ = false;
      this->followup_pending_ = false;
      this->waiting_for_speaker_stop_ = false;
      this->request_follow_up_pending_ = false;
      this->followup_armed_ = false;
      this->cancel_timeout("va_tts_tail");
      this->idle_emit_pending_ = false;
    } else if (this->audio_fill_snapshot_() == 0) {
      // Stale-`idle` guard. prev==REPLYING with NO audio played since the last
      // wake (turn_t_first_audio_out_==0) means this `idle` belongs to a reply
      // that was stopped and then superseded by a new wake while it was still
      // draining: start_session() reset turn_t_first_audio_out_ and the old
      // tail is suppressed, so nothing actually played in THIS session. Opening
      // a follow-up here lights a spurious `listening` window over the fresh
      // wake session (observed live 2026-06-14: web-search → "stop" → bare wake
      // → ~4 s `listening` flicker; device log "Phase -> idle (was replying)").
      // Ignore it: the wake's no-speech watchdog is still armed (we never
      // reached `listening`, so it was never cancelled) and owns the session —
      // it idles the LED and closes the mic. current_phase_ is already IDLE
      // (set at the top of set_phase_), so the next message's `prev` is fine.
      // Tight by construction: a reply that really played sets
      // turn_t_first_audio_out_ (suppression lifts on `listening`), and a
      // follow-up chain keeps it set (only start_session resets it), so no
      // legitimate follow-up is skipped; the dead-turn path (prev==THINKING,
      // watchdog already cancelled) is deliberately left untouched.
      if (prev == Phase::REPLYING && this->turn_t_first_audio_out_ == 0) {
        ESP_LOGI(TAG, "stale replying-idle after wake (no audio this session) "
                      "— ignoring, no follow-up");
        // Distinguished by the mic gate: streaming_==true is a BARE WAKE (mic
        // still open, never reached `replying` this session) → the no-speech
        // watchdog is armed and owns the idle + mic close, so leave the LED on
        // the wake state. streaming_==false means the turn DID reach `replying`
        // (mic gated) but the audio never played — e.g. suppress_incoming_audio_
        // was still set from an earlier stop, so turn_t_first_audio_out_ stayed
        // 0. The watchdog was cancelled when the turn reached `listening`, so
        // nothing else fires idle → fall through to the LED trigger below, else
        // the ring strands on `replying` (observed live 2026-06-14: rapid stops
        // left suppression on, the search reply was dropped, the LED hung).
        if (this->streaming_) {
          this->reply_audio_done_ = false;
          this->fade_out_pending_ = false;
          return;  // bare wake: va_no_speech owns the idle + mic
        }
        // else: fall through to fire the idle LED (still no follow-up).
      } else {
        // Server says response.done and the device has actually played out.
        // Open the follow-up window (mic on so user can answer a question).
        this->open_followup_window_(this->followup_ms_);
      }
      // fall through to fire the trigger normally below (LED -> idle); the
      // follow-up window, if any, fires its own `listening` LED after its delay.
    } else {
      // Server says response.done, but we still have seconds of TTS queued
      // in PSRAM + downstream rings. Two things wait on the queue:
      //   1) the LED transition to idle (otherwise it goes off while the
      //      device is still speaking)
      //   2) opening the follow-up mic window (echo + false VAD trigger)
      // Mark both pending; the drain handler in loop() releases them
      // together after the speaker actually finishes.
      ESP_LOGI(TAG, "phase=idle but %u bytes still queued; LED + follow-up deferred",
               (unsigned) this->audio_fill_snapshot_());
      this->followup_pending_ = true;
      this->idle_emit_pending_ = true;
      return;  // suppress immediate trigger fire — open_followup_window_ will fire it later
    }
  }

  // set_phase_ may be called from the websocket task; ESPHome triggers and
  // most component APIs are not thread-safe. Marshal the side effects onto
  // the main loop via defer().
  std::string phase_copy = phase;
  this->defer([this, phase_copy]() {
    // We deliberately do NOT call speaker->stop() on "listening" anymore:
    // the speaker task runs continuously after setup() and play() just
    // appends to its ring buffer. Stop/start churn was creating multiple
    // speaker_task instances racing for the i2s channel ("Parent bus is
    // busy"). For barge-in/interrupt we'll add a buffer-flush API in M3.
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

void VaClient::start_session() {
  // Open the streaming window. on_mic_data_ will start forwarding frames to
  // the server until "phase":"idle" comes back (response.done). Without this
  // gate, OpenAI Realtime's server VAD would respond to any speech in the
  // room — wake word would be cosmetic.

  // Belt-and-suspenders barge-in. The yaml wake handler calls send_interrupt()
  // when it observes voice_assistant_phase == replying, but two windows slip
  // past that check:
  //   1) server already sent phase=idle yet PSRAM still has seconds of TTS
  //      queued (idle_emit_pending_). yaml's voice_assistant_phase has been
  //      reset to idle and the wake handler takes the "fresh session" path —
  //      no interrupt — so the new reply overlaps with the tail of the old.
  //   2) wake fires mid-reply on a long answer where the server is still
  //      generating tokens; without an interrupt, OpenAI keeps streaming TTS
  //      we'll never play, burning tokens.
  // The bridge treats interrupt as cheap when there's nothing to cancel
  // (response_cancel_not_active is in its benignCodes set), and
  // input_audio_buffer.clear is safe here because mic frames for the new turn
  // don't start flowing until after this function returns.
  if (!this->ws_connected_ || this->control_channel_dirty_) {
    ESP_LOGW(TAG, "start_session: WebSocket unavailable");
    return;
  }
  this->session_starting_ = true;
  this->streaming_ = false;
  this->clear_mic_tx_();
  const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
  const bool residual_reply =
      this->audio_fill_snapshot_() > 0 ||
      this->idle_emit_pending_ ||
      phase_now == Phase::REPLYING ||
      phase_now == Phase::THINKING;
  if (residual_reply) {
    ESP_LOGI(TAG, "start_session: interrupting residual reply (phase=%s, fill=%u)",
             phase_name_(phase_now), (unsigned) this->audio_fill_snapshot_());
    this->send_interrupt();
    if (!this->ws_connected_)
      return;
    this->session_starting_ = true;
  }

  // Discard (do NOT replay) the pre-roll captured before this session. The ring
  // caught the wake chime leaking through the mic during the chime/tail-delay
  // window; replaying it fed the chime back to OpenAI as a phantom "Au!". The
  // mic task does the actual ring reset (its sole owner) when it sees this flag.
  this->preroll_discard_pending_ = true;
  this->fade_in_pending_ = true;
  this->fade_out_pending_ = false;
  this->reply_audio_done_ = false;
  this->prime_armed_ = true;
  // Tell the backend a fresh wake started (dangling-VAD guard, A). Sent AFTER
  // the residual-reply interrupt above so the backend sees interrupt → wake in
  // order. The first real mic frame for this turn doesn't flow until after this
  // returns, so the guard's "speech since wake" tracker starts clean.
  if (!this->send_wake_()) {
    this->session_starting_ = false;
    return;
  }
  if (!this->ws_connected_) {
    this->session_starting_ = false;
    return;
  }
  // New wake word starts a fresh session — drop any pending or active
  // follow-up window from the previous turn.
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  // A genuine new turn starts here — drop the post-stop `thinking` guard so
  // this turn's `thinking` shows normally. (Set last, AFTER the residual-reply
  // send_interrupt() above re-set it, so the wake always ends with it clear.)
  this->post_stop_guard_ = false;
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_tts_tail");
  // Anchor turn-latency timestamps for the new turn.
  this->turn_t_wake_ = millis();
  this->turn_t_listening_ = 0;
  this->turn_t_thinking_ = 0;
  this->turn_t_first_audio_out_ = 0;
  // Reset audio-quality detectors for this turn.
  this->last_binary_ms_ = 0;
  this->ws_gap_count_ = 0;
  this->ws_gap_max_ms_ = 0;
  this->clipped_samples_ = 0;
  this->underrun_logged_this_turn_ = false;
  // Watchdog: if server doesn't hear us within kNoSpeechTimeoutMs, abort the
  // session so we're not stuck with the mic open after a misfire.
  this->set_timeout("va_no_speech", kNoSpeechTimeoutMs, [this]() {
    ESP_LOGI(TAG, "no speech detected for %u ms — aborting session",
             (unsigned) kNoSpeechTimeoutMs);
    this->streaming_ = false;
    this->clear_mic_tx_();
    const char m[] = "{\"type\":\"interrupt\"}";
    this->send_control_(m, sizeof(m) - 1, "no-speech interrupt");
    this->turn_t_wake_ = 0;
    // Force LED back to idle from yaml side.
    this->defer([this]() {
      for (auto *t : this->phase_triggers_) {
        t->trigger("idle");
      }
    });
  });
  if (!this->ws_connected_ || this->control_channel_dirty_) {
    this->session_starting_ = false;
    return;
  }
  this->streaming_ = true;
  this->session_starting_ = false;
  ESP_LOGI(TAG, "start_session() — wake sent, streaming on");
}

void VaClient::open_followup_window_(uint32_t duration_ms) {
  // If a phase=idle LED transition was held back while audio drained, fire
  // it now so the LED goes to idle in sync with the speaker actually going
  // quiet (instead of as soon as the server emitted response.done).
  if (this->idle_emit_pending_) {
    this->idle_emit_pending_ = false;
    this->defer([this]() {
      for (auto *t : this->phase_triggers_) {
        t->trigger("idle");
      }
    });
    // Per-turn latency summary. Anchors are zero if we skipped a milestone
    // (e.g. interrupt mid-reply); show "?" so the line stays readable.
    if (this->turn_t_wake_ != 0) {
      uint32_t now = millis();
      auto fmt = [](uint32_t from, uint32_t to) -> std::string {
        if (from == 0 || to == 0 || to < from)
          return "?";
        return std::to_string(to - from) + "ms";
      };
      ESP_LOGI(TAG,
               "turn latency: wake→listening=%s listening→thinking=%s "
               "thinking→first_audio=%s first_audio→played_out=%s "
               "total=%s",
               fmt(this->turn_t_wake_, this->turn_t_listening_).c_str(),
               fmt(this->turn_t_listening_, this->turn_t_thinking_).c_str(),
               fmt(this->turn_t_thinking_, this->turn_t_first_audio_out_).c_str(),
               fmt(this->turn_t_first_audio_out_, now).c_str(),
               fmt(this->turn_t_wake_, now).c_str());
      // Audio-quality summary: only logged if anything anomalous fired.
      // A clean turn produces no line — keeps the noise floor low.
      if (this->ws_gap_count_ > 0 || this->clipped_samples_ > 0 ||
          this->underrun_logged_this_turn_) {
        ESP_LOGW(TAG,
                 "turn audio: ws_gaps=%u (max=%ums) clipped_samples=%u underrun=%s",
                 (unsigned) this->ws_gap_count_,
                 (unsigned) this->ws_gap_max_ms_,
                 (unsigned) this->clipped_samples_,
                 this->underrun_logged_this_turn_ ? "yes" : "no");
      }
      this->turn_t_wake_ = 0;  // mark turn as logged
    }
  }
  if (duration_ms == 0) {
    // Follow-up disabled for this call: turn-based behaviour like the
    // original pipeline. Leave the mic closed; user must say a wake word
    // for the next turn. (The LED idle was already emitted above / by the
    // set_phase_ tail.)
    this->streaming_ = false;
    this->clear_mic_tx_();
    return;
  }
  // Follow-up dialog window. We do NOT open the mic immediately: has_buffered_
  // data() (the drain signal that got us here) goes false ~500 ms before true
  // silence — there's still the i2s ring + DAC tail playing out. Opening the
  // mic now would let that tail leak back in (XMOS AEC ~10x) and false-trigger
  // the server VAD. So wait followup_open_delay_ms_ (from the backend) for the
  // tail to clear, THEN open the mic and show `listening` so the user can see
  // the device is waiting for them to answer (without a wake word). The LED
  // stays idle (emitted just above) during this short gap. Any new turn / wake /
  // stop / interrupt cancels "va_followup_open" before it fires (see set_phase_,
  // start_session, send_interrupt), so a new turn won't reopen the mic under it.
  const uint32_t open_delay = this->followup_open_delay_ms_;
  ESP_LOGI(TAG, "follow-up: mic opens in %u ms, then listening for %u ms",
           (unsigned) open_delay, (unsigned) duration_ms);
  this->set_timeout("va_followup_open", open_delay, [this, duration_ms]() {
    if (!this->ws_connected_) {
      // The reply drained into a dead connection (WS dropped mid-reply).
      // Opening the mic would show a "listening" LED while on_mic_data_
      // drops every frame — a window the user talks into for nothing.
      // Leave the LED on idle; a fresh wake after reconnect starts clean.
      ESP_LOGI(TAG, "follow-up window skipped — WS disconnected");
      return;
    }
    ESP_LOGI(TAG, "follow-up window open (mic on, listening for %u ms)", (unsigned) duration_ms);
    this->streaming_ = true;
    this->fire_phase_led_("listening");  // blue ring: user may answer now
    this->set_timeout("va_followup", duration_ms, [this]() {
      if (this->streaming_) {
        ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
        this->streaming_ = false;
        this->clear_mic_tx_();
        this->send_mic_flush_();        // drop any uncommitted partial utterance
        this->fire_phase_led_("idle");  // no answer came; back to idle
      }
    });
  });
}

void VaClient::send_mic_flush_() {
  // The mic gate just closed mid-stream because a follow-up window timed out.
  // If the user had started (but not finished) speaking, that audio sits
  // UNCOMMITTED in OpenAI's input_audio_buffer; left there, a later wake's
  // audio "completes" it and the model answers a stale half-sentence. Drop it
  // NOW, at the cut-off, so no reactive clear-on-wake is needed (that disturbed
  // the server VAD and caused garbage commits). This timer only fires when the
  // user did NOT trigger speech — `listening` cancels va_followup — so it can
  // never drop a valid command. Cheap no-op when the buffer was empty.
  this->clear_mic_tx_();
  if (this->ws_connected_ && this->ws_handle_ != nullptr) {
    const char msg[] = "{\"type\":\"flush\"}";
    if (!this->send_control_(msg, sizeof(msg) - 1, "flush"))
      return;
    ESP_LOGI(TAG, "follow-up window closed — sent flush (drop uncommitted mic audio)");
  }
}

bool VaClient::send_wake_() {
  // Tell the backend a fresh wake started (dangling-VAD guard, A). Until the
  // user actually speaks this turn, OpenAI's server VAD can still fire an
  // end-of-turn for a PREVIOUS utterance that never closed (the reply gated the
  // mic mid-sentence) — committing it auto-creates a garbage answer to an empty
  // turn. The backend uses this signal to suppress that stale thinking + cancel
  // the racing response. Sent on every start_session(); old backends ignore it.
  if (this->ws_connected_ && this->ws_handle_ != nullptr) {
    const char msg[] = "{\"type\":\"wake\"}";
    if (!this->send_control_(msg, sizeof(msg) - 1, "wake"))
      return false;
    ESP_LOGI(TAG, "wake — sent {\"type\":\"wake\"} (dangling-VAD guard)");
    return true;
  }
  return false;
}

void VaClient::fire_phase_led_(const std::string &phase) {
  // Drive the yaml on_phase automation (LED ring + voice_assistant_phase global)
  // from a device-side timer, not a server message. Marshalled via defer() so it
  // runs on the main loop even if called from another task.
  std::string phase_copy = phase;
  this->defer([this, phase_copy]() {
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

void VaClient::commit_followup_mic() {
  // Called from yaml's on_followup_opened automation once the chime has
  // finished playing AND the i2s tail has cleared (wait_until + delay).
  // If anything pre-empted us between trigger fire and here (a fresh
  // wake word, a Stop, send_interrupt, or a new turn starting) the
  // armed flag was cleared — silently no-op so we don't reopen the mic
  // out of nowhere.
  if (!this->followup_armed_) {
    ESP_LOGD(TAG, "commit_followup_mic: not armed, ignoring");
    return;
  }
  this->followup_armed_ = false;
  ESP_LOGI(TAG, "follow-up mic armed by yaml (window %u ms)",
           (unsigned) kRequestFollowUpMs);
  // Discard the pre-roll: the request_follow_up chime just played and leaked
  // into the ring; don't replay it to OpenAI. (Same "Au!" guard as the wake
  // path; consumed by the mic task on the next frame.)
  this->preroll_discard_pending_ = true;
  this->clear_mic_tx_();
  this->streaming_ = true;
  this->set_timeout("va_followup", kRequestFollowUpMs, [this]() {
    if (this->streaming_) {
      ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
      this->streaming_ = false;
      this->clear_mic_tx_();
      this->send_mic_flush_();  // drop any uncommitted partial utterance
    }
  });
}

void VaClient::send_interrupt() {
  // Best-effort cancel to the backend — ONLY if the socket is alive. The local
  // cleanup below must ALWAYS run: returning early on a dead socket (the old
  // behaviour) left streaming_ on, the PSRAM ring full and the follow-up timers
  // armed after a "stop" on a dead link, so the mic would resume streaming the
  // room the instant we reconnected.
  if (this->ws_connected_ && this->ws_handle_ != nullptr) {
    const char msg[] = "{\"type\":\"interrupt\"}";
    this->streaming_ = false;
    this->clear_mic_tx_();
    this->send_control_(msg, sizeof(msg) - 1, "interrupt");
  } else {
    ESP_LOGW(TAG, "send_interrupt: WS not connected — local cleanup only");
  }
  // Flush our PSRAM playback queue — what's already been pushed into the
  // resampler/mixer/leaf will still drain (~600 ms residual), but everything
  // we have yet to hand off is dropped. The yaml side stops the resampler
  // explicitly. Reset deferred state too so we don't accidentally hold an
  // "idle" emit waiting for the (now-empty) queue. The ring reset has to
  // happen under the mux: the WS task could be mid-write and seeing
  // head=tail=fill=0 partway through would let it write into a "freshly
  // empty" buffer the user just barge-cancelled.
  portENTER_CRITICAL(&this->ring_mux_);
  this->audio_head_ = 0;
  this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  ++this->audio_ring_epoch_;
  ++this->pcm_flush_epoch_;
  portEXIT_CRITICAL(&this->ring_mux_);
  // Drop further incoming TTS until the backend confirms the turn boundary —
  // it keeps streaming the rest of the (already-generated) reply otherwise.
  this->suppress_incoming_audio_ = true;
  // Ring was just flushed; re-arm the jitter buffer fresh for the next reply.
  this->playback_priming_ = false;
  this->prime_armed_ = true;
  this->fade_out_pending_ = false;
  // Abandon any in-progress cold-start silence-prime; the next reply will detect
  // cold and re-prime cleanly.
  this->chain_prime_remaining_ = 0;
  // Close the mic gate. An interrupt during the OPEN follow-up window would
  // otherwise leave streaming_ true while the va_followup close-timer gets
  // cancelled just below — mic open + streaming to OpenAI indefinitely, so any
  // later room speech becomes an unprompted turn. Callers that start a fresh
  // turn (start_session) re-open it themselves right after.
  this->streaming_ = false;
  this->clear_mic_tx_();
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->cancel_timeout("va_no_speech");
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_tts_tail");
  // The phase=idle the server is about to send shouldn't open a follow-up
  // mic window — the user said "stop", not "wait for me to keep talking".
  this->suppress_followup_ = true;
  // Mic gate is now closed: no new turn can begin until a wake. Ignore any
  // `thinking` the backend emits in the meantime — it's the server VAD's
  // end-of-turn for the utterance we just cancelled, not a real new turn.
  // Cleared in start_session() (the next wake). See set_phase_.
  this->post_stop_guard_ = true;
  this->cancel_timeout("va_followup_open");
  ESP_LOGI(TAG, "send_interrupt — WS msg sent, queue flushed");
}

}  // namespace va_client
}  // namespace esphome
