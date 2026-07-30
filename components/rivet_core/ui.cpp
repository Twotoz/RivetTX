#include "rivettx/ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace rivettx {

namespace {

struct Glyph {
  char character;
  std::array<uint8_t, 5> columns;
};

constexpr std::array<Glyph, 43> kGlyphs{{
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'%', {0x63, 0x13, 0x08, 0x64, 0x63}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
}};

const std::array<uint8_t, 5>& glyph_for(char character)
{
  const char normalized =
      static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  for (const auto& glyph : kGlyphs) {
    if (glyph.character == normalized) {
      return glyph.columns;
    }
  }
  return kGlyphs.back().columns;
}

std::string number_string(int32_t value)
{
  return std::to_string(value);
}

}  // namespace

void Canvas::horizontal_line(int16_t x, int16_t y, int16_t length, bool on)
{
  for (int16_t i = 0; i < length; ++i) {
    pixel(static_cast<int16_t>(x + i), y, on);
  }
}

void Canvas::vertical_line(int16_t x, int16_t y, int16_t length, bool on)
{
  for (int16_t i = 0; i < length; ++i) {
    pixel(x, static_cast<int16_t>(y + i), on);
  }
}

void Canvas::rectangle(Rect rect, bool filled, bool on)
{
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }
  if (filled) {
    for (int16_t y = 0; y < rect.height; ++y) {
      horizontal_line(rect.x, static_cast<int16_t>(rect.y + y), rect.width,
                      on);
    }
  } else {
    horizontal_line(rect.x, rect.y, rect.width, on);
    horizontal_line(rect.x,
                    static_cast<int16_t>(rect.y + rect.height - 1),
                    rect.width, on);
    vertical_line(rect.x, rect.y, rect.height, on);
    vertical_line(static_cast<int16_t>(rect.x + rect.width - 1), rect.y,
                  rect.height, on);
  }
}

void Canvas::text(int16_t x, int16_t y, const std::string& value,
                  bool inverted, uint8_t scale)
{
  const uint8_t safe_scale = std::max<uint8_t>(1, scale);
  int16_t cursor = x;
  for (const char character : value) {
    const auto& columns = glyph_for(character);
    for (uint8_t column = 0; column < columns.size(); ++column) {
      for (uint8_t row = 0; row < 7; ++row) {
        const bool glyph_pixel = (columns[column] & (1U << row)) != 0;
        const bool on = inverted ? !glyph_pixel : glyph_pixel;
        if (glyph_pixel || inverted) {
          for (uint8_t sx = 0; sx < safe_scale; ++sx) {
            for (uint8_t sy = 0; sy < safe_scale; ++sy) {
              pixel(static_cast<int16_t>(
                        cursor + column * safe_scale + sx),
                    static_cast<int16_t>(y + row * safe_scale + sy), on);
            }
          }
        }
      }
    }
    cursor = static_cast<int16_t>(cursor + 6 * safe_scale);
  }
}

int16_t Canvas::text_width(const std::string& value, uint8_t scale) const
{
  return static_cast<int16_t>(value.size() * 6 *
                              std::max<uint8_t>(1, scale));
}

MonoCanvas::MonoCanvas(uint16_t width, uint16_t height)
    : width_(width),
      height_(height),
      buffer_((static_cast<std::size_t>(width) * height + 7) / 8)
{
}

uint16_t MonoCanvas::width() const
{
  return width_;
}

uint16_t MonoCanvas::height() const
{
  return height_;
}

void MonoCanvas::clear(bool on)
{
  std::fill(buffer_.begin(), buffer_.end(), on ? 0xFF : 0x00);
}

void MonoCanvas::pixel(int16_t x, int16_t y, bool on)
{
  if (x < 0 || y < 0 || x >= static_cast<int16_t>(width_) ||
      y >= static_cast<int16_t>(height_)) {
    return;
  }
  const std::size_t bit =
      static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
  const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
  if (on) {
    buffer_[bit / 8] |= mask;
  } else {
    buffer_[bit / 8] &= static_cast<uint8_t>(~mask);
  }
}

bool MonoCanvas::pixel_at(int16_t x, int16_t y) const
{
  if (x < 0 || y < 0 || x >= static_cast<int16_t>(width_) ||
      y >= static_cast<int16_t>(height_)) {
    return false;
  }
  const std::size_t bit =
      static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
  return (buffer_[bit / 8] & (1U << (bit % 8))) != 0;
}

const std::vector<uint8_t>& MonoCanvas::buffer() const
{
  return buffer_;
}

std::vector<uint8_t>& MonoCanvas::buffer()
{
  return buffer_;
}

LayoutMetrics ResponsiveLayout::metrics(
    const DisplayCapabilities& capabilities)
{
  LayoutMetrics result{};
  result.density = capabilities.density();
  switch (result.density) {
    case DisplayDensity::Compact:
      result.margin = 1;
      result.header_height = 9;
      result.row_height = 9;
      result.columns = 1;
      result.font_scale = 1;
      break;
    case DisplayDensity::Medium:
      result.margin = 3;
      result.header_height = 12;
      result.row_height = 11;
      result.columns = capabilities.width >= 240 ? 2 : 1;
      result.font_scale = 1;
      break;
    case DisplayDensity::Large:
      result.margin = 6;
      result.header_height = 22;
      result.row_height = 20;
      result.columns = 2;
      result.font_scale = 2;
      break;
  }
  const int16_t available =
      static_cast<int16_t>(capabilities.height) - result.header_height -
      result.margin * 2;
  result.visible_rows = static_cast<uint8_t>(
      std::max<int16_t>(1, available / result.row_height) * result.columns);
  return result;
}

Rect ResponsiveLayout::content_rect(
    const DisplayCapabilities& capabilities, const LayoutMetrics& metrics)
{
  return {metrics.margin,
          static_cast<int16_t>(metrics.header_height + metrics.margin),
          static_cast<int16_t>(capabilities.width - metrics.margin * 2),
          static_cast<int16_t>(capabilities.height - metrics.header_height -
                               metrics.margin * 2)};
}

Rect ResponsiveLayout::field_rect(
    std::size_t visible_index, const DisplayCapabilities& capabilities,
    const LayoutMetrics& metrics)
{
  const Rect content = content_rect(capabilities, metrics);
  const std::size_t rows_per_column =
      std::max<std::size_t>(1, metrics.visible_rows / metrics.columns);
  const std::size_t column =
      std::min<std::size_t>(metrics.columns - 1,
                            visible_index / rows_per_column);
  const std::size_t row = visible_index % rows_per_column;
  const int16_t column_width =
      static_cast<int16_t>(content.width / metrics.columns);
  return {static_cast<int16_t>(content.x + column * column_width),
          static_cast<int16_t>(content.y + row * metrics.row_height),
          column_width, metrics.row_height};
}

UiController::UiController(IDisplaySink& display, MonoCanvas& canvas)
    : display_(display), canvas_(canvas)
{
}

void UiController::set_screen(UiScreen screen)
{
  if (screen.id == screen_.id) {
    const std::size_t previous_selection = selected_index_;
    const std::size_t previous_scroll = scroll_offset_;
    screen_ = std::move(screen);
    selected_index_ = screen_.fields.empty()
                          ? 0
                          : std::min(previous_selection,
                                     screen_.fields.size() - 1);
    scroll_offset_ = std::min(previous_scroll, selected_index_);
    if (screen_.fields.empty() ||
        !screen_.fields[selected_index_].editable) {
      editing_ = false;
    }
    return;
  }
  screen_ = std::move(screen);
  selected_index_ = 0;
  scroll_offset_ = 0;
  editing_ = false;
}

const UiScreen& UiController::screen() const
{
  return screen_;
}

void UiController::keep_selection_visible(const LayoutMetrics& metrics)
{
  if (selected_index_ < scroll_offset_) {
    scroll_offset_ = selected_index_;
  } else if (selected_index_ >= scroll_offset_ + metrics.visible_rows) {
    scroll_offset_ = selected_index_ - metrics.visible_rows + 1;
  }
}

bool UiController::handle(const UiEvent& event)
{
  if (screen_.fields.empty()) {
    return false;
  }
  auto find_visible = [this](int direction) {
    std::size_t candidate = selected_index_;
    while (true) {
      if (direction > 0) {
        if (candidate + 1 >= screen_.fields.size()) {
          break;
        }
        ++candidate;
      } else {
        if (candidate == 0) {
          break;
        }
        --candidate;
      }
      if (screen_.fields[candidate].visible) {
        selected_index_ = candidate;
        break;
      }
    }
  };

  auto& field = screen_.fields[selected_index_];
  switch (event.type) {
    case UiEventType::Up:
      if (editing_ && field.editable) {
        field.value =
            clamp<int32_t>(field.minimum, field.value + 1, field.maximum);
        field.value_text = number_string(field.value);
        pending_change_ = {screen_.id, field.id, field.value};
        change_pending_ = true;
      } else {
        find_visible(-1);
      }
      break;
    case UiEventType::Down:
      if (editing_ && field.editable) {
        field.value =
            clamp<int32_t>(field.minimum, field.value - 1, field.maximum);
        field.value_text = number_string(field.value);
        pending_change_ = {screen_.id, field.id, field.value};
        change_pending_ = true;
      } else {
        find_visible(1);
      }
      break;
    case UiEventType::Enter:
      if (field.kind == UiFieldKind::Action) {
        pending_change_ = {screen_.id, field.id, 1};
        change_pending_ = true;
      } else if (field.editable) {
        editing_ = !editing_;
      }
      break;
    case UiEventType::Back:
      editing_ = false;
      break;
    case UiEventType::Left:
    case UiEventType::Right:
    case UiEventType::Rotate:
      if (editing_ && field.editable) {
        const int32_t delta =
            event.type == UiEventType::Left
                ? -1
                : (event.type == UiEventType::Right
                       ? 1
                       : static_cast<int32_t>(event.value));
        field.value =
            clamp<int32_t>(field.minimum, field.value + delta, field.maximum);
        field.value_text = number_string(field.value);
        pending_change_ = {screen_.id, field.id, field.value};
        change_pending_ = true;
      }
      break;
    case UiEventType::TouchPress: {
      const auto metrics = ResponsiveLayout::metrics(
          display_.capabilities());
      for (std::size_t i = 0; i < metrics.visible_rows; ++i) {
        const Rect rect = ResponsiveLayout::field_rect(
            i, display_.capabilities(), metrics);
        if (event.x >= rect.x && event.x < rect.x + rect.width &&
            event.y >= rect.y && event.y < rect.y + rect.height) {
          const std::size_t index = scroll_offset_ + i;
          if (index < screen_.fields.size() && screen_.fields[index].visible) {
            selected_index_ = index;
            editing_ = screen_.fields[index].editable;
          }
          break;
        }
      }
      break;
    }
    default:
      return false;
  }
  keep_selection_visible(
      ResponsiveLayout::metrics(display_.capabilities()));
  return true;
}

void UiController::draw_header(const LayoutMetrics& metrics)
{
  canvas_.rectangle(
      {0, 0, static_cast<int16_t>(canvas_.width()), metrics.header_height},
      true, true);
  canvas_.text(metrics.margin, metrics.margin, screen_.title, true,
               metrics.font_scale);
}

void UiController::draw_field(const UiField& field, Rect rect, bool selected,
                              const LayoutMetrics& metrics)
{
  if (selected) {
    canvas_.rectangle(rect, true, true);
  }
  const bool inverted = selected;
  const int16_t y = static_cast<int16_t>(
      rect.y + std::max<int16_t>(0, (rect.height - 7 * metrics.font_scale) / 2));
  canvas_.text(static_cast<int16_t>(rect.x + 1), y, field.label, inverted,
               metrics.font_scale);

  std::string value = field.value_text;
  if (value.empty() && field.kind != UiFieldKind::Label) {
    value = number_string(field.value);
  }
  if (field.kind == UiFieldKind::Boolean) {
    value = field.value != 0 ? "ON" : "OFF";
  }
  if (field.kind == UiFieldKind::Progress) {
    const int16_t label_width =
        static_cast<int16_t>(rect.width / 2);
    const Rect gauge{
        static_cast<int16_t>(rect.x + label_width),
        static_cast<int16_t>(rect.y + 2),
        static_cast<int16_t>(rect.width - label_width - 2),
        static_cast<int16_t>(rect.height - 4)};
    canvas_.rectangle(gauge, false, !inverted);
    const int32_t range = std::max<int32_t>(1, field.maximum - field.minimum);
    const int16_t fill = static_cast<int16_t>(
        (gauge.width - 2) *
        clamp<int32_t>(0, field.value - field.minimum, range) / range);
    canvas_.rectangle(
        {static_cast<int16_t>(gauge.x + 1),
         static_cast<int16_t>(gauge.y + 1), fill,
         static_cast<int16_t>(gauge.height - 2)},
        true, !inverted);
  } else if (!value.empty()) {
    const int16_t width = canvas_.text_width(value, metrics.font_scale);
    canvas_.text(static_cast<int16_t>(rect.x + rect.width - width - 2), y,
                 value, inverted, metrics.font_scale);
  }
}

bool UiController::render()
{
  const auto& capabilities = display_.capabilities();
  if (canvas_.width() != capabilities.width ||
      canvas_.height() != capabilities.height) {
    return false;
  }
  const auto metrics = ResponsiveLayout::metrics(capabilities);
  keep_selection_visible(metrics);
  canvas_.clear(false);
  draw_header(metrics);

  std::size_t visible_slot = 0;
  for (std::size_t i = scroll_offset_;
       i < screen_.fields.size() && visible_slot < metrics.visible_rows; ++i) {
    if (!screen_.fields[i].visible) {
      continue;
    }
    const Rect rect =
        ResponsiveLayout::field_rect(visible_slot, capabilities, metrics);
    draw_field(screen_.fields[i], rect, i == selected_index_, metrics);
    ++visible_slot;
  }
  return display_.flush(canvas_);
}

bool UiController::take_change(UiChange& change)
{
  if (!change_pending_) {
    return false;
  }
  change = pending_change_;
  change_pending_ = false;
  return true;
}

std::size_t UiController::selected_index() const
{
  return selected_index_;
}

std::size_t UiController::scroll_offset() const
{
  return scroll_offset_;
}

bool UiController::editing() const
{
  return editing_;
}

UiScreen make_main_screen(const Model& model, const ChannelFrame& channels,
                          uint16_t battery_mv, uint8_t link_quality,
                          bool safety_enabled)
{
  UiScreen screen{};
  screen.id = "main";
  screen.title = model.name.data();
  screen.fields.push_back(
      {"safety", "OUTPUT", safety_enabled ? "LIVE" : "SAFE",
       UiFieldKind::Label, 0, 0, 0, false, true});
  screen.fields.push_back(
      {"battery", "BAT", std::to_string(battery_mv) + "MV",
       UiFieldKind::Label, battery_mv, 0, 0, false, true});
  screen.fields.push_back(
      {"lq", "LINK", std::to_string(link_quality) + "%",
       UiFieldKind::Progress, link_quality, 0, 100, false, true});
  for (std::size_t i = 0; i < 4; ++i) {
    screen.fields.push_back(
        {"ch" + std::to_string(i + 1), "CH" + std::to_string(i + 1), "",
         UiFieldKind::Progress, channels.channels[i], -kResolution,
         kResolution, false, true});
  }
  return screen;
}

UiScreen make_outputs_screen(const ChannelFrame& channels)
{
  UiScreen screen{};
  screen.id = "outputs";
  screen.title = "Outputs";
  for (std::size_t i = 0; i < channels.channels.size(); ++i) {
    screen.fields.push_back(
        {"ch" + std::to_string(i + 1), "CH" + std::to_string(i + 1), "",
         UiFieldKind::Progress, channels.channels[i], -kResolution,
         kResolution, false, true});
  }
  return screen;
}

UiScreen make_calibration_screen(uint8_t step, uint8_t progress)
{
  UiScreen screen{};
  screen.id = "calibration";
  screen.title = "Calibration";
  const char* instruction = "WAIT";
  switch (step) {
    case 1:
      instruction = "CENTER STICKS";
      break;
    case 2:
      instruction = "MOVE EXTREMES";
      break;
    case 3:
      instruction = "REVIEW";
      break;
    case 4:
      instruction = "SAVED";
      break;
    case 5:
      instruction = "CANCELLED";
      break;
    default:
      break;
  }
  screen.fields.push_back(
      {"step", "STEP", instruction,
       UiFieldKind::Label, step, 0, 0, false, true});
  screen.fields.push_back(
      {"progress", "PROGRESS", "", UiFieldKind::Progress, progress, 0, 100,
       false, true});
  screen.fields.push_back(
      {"hint", "ENTER NEXT", "BACK CANCEL", UiFieldKind::Label, 0, 0, 0,
       false, true});
  return screen;
}

UiScreen make_model_setup_screen(const Model& model)
{
  UiScreen screen{"model", "Model setup", {}};
  screen.fields.push_back(
      {"model_id", "MODEL ID", "", UiFieldKind::Number, model.model_id, 0,
       63, true, true});
  screen.fields.push_back(
      {"throttle_axis", "THR AXIS", "", UiFieldKind::Number,
       model.throttle_axis, 0, static_cast<int32_t>(kMaxAxes - 1), true,
       true});
  screen.fields.push_back(
      {"throttle_channel", "THR CH", "", UiFieldKind::Number,
       model.throttle_channel + 1, 1,
       static_cast<int32_t>(kChannelCount), true, true});
  return screen;
}

UiScreen make_inputs_screen(const Model& model)
{
  UiScreen screen{"inputs", "Inputs", {}};
  const auto count = std::min<std::size_t>(model.input_count, kMaxInputs);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& input = model.inputs[i];
    screen.fields.push_back(
        {"input." + std::to_string(i) + ".weight",
         "I" + std::to_string(i + 1) + " WEIGHT", "",
         UiFieldKind::Number, input.weight_percent, -100, 100, true, true});
    screen.fields.push_back(
        {"input." + std::to_string(i) + ".expo",
         "I" + std::to_string(i + 1) + " EXPO", "",
         UiFieldKind::Number, input.expo_percent, -100, 100, true, true});
  }
  return screen;
}

UiScreen make_mixes_screen(const Model& model)
{
  UiScreen screen{"mixes", "Mixes", {}};
  const auto count = std::min<std::size_t>(model.mix_count, kMaxMixes);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& mix = model.mixes[i];
    screen.fields.push_back(
        {"mix." + std::to_string(i) + ".weight",
         "M" + std::to_string(i + 1) + " CH" +
             std::to_string(mix.destination + 1),
         "", UiFieldKind::Number, mix.weight_percent, -200, 200, true,
         true});
    screen.fields.push_back(
        {"mix." + std::to_string(i) + ".offset",
         "M" + std::to_string(i + 1) + " OFFSET", "",
         UiFieldKind::Number, mix.offset, -kResolution, kResolution, true,
         true});
  }
  return screen;
}

UiScreen make_output_limits_screen(const Model& model)
{
  UiScreen screen{"limits", "Output limits", {}};
  for (std::size_t i = 0; i < model.outputs.size(); ++i) {
    const auto& output = model.outputs[i];
    const std::string prefix = "output." + std::to_string(i);
    const std::string label = "CH" + std::to_string(i + 1);
    screen.fields.push_back(
        {prefix + ".minimum", label + " MIN", "", UiFieldKind::Number,
         output.minimum, -1536, 0, true, true});
    screen.fields.push_back(
        {prefix + ".maximum", label + " MAX", "", UiFieldKind::Number,
         output.maximum, 0, 1536, true, true});
    screen.fields.push_back(
        {prefix + ".subtrim", label + " SUB", "", UiFieldKind::Number,
         output.subtrim, -512, 512, true, true});
    screen.fields.push_back(
        {prefix + ".reversed", label + " REV", "", UiFieldKind::Boolean,
         output.reversed ? 1 : 0, 0, 1, true, true});
  }
  return screen;
}

UiScreen make_flight_modes_screen(const Model& model)
{
  UiScreen screen{"flight_modes", "Flight modes", {}};
  const auto count =
      std::min<std::size_t>(model.flight_mode_count, kMaxFlightModes);
  for (std::size_t mode = 0; mode < count; ++mode) {
    for (std::size_t axis = 0; axis < 4; ++axis) {
      screen.fields.push_back(
          {"flight." + std::to_string(mode) + ".trim." +
               std::to_string(axis),
           "FM" + std::to_string(mode) + " TR" + std::to_string(axis + 1),
           "", UiFieldKind::Number, model.flight_modes[mode].trims[axis],
           -512, 512, true, true});
    }
  }
  return screen;
}

UiScreen make_curves_screen(const Model& model)
{
  UiScreen screen{"curves", "Curves", {}};
  const auto count = std::min<std::size_t>(model.curve_count, kMaxCurves);
  for (std::size_t curve = 0; curve < count; ++curve) {
    for (std::size_t point = 0; point < kCurvePoints; ++point) {
      screen.fields.push_back(
          {"curve." + std::to_string(curve) + ".point." +
               std::to_string(point),
           "CV" + std::to_string(curve + 1) + " P" +
               std::to_string(point + 1),
           "", UiFieldKind::Number, model.curves[curve].points[point],
           -kResolution, kResolution, true, true});
    }
  }
  return screen;
}

UiScreen make_logical_switches_screen(const Model& model)
{
  UiScreen screen{"logical", "Logical switches", {}};
  const auto count = std::min<std::size_t>(
      model.logical_switch_count, kMaxLogicalSwitches);
  for (std::size_t i = 0; i < count; ++i) {
    screen.fields.push_back(
        {"logical." + std::to_string(i) + ".threshold",
         "L" + std::to_string(i + 1) + " THRESH", "",
         UiFieldKind::Number, model.logical_switches[i].threshold,
         -kResolution, kResolution, true, true});
    screen.fields.push_back(
        {"logical." + std::to_string(i) + ".delay",
         "L" + std::to_string(i + 1) + " DELAY", "",
         UiFieldKind::Number, model.logical_switches[i].delay_ms, 0, 30000,
         true, true});
  }
  return screen;
}

UiScreen make_special_functions_screen(const Model& model)
{
  UiScreen screen{"special", "Special functions", {}};
  const auto count = std::min<std::size_t>(
      model.special_function_count, kMaxSpecialFunctions);
  for (std::size_t i = 0; i < count; ++i) {
    screen.fields.push_back(
        {"special." + std::to_string(i) + ".enabled",
         "SF" + std::to_string(i + 1), "", UiFieldKind::Boolean,
         model.special_functions[i].enabled ? 1 : 0, 0, 1, true, true});
    screen.fields.push_back(
        {"special." + std::to_string(i) + ".parameter",
         "SF" + std::to_string(i + 1) + " PARAM", "",
         UiFieldKind::Number, model.special_functions[i].parameter,
         INT16_MIN, INT16_MAX, true, true});
  }
  return screen;
}

UiScreen make_timers_screen(
    const Model& model,
    const std::array<TimerState, kMaxTimers>& states)
{
  UiScreen screen{"timers", "Timers", {}};
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    screen.fields.push_back(
        {"timer." + std::to_string(i) + ".start",
         "T" + std::to_string(i + 1) + " START", "",
         UiFieldKind::Number, model.timers[i].start_seconds, -359999,
         359999, true, true});
    screen.fields.push_back(
        {"timer." + std::to_string(i) + ".elapsed",
         "T" + std::to_string(i + 1) + " ELAPSED",
         std::to_string(states[i].elapsed_ms / 1000) + "S",
         UiFieldKind::Label, 0, 0, 0, false, true});
  }
  return screen;
}

UiScreen make_telemetry_screen(const std::vector<UiField>& sensors)
{
  UiScreen screen{"telemetry", "Telemetry", sensors};
  return screen;
}

UiScreen make_elrs_screen(const ElrsManagerStatus& status,
                          bool maintenance_allowed)
{
  UiScreen screen{"elrs", "ExpressLRS", {}};
  const auto state_text = [&]() -> std::string {
    switch (status.state) {
      case ElrsManagerState::Discovering:
        return "DISCOVERING";
      case ElrsManagerState::Ready:
        return "READY";
      case ElrsManagerState::Applying:
        return "APPLYING";
      case ElrsManagerState::CommandRunning:
        return "RUNNING";
      case ElrsManagerState::WifiUpdate:
        return "WIFI UPDATE";
      case ElrsManagerState::Unavailable:
        return "OFFLINE";
    }
    return "UNKNOWN";
  };
  const auto selection_text = [](const ElrsSelection& selection) {
    return selection.value < selection.option_count
               ? std::string(selection.options[selection.value].data())
               : std::string("?");
  };
  const bool ready = status.state == ElrsManagerState::Ready;
  screen.fields.push_back(
      {"state", "MODULE", state_text(), UiFieldKind::Label,
       0, 0, 0, false, true});
  if (status.device_name[0] != '\0') {
    screen.fields.push_back(
        {"device", "DEVICE", status.device_name.data(),
         UiFieldKind::Label, 0, 0, 0, false, true});
  }
  if (status.firmware_version != 0) {
    const uint8_t major =
        static_cast<uint8_t>((status.firmware_version >> 16U) & 0xFFU);
    const uint8_t minor =
        static_cast<uint8_t>((status.firmware_version >> 8U) & 0xFFU);
    const uint8_t patch =
        static_cast<uint8_t>(status.firmware_version & 0xFFU);
    screen.fields.push_back(
        {"firmware", "FIRMWARE",
         std::to_string(major) + "." + std::to_string(minor) + "." +
             std::to_string(patch),
         UiFieldKind::Label, 0, 0, 0, false, true});
  }
  if (status.power.available) {
    screen.fields.push_back(
        {"power", "MAX POWER", selection_text(status.power),
         UiFieldKind::Choice, status.power.value, status.power.minimum,
         status.power.maximum, ready, true});
  }
  if (status.dynamic_power.available) {
    screen.fields.push_back(
        {"dynamic", "DYNAMIC", selection_text(status.dynamic_power),
         UiFieldKind::Choice, status.dynamic_power.value,
         status.dynamic_power.minimum, status.dynamic_power.maximum,
         ready, true});
  }
  if (status.switch_mode.available) {
    screen.fields.push_back(
        {"switch_mode", "SWITCH MODE",
         selection_text(status.switch_mode), UiFieldKind::Choice,
         status.switch_mode.value, status.switch_mode.minimum,
         status.switch_mode.maximum, ready, true});
  }
  if (status.telemetry_ratio.available) {
    screen.fields.push_back(
        {"telemetry_ratio", "TELEM RATIO",
         selection_text(status.telemetry_ratio), UiFieldKind::Choice,
         status.telemetry_ratio.value, status.telemetry_ratio.minimum,
         status.telemetry_ratio.maximum, ready, true});
  }
  if (!maintenance_allowed) {
    screen.fields.push_back(
        {"lock", "ACTIONS", "LOCK OUTPUTS", UiFieldKind::Label,
         0, 0, 0, false, true});
  } else {
    if (status.bind_available) {
      screen.fields.push_back(
          {"bind", "BIND", "PRESS ENTER", UiFieldKind::Action,
           0, 0, 1, false, ready});
    }
    if (status.wifi_update_available) {
      screen.fields.push_back(
          {"wifi_update", "UPDATE ELRS", "START WIFI",
           UiFieldKind::Action, 0, 0, 1, false, ready});
    }
  }
  if (status.message[0] != '\0') {
    screen.fields.push_back(
        {"message", "STATUS", status.message.data(), UiFieldKind::Label,
         0, 0, 0, false, true});
  }
  if (status.state == ElrsManagerState::WifiUpdate) {
    screen.fields.push_back(
        {"wifi_ap", "WIFI", "ExpressLRS TX", UiFieldKind::Label,
         0, 0, 0, false, true});
    screen.fields.push_back(
        {"wifi_password", "PASSWORD", "expresslrs",
         UiFieldKind::Label, 0, 0, 0, false, true});
    screen.fields.push_back(
        {"wifi_url", "OPEN", "elrs_tx.local", UiFieldKind::Label,
         0, 0, 0, false, true});
    screen.fields.push_back(
        {"wifi_file", "UPLOAD", "firmware.bin", UiFieldKind::Label,
         0, 0, 0, false, true});
  }
  return screen;
}

UiScreen make_elrs_finder_screen(const ElrsFinderStatus& status)
{
  UiScreen screen{"elrs_finder", "ELRS Finder", {}};
  screen.fields.push_back(
      {"signal", "SIGNAL",
       status.signal_fresh ? "LIVE" : "NO TELEMETRY",
       UiFieldKind::Label, 0, 0, 0, false, true});
  screen.fields.push_back(
      {"rssi", "RSSI",
       status.signal_fresh
           ? std::to_string(status.filtered_rssi_dbm) + "dBm"
           : "---",
       UiFieldKind::Label, status.filtered_rssi_dbm, -140, 0, false,
       true});
  screen.fields.push_back(
      {"strength", "STRENGTH",
       std::to_string(status.strength_percent) + "%",
       UiFieldKind::Progress, status.strength_percent, 0, 100, false,
       true});
  screen.fields.push_back(
      {"audio", "GEIGER",
       status.audio_available ? "ON" : "NO BUZZER",
       UiFieldKind::Label, 0, 0, 0, false, true});
  screen.fields.push_back(
      {"hint", "SEARCH", "ROTATE SLOWLY", UiFieldKind::Label,
       0, 0, 0, false, true});
  return screen;
}

UiScreen make_system_screen(uint16_t battery_mv, uint32_t free_memory,
                            uint32_t missed_deadlines,
                            const std::string& version)
{
  UiScreen screen{"system", "Radio", {}};
  screen.fields.push_back(
      {"version", "VERSION", version, UiFieldKind::Label, 0, 0, 0, false,
       true});
  screen.fields.push_back(
      {"battery", "BATTERY", std::to_string(battery_mv) + "MV",
       UiFieldKind::Label, 0, 0, 0, false, true});
  screen.fields.push_back(
      {"memory", "FREE RAM", std::to_string(free_memory),
       UiFieldKind::Label, 0, 0, 0, false, true});
  screen.fields.push_back(
      {"deadlines", "DEADLINES", std::to_string(missed_deadlines),
       UiFieldKind::Label, 0, 0, 0, false, true});
  return screen;
}

namespace {

bool parse_indexed_field(const std::string& value, const std::string& prefix,
                         std::size_t& first, std::string& property,
                         std::size_t* second = nullptr)
{
  if (value.rfind(prefix + ".", 0) != 0) {
    return false;
  }
  std::stringstream stream(value.substr(prefix.size() + 1));
  std::string token;
  if (!std::getline(stream, token, '.')) {
    return false;
  }
  first = static_cast<std::size_t>(std::strtoul(token.c_str(), nullptr, 10));
  if (second != nullptr) {
    if (!std::getline(stream, property, '.') ||
        !std::getline(stream, token, '.')) {
      return false;
    }
    *second =
        static_cast<std::size_t>(std::strtoul(token.c_str(), nullptr, 10));
    property.clear();
    std::getline(stream, property, '.');
    return true;
  }
  return static_cast<bool>(std::getline(stream, property, '.'));
}

bool in_range(int32_t value, int32_t minimum, int32_t maximum)
{
  return value >= minimum && value <= maximum;
}

bool apply_model_change(Model& model, const UiChange& change)
{
  if (change.screen_id == "model") {
    if (change.field_id == "model_id") {
      if (!in_range(change.value, 0, UINT8_MAX)) return false;
      model.model_id = static_cast<uint8_t>(change.value);
      return true;
    }
    if (change.field_id == "throttle_axis") {
      if (!in_range(change.value, 0, kMaxAxes - 1)) return false;
      model.throttle_axis = static_cast<uint8_t>(change.value);
      return true;
    }
    if (change.field_id == "throttle_channel") {
      if (!in_range(change.value, 1, kChannelCount)) return false;
      model.throttle_channel = static_cast<uint8_t>(change.value - 1);
      return true;
    }
  }

  std::size_t index = 0;
  std::string property;
  if (parse_indexed_field(change.field_id, "input", index, property) &&
      index < model.input_count) {
    if (property == "weight") {
      if (!in_range(change.value, -100, 100)) return false;
      model.inputs[index].weight_percent =
          static_cast<int16_t>(change.value);
      return true;
    }
    if (property == "expo") {
      if (!in_range(change.value, -100, 100)) return false;
      model.inputs[index].expo_percent = static_cast<int8_t>(change.value);
      return true;
    }
  }
  if (parse_indexed_field(change.field_id, "mix", index, property) &&
      index < model.mix_count) {
    if (property == "weight") {
      if (!in_range(change.value, -100, 100)) return false;
      model.mixes[index].weight_percent =
          static_cast<int16_t>(change.value);
      return true;
    }
    if (property == "offset") {
      if (!in_range(change.value, -kResolution, kResolution)) return false;
      model.mixes[index].offset = static_cast<int16_t>(change.value);
      return true;
    }
  }
  if (parse_indexed_field(change.field_id, "output", index, property) &&
      index < model.outputs.size()) {
    auto& output = model.outputs[index];
    if (property == "minimum" &&
        in_range(change.value, -kResolution, kResolution)) {
      output.minimum = static_cast<int16_t>(change.value);
    } else if (property == "maximum" &&
               in_range(change.value, -kResolution, kResolution)) {
      output.maximum = static_cast<int16_t>(change.value);
    } else if (property == "subtrim" &&
               in_range(change.value, -kResolution, kResolution)) {
      output.subtrim = static_cast<int16_t>(change.value);
    } else if (property == "reversed" &&
               in_range(change.value, 0, 1)) {
      output.reversed = change.value != 0;
    }
    else return false;
    return output.minimum <= output.maximum;
  }
  if (parse_indexed_field(change.field_id, "logical", index, property) &&
      index < model.logical_switch_count) {
    if (property == "threshold") {
      if (!in_range(change.value, -kResolution, kResolution)) return false;
      model.logical_switches[index].threshold =
          static_cast<int16_t>(change.value);
      return true;
    }
    if (property == "delay") {
      if (!in_range(change.value, 0, UINT16_MAX)) return false;
      model.logical_switches[index].delay_ms =
          static_cast<uint16_t>(change.value);
      return true;
    }
  }
  if (parse_indexed_field(change.field_id, "special", index, property) &&
      index < model.special_function_count) {
    if (property == "enabled") {
      if (!in_range(change.value, 0, 1)) return false;
      model.special_functions[index].enabled = change.value != 0;
      return true;
    }
    if (property == "parameter") {
      if (!in_range(change.value, INT16_MIN, INT16_MAX)) return false;
      model.special_functions[index].parameter =
          static_cast<int16_t>(change.value);
      return true;
    }
  }
  if (parse_indexed_field(change.field_id, "timer", index, property) &&
      index < kMaxTimers && property == "start") {
    if (!in_range(change.value, -86400, 86400)) return false;
    model.timers[index].start_seconds = change.value;
    return true;
  }

  // Three-level fields are deliberately parsed separately to keep the
  // model-facing transaction explicit and range checked.
  std::stringstream stream(change.field_id);
  std::string group;
  std::string first;
  std::string middle;
  std::string second;
  if (std::getline(stream, group, '.') &&
      std::getline(stream, first, '.') &&
      std::getline(stream, middle, '.') &&
      std::getline(stream, second, '.')) {
    const std::size_t a =
        static_cast<std::size_t>(std::strtoul(first.c_str(), nullptr, 10));
    const std::size_t b =
        static_cast<std::size_t>(std::strtoul(second.c_str(), nullptr, 10));
    if (group == "flight" && middle == "trim" &&
        a < model.flight_mode_count && b < kMaxAxes) {
      if (!in_range(change.value, -kResolution, kResolution)) return false;
      model.flight_modes[a].trims[b] =
          static_cast<int16_t>(change.value);
      return true;
    }
    if (group == "curve" && middle == "point" &&
        a < model.curve_count && b < kCurvePoints) {
      if (!in_range(change.value, -kResolution, kResolution)) return false;
      model.curves[a].points[b] = static_cast<int16_t>(change.value);
      return true;
    }
  }
  return false;
}

}  // namespace

bool ModelEditor::apply(Model& model, const UiChange& change)
{
  Model candidate = model;
  if (!apply_model_change(candidate, change)) {
    return false;
  }
  model = candidate;
  return true;
}

}  // namespace rivettx
