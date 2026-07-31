#include "rivettx/product.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace rivettx {

namespace {

constexpr std::array<std::array<uint16_t, kVrxChannelsPerBand>,
                     kVrxBandCount>
    kVrxFrequencies{{
        {{5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
        {{5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
        {{5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
        {{5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
        {{5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
        {{5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
    }};

uint8_t rssi_percent(int16_t rssi)
{
  return static_cast<uint8_t>(
      clamp<int32_t>(0, (static_cast<int32_t>(rssi) + 100) * 100 / 80,
                     100));
}

}  // namespace

uint16_t vrx_frequency_mhz(uint8_t band, uint8_t channel)
{
  if (band >= kVrxBandCount || channel >= kVrxChannelsPerBand) {
    return 0;
  }
  return kVrxFrequencies[band][channel];
}

VrxController::VrxController(IVrxHardware& hardware,
                             uint32_t scan_dwell_ms)
    : hardware_(hardware),
      scan_dwell_ms_(std::max<uint32_t>(20, scan_dwell_ms))
{
}

bool VrxController::select(uint8_t band, uint8_t channel, TimeUs now_us)
{
  const uint16_t frequency = vrx_frequency_mhz(band, channel);
  if (frequency == 0 || !hardware_.tune(frequency)) {
    status_.available = false;
    return false;
  }
  status_.band = band;
  status_.channel = channel;
  status_.frequency_mhz = frequency;
  status_.available = true;
  status_.scanning = false;
  status_.signal_fresh = false;
  last_sample_us_ = now_us;
  return true;
}

bool VrxController::begin_scan(TimeUs now_us)
{
  scan_index_ = 0;
  best_index_ = 0;
  best_rssi_ = INT16_MIN;
  status_.scanning = true;
  next_scan_step_us_ = now_us;
  return tune_scan_candidate(now_us);
}

void VrxController::cancel_scan()
{
  status_.scanning = false;
}

bool VrxController::tune_scan_candidate(TimeUs now_us)
{
  if (scan_index_ >= kVrxBandCount * kVrxChannelsPerBand) {
    status_.scanning = false;
    const uint8_t band =
        static_cast<uint8_t>(best_index_ / kVrxChannelsPerBand);
    const uint8_t channel =
        static_cast<uint8_t>(best_index_ % kVrxChannelsPerBand);
    return select(band, channel, now_us);
  }
  const uint8_t band =
      static_cast<uint8_t>(scan_index_ / kVrxChannelsPerBand);
  const uint8_t channel =
      static_cast<uint8_t>(scan_index_ % kVrxChannelsPerBand);
  const uint16_t frequency = vrx_frequency_mhz(band, channel);
  if (!hardware_.tune(frequency)) {
    status_.available = false;
    status_.scanning = false;
    return false;
  }
  status_.band = band;
  status_.channel = channel;
  status_.frequency_mhz = frequency;
  status_.available = true;
  status_.signal_fresh = false;
  next_scan_step_us_ =
      now_us + static_cast<TimeUs>(scan_dwell_ms_) * 1000;
  return true;
}

void VrxController::tick(TimeUs now_us)
{
  if (!status_.available) {
    return;
  }
  int16_t rssi = 0;
  bool video_signal = false;
  if (hardware_.sample(rssi, video_signal)) {
    status_.rssi = rssi;
    status_.strength_percent = rssi_percent(rssi);
    status_.video_signal = video_signal;
    status_.signal_fresh = true;
    last_sample_us_ = now_us;
    if (status_.scanning && rssi > best_rssi_) {
      best_rssi_ = rssi;
      best_index_ = scan_index_;
    }
  } else if (last_sample_us_ == 0 ||
             now_us - last_sample_us_ > 1000000) {
    status_.signal_fresh = false;
    status_.video_signal = false;
  }
  if (status_.scanning && now_us >= next_scan_step_us_) {
    ++scan_index_;
    (void)tune_scan_candidate(now_us);
  }
}

const VrxStatus& VrxController::status() const
{
  return status_;
}

char CharacterOsdFrame::at(std::size_t column, std::size_t row) const
{
  return column < kOsdColumns && row < kOsdRows
             ? cells[row * kOsdColumns + column]
             : '\0';
}

UiScreen make_openpocket_home_screen(const Model& model,
                                     const UiHomeStatus& home)
{
  UiScreen screen{};
  screen.id = "openpocket.home";
  screen.title = model.name.data();
  screen.scrollable = false;
  screen.kind = UiScreenKind::Home;
  screen.home = home;
  return screen;
}

UiScreen make_openpocket_main_menu_screen()
{
  UiScreen screen{"openpocket.menu", "OpenPocket", {}};
  const std::array<std::pair<const char*, const char*>, 7> groups{{
      {"group.model", "MODEL"},
      {"group.radio", "RADIO"},
      {"group.elrs", "EXPRESSLRS"},
      {"group.video", "VIDEO"},
      {"group.usb", "USB"},
      {"group.diagnostics", "DIAGNOSTICS"},
      {"group.system", "SYSTEM"},
  }};
  for (const auto& group : groups) {
    screen.fields.push_back(
        {group.first, group.second, "OPEN", UiFieldKind::Action,
         0, 0, 1, false, true});
  }
  return screen;
}

UiScreen make_openpocket_group_menu_screen(OpenPocketMenuGroup group)
{
  UiScreen screen{};
  const auto add_action = [&screen](const char* id, const char* label) {
    screen.fields.push_back(
        {id, label, "OPEN", UiFieldKind::Action,
         0, 0, 1, false, true});
  };
  switch (group) {
    case OpenPocketMenuGroup::Model:
      screen = {"openpocket.group.model", "Model", {}};
      add_action("models", "MODELS");
      add_action("model", "MODEL SETUP");
      add_action("inputs", "INPUTS");
      add_action("mixes", "MIXES");
      add_action("limits", "OUTPUT LIMITS");
      add_action("flight_modes", "FLIGHT MODES");
      add_action("curves", "CURVES");
      add_action("logical", "LOGICAL SWITCHES");
      add_action("special", "SPECIAL FUNCTIONS");
      add_action("timers", "TIMERS");
      break;
    case OpenPocketMenuGroup::Radio:
      screen = {"openpocket.group.radio", "Radio", {}};
      add_action("outputs", "CHANNEL OUTPUTS");
      add_action("power", "POWER");
      break;
    case OpenPocketMenuGroup::Elrs:
      screen = {"openpocket.group.elrs", "ExpressLRS", {}};
      add_action("elrs", "ELRS SETTINGS");
      add_action("finder", "ELRS FINDER");
      break;
    case OpenPocketMenuGroup::Video:
      screen = {"openpocket.group.video", "Video", {}};
      add_action("video", "VIDEO RECEIVER");
      break;
    case OpenPocketMenuGroup::Usb:
      screen = {"openpocket.group.usb", "USB", {}};
      add_action("usb", "USB SIMULATOR");
      break;
    case OpenPocketMenuGroup::Diagnostics:
      screen = {"openpocket.group.diagnostics", "Diagnostics", {}};
      add_action("warnings", "WARNINGS");
      add_action("telemetry", "TELEMETRY");
      break;
    case OpenPocketMenuGroup::System:
      screen = {"openpocket.group.system", "System", {}};
      add_action("web", "WEB CONFIG");
      add_action("system", "RADIO SYSTEM");
      break;
  }
  return screen;
}

UiScreen make_openpocket_video_screen(const VrxStatus& vrx)
{
  UiScreen screen{"video", "Video receiver", {}};
  screen.fields.push_back(
      {"band", "BAND", "", UiFieldKind::Choice,
       static_cast<int32_t>(vrx.band + 1), 1,
       static_cast<int32_t>(kVrxBandCount), true, true});
  screen.fields.push_back(
      {"channel", "CHANNEL", "", UiFieldKind::Choice,
       static_cast<int32_t>(vrx.channel + 1), 1,
       static_cast<int32_t>(kVrxChannelsPerBand), true, true});
  screen.fields.push_back(
      {"frequency", "FREQUENCY",
       vrx.frequency_mhz == 0
           ? "NOT TUNED"
           : std::to_string(vrx.frequency_mhz) + "MHZ",
       UiFieldKind::Label, vrx.frequency_mhz, 0, 0, false, true});
  screen.fields.push_back(
      {"signal", "VIDEO",
       !vrx.signal_fresh
           ? "UNKNOWN"
           : (vrx.video_signal ? "SIGNAL OK" : "NO SIGNAL"),
       UiFieldKind::Label, vrx.video_signal ? 1 : 0, 0, 1, false, true});
  screen.fields.push_back(
      {"rssi", "VRX STRENGTH",
       std::to_string(vrx.strength_percent) + "%",
       UiFieldKind::Progress, vrx.strength_percent, 0, 100, false, true});
  screen.fields.push_back(
      {"scan", vrx.scanning ? "CANCEL SCAN" : "START SCAN",
       "ENTER", UiFieldKind::Action, 0, 0, 1, false, true});
  return screen;
}

void CharacterOsdComposer::clear()
{
  frame_.cells.fill(' ');
}

void CharacterOsdComposer::text(std::size_t column, std::size_t row,
                                const char* value)
{
  if (row >= kOsdRows || value == nullptr) {
    return;
  }
  for (std::size_t index = 0;
       value[index] != '\0' && column + index < kOsdColumns; ++index) {
    frame_.cells[row * kOsdColumns + column + index] = value[index];
  }
}

void CharacterOsdComposer::text(std::size_t column, std::size_t row,
                                const std::string& value,
                                std::size_t maximum)
{
  if (row >= kOsdRows || column >= kOsdColumns) {
    return;
  }
  const std::size_t count =
      std::min({value.size(), maximum, kOsdColumns - column});
  for (std::size_t index = 0; index < count; ++index) {
    frame_.cells[row * kOsdColumns + column + index] = value[index];
  }
}

void CharacterOsdComposer::right_text(std::size_t row, const char* value)
{
  if (row >= kOsdRows || value == nullptr) {
    return;
  }
  const std::size_t length = std::min<std::size_t>(
      std::strlen(value), kOsdColumns);
  text(kOsdColumns - length, row, value);
}

void CharacterOsdComposer::number(std::size_t column, std::size_t row,
                                  int32_t value)
{
  char buffer[16]{};
  (void)std::snprintf(buffer, sizeof(buffer), "%ld",
                      static_cast<long>(value));
  text(column, row, buffer);
}

void CharacterOsdComposer::compose(const Model& model,
                                   const UiHomeStatus& home,
                                   const VrxStatus& vrx)
{
  const UiScreen screen = make_openpocket_home_screen(model, home);
  compose(screen, home, vrx, 0, 0, false);
}

void CharacterOsdComposer::compose(
    const UiScreen& screen, const UiHomeStatus& home,
    const VrxStatus& vrx, std::size_t selected_index,
    std::size_t scroll_offset, bool editing)
{
  clear();
  if (screen.kind == UiScreenKind::Home) {
    compose_home(screen, home, vrx);
  } else {
    compose_list(screen, home, selected_index, scroll_offset, editing);
  }
}

void CharacterOsdComposer::compose_home(
    const UiScreen& screen, const UiHomeStatus& home,
    const VrxStatus& vrx)
{
  text(0, 0, screen.title, 20);
  right_text(0, home.outputs_enabled ? "LIVE" : "SAFE");
  text(0, 1, "------------------------------");

  text(0, 3, "ELRS");
  text(6, 3, home.module_online ? "ONLINE" : "OFFLINE");
  text(18, 3, "LQ");
  number(21, 3, home.link_quality);
  text(25, 3, "%");

  text(0, 5, "TX BAT");
  number(7, 5, home.battery_mv);
  text(12, 5, "MV");
  if (home.battery_percent_valid) {
    number(18, 5, home.battery_percent);
    text(21, 5, "%");
  }

  char vrx_line[30]{};
  if (vrx.available) {
    (void)std::snprintf(
        vrx_line, sizeof(vrx_line), "VRX B%u CH%u %uMHZ",
        static_cast<unsigned>(vrx.band + 1),
        static_cast<unsigned>(vrx.channel + 1),
        static_cast<unsigned>(vrx.frequency_mhz));
  } else {
    (void)std::snprintf(vrx_line, sizeof(vrx_line), "VRX NOT AVAILABLE");
  }
  text(0, 7, vrx_line);
  text(0, 8,
       vrx.scanning
           ? "VIDEO SCANNING"
           : (vrx.video_signal ? "VIDEO SIGNAL OK" : "VIDEO NO SIGNAL"));

  text(0, 10, "WARNING");
  text(9, 10,
       home.warning_count == 0
           ? "NONE"
           : ui_warning_text(home.warnings[0]));
  if (home.warning_count > 1) {
    text(9, 11, "+");
    number(10, 11, static_cast<int32_t>(home.warning_count - 1));
    text(13, 11, "MORE");
  }

  text(0, 13, "ARM CH5");
  text(9, 13, home.channels[4] > 0 ? "HIGH" : "LOW");
  number(15, 13, home.channels[4]);
  text(0, 15, "ENTER MENU");
  right_text(15, "RIVETTX");
}

void CharacterOsdComposer::compose_list(
    const UiScreen& screen, const UiHomeStatus& home,
    std::size_t selected_index, std::size_t scroll_offset,
    bool editing)
{
  constexpr std::size_t kVisibleRows = 12;
  text(0, 0, screen.title, 20);
  right_text(0, editing ? "EDIT"
                       : (home.outputs_enabled ? "LIVE" : "SAFE"));
  text(0, 1, "------------------------------");

  const std::size_t visible_count = static_cast<std::size_t>(
      std::count_if(screen.fields.begin(), screen.fields.end(),
                    [](const UiField& field) { return field.visible; }));
  std::size_t visible_index = 0;
  std::size_t rendered = 0;
  std::size_t selected_visible = 0;
  for (std::size_t index = 0; index < screen.fields.size(); ++index) {
    const auto& field = screen.fields[index];
    if (!field.visible) {
      continue;
    }
    if (index == selected_index) {
      selected_visible = visible_index;
    }
    if (visible_index++ < scroll_offset || rendered >= kVisibleRows) {
      continue;
    }

    const std::size_t row = 2 + rendered++;
    frame_.cells[row * kOsdColumns] =
        index == selected_index ? (editing ? '*' : '>') : ' ';
    text(2, row, field.label, 17);
    if (!field.value_text.empty()) {
      text(20, row, field.value_text, 10);
    } else if (field.kind == UiFieldKind::Boolean) {
      text(25, row, field.value != 0 ? "ON" : "OFF");
    } else if (field.kind == UiFieldKind::Number ||
               field.kind == UiFieldKind::Choice ||
               field.kind == UiFieldKind::Progress) {
      number(24, row, field.value);
    }
  }

  if (scroll_offset > 0) {
    frame_.cells[2 * kOsdColumns + 29] = '^';
  }
  if (scroll_offset + kVisibleRows < visible_count) {
    frame_.cells[13 * kOsdColumns + 29] = 'v';
  }
  char position[24]{};
  (void)std::snprintf(
      position, sizeof(position), "%u/%u",
      static_cast<unsigned>(visible_count == 0 ? 0 : selected_visible + 1),
      static_cast<unsigned>(visible_count));
  text(0, 14, position);
  right_text(14, home.module_online ? "ELRS OK" : "ELRS LOST");
  text(0, 15,
       editing ? "ROTATE CHANGE ENTER SAVE"
               : "ENTER SELECT  BACK");
}

const CharacterOsdFrame& CharacterOsdComposer::frame() const
{
  return frame_;
}

void CharacterOsdUi::set_screen(UiScreen screen)
{
  const bool same_screen = screen.id == screen_.id;
  std::string selected_id;
  int32_t staged_value = 0;
  std::string staged_text;
  const bool preserve_edit =
      same_screen && editing_ && selected_index_ < screen_.fields.size() &&
      screen_.fields[selected_index_].editable;
  if (same_screen && selected_index_ < screen_.fields.size()) {
    selected_id = screen_.fields[selected_index_].id;
    if (preserve_edit) {
      staged_value = screen_.fields[selected_index_].value;
      staged_text = screen_.fields[selected_index_].value_text;
    }
  }
  screen_ = std::move(screen);
  if (screen_.kind == UiScreenKind::Home) {
    home_ = screen_.home;
  }
  selected_index_ = 0;
  if (same_screen && !selected_id.empty()) {
    for (std::size_t index = 0; index < screen_.fields.size(); ++index) {
      if (screen_.fields[index].id == selected_id) {
        selected_index_ = index;
        break;
      }
    }
  }
  select_first_visible();
  editing_ = preserve_edit && selected_index_ < screen_.fields.size() &&
             screen_.fields[selected_index_].id == selected_id &&
             screen_.fields[selected_index_].editable;
  if (editing_) {
    screen_.fields[selected_index_].value = staged_value;
    screen_.fields[selected_index_].value_text = staged_text;
  }
  keep_selection_visible();
}

void CharacterOsdUi::update_home(const UiHomeStatus& home)
{
  home_ = home;
  if (screen_.kind == UiScreenKind::Home) {
    screen_.home = home;
  }
}

void CharacterOsdUi::select_first_visible()
{
  if (screen_.fields.empty()) {
    selected_index_ = 0;
    scroll_offset_ = 0;
    return;
  }
  if (selected_index_ < screen_.fields.size() &&
      screen_.fields[selected_index_].visible) {
    return;
  }
  for (std::size_t index = 0; index < screen_.fields.size(); ++index) {
    if (screen_.fields[index].visible) {
      selected_index_ = index;
      return;
    }
  }
  selected_index_ = 0;
}

void CharacterOsdUi::move_selection(int direction, int steps)
{
  if (screen_.fields.empty() || direction == 0) {
    return;
  }
  for (int step = 0; step < steps; ++step) {
    std::size_t candidate = selected_index_;
    while (true) {
      if (direction > 0) {
        if (candidate + 1 >= screen_.fields.size()) {
          return;
        }
        ++candidate;
      } else {
        if (candidate == 0) {
          return;
        }
        --candidate;
      }
      if (screen_.fields[candidate].visible) {
        selected_index_ = candidate;
        break;
      }
    }
  }
}

void CharacterOsdUi::keep_selection_visible()
{
  constexpr std::size_t kVisibleRows = 12;
  std::size_t selected_visible = 0;
  for (std::size_t index = 0; index < selected_index_ &&
                              index < screen_.fields.size(); ++index) {
    if (screen_.fields[index].visible) {
      ++selected_visible;
    }
  }
  if (selected_visible < scroll_offset_) {
    scroll_offset_ = selected_visible;
  } else if (selected_visible >= scroll_offset_ + kVisibleRows) {
    scroll_offset_ = selected_visible - kVisibleRows + 1;
  }
}

void CharacterOsdUi::update_value_text(UiField& field)
{
  if (field.kind == UiFieldKind::Boolean) {
    field.value_text = field.value != 0 ? "ON" : "OFF";
    return;
  }
  char value[16]{};
  (void)std::snprintf(value, sizeof(value), "%ld",
                      static_cast<long>(field.value));
  field.value_text = value;
}

bool CharacterOsdUi::handle(const UiEvent& event)
{
  if (screen_.kind == UiScreenKind::Home) {
    if (event.type == UiEventType::Enter) {
      pending_change_ = {screen_.id, "menu", 1};
      change_pending_ = true;
      return true;
    }
    return false;
  }
  if (event.type == UiEventType::Back) {
    if (editing_ && selected_index_ < screen_.fields.size()) {
      auto& field = screen_.fields[selected_index_];
      field.value = edit_original_value_;
      field.value_text = edit_original_text_;
      editing_ = false;
    } else {
      back_pending_ = true;
    }
    return true;
  }
  if (screen_.fields.empty() || selected_index_ >= screen_.fields.size()) {
    return false;
  }

  auto& field = screen_.fields[selected_index_];
  auto adjust = [this, &field](int32_t delta) {
    if (!editing_ || !field.editable || delta == 0) {
      return;
    }
    field.value =
        clamp<int32_t>(field.minimum, field.value + delta, field.maximum);
    update_value_text(field);
  };

  switch (event.type) {
    case UiEventType::Up:
      if (editing_) {
        adjust(1);
      } else {
        move_selection(-1);
      }
      break;
    case UiEventType::Down:
      if (editing_) {
        adjust(-1);
      } else {
        move_selection(1);
      }
      break;
    case UiEventType::Left:
      adjust(-1);
      break;
    case UiEventType::Right:
      adjust(1);
      break;
    case UiEventType::Rotate:
      if (editing_) {
        adjust(event.value);
      } else if (event.value != 0) {
        move_selection(event.value > 0 ? 1 : -1,
                       std::min(16, std::abs(event.value)));
      }
      break;
    case UiEventType::Enter:
      if (field.kind == UiFieldKind::Action) {
        pending_change_ = {screen_.id, field.id, 1};
        change_pending_ = true;
      } else if (field.editable && !editing_) {
        edit_original_value_ = field.value;
        edit_original_text_ = field.value_text;
        editing_ = true;
      } else if (field.editable) {
        pending_change_ = {screen_.id, field.id, field.value};
        change_pending_ = true;
        editing_ = false;
      }
      break;
    default:
      return false;
  }
  keep_selection_visible();
  return true;
}

bool CharacterOsdUi::render(const VrxStatus& vrx)
{
  composer_.compose(screen_, home_, vrx, selected_index_, scroll_offset_,
                    editing_);
  return true;
}

bool CharacterOsdUi::take_change(UiChange& change)
{
  if (!change_pending_) {
    return false;
  }
  change = pending_change_;
  change_pending_ = false;
  return true;
}

bool CharacterOsdUi::take_back_request()
{
  const bool requested = back_pending_;
  back_pending_ = false;
  return requested;
}

const UiScreen& CharacterOsdUi::screen() const
{
  return screen_;
}

const CharacterOsdFrame& CharacterOsdUi::frame() const
{
  return composer_.frame();
}

std::size_t CharacterOsdUi::selected_index() const
{
  return selected_index_;
}

std::size_t CharacterOsdUi::scroll_offset() const
{
  return scroll_offset_;
}

bool CharacterOsdUi::editing() const
{
  return editing_;
}

OpenPocketMenuController::OpenPocketMenuController(
    IOpenPocketScreenProvider& screens)
    : screens_(screens)
{
}

UiScreen OpenPocketMenuController::screen_for(OpenPocketPage page)
{
  switch (page) {
    case OpenPocketPage::MainMenu:
      return make_openpocket_main_menu_screen();
    case OpenPocketPage::ModelMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Model);
    case OpenPocketPage::RadioMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Radio);
    case OpenPocketPage::ElrsMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Elrs);
    case OpenPocketPage::VideoMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Video);
    case OpenPocketPage::UsbMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Usb);
    case OpenPocketPage::DiagnosticsMenu:
      return make_openpocket_group_menu_screen(
          OpenPocketMenuGroup::Diagnostics);
    case OpenPocketPage::SystemMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::System);
    default:
      return screens_.screen(page);
  }
}

void OpenPocketMenuController::start(const UiHomeStatus& home)
{
  home_ = home;
  history_size_ = 0;
  page_ = OpenPocketPage::Home;
  ui_.set_screen(screen_for(page_));
  ui_.update_home(home_);
  change_pending_ = false;
}

void OpenPocketMenuController::refresh(const UiHomeStatus& home)
{
  home_ = home;
  ui_.set_screen(screen_for(page_));
  ui_.update_home(home_);
}

bool OpenPocketMenuController::route(
    const UiChange& change, OpenPocketPage& target) const
{
  if (page_ == OpenPocketPage::Home && change.field_id == "menu") {
    target = OpenPocketPage::MainMenu;
    return true;
  }
  if (page_ == OpenPocketPage::MainMenu) {
    const std::array<std::pair<const char*, OpenPocketPage>, 7> groups{{
        {"group.model", OpenPocketPage::ModelMenu},
        {"group.radio", OpenPocketPage::RadioMenu},
        {"group.elrs", OpenPocketPage::ElrsMenu},
        {"group.video", OpenPocketPage::VideoMenu},
        {"group.usb", OpenPocketPage::UsbMenu},
        {"group.diagnostics", OpenPocketPage::DiagnosticsMenu},
        {"group.system", OpenPocketPage::SystemMenu},
    }};
    for (const auto& group : groups) {
      if (change.field_id == group.first) {
        target = group.second;
        return true;
      }
    }
    return false;
  }

  struct Route {
    OpenPocketPage parent;
    const char* field;
    OpenPocketPage target;
  };
  static constexpr std::array<Route, 19> routes{{
      {OpenPocketPage::ModelMenu, "models", OpenPocketPage::Models},
      {OpenPocketPage::ModelMenu, "model", OpenPocketPage::ModelSetup},
      {OpenPocketPage::ModelMenu, "inputs", OpenPocketPage::Inputs},
      {OpenPocketPage::ModelMenu, "mixes", OpenPocketPage::Mixes},
      {OpenPocketPage::ModelMenu, "limits", OpenPocketPage::Limits},
      {OpenPocketPage::ModelMenu, "flight_modes",
       OpenPocketPage::FlightModes},
      {OpenPocketPage::ModelMenu, "curves", OpenPocketPage::Curves},
      {OpenPocketPage::ModelMenu, "logical",
       OpenPocketPage::LogicalSwitches},
      {OpenPocketPage::ModelMenu, "special",
       OpenPocketPage::SpecialFunctions},
      {OpenPocketPage::ModelMenu, "timers", OpenPocketPage::Timers},
      {OpenPocketPage::RadioMenu, "outputs", OpenPocketPage::Outputs},
      {OpenPocketPage::RadioMenu, "power", OpenPocketPage::Power},
      {OpenPocketPage::ElrsMenu, "elrs", OpenPocketPage::Elrs},
      {OpenPocketPage::ElrsMenu, "finder", OpenPocketPage::Finder},
      {OpenPocketPage::VideoMenu, "video", OpenPocketPage::Video},
      {OpenPocketPage::UsbMenu, "usb", OpenPocketPage::Usb},
      {OpenPocketPage::DiagnosticsMenu, "warnings",
       OpenPocketPage::Warnings},
      {OpenPocketPage::DiagnosticsMenu, "telemetry",
       OpenPocketPage::Telemetry},
      {OpenPocketPage::SystemMenu, "web", OpenPocketPage::Web},
  }};
  for (const auto& item : routes) {
    if (page_ == item.parent && change.field_id == item.field) {
      target = item.target;
      return true;
    }
  }
  if (page_ == OpenPocketPage::SystemMenu &&
      change.field_id == "system") {
    target = OpenPocketPage::System;
    return true;
  }
  return false;
}

void OpenPocketMenuController::navigate(OpenPocketPage target)
{
  if (history_size_ < history_.size()) {
    history_[history_size_++] = page_;
  }
  page_ = target;
  ui_.set_screen(screen_for(page_));
  ui_.update_home(home_);
}

void OpenPocketMenuController::go_back()
{
  if (history_size_ == 0) {
    go_home();
    return;
  }
  page_ = history_[--history_size_];
  ui_.set_screen(screen_for(page_));
  ui_.update_home(home_);
}

void OpenPocketMenuController::go_home()
{
  history_size_ = 0;
  page_ = OpenPocketPage::Home;
  ui_.set_screen(screen_for(page_));
  ui_.update_home(home_);
}

bool OpenPocketMenuController::handle(const UiEvent& event)
{
  if (event.type == UiEventType::Home) {
    go_home();
    return true;
  }
  const bool handled = ui_.handle(event);
  UiChange change{};
  if (ui_.take_change(change)) {
    OpenPocketPage target = page_;
    if (route(change, target)) {
      navigate(target);
    } else {
      pending_change_ = change;
      change_pending_ = true;
    }
  }
  if (ui_.take_back_request()) {
    go_back();
  }
  return handled;
}

bool OpenPocketMenuController::render(const VrxStatus& vrx)
{
  return ui_.render(vrx);
}

bool OpenPocketMenuController::take_change(UiChange& change)
{
  if (!change_pending_) {
    return false;
  }
  change = pending_change_;
  change_pending_ = false;
  return true;
}

OpenPocketPage OpenPocketMenuController::page() const
{
  return page_;
}

std::size_t OpenPocketMenuController::depth() const
{
  return history_size_;
}

const UiScreen& OpenPocketMenuController::screen() const
{
  return ui_.screen();
}

const CharacterOsdFrame& OpenPocketMenuController::frame() const
{
  return ui_.frame();
}

std::size_t OpenPocketMenuController::selected_index() const
{
  return ui_.selected_index();
}

std::size_t OpenPocketMenuController::scroll_offset() const
{
  return ui_.scroll_offset();
}

bool OpenPocketMenuController::editing() const
{
  return ui_.editing();
}

bool UsbSimulator::enter(bool outputs_locked, bool rf_safety_lock)
{
  if (!outputs_locked) {
    return false;
  }
  active_ = true;
  rf_safety_lock_ = rf_safety_lock;
  return true;
}

void UsbSimulator::leave()
{
  active_ = false;
  rf_safety_lock_ = true;
}

bool UsbSimulator::active() const
{
  return active_;
}

bool UsbSimulator::rf_output_allowed() const
{
  return !active_ || !rf_safety_lock_;
}

UsbGamepadReport UsbSimulator::report(
    const ControlInputs& controls, const ChannelFrame& channels) const
{
  UsbGamepadReport result{};
  for (std::size_t axis = 0; axis < result.axes.size(); ++axis) {
    result.axes[axis] = controls.axes[axis];
  }
  (void)channels;
  for (std::size_t index = 0; index < kMaxSwitches; ++index) {
    if (controls.switches[index]) {
      result.buttons |= static_cast<uint32_t>(1UL << index);
    }
  }
  return result;
}

BatteryEstimator::BatteryEstimator(uint16_t empty_mv, uint16_t full_mv)
    : empty_mv_(std::min(empty_mv, full_mv)),
      full_mv_(std::max<uint16_t>(empty_mv + 1, full_mv))
{
}

ProductPowerStatus BatteryEstimator::estimate(
    uint16_t voltage_mv, bool sample_valid, ChargeState charge,
    bool external_power, uint16_t average_current_ma,
    uint16_t remaining_capacity_mah) const
{
  ProductPowerStatus result{};
  result.voltage_mv = voltage_mv;
  result.charge = charge;
  result.external_power = external_power;
  result.sensor_fault = !sample_valid;
  if (!sample_valid || voltage_mv == 0) {
    return result;
  }
  result.percentage = static_cast<uint8_t>(clamp<int32_t>(
      0, (static_cast<int32_t>(voltage_mv) - empty_mv_) * 100 /
             (full_mv_ - empty_mv_),
      100));
  result.percentage_valid = true;
  if (average_current_ma != 0 && remaining_capacity_mah != 0) {
    result.runtime_minutes = static_cast<uint16_t>(
        std::min<uint32_t>(
            UINT16_MAX,
            static_cast<uint32_t>(remaining_capacity_mah) * 60 /
                average_current_ma));
    result.runtime_valid = true;
  }
  return result;
}

void OnboardingGuide::begin()
{
  step_ = OnboardingStep::Welcome;
}

bool OnboardingGuide::evidence_satisfies_step(
    const OnboardingEvidence& evidence) const
{
  switch (step_) {
    case OnboardingStep::Welcome:
      return true;
    case OnboardingStep::StickCalibration:
      return evidence.calibration_valid;
    case OnboardingStep::ArmSwitch:
      return evidence.arm_switch_identified;
    case OnboardingStep::AuxSwitches:
      return evidence.aux_positions_verified;
    case OnboardingStep::Elrs:
      return evidence.elrs_online;
    case OnboardingStep::Video:
      return !evidence.video_required || evidence.video_verified;
    case OnboardingStep::Battery:
      return evidence.battery_profile_valid;
    case OnboardingStep::ChannelPreview:
      return evidence.channel_preview_valid && evidence.arm_channel_low;
    case OnboardingStep::Complete:
      return false;
  }
  return false;
}

bool OnboardingGuide::advance(const OnboardingEvidence& evidence)
{
  if (!evidence_satisfies_step(evidence) ||
      step_ == OnboardingStep::Complete) {
    return false;
  }
  step_ = static_cast<OnboardingStep>(
      static_cast<uint8_t>(step_) + 1);
  return true;
}

void OnboardingGuide::back()
{
  if (step_ != OnboardingStep::Welcome) {
    step_ = static_cast<OnboardingStep>(
        static_cast<uint8_t>(step_) - 1);
  }
}

OnboardingStep OnboardingGuide::step() const
{
  return step_;
}

bool OnboardingGuide::complete() const
{
  return step_ == OnboardingStep::Complete;
}

}  // namespace rivettx
