#include "lanicom_component.h"

#include <cinttypes>
#include <cstring>

#include <esp_random.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <opus.h>

#include "esphome/components/audio/audio.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::lanicom {

static const char *const TAG = "lanicom";
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint32_t PLAYOUT_SAMPLES = SAMPLE_RATE / 100;  // 10 ms

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};

// --- setup / config ---------------------------------------------------------

void LanicomComponent::setup() {
  this->engine_mutex_ = xSemaphoreCreateMutex();
  this->rx_mutex_ = xSemaphoreCreateMutex();
  this->events_ = xQueueCreate(16, sizeof(Event));
  RAMAllocator<int16_t> alloc;
  this->capture_ = alloc.allocate(CAPTURE_SAMPLES);
  if (!this->engine_mutex_ || !this->rx_mutex_ || !this->events_ || !this->capture_) {
    ESP_LOGE(TAG, "Out of memory");
    this->mark_failed();
    return;
  }

  // Identity. The boot counter must reach flash before the first packet: it makes the
  // epoch (and so every nonce) unique even if the RNG were to repeat itself.
  this->pref_ = global_preferences->make_preference<Persisted>(fnv1_hash("lanicom_identity"), true);
  if (!this->pref_.load(&this->persisted_) || this->persisted_.sender_id == 0) {
    do {
      this->persisted_.sender_id = esp_random();
    } while (this->persisted_.sender_id == 0);
    this->persisted_.boot_counter = 0;
  }
  this->persisted_.boot_counter++;
  if (!this->pref_.save(&this->persisted_) || !global_preferences->sync())
    ESP_LOGW(TAG, "Could not persist the boot counter; epochs rely on the RNG alone");

  if (this->name_.empty())
    this->name_ = App.get_friendly_name().empty() ? App.get_name().str() : App.get_friendly_name().str();
  this->pending_key_ = this->key_string_;
#ifdef USE_TEXT
  if (this->key_text_ != nullptr) {
    if (!this->key_text_->state.empty())
      this->pending_key_ = this->key_text_->state;
    this->key_text_->add_on_state_callback([this](const std::string &value) {
      {
        Lock lock(this->engine_mutex_);
        this->pending_key_ = value.empty() ? this->key_string_ : value;
      }
      this->key_dirty_ = true;
    });
  }
  if (this->name_text_ != nullptr)
    this->name_text_->add_on_state_callback([this](const std::string &) { this->identity_dirty_ = true; });
  if (this->zones_text_ != nullptr)
    this->zones_text_->add_on_state_callback([this](const std::string &) { this->identity_dirty_ = true; });
#endif
  this->apply_identity_();

  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
      if (!this->mic_ready_ || millis() < this->mic_discard_until_)
        return;
      const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
      size_t n = data.size() / sizeof(int16_t);
      uint32_t w = this->capture_write_.load(std::memory_order_relaxed);
      uint32_t r = this->capture_read_.load(std::memory_order_acquire);
      size_t space = CAPTURE_SAMPLES - (w - r);
      if (n > space)
        n = space;  // overrun: the encoder fell behind; drop the newest samples
      for (size_t i = 0; i < n; i++)
        this->capture_[(w + i) % CAPTURE_SAMPLES] = samples[i];
      this->capture_write_.store(w + n, std::memory_order_release);
      if (this->audio_task_handle_ != nullptr)
        xTaskNotifyGive(this->audio_task_handle_);
    });
  }

  lc_rx_init(&this->rx_, this->jitter_target_ms_, this->jitter_max_ms_, cb_decode, cb_stream_begin, cb_stream_end,
             this);

  // Audio at high priority on its own core; networking at normal priority.
  if (xTaskCreatePinnedToCore(audio_task, "lanicom_audio", 16384, this, 18, &this->audio_task_handle_,
                              this->task_core_) != pdPASS ||
      xTaskCreate(net_task, "lanicom_net", 8192, this, 10, &this->net_task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Could not start tasks");
    this->mark_failed();
  }
}

void LanicomComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "lanicom:\n"
                "  Device id: %08" PRIx32 " (boot %" PRIu32 ")\n"
                "  Port: %u%s\n"
                "  Frames: %" PRIu32 " ms, %" PRId32 " bit/s, complexity %" PRId32 "\n"
                "  Jitter buffer: %" PRIu32 "-%" PRIu32 " ms",
                this->persisted_.sender_id, this->persisted_.boot_counter, this->port_,
                this->multicast_ ? ", multicast" : "", this->frame_ms_, this->bitrate_, this->complexity_,
                this->jitter_target_ms_, this->jitter_max_ms_);
  for (const auto &peer : this->static_peers_)
    ESP_LOGCONFIG(TAG, "  Static peer: %s", peer.c_str());
  if (this->pending_key_.empty())
    ESP_LOGW(TAG, "  No network key set");
}

void LanicomComponent::apply_identity_() {
  std::string name = this->name_, zones = this->zones_;
#ifdef USE_TEXT
  if (this->name_text_ != nullptr && !this->name_text_->state.empty())
    name = this->name_text_->state;
  if (this->zones_text_ != nullptr && this->zones_text_->has_state())
    zones = this->zones_text_->state;
#endif
  uint32_t caps = (this->speaker_ != nullptr ? LC_CAP_PLAYBACK : 0) | (this->mic_ != nullptr ? LC_CAP_CAPTURE : 0);
  Lock lock(this->engine_mutex_);
  this->name_ = name;
  this->zones_ = zones;
  if (this->ready_)
    lc_engine_set_identity(&this->engine_, name.c_str(), zones.c_str(), caps);
}

void LanicomComponent::on_shutdown() {
  if (!this->ready_ || this->engine_mutex_ == nullptr)
    return;
  Lock lock(this->engine_mutex_);
  lc_engine_bye(&this->engine_);
}

// --- main loop ---------------------------------------------------------------

void LanicomComponent::loop() {
  this->network_up_ = network::is_connected();
  if (this->identity_dirty_.exchange(false))
    this->apply_identity_();

  Event ev;
  while (xQueueReceive(this->events_, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case EventType::PEER_ADDED:
        ESP_LOGI(TAG, "Peer joined: %s", ev.name);
        this->peer_added_trigger_.trigger(std::string(ev.name));
        break;
      case EventType::PEER_REMOVED:
        ESP_LOGI(TAG, "Peer left: %s", ev.name);
        this->peer_removed_trigger_.trigger(std::string(ev.name));
        break;
      case EventType::RX_START:
        ESP_LOGD(TAG, "Receiving from %s", ev.name);
        this->receive_start_trigger_.trigger(std::string(ev.name));
        break;
      case EventType::RX_END:
        this->receive_end_trigger_.trigger();
        break;
      case EventType::SENDER_ID_CHANGED: {
        Lock lock(this->engine_mutex_);
        this->persisted_.sender_id = this->engine_.sender_id;
        this->pref_.save(&this->persisted_);
        break;
      }
    }
  }

  if (this->ready_)
    this->status_clear_warning();
  else
    this->status_set_warning("Not running (is a network key set?)");

  this->manage_bus_();
}

void LanicomComponent::start_talking(const std::string &target) {
  lc_target_t t;
  if (lc_target_parse(target.c_str(), &t) != 0) {
    ESP_LOGW(TAG, "Bad target '%s' (use all, zone:<name> or device:<id>)", target.c_str());
    return;
  }
  if (this->mic_ == nullptr)
    return;
  {
    Lock lock(this->engine_mutex_);
    this->talk_target_ = target;
  }
  this->talk_requested_ = true;
  this->talk_begin_ = true;
}

void LanicomComponent::stop_talking() {
  if (!this->talk_requested_)
    return;
  this->talk_requested_ = false;
  this->talk_end_ = true;
}

// The mic and the speaker share one I2S bus: only one of them runs at a time (half duplex).
// Talking wins; after receiving or talking the speaker stays up for a while so replies
// start without the I2S restart delay.
void LanicomComponent::manage_bus_() {
  uint32_t now = millis();
  if (this->receiving_ || this->transmitting_)
    this->speaker_hold_until_ = now + this->speaker_hold_ms_;
  Bus desired = Bus::NONE;
  if (this->talk_requested_ && this->mic_ != nullptr)
    desired = Bus::MIC;
  else if (this->speaker_ != nullptr && (this->receiving_ || (int32_t) (this->speaker_hold_until_ - now) > 0))
    desired = Bus::SPEAKER;

  if (this->bus_ == Bus::SPEAKER && !this->bus_releasing_ && this->speaker_->is_stopped()) {
    this->speaker_ready_ = false;
    this->bus_ = Bus::NONE;  // stopped on its own (e.g. its idle timeout)
  }
  if (this->bus_ == Bus::SPEAKER && !this->speaker_ready_ && this->speaker_->is_running())
    this->speaker_ready_ = true;

  bool busy = this->bus_ != desired || this->bus_releasing_ || this->receiving_ ||
              (this->bus_ == Bus::SPEAKER && !this->speaker_ready_);
  if (busy)
    this->high_freq_.start();
  else
    this->high_freq_.stop();
  if (this->bus_ == desired && !this->bus_releasing_)
    return;

  if (this->bus_releasing_) {
    bool stopped = this->bus_ == Bus::MIC ? this->mic_->is_stopped() : this->speaker_->is_stopped();
    if (!stopped)
      return;
    this->bus_ = Bus::NONE;
    this->bus_releasing_ = false;
  }
  if (this->bus_ != Bus::NONE) {
    if (this->bus_ == Bus::MIC) {
      this->mic_ready_ = false;
      this->mic_->stop();
    } else {
      this->speaker_ready_ = false;
      this->speaker_->stop();
    }
    this->bus_releasing_ = true;
    return;
  }
  if (desired == Bus::MIC) {
    this->capture_read_.store(this->capture_write_.load());
    this->mic_discard_until_ = now + this->mic_warmup_ms_;
    this->mic_ready_ = true;
    this->mic_->start();
  } else if (desired == Bus::SPEAKER) {
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, SAMPLE_RATE));
    this->speaker_->start();
  }
  this->bus_ = desired;
}

void LanicomComponent::post_event_(EventType type, const char *name) {
  Event ev{};
  ev.type = type;
  if (name != nullptr)
    strncpy(ev.name, name, sizeof(ev.name) - 1);
  xQueueSend(this->events_, &ev, 0);
}

std::string LanicomComponent::peers_summary() {
  std::string out;
  Lock lock(this->engine_mutex_);
  for (const auto &p : this->engine_.peers) {
    if (!p.in_use || !p.verified || !p.announced)
      continue;
    if (!out.empty())
      out += ", ";
    out += p.info.name[0] ? p.info.name : "?";
  }
  return out.size() > 250 ? out.substr(0, 247) + "..." : out;
}

lc_stats_t LanicomComponent::get_stats() {
  Lock lock(this->engine_mutex_);
  return this->engine_.stats;
}

// --- network task --------------------------------------------------------------

void LanicomComponent::net_task(void *arg) {
  static_cast<LanicomComponent *>(arg)->net_run_();
  vTaskDelete(nullptr);
}

bool LanicomComponent::open_socket_() {
  int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0)
    return false;
  int one = 1;
  lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  int tos = 0xB8;  // DSCP EF (voice): Wi-Fi WMM maps it to the voice access category
  lwip_setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind(%u) failed: %d", this->port_, errno);
    lwip_close(s);
    return false;
  }
  if (this->multicast_) {
    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = htonl(LC_MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (lwip_setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
      ESP_LOGW(TAG, "Joining the multicast group failed: %d", errno);
    uint8_t ttl = 1, loop = 0;
    lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  }
  this->sock_ = s;
  return true;
}

bool LanicomComponent::restart_engine_() {
  std::string key_string;
  {
    Lock lock(this->engine_mutex_);
    key_string = this->pending_key_;
  }
  this->key_dirty_ = false;
  if (key_string.empty()) {
    this->ready_ = false;
    return false;
  }
  // PBKDF2 takes about a second; do it outside the lock.
  lc_key_t key;
  uint32_t start = millis();
  if (lc_key_derive(&key, key_string.c_str(), LC_PBKDF2_ITERATIONS) != 0) {
    ESP_LOGE(TAG, "Key derivation failed");
    return false;
  }
  ESP_LOGI(TAG, "Network key %04x derived in %" PRIu32 " ms", key.key_id, millis() - start);

  Lock lock(this->engine_mutex_);
  if (this->ready_)
    lc_engine_bye(&this->engine_);
  this->ready_ = false;
  this->key_ = key;
  lc_callbacks_t cb = {};
  cb.send = cb_send;
  cb.random32 = cb_random;
  cb.peer_added = cb_peer_added;
  cb.peer_removed = cb_peer_removed;
  cb.talk_stop = cb_talk_stop;
  cb.audio = cb_audio;
  cb.sender_id_changed = cb_sender_id_changed;
  cb.ctx = this;
  uint64_t epoch = (uint64_t) this->persisted_.boot_counter << 32 | esp_random();
  lc_engine_init(&this->engine_, &this->key_, this->persisted_.sender_id, epoch, &cb, millis());
  this->engine_.port = this->port_;
  this->engine_.multicast = this->multicast_;
  for (const auto &peer : this->static_peers_) {
    struct in_addr a;
    if (inet_aton(peer.c_str(), &a))
      lc_engine_add_static_peer(&this->engine_, lc_addr_t{ntohl(a.s_addr), this->port_});
    else
      ESP_LOGW(TAG, "Static peer '%s' is not an IPv4 address", peer.c_str());
  }
  uint32_t caps = (this->speaker_ != nullptr ? LC_CAP_PLAYBACK : 0) | (this->mic_ != nullptr ? LC_CAP_CAPTURE : 0);
  lc_engine_set_identity(&this->engine_, this->name_.c_str(), this->zones_.c_str(), caps);
  this->peer_count_ = 0;
  this->ready_ = true;
  return true;
}

void LanicomComponent::net_run_() {
  static uint8_t buf[LC_MAX_PACKET + 1];
  while (true) {
    if (!this->network_up_) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (this->key_dirty_)
      this->restart_engine_();
    if (!this->ready_) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (this->sock_ < 0 && !this->open_socket_()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    struct sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int n = lwip_recvfrom(this->sock_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr *>(&from), &from_len);
    Lock lock(this->engine_mutex_);
    if (n > 0) {
      lc_addr_t addr = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
      lc_engine_receive(&this->engine_, buf, (size_t) n, addr, millis());
    }
    lc_engine_tick(&this->engine_, millis());
  }
}

// --- engine callbacks (engine_mutex_ held) ------------------------------------------

void LanicomComponent::cb_send(void *ctx, lc_addr_t to, const uint8_t *data, size_t len) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  if (self->sock_ < 0)
    return;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(to.port);
  addr.sin_addr.s_addr = htonl(to.ip);
  lwip_sendto(self->sock_, data, len, 0, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
}

uint32_t LanicomComponent::cb_random(void *) { return esp_random(); }

void LanicomComponent::cb_peer_added(void *ctx, const lc_peer_t *peer) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  self->peer_count_ = lc_engine_peer_count(&self->engine_);
  self->post_event_(EventType::PEER_ADDED, peer->info.name);
}

void LanicomComponent::cb_peer_removed(void *ctx, const lc_peer_t *peer) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  self->peer_count_ = lc_engine_peer_count(&self->engine_) - (peer->verified && peer->announced ? 1 : 0);
  self->post_event_(EventType::PEER_REMOVED, peer->info.name);
}

void LanicomComponent::cb_sender_id_changed(void *ctx, uint32_t) {
  static_cast<LanicomComponent *>(ctx)->post_event_(EventType::SENDER_ID_CHANGED, nullptr);
}

void LanicomComponent::cb_audio(void *ctx, const lc_peer_t *peer, uint32_t stream_id, uint32_t ts,
                                const uint8_t *opus, size_t len) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  int samples = opus_packet_get_nb_samples(opus, (opus_int32) len, SAMPLE_RATE);  // parses the TOC only
  if (samples <= 0)
    return;
  bool began;
  {
    Lock lock(self->rx_mutex_);
    self->stream_began_ = false;
    lc_rx_push(&self->rx_, peer->sender_id, stream_id, ts, opus, len, (uint32_t) samples * LC_TICKS_PER_SAMPLE,
               millis());
    began = self->stream_began_;
  }
  self->receiving_ = true;
  if (began)
    self->post_event_(EventType::RX_START, peer->info.name);
}

void LanicomComponent::cb_talk_stop(void *ctx, const lc_peer_t *peer, uint32_t stream_id) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  Lock lock(self->rx_mutex_);
  lc_rx_stop(&self->rx_, peer->sender_id, stream_id);
}

// --- rx callbacks (rx_mutex_ held) ---------------------------------------------------

void LanicomComponent::cb_stream_begin(void *ctx, int slot, uint32_t, uint32_t) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  self->decoder_reset_[slot] = true;  // applied by the audio task before the first decode
  self->stream_began_ = true;
}

void LanicomComponent::cb_stream_end(void *ctx, int, uint32_t, uint32_t) {
  static_cast<LanicomComponent *>(ctx)->post_event_(EventType::RX_END, nullptr);
}

int LanicomComponent::cb_decode(void *ctx, int slot, lc_jb_kind_t kind, const uint8_t *data, size_t len,
                                uint32_t samples, int16_t *pcm) {
  auto *self = static_cast<LanicomComponent *>(ctx);
  OpusDecoder *dec = self->decoders_[slot];
  if (dec == nullptr)
    return -1;
  if (self->decoder_reset_[slot]) {
    opus_decoder_ctl(dec, OPUS_RESET_STATE);
    self->decoder_reset_[slot] = false;
  }
  switch (kind) {
    case LC_JB_FRAME:
      return opus_decode(dec, data, (opus_int32) len, pcm, LC_RX_PCM_MAX, 0);
    case LC_JB_FEC:
      return opus_decode(dec, data, (opus_int32) len, pcm, (int) samples, 1);
    default:
      return opus_decode(dec, nullptr, 0, pcm, (int) samples, 0);
  }
}

// --- audio task ------------------------------------------------------------------------

void LanicomComponent::audio_task(void *arg) {
  static_cast<LanicomComponent *>(arg)->audio_run_();
  vTaskDelete(nullptr);
}

void LanicomComponent::audio_run_() {
  int err = 0;
  if (this->mic_ != nullptr) {
    this->encoder_ = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (this->encoder_ == nullptr) {
      ESP_LOGE(TAG, "Opus encoder: %d", err);
    } else {
      opus_encoder_ctl(this->encoder_, OPUS_SET_BITRATE(this->bitrate_));
      opus_encoder_ctl(this->encoder_, OPUS_SET_COMPLEXITY(this->complexity_));
      opus_encoder_ctl(this->encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
      opus_encoder_ctl(this->encoder_, OPUS_SET_INBAND_FEC(1));
      opus_encoder_ctl(this->encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
    }
  }
  for (auto &dec : this->decoders_) {
    dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (dec == nullptr)
      ESP_LOGE(TAG, "Opus decoder: %d", err);
  }

  const uint32_t frame_samples = SAMPLE_RATE * this->frame_ms_ / 1000;
  std::vector<int16_t> frame(frame_samples);
  uint8_t packet[400];
  int16_t pcm[PLAYOUT_SAMPLES];
  bool primed = false;
  TickType_t next_playout = xTaskGetTickCount();

  while (true) {
    TickType_t now_ticks = xTaskGetTickCount();
    TickType_t wait = (int32_t) (next_playout - now_ticks) > 0 ? next_playout - now_ticks : 0;
    ulTaskNotifyTake(pdTRUE, wait);

    // --- transmit: encode every complete frame as soon as the mic delivers it ---
    if (this->talk_begin_.exchange(false)) {
      Lock lock(this->engine_mutex_);
      if (this->talk_.active)
        lc_talk_end(&this->engine_, &this->talk_);  // new target while talking: start a new stream
      if (this->talk_requested_ && this->encoder_ != nullptr) {
        lc_target_t target;
        lc_target_parse(this->talk_target_.c_str(), &target);
        lc_talk_begin(&this->engine_, &this->talk_, &target);
        opus_encoder_ctl(this->encoder_, OPUS_RESET_STATE);
        this->transmitting_ = true;
      }
    }
    while (this->talk_.active) {
      uint32_t r = this->capture_read_.load(std::memory_order_relaxed);
      uint32_t w = this->capture_write_.load(std::memory_order_acquire);
      if (w - r < frame_samples)
        break;
      for (uint32_t i = 0; i < frame_samples; i++)
        frame[i] = this->capture_[(r + i) % CAPTURE_SAMPLES];
      this->capture_read_.store(r + frame_samples, std::memory_order_release);
      int n = opus_encode(this->encoder_, frame.data(), (int) frame_samples, packet, sizeof(packet));
      if (n > 0) {
        Lock lock(this->engine_mutex_);
        if (this->ready_)
          lc_talk_frame(&this->engine_, &this->talk_, packet, (size_t) n, frame_samples);
      }
    }
    if ((this->talk_end_.exchange(false) || !this->talk_requested_) && this->talk_.active) {
      Lock lock(this->engine_mutex_);
      lc_talk_end(&this->engine_, &this->talk_);
      this->transmitting_ = false;
    }

    // --- receive: one 10 ms block per tick, paced by the FreeRTOS tick (same crystal as I2S) ---
    if ((int32_t) (xTaskGetTickCount() - next_playout) < 0)
      continue;
    next_playout += pdMS_TO_TICKS(10);
    if ((int32_t) (xTaskGetTickCount() - next_playout) > (int32_t) pdMS_TO_TICKS(50))
      next_playout = xTaskGetTickCount();  // fell far behind (e.g. a long stall): don't burst
    bool hold = this->speaker_ != nullptr && !this->speaker_ready_ && !this->talk_requested_;
    int active;
    {
      Lock lock(this->rx_mutex_);
      if (!hold)
        lc_rx_read(&this->rx_, pcm, PLAYOUT_SAMPLES, millis());
      active = lc_rx_active(&this->rx_);
    }
    this->receiving_ = active > 0;
    if (hold)
      continue;  // the speaker is starting: keep the audio buffered instead of playing it into the void
    if (!this->speaker_ready_) {
      primed = false;  // talking (bus owned by the mic) or the speaker is starting: discard
      continue;
    }
    if (!primed) {
      // First write since the speaker started: 10 ms of slack against a late task wakeup.
      static const int16_t silence[PLAYOUT_SAMPLES] = {};
      this->speaker_->play(reinterpret_cast<const uint8_t *>(silence), sizeof(silence), 0);
      primed = true;
    }
    // Keep feeding (silence when idle) while the bus is ours, so the speaker stays up for replies.
    this->speaker_->play(reinterpret_cast<const uint8_t *>(pcm), sizeof(pcm), 0);
  }
}

}  // namespace esphome::lanicom
