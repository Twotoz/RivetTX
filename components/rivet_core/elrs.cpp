#include "rivettx/elrs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace rivettx {

namespace {

constexpr uint8_t kCrsfTextSelection = 9;
constexpr uint8_t kCrsfCommand = 13;
constexpr uint8_t kCommandClick = 1;
constexpr uint8_t kCommandExecuting = 2;
constexpr uint8_t kCommandAskConfirm = 3;
constexpr uint8_t kCommandConfirmed = 4;
constexpr TimeUs kParameterTimeoutUs = 400000;
constexpr TimeUs kOfflineRetryUs = 2000000;
constexpr uint8_t kMaximumDevicePingRetries = 4;
constexpr TimeUs kFinderFreshnessUs = 1000000;
constexpr uint16_t kMaximumPacketRateAt400KBaud = 250;

std::size_t bounded_string_length(const uint8_t* data, std::size_t size)
{
  std::size_t length = 0;
  while (length < size && data[length] != 0) {
    ++length;
  }
  return length;
}

void copy_text(char* destination, std::size_t capacity,
               const uint8_t* source, std::size_t size)
{
  if (capacity == 0) {
    return;
  }
  const std::size_t count = std::min(capacity - 1, size);
  std::copy(source, source + count, destination);
  destination[count] = '\0';
}

uint16_t selection_rate_hz(const ElrsSelection& selection, uint8_t option)
{
  if (option >= selection.option_count) {
    return 0;
  }
  const char* label = selection.options[option].data();
  while (*label != '\0' &&
         !std::isdigit(static_cast<unsigned char>(*label))) {
    ++label;
  }
  uint32_t rate = 0;
  while (std::isdigit(static_cast<unsigned char>(*label))) {
    rate = rate * 10U + static_cast<uint32_t>(*label - '0');
    if (rate > UINT16_MAX) {
      return UINT16_MAX;
    }
    ++label;
  }
  return static_cast<uint16_t>(rate);
}

}  // namespace

ElrsDeviceManager::ElrsDeviceManager(ICrsfTransport& transport,
                                     CrsfParser& parser)
    : transport_(transport), parser_(parser)
{
}

void ElrsDeviceManager::set_message(const char* message)
{
  status_.message.fill('\0');
  if (message != nullptr) {
    const std::size_t length =
        std::min(status_.message.size() - 1, std::strlen(message));
    std::copy(message, message + length, status_.message.begin());
  }
}

void ElrsDeviceManager::start(TimeUs now_us)
{
  pending_action_.store(static_cast<uint8_t>(PendingAction::None),
                        std::memory_order_release);
  reset_discovery(now_us);
}

void ElrsDeviceManager::reset_discovery(TimeUs now_us)
{
  status_ = {};
  status_.state = ElrsManagerState::Discovering;
  set_message("Discovering ELRS...");
  field_data_size_ = 0;
  current_field_ = 0;
  current_chunk_ = 0;
  read_retries_ = 0;
  device_ping_retries_ = 0;
  packet_rate_field_id_ = 0;
  power_field_id_ = 0;
  dynamic_power_field_id_ = 0;
  switch_mode_field_id_ = 0;
  telemetry_ratio_field_id_ = 0;
  model_match_field_id_ = 0;
  bind_field_id_ = 0;
  wifi_field_id_ = 0;
  active_command_ = PendingAction::None;
  response_deadline_us_ = now_us + kParameterTimeoutUs;
  rediscover_at_us_ = 0;
  const auto ping = crsf::make_device_ping();
  (void)transport_.write(ping.bytes.data(), ping.size);
}

bool ElrsDeviceManager::send_read(uint8_t field_id, uint8_t chunk,
                                  TimeUs now_us)
{
  const auto frame = crsf::make_parameter_read_frame(
      crsf::kAddressModule, crsf::kAddressRadio, field_id, chunk);
  response_deadline_us_ = now_us + kParameterTimeoutUs;
  return transport_.write(frame.bytes.data(), frame.size);
}

bool ElrsDeviceManager::send_write(uint8_t field_id, const uint8_t* value,
                                   std::size_t value_size, TimeUs now_us)
{
  const auto frame = crsf::make_parameter_write_frame(
      crsf::kAddressModule, crsf::kAddressRadio, field_id, value,
      value_size);
  response_deadline_us_ = now_us + kParameterTimeoutUs;
  return frame.size != 0 &&
         transport_.write(frame.bytes.data(), frame.size);
}

void ElrsDeviceManager::consume_device_info(const crsf::Frame& frame,
                                            TimeUs now_us)
{
  if (frame.size < 22 || frame.bytes[4] != crsf::kAddressModule) {
    return;
  }
  const std::size_t name_start = 5;
  const std::size_t content_end = frame.size - 1;
  const std::size_t name_size = bounded_string_length(
      frame.bytes.data() + name_start, content_end - name_start);
  const std::size_t metadata = name_start + name_size + 1;
  if (metadata + 14 > content_end) {
    return;
  }
  copy_text(status_.device_name.data(), status_.device_name.size(),
            frame.bytes.data() + name_start, name_size);
  status_.state = ElrsManagerState::Discovering;
  status_.firmware_version =
      (static_cast<uint32_t>(frame.bytes[metadata + 8]) << 24U) |
      (static_cast<uint32_t>(frame.bytes[metadata + 9]) << 16U) |
      (static_cast<uint32_t>(frame.bytes[metadata + 10]) << 8U) |
      frame.bytes[metadata + 11];
  status_.field_count = frame.bytes[metadata + 12];
  status_.fields_discovered = 0;
  current_field_ = 1;
  current_chunk_ = 0;
  field_data_size_ = 0;
  read_retries_ = 0;
  if (status_.field_count == 0) {
    status_.state = ElrsManagerState::Ready;
    set_message("ELRS ready");
  } else {
    request_next_field(now_us);
  }
}

void ElrsDeviceManager::store_selection(ElrsSelection& destination,
                                        const uint8_t* data,
                                        std::size_t size)
{
  destination = {};
  const std::size_t options_size = bounded_string_length(data, size);
  if (options_size == size || options_size + 5 > size) {
    return;
  }
  const uint8_t* unit_start = data + options_size + 5;
  const std::size_t unit_size = bounded_string_length(
      unit_start, size - options_size - 5);
  std::size_t start = 0;
  while (start <= options_size &&
         destination.option_count < kMaximumElrsOptions) {
    std::size_t end = start;
    while (end < options_size && data[end] != ';') {
      ++end;
    }
    auto& label = destination.options[destination.option_count++];
    const std::size_t label_size =
        std::min(label.size() - 1, end - start);
    std::copy(data + start, data + start + label_size, label.begin());
    const std::size_t remaining = label.size() - 1 - label_size;
    const std::size_t copied_unit = std::min(remaining, unit_size);
    std::copy(unit_start, unit_start + copied_unit,
              label.begin() + label_size);
    label[label_size + copied_unit] = '\0';
    if (end == options_size) {
      break;
    }
    start = end + 1;
  }
  destination.value = data[options_size + 1];
  destination.minimum = data[options_size + 2];
  destination.maximum = data[options_size + 3];
  destination.available = destination.option_count != 0;
}

void ElrsDeviceManager::parse_parameter(uint8_t field_id,
                                        const uint8_t* data,
                                        std::size_t size,
                                        TimeUs now_us)
{
  if (size < 3) {
    return;
  }
  const uint8_t type = static_cast<uint8_t>(data[1] & 0x7FU);
  const std::size_t name_size =
      bounded_string_length(data + 2, size - 2);
  if (name_size == size - 2) {
    return;
  }
  std::array<char, 32> name{};
  copy_text(name.data(), name.size(), data + 2, name_size);
  const uint8_t* value = data + 3 + name_size;
  const std::size_t value_size = size - 3 - name_size;

  if (type == kCrsfTextSelection) {
    if (std::strcmp(name.data(), "Packet Rate") == 0) {
      packet_rate_field_id_ = field_id;
      store_selection(status_.packet_rate, value, value_size);
    } else if (std::strcmp(name.data(), "Max Power") == 0) {
      power_field_id_ = field_id;
      store_selection(status_.power, value, value_size);
    } else if (std::strcmp(name.data(), "Dynamic") == 0) {
      dynamic_power_field_id_ = field_id;
      store_selection(status_.dynamic_power, value, value_size);
    } else if (std::strcmp(name.data(), "Switch Mode") == 0) {
      switch_mode_field_id_ = field_id;
      store_selection(status_.switch_mode, value, value_size);
    } else if (std::strcmp(name.data(), "Telem Ratio") == 0) {
      telemetry_ratio_field_id_ = field_id;
      store_selection(status_.telemetry_ratio, value, value_size);
    } else if (std::strcmp(name.data(), "Model Match") == 0) {
      model_match_field_id_ = field_id;
      store_selection(status_.model_match, value, value_size);
    }
  } else if (type == kCrsfCommand) {
    if (std::strcmp(name.data(), "Bind") == 0) {
      bind_field_id_ = field_id;
      status_.bind_available = true;
    } else if (std::strcmp(name.data(), "Enable WiFi") == 0) {
      wifi_field_id_ = field_id;
      status_.wifi_update_available = true;
    }
    if (value_size >= 2 && active_command_ != PendingAction::None) {
      const uint8_t command_state = value[0];
      if (active_command_ == PendingAction::WifiUpdate &&
          command_state == kCommandAskConfirm) {
        const uint8_t confirmed = kCommandConfirmed;
        (void)send_write(field_id, &confirmed, 1, now_us);
        status_.state = ElrsManagerState::WifiUpdate;
        set_message("ELRS WiFi starting");
      } else if (command_state == kCommandExecuting) {
        status_.state =
            active_command_ == PendingAction::WifiUpdate
                ? ElrsManagerState::WifiUpdate
                : ElrsManagerState::CommandRunning;
        if (value_size > 2) {
          const std::size_t info_size =
              bounded_string_length(value + 2, value_size - 2);
          copy_text(status_.message.data(), status_.message.size(),
                    value + 2, info_size);
        }
      }
    }
  }
}

void ElrsDeviceManager::consume_parameter_entry(
    const crsf::Frame& frame, TimeUs now_us)
{
  if (frame.size < 9 || frame.bytes[4] != crsf::kAddressModule) {
    return;
  }
  const uint8_t field_id = frame.bytes[5];
  const uint8_t chunks_remaining = frame.bytes[6];
  const std::size_t chunk_size = frame.size - 8;
  const bool scanning_field = field_id == current_field_;
  const bool command_field =
      field_id == bind_field_id_ || field_id == wifi_field_id_;
  if (!scanning_field && !command_field) {
    return;
  }
  if (current_chunk_ == 0 || !scanning_field) {
    field_data_size_ = 0;
  }
  const std::size_t copy_size =
      std::min(field_data_.size() - field_data_size_, chunk_size);
  std::copy(frame.bytes.begin() + 7,
            frame.bytes.begin() + 7 + copy_size,
            field_data_.begin() + field_data_size_);
  field_data_size_ += copy_size;

  if (chunks_remaining != 0) {
    ++current_chunk_;
    (void)send_read(field_id, current_chunk_, now_us);
    return;
  }
  parse_parameter(field_id, field_data_.data(), field_data_size_, now_us);

  if (scanning_field &&
      status_.state == ElrsManagerState::Discovering) {
    ++status_.fields_discovered;
    ++current_field_;
    current_chunk_ = 0;
    field_data_size_ = 0;
    read_retries_ = 0;
    request_next_field(now_us);
  }
}

void ElrsDeviceManager::consume_frame(const crsf::Frame& frame,
                                      TimeUs now_us)
{
  if (frame.size < 4) {
    return;
  }
  if (frame.bytes[2] == crsf::kFrameDeviceInfo &&
      (status_.state == ElrsManagerState::Discovering ||
       status_.state == ElrsManagerState::Unavailable) &&
      current_field_ == 0) {
    consume_device_info(frame, now_us);
  } else if (frame.bytes[2] == crsf::kFrameParameterEntry) {
    consume_parameter_entry(frame, now_us);
  }
}

void ElrsDeviceManager::request_next_field(TimeUs now_us)
{
  if (current_field_ == 0 || current_field_ > status_.field_count) {
    status_.state = ElrsManagerState::Ready;
    set_message("ELRS ready");
    return;
  }
  (void)send_read(current_field_, current_chunk_, now_us);
}

void ElrsDeviceManager::schedule_rediscovery(TimeUs now_us)
{
  status_.state = ElrsManagerState::Applying;
  set_message("Applying...");
  rediscover_at_us_ = now_us + 150000;
}

void ElrsDeviceManager::process_pending(TimeUs now_us)
{
  const auto action = static_cast<PendingAction>(
      pending_action_.exchange(static_cast<uint8_t>(PendingAction::None),
                               std::memory_order_acq_rel));
  if (action == PendingAction::None) {
    return;
  }
  if (status_.state != ElrsManagerState::Ready) {
    set_message("ELRS busy");
    return;
  }
  const uint8_t value = pending_value_.load(std::memory_order_acquire);
  uint8_t field_id = 0;
  switch (action) {
    case PendingAction::SetPacketRate: {
      const uint16_t rate = selection_rate_hz(status_.packet_rate, value);
      if (rate == 0 || rate > kMaximumPacketRateAt400KBaud) {
        set_message("400K UART: max 250Hz");
        return;
      }
      field_id = packet_rate_field_id_;
      break;
    }
    case PendingAction::SetPower:
      field_id = power_field_id_;
      break;
    case PendingAction::SetDynamicPower:
      field_id = dynamic_power_field_id_;
      break;
    case PendingAction::SetSwitchMode:
      field_id = switch_mode_field_id_;
      break;
    case PendingAction::SetTelemetryRatio:
      field_id = telemetry_ratio_field_id_;
      break;
    case PendingAction::SetModelMatch:
      field_id = model_match_field_id_;
      break;
    case PendingAction::Bind:
      field_id = bind_field_id_;
      break;
    case PendingAction::WifiUpdate:
      field_id = wifi_field_id_;
      break;
    case PendingAction::None:
      break;
  }
  if (field_id == 0) {
    set_message("Feature unavailable");
    return;
  }
  if (action == PendingAction::Bind ||
      action == PendingAction::WifiUpdate) {
    const uint8_t click = kCommandClick;
    if (send_write(field_id, &click, 1, now_us)) {
      active_command_ = action;
      status_.state = ElrsManagerState::CommandRunning;
      command_deadline_us_ = now_us + 3000000;
      set_message(action == PendingAction::Bind ? "Binding..."
                                                : "Starting WiFi...");
    }
  } else if (send_write(field_id, &value, 1, now_us)) {
    schedule_rediscovery(now_us);
  }
}

void ElrsDeviceManager::tick(TimeUs now_us)
{
  crsf::Frame frame{};
  while (parser_.pop_management_frame(frame)) {
    consume_frame(frame, now_us);
  }
  const TimeUs last_frame = parser_.last_valid_frame_us();
  if (last_frame != 0 && now_us >= last_frame &&
      now_us - last_frame > kOfflineRetryUs &&
      status_.state != ElrsManagerState::Unavailable &&
      status_.state != ElrsManagerState::Discovering) {
    status_.state = ElrsManagerState::Unavailable;
    status_.fields_discovered = 0;
    status_.packet_rate.available = false;
    status_.power.available = false;
    status_.dynamic_power.available = false;
    status_.switch_mode.available = false;
    status_.telemetry_ratio.available = false;
    status_.model_match.available = false;
    status_.bind_available = false;
    status_.wifi_update_available = false;
    response_deadline_us_ = now_us;
    set_message("ELRS module offline");
  }
  if (rediscover_at_us_ != 0 && now_us >= rediscover_at_us_) {
    reset_discovery(now_us);
    return;
  }
  if (status_.state == ElrsManagerState::Discovering &&
      now_us >= response_deadline_us_) {
    if (current_field_ == 0) {
      if (device_ping_retries_ >= kMaximumDevicePingRetries) {
        status_.state = ElrsManagerState::Unavailable;
        set_message("ELRS module offline");
        response_deadline_us_ = now_us + kOfflineRetryUs;
        return;
      }
      ++device_ping_retries_;
      const auto ping = crsf::make_device_ping();
      (void)transport_.write(ping.bytes.data(), ping.size);
    } else if (read_retries_ < 2) {
      ++read_retries_;
      (void)send_read(current_field_, current_chunk_, now_us);
      return;
    } else {
      ++current_field_;
      current_chunk_ = 0;
      field_data_size_ = 0;
      read_retries_ = 0;
      request_next_field(now_us);
      return;
    }
    response_deadline_us_ = now_us + kParameterTimeoutUs;
  }
  if (status_.state == ElrsManagerState::Unavailable &&
      now_us >= response_deadline_us_) {
    const auto ping = crsf::make_device_ping();
    (void)transport_.write(ping.bytes.data(), ping.size);
    response_deadline_us_ = now_us + kOfflineRetryUs;
  }
  if (status_.state == ElrsManagerState::CommandRunning &&
      now_us >= command_deadline_us_) {
    status_.state = ElrsManagerState::Ready;
    active_command_ = PendingAction::None;
    set_message("ELRS ready");
  }
  process_pending(now_us);
}

bool ElrsDeviceManager::request_packet_rate(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(
      static_cast<uint8_t>(PendingAction::SetPacketRate),
      std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_power(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(static_cast<uint8_t>(PendingAction::SetPower),
                        std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_dynamic_power(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(
      static_cast<uint8_t>(PendingAction::SetDynamicPower),
      std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_switch_mode(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(static_cast<uint8_t>(PendingAction::SetSwitchMode),
                        std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_telemetry_ratio(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(
      static_cast<uint8_t>(PendingAction::SetTelemetryRatio),
      std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_model_match(uint8_t option)
{
  pending_value_.store(option, std::memory_order_release);
  pending_action_.store(
      static_cast<uint8_t>(PendingAction::SetModelMatch),
      std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_bind()
{
  pending_action_.store(static_cast<uint8_t>(PendingAction::Bind),
                        std::memory_order_release);
  return true;
}

bool ElrsDeviceManager::request_wifi_update()
{
  pending_action_.store(static_cast<uint8_t>(PendingAction::WifiUpdate),
                        std::memory_order_release);
  return true;
}

const ElrsManagerStatus& ElrsDeviceManager::status() const
{
  return status_;
}

ElrsFinder::ElrsFinder(IToneOutput& tones) : tones_(tones)
{
  status_.audio_available = tones.available();
}

void ElrsFinder::set_active(bool active)
{
  if (status_.active == active) {
    return;
  }
  status_.active = active;
  status_.signal_fresh = false;
  next_beep_us_ = 0;
  filter_initialized_ = false;
  if (!active) {
    tones_.stop_tone();
  }
}

void ElrsFinder::tick(const TelemetryRegistry& telemetry, TimeUs now_us)
{
  status_.audio_available = tones_.available();
  if (!status_.active) {
    return;
  }
  const TelemetryEntry* rssi = telemetry.find(crsf::SensorUplinkRssi);
  status_.signal_fresh =
      rssi != nullptr && rssi->discovered &&
      now_us >= rssi->updated_at_us &&
      now_us - rssi->updated_at_us <= kFinderFreshnessUs;
  if (!status_.signal_fresh) {
    status_.strength_percent = 0;
    status_.beep_period_ms = 0;
    tones_.stop_tone();
    return;
  }

  status_.raw_rssi_dbm = static_cast<int16_t>(
      clamp<int32_t>(-140, rssi->value, 0));
  if (!filter_initialized_) {
    status_.filtered_rssi_dbm = status_.raw_rssi_dbm;
    filter_initialized_ = true;
  } else {
    status_.filtered_rssi_dbm = static_cast<int16_t>(
        (static_cast<int32_t>(status_.filtered_rssi_dbm) * 4 +
         status_.raw_rssi_dbm) /
        5);
  }
  status_.strength_percent = static_cast<uint8_t>(
      clamp<int32_t>(0, (status_.filtered_rssi_dbm + 120) * 100 / 80,
                     100));
  status_.beep_period_ms = static_cast<uint16_t>(
      1200 - static_cast<uint16_t>(status_.strength_percent) * 11);
  if (next_beep_us_ == 0 || now_us >= next_beep_us_) {
    const uint16_t frequency = static_cast<uint16_t>(
        600 + static_cast<uint16_t>(status_.strength_percent) * 6);
    (void)tones_.play_tone(frequency, 30);
    next_beep_us_ =
        now_us + static_cast<TimeUs>(status_.beep_period_ms) * 1000;
  }
}

const ElrsFinderStatus& ElrsFinder::status() const
{
  return status_;
}

}  // namespace rivettx
