#pragma once

// ESPHome glue for the lanicom core (lanicom/core): UDP socket, Opus, microphone and speaker.
//
// Threads:
//   net task   - recvfrom + lc_engine_receive/tick (engine_mutex_)
//   audio task - every 10 ms: encode mic frames and send; mix + decode received streams to the
//                speaker. All Opus calls happen here (micro-opus' pseudostack isn't thread-safe).
//   main loop  - I2S bus ownership (mic XOR speaker share one bus), settings, triggers.

#include <atomic>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

extern "C" {
#include "lanicom/lanicom.h"
}

struct OpusEncoder;
struct OpusDecoder;

namespace esphome::lanicom {

class LanicomComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // Configuration (from YAML).
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_key(const std::string &key) { this->key_string_ = key; }
  void set_device_name(const std::string &name) { this->name_ = name; }
#ifdef USE_TEXT
  void set_key_text(text::Text *t) { this->key_text_ = t; }
  void set_name_text(text::Text *t) { this->name_text_ = t; }
#endif
  void set_port(uint16_t port) { this->port_ = port; }
  void set_multicast(bool m) { this->multicast_ = m; }
  void add_static_peer(const std::string &ip) { this->static_peers_.push_back(ip); }
  void set_jitter(uint32_t target_ms, uint32_t max_ms) {
    this->jitter_target_ms_ = target_ms;
    this->jitter_max_ms_ = max_ms;
  }
  void set_frame_ms(uint32_t ms) { this->frame_ms_ = ms; }
  void set_bitrate(int32_t b) { this->bitrate_ = b; }
  void set_complexity(int32_t c) { this->complexity_ = c; }
  void set_mic_warmup_ms(uint32_t ms) { this->mic_warmup_ms_ = ms; }
  void set_speaker_hold_ms(uint32_t ms) { this->speaker_hold_ms_ = ms; }
  void set_task_core(int core) { this->task_core_ = core; }
  // Buttons (1-4): the button's binary sensor and its ring (optional).
  void add_button(binary_sensor::BinarySensor *sensor, output::FloatOutput *ring) {
    this->button_sensors_.push_back(sensor);
    this->button_rings_.push_back(ring);
  }
  // Steady brightness of linked rings (0..1): all at once, or one button (0-based).
  void set_ring_brightness(float level) {
    for (float &l : this->ring_idle_)
      l = level;
  }
  void set_ring_brightness(size_t button, float level) {
    if (button < LC_MAX_BUTTONS)
      this->ring_idle_[button] = level;
  }
  // Night: rings use night_brightness instead, between these hours (needs a clock).
  void set_night_brightness(float level) { this->night_idle_ = level; }
  void set_night_hours(int start, int end) {
    this->night_start_ = start;
    this->night_end_ = end;
  }
  void set_night_start(int hour) { this->night_start_ = hour; }
  void set_night_end(int hour) { this->night_end_ = hour; }
#ifdef USE_TIME
  void set_clock(time::RealTimeClock *clock) { this->clock_ = clock; }
#endif

  // Runtime API (main loop).
  void start_talking(const std::string &target);
  void stop_talking();
  // Buttons, for the web page / Home Assistant (button 0-based).
  size_t button_count() const { return this->button_sensors_.size(); }
  std::string partner_summary(size_t button);
  void unpair(size_t button);
  // Flash every ring (identify: 10 s; also the reset counter, briefly).
  void identify(uint32_t ms = 10000) { this->identify_until_ = millis() + ms; }
  float get_ring_brightness(size_t button) const { return button < LC_MAX_BUTTONS ? this->ring_idle_[button] : 0; }
  bool is_night();
  bool is_talking() const { return this->talk_requested_; }
  bool is_transmitting() const { return this->transmitting_.load(); }
  bool is_receiving() const { return this->receiving_.load(); }
  bool is_ready() const { return this->ready_.load(); }
  size_t peer_count() const { return this->peer_count_.load(); }
  uint32_t get_sender_id() const { return this->engine_.sender_id; }
  std::string peers_summary();
  lc_stats_t get_stats();

  Trigger<std::string> *get_receive_start_trigger() { return &this->receive_start_trigger_; }
  Trigger<> *get_receive_end_trigger() { return &this->receive_end_trigger_; }
  Trigger<std::string> *get_peer_added_trigger() { return &this->peer_added_trigger_; }
  Trigger<std::string> *get_peer_removed_trigger() { return &this->peer_removed_trigger_; }

 protected:
  enum class Bus : uint8_t { NONE, MIC, SPEAKER };
  enum class EventType : uint8_t { PEER_ADDED, PEER_REMOVED, RX_START, RX_END, SENDER_ID_CHANGED };
  struct Event {
    EventType type;
    char name[LC_NAME_MAX + 1];
  };

  static void net_task(void *arg);
  static void audio_task(void *arg);
  void net_run_();
  void audio_run_();
  bool open_socket_();
  bool restart_engine_();
  void manage_bus_();
  void apply_identity_();
  void post_event_(EventType type, const char *name);
  void buttons_loop_();
  void update_rings_();

  // lc_engine callbacks (run in the net or audio task, engine_mutex_ held).
  static void cb_send(void *ctx, lc_addr_t to, const uint8_t *data, size_t len);
  static uint32_t cb_random(void *ctx);
  static void cb_peer_added(void *ctx, const lc_peer_t *peer);
  static void cb_peer_removed(void *ctx, const lc_peer_t *peer);
  static void cb_audio(void *ctx, const lc_peer_t *peer, uint32_t stream_id, uint32_t ts, const uint8_t *opus,
                       size_t len);
  static void cb_talk_stop(void *ctx, const lc_peer_t *peer, uint32_t stream_id);
  static void cb_sender_id_changed(void *ctx, uint32_t sender_id);
  static void cb_hello(void *ctx, const lc_peer_t *peer, const lc_hello_t *hello);
  static void cb_control(void *ctx, const lc_peer_t *peer, const lc_control_t *msg);
  // lc_rx callbacks (rx_mutex_ held).
  static int cb_decode(void *ctx, int slot, lc_jb_kind_t kind, const uint8_t *data, size_t len, uint32_t samples,
                       int16_t *pcm);
  static void cb_stream_begin(void *ctx, int slot, uint32_t sender_id, uint32_t stream_id);
  static void cb_stream_end(void *ctx, int slot, uint32_t sender_id, uint32_t stream_id);

  // Config.
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  std::string key_string_, name_;
#ifdef USE_TEXT
  text::Text *key_text_{nullptr}, *name_text_{nullptr};
#endif
  uint16_t port_{LC_PORT};
  bool multicast_{false};
  std::vector<std::string> static_peers_;
  uint32_t jitter_target_ms_{20}, jitter_max_ms_{60};
  uint32_t frame_ms_{10};
  int32_t bitrate_{24000}, complexity_{3};
  uint32_t mic_warmup_ms_{50};
  uint32_t speaker_hold_ms_{3000};
  int task_core_{1};

  // Identity, persisted: sender id and a boot counter (high half of the epoch).
  struct Persisted {
    uint32_t sender_id;
    uint32_t boot_counter;
  } persisted_{};
  ESPPreferenceObject pref_;

  // Protocol state.
  lc_key_t key_{};
  lc_engine_t engine_{};
  lc_rx_t rx_{};
  lc_talk_t talk_{};
  SemaphoreHandle_t engine_mutex_{nullptr};
  SemaphoreHandle_t rx_mutex_{nullptr};
  // Held around "is the speaker ours?" + play(), and around stopping it: ESPHome's play()
  // restarts a stopped speaker, which would then hold the shared I2S bus forever.
  SemaphoreHandle_t speaker_mutex_{nullptr};
  QueueHandle_t events_{nullptr};
  TaskHandle_t net_task_handle_{nullptr}, audio_task_handle_{nullptr};
  int sock_{-1};
  std::atomic<bool> ready_{false}, network_up_{false}, key_dirty_{true}, identity_dirty_{true};
  std::atomic<bool> receiving_{false}, transmitting_{false};
  std::atomic<size_t> peer_count_{0};
  std::string pending_key_;  // guarded by engine_mutex_
  bool stream_began_{false};  // set by cb_stream_begin, read by cb_audio (both under the locks)

  // Talk state (main loop decides, audio task executes).
  bool talk_requested_{false};
  std::string talk_target_{"all"};
  std::vector<uint32_t> talk_devices_;  // set by the buttons (guarded by engine_mutex_); empty: talk_target_
  std::atomic<bool> talk_begin_{false}, talk_end_{false}, talk_update_{false};

  // Buttons and rings. buttons_ is guarded by engine_mutex_ (the net task feeds it peers' messages).
  std::vector<binary_sensor::BinarySensor *> button_sensors_;
  std::vector<output::FloatOutput *> button_rings_;
  lc_buttons_t buttons_{};
  struct PersistedLinks {
    lc_button_link_t links[LC_MAX_BUTTONS];
  } links_{};
  ESPPreferenceObject links_pref_;
  float ring_idle_[LC_MAX_BUTTONS]{0.1f, 0.1f, 0.1f, 0.1f};
  float night_idle_{0.02f};
  int night_start_{22}, night_end_{7};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif
  uint32_t identify_until_{0};
  uint32_t boot_ms_{0};  // start of the boot sweep
  std::atomic<uint32_t> rx_senders_[LC_RX_STREAMS]{};  // sender of each playing stream, 0: none

  // I2S bus (main loop only).
  Bus bus_{Bus::NONE};
  bool bus_releasing_{false};
  uint32_t speaker_hold_until_{0};
  std::atomic<bool> speaker_ready_{false}, mic_ready_{false};
  std::atomic<uint32_t> mic_discard_until_{0};
  HighFrequencyLoopRequester high_freq_;

  // Capture ring: mic callback (producer) -> audio task (consumer).
  static constexpr size_t CAPTURE_SAMPLES = 3200;  // 200 ms
  int16_t *capture_{nullptr};
  std::atomic<uint32_t> capture_write_{0}, capture_read_{0};

  // Opus (audio task only).
  OpusEncoder *encoder_{nullptr};
  OpusDecoder *decoders_[LC_RX_STREAMS]{};
  bool decoder_reset_[LC_RX_STREAMS]{};

  Trigger<std::string> receive_start_trigger_;
  Trigger<> receive_end_trigger_;
  Trigger<std::string> peer_added_trigger_, peer_removed_trigger_;
};

template<typename... Ts> class StartTalkingAction : public Action<Ts...>, public Parented<LanicomComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, target)
  void play(const Ts &...x) override {
    this->parent_->start_talking(this->target_.has_value() ? this->target_.value(x...) : std::string("all"));
  }
};

template<typename... Ts> class StopTalkingAction : public Action<Ts...>, public Parented<LanicomComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->stop_talking(); }
};

template<typename... Ts> class IsReceivingCondition : public Condition<Ts...>, public Parented<LanicomComponent> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_receiving(); }
};

template<typename... Ts> class IsTalkingCondition : public Condition<Ts...>, public Parented<LanicomComponent> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_talking(); }
};

}  // namespace esphome::lanicom
