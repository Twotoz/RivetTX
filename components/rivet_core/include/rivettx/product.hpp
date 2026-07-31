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

enum class OpenPocketMenuGroup : uint8_t {
  Model,
  Radio,
  Elrs,
  Video,
  Usb,
  Diagnostics,
  System,
};

UiScreen make_openpocket_home_screen(const Model& model,
                                     const UiHomeStatus& home);
UiScreen make_openpocket_main_menu_screen();
UiScreen make_openpocket_group_menu_screen(OpenPocketMenuGroup group);
UiScreen make_openpocket_video_screen(const VrxStatus& vrx);

class CharacterOsdComposer {
 public:
  void compose(const Model& model, const UiHomeStatus& home,
               const VrxStatus& vrx);
  void compose(const UiScreen& screen, const UiHomeStatus& home,
               const VrxStatus& vrx, std::size_t selected_index,
               std::size_t scroll_offset, bool editing);
  const CharacterOsdFrame& frame() const;

 private:
  void clear();
  void text(std::size_t column, std::size_t row, const char* value);
  void text(std::size_t column, std::size_t row, const std::string& value,
            std::size_t maximum);
  void right_text(std::size_t row, const char* value);
  void number(std::size_t column, std::size_t row, int32_t value);
  void compose_home(const UiScreen& screen, const UiHomeStatus& home,
                    const VrxStatus& vrx);
  void compose_list(const UiScreen& screen, const UiHomeStatus& home,
                    std::size_t selected_index, std::size_t scroll_offset,
                    bool editing);

  CharacterOsdFrame frame_{};
};

class CharacterOsdUi {
 public:
  void set_screen(UiScreen screen);
  void update_home(const UiHomeStatus& home);
  bool handle(const UiEvent& event);
  bool render(const VrxStatus& vrx);
  bool take_change(UiChange& change);
  bool take_back_request();
  const UiScreen& screen() const;
  const CharacterOsdFrame& frame() const;
  std::size_t selected_index() const;
  std::size_t scroll_offset() const;
  bool editing() const;

 private:
  void select_first_visible();
  void move_selection(int direction, int steps = 1);
  void keep_selection_visible();
  void update_value_text(UiField& field);

  CharacterOsdComposer composer_{};
  UiScreen screen_{};
  UiHomeStatus home_{};
  std::size_t selected_index_ = 0;
  std::size_t scroll_offset_ = 0;
  int32_t edit_original_value_ = 0;
  std::string edit_original_text_;
  UiChange pending_change_{};
  bool editing_ = false;
  bool change_pending_ = false;
  bool back_pending_ = false;
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
