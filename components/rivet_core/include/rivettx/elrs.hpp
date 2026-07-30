#pragma once

#include "rivettx/crsf.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kMaximumElrsOptions = 10;
constexpr std::size_t kMaximumElrsOptionLength = 18;

enum class ElrsManagerState : uint8_t {
  Discovering,
  Ready,
  Applying,
  CommandRunning,
  WifiUpdate,
  Unavailable,
};

struct ElrsSelection {
  bool available = false;
  uint8_t value = 0;
  uint8_t minimum = 0;
  uint8_t maximum = 0;
  uint8_t option_count = 0;
  std::array<std::array<char, kMaximumElrsOptionLength>,
             kMaximumElrsOptions>
      options{};
};

struct ElrsManagerStatus {
  ElrsManagerState state = ElrsManagerState::Discovering;
  std::array<char, 25> device_name{};
  uint32_t firmware_version = 0;
  uint8_t fields_discovered = 0;
  uint8_t field_count = 0;
  ElrsSelection power{};
  ElrsSelection dynamic_power{};
  ElrsSelection switch_mode{};
  ElrsSelection telemetry_ratio{};
  bool bind_available = false;
  bool wifi_update_available = false;
  std::array<char, 33> message{};
};

class ElrsDeviceManager {
 public:
  ElrsDeviceManager(ICrsfTransport& transport, CrsfParser& parser);

  void start(TimeUs now_us);
  void tick(TimeUs now_us);
  bool request_power(uint8_t option);
  bool request_dynamic_power(uint8_t option);
  bool request_switch_mode(uint8_t option);
  bool request_telemetry_ratio(uint8_t option);
  bool request_bind();
  bool request_wifi_update();
  const ElrsManagerStatus& status() const;

 private:
  enum class PendingAction : uint8_t {
    None,
    SetPower,
    SetDynamicPower,
    SetSwitchMode,
    SetTelemetryRatio,
    Bind,
    WifiUpdate,
  };

  void reset_discovery(TimeUs now_us);
  void consume_frame(const crsf::Frame& frame, TimeUs now_us);
  void consume_device_info(const crsf::Frame& frame, TimeUs now_us);
  void consume_parameter_entry(const crsf::Frame& frame, TimeUs now_us);
  void parse_parameter(uint8_t field_id, const uint8_t* data,
                       std::size_t size, TimeUs now_us);
  void store_selection(ElrsSelection& destination,
                       const uint8_t* data, std::size_t size);
  void request_next_field(TimeUs now_us);
  bool send_read(uint8_t field_id, uint8_t chunk, TimeUs now_us);
  bool send_write(uint8_t field_id, const uint8_t* value,
                  std::size_t value_size, TimeUs now_us);
  void process_pending(TimeUs now_us);
  void schedule_rediscovery(TimeUs now_us);
  void set_message(const char* message);

  ICrsfTransport& transport_;
  CrsfParser& parser_;
  ElrsManagerStatus status_{};
  std::array<uint8_t, 256> field_data_{};
  std::size_t field_data_size_ = 0;
  uint8_t current_field_ = 0;
  uint8_t current_chunk_ = 0;
  uint8_t read_retries_ = 0;
  uint8_t device_ping_retries_ = 0;
  uint8_t power_field_id_ = 0;
  uint8_t dynamic_power_field_id_ = 0;
  uint8_t switch_mode_field_id_ = 0;
  uint8_t telemetry_ratio_field_id_ = 0;
  uint8_t bind_field_id_ = 0;
  uint8_t wifi_field_id_ = 0;
  PendingAction active_command_ = PendingAction::None;
  TimeUs response_deadline_us_ = 0;
  TimeUs rediscover_at_us_ = 0;
  TimeUs command_deadline_us_ = 0;
  std::atomic<uint8_t> pending_action_{
      static_cast<uint8_t>(PendingAction::None)};
  std::atomic<uint8_t> pending_value_{0};
};

class IToneOutput {
 public:
  virtual ~IToneOutput() = default;
  virtual bool play_tone(uint16_t frequency_hz,
                         uint16_t duration_ms) = 0;
  virtual void stop_tone() = 0;
  virtual bool available() const = 0;
};

struct ElrsFinderStatus {
  bool active = false;
  bool signal_fresh = false;
  bool audio_available = false;
  int16_t raw_rssi_dbm = -120;
  int16_t filtered_rssi_dbm = -120;
  uint8_t strength_percent = 0;
  uint16_t beep_period_ms = 0;
};

class ElrsFinder {
 public:
  explicit ElrsFinder(IToneOutput& tones);

  void set_active(bool active);
  void tick(const TelemetryRegistry& telemetry, TimeUs now_us);
  const ElrsFinderStatus& status() const;

 private:
  IToneOutput& tones_;
  ElrsFinderStatus status_{};
  TimeUs next_beep_us_ = 0;
  bool filter_initialized_ = false;
};

}  // namespace rivettx
