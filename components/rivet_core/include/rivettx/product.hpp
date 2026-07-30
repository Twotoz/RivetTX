#pragma once

#include "rivettx/types.hpp"
#include "rivettx/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kVrxBandCount = 6;
constexpr std::size_t kVrxChannelsPerBand = 8;

uint16_t vrx_frequency_mhz(uint8_t band, uint8_t channel);

class IVrxHardware {
 public:
  virtual ~IVrxHardware() = default;
  virtual bool tune(uint16_t frequency_mhz) = 0;
  virtual bool sample(int16_t& rssi, bool& video_signal) = 0;
};

struct VrxStatus {
  uint8_t band = 0;
  uint8_t channel = 0;
  uint16_t frequency_mhz = 0;
  int16_t rssi = 0;
  uint8_t strength_percent = 0;
  bool available = false;
  bool scanning = false;
  bool signal_fresh = false;
  bool video_signal = false;
};

class VrxController {
 public:
  explicit VrxController(IVrxHardware& hardware,
                         uint32_t scan_dwell_ms = 80);

  bool select(uint8_t band, uint8_t channel, TimeUs now_us);
  bool begin_scan(TimeUs now_us);
  void cancel_scan();
  void tick(TimeUs now_us);
  const VrxStatus& status() const;

 private:
  bool tune_scan_candidate(TimeUs now_us);

  IVrxHardware& hardware_;
  VrxStatus status_{};
  uint32_t scan_dwell_ms_ = 80;
  uint8_t scan_index_ = 0;
  uint8_t best_index_ = 0;
  int16_t best_rssi_ = INT16_MIN;
  TimeUs next_scan_step_us_ = 0;
  TimeUs last_sample_us_ = 0;
};

constexpr std::size_t kOsdColumns = 30;
constexpr std::size_t kOsdRows = 16;

struct CharacterOsdFrame {
  std::array<char, kOsdColumns * kOsdRows> cells{};

  char at(std::size_t column, std::size_t row) const;
};

class CharacterOsdComposer {
 public:
  void compose(const Model& model, const UiHomeStatus& home,
               const VrxStatus& vrx);
  const CharacterOsdFrame& frame() const;

 private:
  void clear();
  void text(std::size_t column, std::size_t row, const char* value);
  void number(std::size_t column, std::size_t row, int32_t value);

  CharacterOsdFrame frame_{};
};

struct UsbGamepadReport {
  std::array<int16_t, 8> axes{};
  uint32_t buttons = 0;
};

class UsbSimulator {
 public:
  bool enter(bool outputs_locked, bool rf_safety_lock);
  void leave();
  bool active() const;
  bool rf_output_allowed() const;
  UsbGamepadReport report(const ControlInputs& controls,
                          const ChannelFrame& channels) const;

 private:
  bool active_ = false;
  bool rf_safety_lock_ = true;
};

enum class ChargeState : uint8_t {
  Unknown,
  Disconnected,
  Charging,
  Full,
  Fault,
};

struct ProductPowerStatus {
  uint16_t voltage_mv = 0;
  uint8_t percentage = 0;
  uint16_t runtime_minutes = 0;
  ChargeState charge = ChargeState::Unknown;
  bool percentage_valid = false;
  bool runtime_valid = false;
  bool external_power = false;
  bool sensor_fault = false;
};

class BatteryEstimator {
 public:
  BatteryEstimator(uint16_t empty_mv = 3300,
                   uint16_t full_mv = 4200);
  ProductPowerStatus estimate(uint16_t voltage_mv, bool sample_valid,
                              ChargeState charge, bool external_power,
                              uint16_t average_current_ma = 0,
                              uint16_t remaining_capacity_mah = 0) const;

 private:
  uint16_t empty_mv_ = 3300;
  uint16_t full_mv_ = 4200;
};

enum class OnboardingStep : uint8_t {
  Welcome,
  StickCalibration,
  ArmSwitch,
  AuxSwitches,
  Elrs,
  Video,
  Battery,
  ChannelPreview,
  Complete,
};

struct OnboardingEvidence {
  bool calibration_valid = false;
  bool arm_switch_identified = false;
  bool aux_positions_verified = false;
  bool elrs_online = false;
  bool video_verified = false;
  bool video_required = false;
  bool battery_profile_valid = false;
  bool channel_preview_valid = false;
  bool arm_channel_low = false;
};

class OnboardingGuide {
 public:
  void begin();
  bool advance(const OnboardingEvidence& evidence);
  void back();
  OnboardingStep step() const;
  bool complete() const;

 private:
  bool evidence_satisfies_step(const OnboardingEvidence& evidence) const;

  OnboardingStep step_ = OnboardingStep::Welcome;
};

}  // namespace rivettx
