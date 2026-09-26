#pragma once

#include "esphome/core/component.h"
#include "esphome/core/static_task.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "mic_tx_ring.h"
#include "va_timers.h"
#include "ws_frame_assembler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome {
namespace va_client {

class OnPhaseTrigger;
class OnRepeatedFailureTrigger;
class OnFollowupOpenedTrigger;
class OnTimerStartedTrigger;
class OnTimerFinishedTrigger;
class OnTimerCancelledTrigger;
class OnTimerTickTrigger;

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_mic_channel(uint8_t c) { mic_channel_ = c & 0x1; }
  // Digital gain applied to the outgoing mic stream (saturating), for XMOS
  // builds whose processed channel is too quiet for OpenAI's VAD.
  void set_mic_gain(float g) {
    if (!(g >= 0.25f)) g = 0.25f;
    if (g > 16.0f) g = 16.0f;
    mic_gain_q8_ = static_cast<int32_t>(g * 256.0f + 0.5f);
  }
  float get_mic_gain() const { return mic_gain_q8_.load() / 256.0f; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
  // Requests handsfree barge-in when the server also advertises support.
  // When both are true the mic keeps streaming through the
  // `thinking`/`replying` phases instead of being gated off, so the backend's
  // server VAD can hear the user talk over the assistant. Mic TX pauses at the
  // start of reply playback for the configured holdoff. A server-confirmed
  // barge-in fades out the remaining PSRAM audio. When false the firmware
  // keeps the original turn-based behaviour (mic off while the assistant
  // speaks). Relies on the XMOS AEC to suppress speaker→mic echo; see the
  // ~10x leak caveat in CLAUDE.md. Safe to change from a HA switch at runtime.
  void set_barge_in(bool v);
  void set_barge_in_holdoff_ms(uint32_t ms);
  bool is_server_barge_in_enabled() const { return this->server_barge_in_.load(); }
  // Sets the output-volume multiplier applied to TTS in handle_binary_.
  // Driven from yaml by external_media_player's volume / mute state so the
  // device's physical +/- buttons and mute switch scale our TTS the same
  // way they scale chime announcements played through media_player. (No
  // HA api: block on this firmware — there's no remote slider.) Range
  // [0, 1]; values are clamped on read so callers don't have to bounds-check.
  void set_volume(float v) { volume_ = v; }
  void add_on_phase_trigger(OnPhaseTrigger *t) { phase_triggers_.push_back(t); }
  void add_on_repeated_failure_trigger(OnRepeatedFailureTrigger *t) {
    repeated_failure_triggers_.push_back(t);
  }
  void add_on_followup_opened_trigger(OnFollowupOpenedTrigger *t) {
    followup_opened_triggers_.push_back(t);
  }
  void add_on_timer_started_trigger(OnTimerStartedTrigger *t) { timer_started_triggers_.push_back(t); }
  void add_on_timer_finished_trigger(OnTimerFinishedTrigger *t) { timer_finished_triggers_.push_back(t); }
  void add_on_timer_cancelled_trigger(OnTimerCancelledTrigger *t) { timer_cancelled_triggers_.push_back(t); }
  void add_on_timer_tick_trigger(OnTimerTickTrigger *t) { timer_tick_triggers_.push_back(t); }

  bool has_active_timers() const { return timers_.has_active(millis()); }
  bool has_ringing_timers() const { return timers_.has_ringing(); }
  TimerInfo first_active_timer() const { return timers_.first_active(millis()); }
  void stop_ringing();

  bool is_connected() const { return ws_connected_.load(); }
  // False if setup could not allocate playback buffers; safe for YAML
  // availability checks even while the WebSocket is disconnected.
  bool is_audio_ready() const {
    return this->audio_buf_ != nullptr && this->audio_drain_buf_ != nullptr && !this->is_failed();
  }

  // Delay (ms) the yaml wake handler waits after the wake chime before opening
  // the mic, so the chime's i2s/DAC tail can't leak into the fresh mic and
  // become a ghost turn. Pushed from the backend `hello` (wake_open_delay_ms);
  // the wake automation reads it via `- delay: !lambda`. Defaults to the safe
  // kWakeOpenDelayMs so an old backend (no key) still behaves sensibly.
  uint32_t get_wake_open_delay_ms() const { return wake_open_delay_ms_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // YAML-callable actions.
  void start_session();
  void send_interrupt();
  // Called from yaml's on_followup_opened automation AFTER the chime has
  // finished announcing through the speaker (wait_until !is_announcing +
  // i2s tail). Opens the mic for kRequestFollowUpMs. No-op if the device
  // is no longer armed (e.g. user already pressed wake before the chime
  // finished — the new session takes priority).
  void commit_followup_mic();
  // True once this turn has produced audible reply audio (the first
  // non-suppressed binary chunk set turn_t_first_audio_out_; reset to 0 on
  // every start_session()). The yaml "stop" handler uses this to ignore a
  // detection while the user is still asking / the model is still thinking —
  // there is nothing to stop yet, and a false "stop" on the user's own speech
  // would otherwise corrupt the turn (no follow-up window). Reply/follow-up/
  // request-follow-up windows all have it set, so a real barge-in still works.
  bool turn_has_reply_audio() const { return this->turn_t_first_audio_out_.load() != 0; }

  // Called from the static esp-idf event handler trampoline.
  void on_ws_event(int32_t event_id, void *event_data);

 protected:
  void connect_();
  void schedule_reconnect_();
  void on_mic_data_(const std::vector<uint8_t> &samples);
  // Tell the backend to drop any uncommitted mic audio NOW. Sent when the mic
  // gate closes mid-stream by TIMER (a follow-up window expiring) — a partial
  // utterance left in OpenAI's input buffer would otherwise be "completed" by a
  // later wake and answered as a stale half-sentence. Clearing at the cut-off
  // source means no reactive clear-on-wake is needed (that one disturbed the
  // server VAD and caused spurious garbage commits). No-op if nothing buffered.
  void send_mic_flush_();
  // Tell the backend a fresh wake started ({"type":"wake"}), for the
  // dangling-VAD guard: a server-VAD end-of-turn before the user speaks is a
  // stale pre-wake segment → backend suppresses its thinking + cancels its
  // garbage response. Sent on every start_session(); old backends ignore it.
  bool send_wake_();
  bool send_control_(const char *message, size_t length, const char *operation,
                     bool start_marker = false);
  void fail_control_channel_(const char *operation);
  bool effective_barge_in_() const {
    return this->barge_in_.load() && this->server_barge_in_.load();
  }
  bool barge_in_mic_allowed_(uint32_t now_ms) const;
  void reset_connection_state_();
  void clear_mic_tx_();
  static void mic_sender_task_trampoline_(void *arg);
  void mic_sender_loop_();
  static void ws_teardown_task_trampoline_(void *arg);
  void ws_teardown_loop_();
  void request_ws_teardown_();
  bool ws_idle_() const;
  void log_mic_health_();
  void fade_ring_tail_();
  size_t audio_fill_snapshot_();
  // Mic pre-roll helper (mic-task only, no lock). push appends to the rolling
  // ring while the session is closed; the ring is DISCARDED (not replayed) on
  // session open — see preroll_discard_pending_.
  void preroll_push_(const int16_t *data, size_t n);
  void handle_text_(const char *data, size_t len);
  void handle_timer_command_(const TimerCommand &command);
  void send_timer_state_();
  void send_timer_ack_(const std::string &request_id, VaTimers::Result result);
  void handle_binary_(const uint8_t *data, size_t len);
  void set_phase_(const std::string &phase);
  // Marshal a phase-LED trigger fire onto the main loop (used to drive the LED
  // ring to `listening`/`idle` from timer callbacks during the follow-up window,
  // independently of a server-sent phase).
  void fire_phase_led_(const std::string &phase);
  void open_followup_window_(uint32_t duration_ms);

  std::string url_;
  std::atomic<uint8_t> mic_channel_{0};
  std::atomic<int32_t> mic_gain_q8_{256};

  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  // esp_websocket_client_handle_t kept opaque to avoid leaking esp-idf into the header.
  void *ws_handle_{nullptr};
  std::atomic<bool> ws_connected_{false};
  std::atomic<bool> streaming_{false};
  std::atomic<bool> session_starting_{false};
  std::atomic<bool> control_channel_dirty_{false};
  std::atomic<bool> reconnect_requested_{false};
  std::atomic<bool> ws_task_finished_{true};
  std::atomic<bool> ws_stop_completed_{true};
  std::atomic<bool> ws_destroy_requested_{false};
  std::atomic<uint32_t> ws_teardown_requests_{0};
  std::atomic<uint32_t> ws_teardown_done_{0};
  bool reconnect_start_pending_{false};
  TaskHandle_t ws_teardown_task_{nullptr};
  StaticTask ws_teardown_static_task_;
  std::atomic<uint32_t> connection_epoch_{0};
  static constexpr uint32_t kWsControlSendTimeoutMs = 250;
  static constexpr size_t kMicTxBufferBytes = 64 * 1024;
  static constexpr size_t kMicTxPacketBytes = 640;  // 20 ms of mono PCM16 at 16 kHz
  uint8_t *mic_tx_storage_{nullptr};
  MicTxRing mic_tx_ring_;
  portMUX_TYPE mic_tx_mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<uint32_t> mic_tx_generation_{0};
  SemaphoreHandle_t ws_send_mutex_{nullptr};
  TaskHandle_t mic_sender_task_{nullptr};
  StaticTask mic_sender_static_task_;
  std::atomic<uint32_t> mic_callback_count_{0};
  std::atomic<uint32_t> mic_callback_max_us_{0};
  std::atomic<uint32_t> last_mic_callback_ms_{0};
  std::atomic<uint32_t> mic_signal_peak_{0};
  std::atomic<uint32_t> mic_zero_frame_count_{0};
  std::atomic<uint32_t> mic_tx_dropped_frames_{0};
  std::atomic<uint32_t> mic_tx_send_failures_{0};
  uint32_t last_mic_health_ms_{0};

  uint32_t reconnect_delay_ms_{1000};
  // Set when a reconnect timer is in flight. esp_websocket_client emits both
  // DISCONNECTED and CLOSED (and sometimes ERROR) per failure; without this
  // guard we'd double-bump the backoff delay and double-log.
  std::atomic<bool> reconnect_pending_{false};

  // Server-driven phase, stored as an atomic enum. set_phase_ WRITES it on
  // the WS task while start_session() (main loop) READS it for the residual-
  // reply check — as a std::string that was a cross-task data race (benign in
  // practice thanks to SSO, but UB). The yaml trigger path still receives the
  // phase as a string parameter; only this cross-task state is an enum.
  enum class Phase : uint8_t { IDLE = 0, LISTENING, THINKING, REPLYING };
  static Phase phase_from_string_(const std::string &phase);
  static const char *phase_name_(Phase p);
  std::atomic<uint8_t> current_phase_{static_cast<uint8_t>(Phase::IDLE)};
  std::vector<OnPhaseTrigger *> phase_triggers_;
  std::vector<OnRepeatedFailureTrigger *> repeated_failure_triggers_;
  std::vector<OnFollowupOpenedTrigger *> followup_opened_triggers_;
  std::vector<OnTimerStartedTrigger *> timer_started_triggers_;
  std::vector<OnTimerFinishedTrigger *> timer_finished_triggers_;
  std::vector<OnTimerCancelledTrigger *> timer_cancelled_triggers_;
  std::vector<OnTimerTickTrigger *> timer_tick_triggers_;
  // Main-loop owned: WS callbacks enqueue commands with defer(), never touch timers_.
  VaTimers timers_;
  uint32_t last_timer_tick_ms_{0};

  // Counts consecutive failed reconnect attempts. Reset to 0 on a clean
  // WS_CONNECTED event. When it hits kRepeatedFailureThreshold we fire the
  // on_repeated_failure trigger exactly once (until the count resets) — yaml
  // plays an audible error chime so the user knows the link is dead.
  uint32_t consecutive_failures_{0};
  bool repeated_failure_fired_{false};
  static constexpr uint32_t kRepeatedFailureThreshold = 5;
  // Don't reset the failure counter/flag the moment WS reconnects — a
  // flapping link (connect → 2 s later disconnect → 5 more fails → another
  // chime) would spam the user. Require kStableConnectionMs of unbroken
  // uptime before declaring "we're properly back" and re-arming the chime.
  static constexpr uint32_t kStableConnectionMs = 30000;

  // Scratch buffers reused on the hot paths to avoid per-callback heap
  // allocation. Each is OWNED BY ONE TASK — never share them across tasks:
  //   mono_buf_  — mic task only (on_mic_data_): i2s stereo-int32 → mono-int16.
  //   tts_buf_   — WS task only (handle_binary_): TTS volume-scaling scratch.
  // They used to be one shared vector; the mic task kept refilling it while
  // the WS task scaled TTS through it → cross-task realloc/write races that
  // dropped garbage samples into the playback ring (audible as hiss).
  std::vector<int16_t> mono_buf_;
  std::vector<int16_t> tts_buf_;

  // Mic pre-roll ring (int16 mono @ kMicSampleRate), allocated in PSRAM. The
  // rolling ring continuously retains the most recent kPreRollMs of mic audio
  // while the session is closed. We DO NOT replay it on session open: during
  // the wake-chime + tail-delay window the ring inevitably captures the chime
  // leaking through the mic (XMOS AEC leaves ~10x), and replaying it fed the
  // chime back to OpenAI as a phantom utterance ("Au!"). Instead, on session
  // open we DISCARD the ring (preroll_discard_pending_), matching marcinnowak79
  // gemini_proxy's ring_buffer_->reset() on start. Trade-off: a word spoken
  // *during* the chime is lost; the user speaks once the listening ring lights.
  // Touched ONLY by the mic task (on_mic_data_) — no lock needed.
  // preroll_discard_pending_ is set by start_session()/commit_followup_mic()
  // (main loop) and consumed by the mic task atomically.
  static constexpr uint32_t kMicSampleRate = 16000;  // i2s_mics rate (16 samples/ms)
  static constexpr uint32_t kPreRollMs = 600;
  int16_t *preroll_buf_{nullptr};
  size_t preroll_capacity_samples_{0};
  size_t preroll_head_{0};   // next write index
  size_t preroll_count_{0};  // valid samples (<= capacity)
  std::atomic<bool> preroll_discard_pending_{false};

  // Streaming gate (streaming_ above). True while the mic should be forwarded:
  //   - between wake-word start_session() and "listening"/"thinking"
  //   - and again after "idle" for kFollowupMs (in case AI asked a question)
  // Handsfree barge-in toggle, set from yaml (`barge_in:`). See set_barge_in().
  std::atomic<bool> barge_in_{false};
  std::atomic<bool> server_barge_in_{false};
  std::atomic<uint32_t> barge_in_holdoff_ms_{400};
  // Zero until the first real (not silence-prime) PCM is fed in this reply.
  std::atomic<uint32_t> reply_playback_started_ms_{0};
  std::atomic<bool> drop_until_next_turn_{false};
  std::atomic<bool> barge_in_trim_pending_{false};  // set by WS task, applied in loop()
  std::atomic<uint32_t> barge_in_dropped_bytes_{0};
  std::atomic<bool> server_hello_received_{false};
  std::atomic<bool> server_barge_warning_logged_{false};
  // Set on phase=idle when there's still TTS audio queued — we can't open
  // the mic until the speaker drains, otherwise it picks up its own output.
  // loop() flips this to a live followup window once audio_fill_ hits 0.
  std::atomic<bool> followup_pending_{false};
  // Tracks whether the pending follow-up was requested by the server's
  // request_follow_up tool (model asked a question) vs the natural
  // post-reply path. The former wants a longer mic window
  // (kRequestFollowUpMs); the latter uses kFollowupMs (which is 0 by
  // default — no auto-follow-up).
  std::atomic<bool> request_follow_up_pending_{false};
  // Set when on_followup_opened has fired and we're waiting on yaml to
  // play the chime + call commit_followup_mic(). Cleared on commit or
  // when a new session preempts. Without this flag a stale
  // commit_followup_mic() call (e.g. delayed lambda after a `Stop` wake
  // word already reset state) would re-open the mic out of nowhere.
  std::atomic<bool> followup_armed_{false};
  // Server sends phase=idle when OpenAI is done generating, but we still
  // have audio queued in PSRAM + downstream rings. If we fire the LED
  // trigger immediately the device looks idle while still speaking. Hold
  // the "idle" emission until the queue drains + kFollowupOpenDelayMs.
  std::atomic<bool> idle_emit_pending_{false};
  // Set by send_interrupt() so the phase=idle that follows from the server
  // doesn't trigger a follow-up mic window. The user explicitly asked us to
  // stop — they don't want the device sitting there listening.
  std::atomic<bool> suppress_followup_{false};
  // Set by send_interrupt() ("stop" word / barge-in). OpenAI bursts the whole
  // reply faster than real-time, so by the time the user says "stop" the audio
  // is already buffered (backend + our PSRAM) and the backend keeps streaming
  // the rest. Flushing our queue isn't enough — handle_binary_ just refills it
  // from the in-flight frames. While this is true we DROP incoming audio so the
  // cancelled reply actually goes silent. Cleared in set_phase_ only on
  // "listening" (a fresh turn's audio is legitimate).
  std::atomic<bool> suppress_incoming_audio_{false};
  // Set by send_interrupt() (a local "stop"), cleared in start_session() (the
  // next wake). After a stop the mic gate is CLOSED, so no new turn can begin
  // until a wake — yet OpenAI's server VAD can still fire an end-of-turn for
  // the utterance we just cancelled, which the backend turns into a `thinking`
  // phase. Acting on that strands the LED in `thinking` until the backend's
  // 15 s watchdog (observed live 2026-06-14: "stop" mid-question -> 15 s stuck
  // thinking). While this is true, set_phase_ IGNORES `thinking` — it can only
  // be the stale tail of the cancelled turn (a legitimate `thinking` is always
  // preceded by a wake, which clears this). Scoped to `thinking` only: a web
  // search's replying->thinking has no stop so this stays false (don't break
  // the search animation), and a reply-drain emits no `thinking` (mic gated).
  std::atomic<bool> post_stop_guard_{false};
  // Live duration (ms) of the post-reply follow-up window: how long the mic
  // stays open after the assistant finishes so the user can answer back
  // WITHOUT re-saying the wake word. Pushed from the backend add-on in the
  // `hello` handshake (`"follow_up_ms":N`) so it's tunable from the add-on
  // config without reflashing; 0 = disabled (turn-based, mic closes after each
  // reply). Clamped to kFollowupMsMax on parse. The window only opens AFTER the
  // speaker chain drains + kFollowupOpenDelayMs so the assistant's own TTS tail
  // can't leak into the open mic (XMOS AEC ~10x leak).
  std::atomic<uint32_t> followup_ms_{0};
  static constexpr uint32_t kFollowupMsMax = 60000;
  // Live delay (ms) between the speaker draining and the follow-up mic opening,
  // also pushed from the backend `hello` ("follow_up_open_delay_ms":N). Covers
  // the i2s ring + DAC tail that plays out after has_buffered_data() goes false,
  // so the mic doesn't catch the reply's own tail. Defaults to the conservative
  // kFollowupOpenDelayMs but is tunable from the add-on (lower = snappier, risk
  // of hearing the tail; higher = safer). Clamped to kFollowupOpenDelayMaxMs.
  std::atomic<uint32_t> followup_open_delay_ms_{kFollowupOpenDelayMs};
  static constexpr uint32_t kFollowupOpenDelayMaxMs = 5000;
  // Wake-chime echo guard: delay (ms) the yaml wake handler waits after the
  // wake chime before opening the mic. The wake-path twin of
  // followup_open_delay_ms_ (the follow-up boundary). Pushed from the backend
  // `hello` ("wake_open_delay_ms":N); read by yaml via get_wake_open_delay_ms().
  // Default 700 matches the backend default so a device on an old backend (no
  // key in hello) still gets the safe value rather than the old hardcoded 400.
  static constexpr uint32_t kWakeOpenDelayMs = 700;
  std::atomic<uint32_t> wake_open_delay_ms_{kWakeOpenDelayMs};
  // Playback jitter buffer ("prebuffer"). Before starting/resuming playback we
  // hold audio in the PSRAM ring until at least this many ms have accumulated
  // (or a short deadline elapses), so the downstream resampler/mixer/i2s chain
  // starts with a cushion and a network jitter gap (we see 100-340 ms gaps)
  // doesn't dry it out → audible crackle. Pushed from the backend `hello`
  // ("playback_prebuffer_ms":N) so it's tunable without reflashing; clamped to
  // kPlaybackPrebufferMaxMs. 0 = disabled (play immediately, old behaviour).
  // Primes once per reply start (see prime_armed_), never mid-reply.
  std::atomic<uint32_t> playback_prebuffer_ms_{0};
  // Allows at most one prebuffer hold per empty ring. Mid-reply the PSRAM ring
  // and resampler routinely run near-empty while the mixer/i2s still play, so
  // re-priming on "ring empty + chain dry" held playback repeatedly and made
  // the reply choppy. Armed at session/turn boundaries (wake, listening,
  // thinking, audio_done, idle) and ring flushes; consumed by the next PCM.
  std::atomic<bool> prime_armed_{true};
  static constexpr uint32_t kPlaybackPrebufferMaxMs = 2000;
  static constexpr uint32_t kPlaybackSampleRate = 24000;  // incoming TTS PCM rate
  // True while we're accumulating the prebuffer cushion (holding playback).
  // Touched by handle_binary_ (WS task, arms it) + loop() (main task, releases);
  // Shared with the WS task; stored atomically.
  std::atomic<bool> playback_priming_{false};
  std::atomic<bool> fade_in_pending_{true};
  std::atomic<bool> fade_out_pending_{false};
  // Also set by audio_done (before the debounced idle), when the downstream
  // chain runs dry with only the fade window held back, or when a replying ->
  // thinking transition ends a reply segment.
  std::atomic<bool> reply_audio_done_{false};
  std::atomic<bool> thinking_tail_pending_{false};
  // millis() when priming started (first byte after the ring was empty); used
  // for the prime deadline so real-time (non-burst) audio still starts promptly.
  std::atomic<uint32_t> prime_started_ms_{0};

  // Resampler cold-start SILENCE-PRIME (crackle fix). The resampler does NOT
  // idle-timeout (verified vs ESPHome source): resample(stop_gracefully=false)
  // never returns FINISHED and its output mixer-source is timeout:never, so the
  // chain stays WARM between normal replies. It goes COLD only after an explicit
  // `speaker.stop: media_resampling_speaker` (yaml interrupt / "stop" / wake /
  // follow-up), which tears the task down (is_stopped()==true). The next reply
  // then cold-starts a fresh AudioResampler whose windowed-sinc FIR begins from a
  // zero state → a startup-transient click. A PSRAM prebuffer can't fix it (the
  // transient is downstream of the ring). Fix: when cold, feed kChainPrimeMs of
  // SILENCE first so the FIR settles to a clean zero output before real audio.
  // Cold = resampler is_stopped() (precise, true exactly post-speaker.stop) OR,
  // as a backup, nothing fed for > kChainColdMs. Both are only ever true at a
  // real cold reply-start, never mid-speech; a needless prime on a warm chain is
  // harmless (60ms silence).
  static constexpr uint32_t kChainPrimeMs = 60;   // silence burst to warm the filter
  static constexpr uint32_t kChainColdMs = 600;   // backup timer; is_stopped() is the primary signal
  // Bytes of silence still to feed this cold-start (24kHz mono 16-bit). >0 while
  // priming; loop() feeds silence and holds real-audio drain until it reaches 0.
  std::atomic<size_t> chain_prime_remaining_{0};
  // millis() of the last time we fed the resampler ANYTHING (silence or real).
  // Used to detect a cold chain: now - last_fed_ms_ > kChainColdMs. 0 = never fed.
  uint32_t last_fed_ms_{0};
  // Legacy compile-time default, kept for reference. The live value now comes
  // from the backend (followup_ms_); this stays 0 so a device talking to an
  // old backend that doesn't send follow_up_ms keeps the turn-based behaviour.
  static constexpr uint32_t kFollowupMs = 0;
  // Used when the server explicitly requests a follow-up via the
  // request_follow_up tool — overrides kFollowupMs for a single turn.
  // Longer than the default because the model asked a real question
  // and the user might pause before answering.
  static constexpr uint32_t kRequestFollowUpMs = 10000;
  // After start_session() we wait this long for the server to emit
  // phase=listening (i.e. server VAD heard speech). If nothing comes, the
  // user pressed wake/button and stayed silent — close the session so we
  // don't sit there with the mic open eating OpenAI minutes.
  static constexpr uint32_t kNoSpeechTimeoutMs = 7000;
  // After our PSRAM queue drains there's still audio in flight:
  //   resampler ring 4800 B (≈ 50 ms)
  //   mixer source buffer 100 ms
  //   i2s_audio_speaker buffer_duration 500 ms
  //   XMOS DSP pipeline / DAC analog tail ≈ 100 ms
  // Sum ≈ 750 ms. We add headroom so a slow drain doesn't leak TTS into
  // the mic — every leak triggers the server VAD because the XMOS AEC
  // doesn't fully cancel our own speaker output (M3.2 measured ~10× leak).
  static constexpr uint32_t kFollowupOpenDelayMs = 1500;
  // Hard ceiling on how long we'll wait for the speaker chain to drain
  // (resampler ring + mixer source ring, via has_buffered_data()) after
  // PSRAM hits 0 before giving up and proceeding anyway. Should be >
  // the worst-case downstream buffer (resampler + mixer source ~150 ms,
  // plus play-out time of whatever was in flight) by a comfortable
  // margin, but short enough that a wedged speaker doesn't lock the
  // LED in `replying` forever.
  static constexpr uint32_t kSpeakerStopTimeoutMs = 3000;

  // True while we're waiting for the downstream speaker chain to actually
  // finish playing the TTS we wrote into it. Entered when audio_fill_
  // hits 0 with followup_pending_ set; exited when
  // !speaker_->has_buffered_data() OR kSpeakerStopTimeoutMs elapses.
  std::atomic<bool> waiting_for_speaker_stop_{false};
  // millis() snapshot from when waiting_for_speaker_stop_ went true.
  // Used to fire the fallback timeout if the chain never drains.
  uint32_t speaker_stop_wait_started_ms_{0};

  // WS event task owns both assemblers; main-task flushes invalidate the
  // PCM assembler through audio_ring_epoch_ before the next data event.
  bool last_data_was_binary_{false};
  WsTextAssembler text_assembler_;
  Pcm16FrameAssembler pcm_assembler_;
  std::vector<uint8_t> pcm_chunk_;
  uint32_t pcm_epoch_{0};
  uint32_t pcm_flush_epoch_{0};  // guarded by ring_mux_
  uint32_t frame_connection_epoch_{0};

  // Output volume multiplier in [0, 1], updated from yaml whenever
  // external_media_player.volume / mute changes. Defaults to 1.0 so a
  // stand-alone va_client (no media_player wiring) still plays audibly.
  std::atomic<float> volume_{1.0f};

  // Ring buffer for pending TTS audio, allocated in PSRAM. The server can
  // burst the entire response in ~200 ms; we buffer here and drain into
  // speaker.play() from loop() to keep playback smooth.
  //
  // 2 MB / (24000 Hz × 2 B) ≈ 43 s of headroom. A 30 s monologue arriving
  // in ~1 s would peak at ~1.4 MB; this size gives ~40 % overhead on top.
  // PSRAM is 8 MB on the Voice PE so cost is negligible.
  uint8_t *audio_buf_{nullptr};
  uint8_t *audio_drain_buf_{nullptr};
  static constexpr size_t kAudioDrainChunkBytes = 2048;
  static constexpr size_t kAudioBufBytes = 2 * 1024 * 1024;
  size_t audio_head_{0};  // read pos (next byte to play)
  size_t audio_tail_{0};  // write pos (next byte to fill)
  size_t audio_fill_{0};  // bytes currently queued (audio_tail_ ≥ audio_head_ when not wrapped)
  uint32_t audio_ring_epoch_{0};  // incremented whenever a flush invalidates a drain snapshot
  // Ring position of the first PCM from this reply, so a previous tool-call
  // reply's still-draining tail cannot start the new reply's mic holdoff.
  bool reply_pcm_marker_pending_{false};  // guarded by ring_mux_
  bool reply_pcm_marker_valid_{false};    // guarded by ring_mux_
  size_t reply_pcm_marker_{0};           // guarded by ring_mux_
  uint32_t reply_generation_{0};         // guarded by ring_mux_
  // ESP32-S3 is dual-core: handle_binary_ runs in the esp-idf
  // websocket task (background) while loop() runs in the main app task,
  // typically on the other core. Both touch audio_head_/tail_/fill_
  // and the PSRAM data they index; without sync we hit races where
  // fill_ goes inconsistent, the reader sees the new tail before the
  // memcpy is visible, or two non-atomic increments lose each other.
  // The DAC then plays whatever bytes happened to be at those PSRAM
  // offsets — audible as "speech drops into hiss" mid-utterance.
  // portMUX is the cheapest cross-core primitive: a spinlock that
  // also masks interrupts on the holding core. Critical sections
  // are tiny (a few field updates + ≤2 memcpys of at most a few KB
  // per WS frame), so contention is negligible.
  portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;
  static constexpr size_t kFadeSamples = kPlaybackSampleRate / 100;  // 10 ms

  // Per-turn latency anchors (millis()-relative). Captured at each state
  // transition; flushed as one summary line when the deferred phase=idle
  // emit fires (i.e. when the speaker has actually drained). Zero means
  // "not yet hit this milestone this turn".
  std::atomic<uint32_t> turn_t_wake_{0};               // start_session() (wake-word handler)
  std::atomic<uint32_t> turn_t_listening_{0};          // server's first phase=listening
  std::atomic<uint32_t> turn_t_thinking_{0};           // server's phase=thinking (end-of-speech)
  std::atomic<uint32_t> turn_t_first_audio_out_{0};    // first binary chunk arrived from server

  // Diagnostics for the "speech sometimes drops into hiss / noise"
  // symptom. We don't know the cause yet, so we measure three things
  // simultaneously and let the logs tell us which one (if any) fires
  // during a bad reply.
  //
  //  1) WS frame inter-arrival time. If the bridge stalls and audio
  //     arrives in bursts with > kWsGapWarnMs silence between, the
  //     downstream chain may underrun and inject silence/noise. We log
  //     each large gap with the duration and how full the PSRAM ring
  //     was at the time.
  //  2) TTS clipping. Software gain is disabled, so clipping is
  //     mathematically impossible while volume_ ≤ 1. Counter stays as
  //     a tripwire — if anyone reintroduces a >1 scale factor, the
  //     per-turn summary will surface it before users hear the rasp.
  //  3) Downstream underrun. speaker_->has_buffered_data() = false
  //     while audio_fill_ > 0 means the resampler/mixer/i2s chain ran
  //     dry while we still had PSRAM to feed it — bug or stall in the
  //     downstream side. We log the first underrun per reply.
  std::atomic<uint32_t> last_binary_ms_{0};
  std::atomic<uint32_t> ws_gap_count_{0};       // # gaps > kWsGapWarnMs in this turn
  std::atomic<uint32_t> ws_gap_max_ms_{0};      // largest gap observed this turn
  std::atomic<uint32_t> clipped_samples_{0};    // clipped samples in this turn
  std::atomic<bool> underrun_logged_this_turn_{false};
  static constexpr uint32_t kWsGapWarnMs = 80;  // > ~3× normal 20 ms frame
  // Fade the held tail out once the downstream estimate drops below this and
  // no audio has arrived for kStarveGapMs (about one frame).
  static constexpr uint32_t kStarveGuardMs = 40;
  static constexpr uint32_t kStarveGapMs = 20;
  PlaybackClock playback_clock_;  // loop() only
  // Click diagnostics (loop() only).
  void note_playback_edges_(const int16_t *samples, size_t count);
  static constexpr int kEdgeQuietLevel = 256;
  static constexpr int kEdgeStepLevel = 12000;
  int16_t edge_last_sample_{0};
  bool edge_dry_{true};
  std::atomic<uint32_t> downstream_queued_ms_{0};  // snapshot for WS-task logs
  // Resume fade-in progress in samples (>= kFadeSamples: none). WS task, under ring_mux_.
  size_t resume_fade_pos_{kFadeSamples};
};

}  // namespace va_client
}  // namespace esphome
