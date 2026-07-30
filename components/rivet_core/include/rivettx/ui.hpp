#pragma once

#include "rivettx/elrs.hpp"
#include "rivettx/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rivettx {

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual uint16_t width() const = 0;
  virtual uint16_t height() const = 0;
  virtual void clear(bool on = false) = 0;
  virtual void pixel(int16_t x, int16_t y, bool on = true) = 0;
  virtual bool pixel_at(int16_t x, int16_t y) const = 0;

  void horizontal_line(int16_t x, int16_t y, int16_t length, bool on = true);
  void vertical_line(int16_t x, int16_t y, int16_t length, bool on = true);
  void rectangle(Rect rect, bool filled, bool on = true);
  void text(int16_t x, int16_t y, const std::string& value,
            bool inverted = false, uint8_t scale = 1);
  int16_t text_width(const std::string& value, uint8_t scale = 1) const;
};

class MonoCanvas final : public Canvas {
 public:
  MonoCanvas(uint16_t width, uint16_t height);

  uint16_t width() const override;
  uint16_t height() const override;
  void clear(bool on = false) override;
  void pixel(int16_t x, int16_t y, bool on = true) override;
  bool pixel_at(int16_t x, int16_t y) const override;
  const std::vector<uint8_t>& buffer() const;
  std::vector<uint8_t>& buffer();

 private:
  uint16_t width_;
  uint16_t height_;
  std::vector<uint8_t> buffer_;
};

class IDisplaySink {
 public:
  virtual ~IDisplaySink() = default;
  virtual const DisplayCapabilities& capabilities() const = 0;
  virtual bool flush(const MonoCanvas& canvas) = 0;
};

enum class UiEventType : uint8_t {
  None,
  Up,
  Down,
  Left,
  Right,
  Enter,
  Back,
  Rotate,
  TouchPress,
  TouchDrag,
};

struct UiEvent {
  UiEventType type = UiEventType::None;
  int16_t value = 0;
  int16_t x = 0;
  int16_t y = 0;
};

enum class UiFieldKind : uint8_t {
  Label,
  Number,
  Boolean,
  Choice,
  Progress,
  Action,
};

enum class UiScreenKind : uint8_t {
  List,
  Home,
  Outputs,
};

enum class UiWarningCode : uint8_t {
  None,
  StorageInvalid,
  CalibrationRequired,
  InputInvalid,
  InputStale,
  ThrottleHigh,
  ArmSwitch,
  MixerDeadline,
  WatchdogRecovery,
  BatteryCritical,
  BatterySensor,
  BatteryLow,
  ModuleOffline,
  LinkLost,
  LinkCritical,
  LinkWeak,
  LoggingFailed,
  ModelUnsaved,
  Maintenance,
  VideoNoSignal,
};

constexpr std::size_t kMaximumUiWarnings = 16;

struct UiHomeStatus {
  std::array<int16_t, 4> axes{};
  std::array<int16_t, kChannelCount> channels{};
  std::array<UiWarningCode, kMaximumUiWarnings> warnings{};
  uint8_t warning_count = 0;
  uint16_t battery_mv = 0;
  uint8_t battery_percent = 0;
  uint8_t link_quality = 0;
  uint8_t vrx_band = 0;
  uint8_t vrx_channel = 0;
  bool outputs_enabled = false;
  bool battery_percent_valid = false;
  bool module_online = false;
  bool logging = false;
  bool video_signal = false;
};

struct UiField {
  std::string id;
  std::string label;
  std::string value_text;
  UiFieldKind kind = UiFieldKind::Label;
  int32_t value = 0;
  int32_t minimum = 0;
  int32_t maximum = 100;
  bool editable = false;
  bool visible = true;
};

struct UiScreen {
  std::string id;
  std::string title;
  std::vector<UiField> fields;
  bool scrollable = true;
  UiScreenKind kind = UiScreenKind::List;
  UiHomeStatus home{};
};

struct UiChange {
  std::string screen_id;
  std::string field_id;
  int32_t value = 0;
};

struct LayoutMetrics {
  DisplayDensity density = DisplayDensity::Compact;
  int16_t margin = 1;
  int16_t header_height = 9;
  int16_t row_height = 9;
  uint8_t columns = 1;
  uint8_t font_scale = 1;
  uint8_t visible_rows = 5;
};

class ResponsiveLayout {
 public:
  static LayoutMetrics metrics(const DisplayCapabilities& capabilities);
  static Rect content_rect(const DisplayCapabilities& capabilities,
                           const LayoutMetrics& metrics);
  static Rect field_rect(std::size_t visible_index,
                         const DisplayCapabilities& capabilities,
                         const LayoutMetrics& metrics);
};

class UiController {
 public:
  UiController(IDisplaySink& display, MonoCanvas& canvas);

  void set_screen(UiScreen screen);
  void update_home(const UiHomeStatus& status);
  void update_outputs(const ChannelFrame& channels);
  const UiScreen& screen() const;
  bool handle(const UiEvent& event);
  bool render();
  bool take_change(UiChange& change);
  std::size_t selected_index() const;
  std::size_t scroll_offset() const;
  bool editing() const;

 private:
  void keep_selection_visible(const LayoutMetrics& metrics);
  void draw_header(const LayoutMetrics& metrics);
  void draw_home(const LayoutMetrics& metrics);
  void draw_outputs_graph(const LayoutMetrics& metrics);
  void draw_field(const UiField& field, Rect rect, bool selected,
                  const LayoutMetrics& metrics);

  IDisplaySink& display_;
  MonoCanvas& canvas_;
  UiScreen screen_{};
  std::size_t selected_index_ = 0;
  std::size_t scroll_offset_ = 0;
  bool editing_ = false;
  UiChange pending_change_{};
  bool change_pending_ = false;
};

UiScreen make_main_screen(const Model& model, const ChannelFrame& channels,
                          uint16_t battery_mv, uint8_t link_quality,
                          bool safety_enabled);
UiScreen make_openpocket_home_screen(const Model& model,
                                     const UiHomeStatus& status);
UiScreen make_main_menu_screen();
UiScreen make_warnings_screen(const UiHomeStatus& status);
UiScreen make_outputs_screen(const ChannelFrame& channels);
UiScreen make_calibration_screen(uint8_t step, uint8_t progress);
UiScreen make_model_setup_screen(const Model& model);
UiScreen make_inputs_screen(const Model& model);
UiScreen make_mixes_screen(const Model& model);
UiScreen make_output_limits_screen(const Model& model);
UiScreen make_flight_modes_screen(const Model& model);
UiScreen make_curves_screen(const Model& model);
UiScreen make_logical_switches_screen(const Model& model);
UiScreen make_special_functions_screen(const Model& model);
UiScreen make_timers_screen(const Model& model,
                            const std::array<TimerState, kMaxTimers>& states);
UiScreen make_telemetry_screen(const std::vector<UiField>& sensors);
UiScreen make_elrs_screen(const ElrsManagerStatus& status,
                          bool maintenance_allowed);
UiScreen make_elrs_finder_screen(const ElrsFinderStatus& status);
UiScreen make_system_screen(uint16_t battery_mv, uint32_t free_memory,
                            uint32_t missed_deadlines,
                            const std::string& version);

class ModelEditor {
 public:
  static bool apply(Model& model, const UiChange& change);
};

}  // namespace rivettx
