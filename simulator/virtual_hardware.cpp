#include "virtual_hardware.hpp"

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

namespace rivettx::sim {

namespace {

constexpr uint32_t kCrsfBaud = 400000;
constexpr std::size_t kMaximumQueuedBytes = 4096;

void write_u16_be(uint8_t* output, uint16_t value)
{
  output[0] = static_cast<uint8_t>(value >> 8U);
  output[1] = static_cast<uint8_t>(value & 0xFFU);
}

void write_u24_be(uint8_t* output, uint32_t value)
{
  output[0] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  output[2] = static_cast<uint8_t>(value & 0xFFU);
}

void write_u32_be(uint8_t* output, uint32_t value)
{
  output[0] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
  output[1] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  output[2] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  output[3] = static_cast<uint8_t>(value & 0xFFU);
}

}  // namespace

VirtualElrsModule::VirtualElrsModule(LinkFaultPlan faults)
    : faults_(faults)
{
}

void VirtualElrsModule::set_fault_plan(LinkFaultPlan faults)
{
  faults_ = faults;
}

bool VirtualElrsModule::fault_window_active() const
{
  return faults_.disconnect_end_us > faults_.disconnect_start_us &&
         now_us_ >= faults_.disconnect_start_us &&
         now_us_ < faults_.disconnect_end_us;
}

bool VirtualElrsModule::link_available() const
{
  return baud_rate_ == kCrsfBaud && !fault_window_active();
}

void VirtualElrsModule::advance(TimeUs now_us)
{
  now_us_ = now_us;
  while (now_us_ >= next_link_statistics_us_) {
    if (link_available()) {
      enqueue_link_statistics();
    }
    next_link_statistics_us_ += 100000;
  }
  while (now_us_ >= next_battery_us_) {
    if (link_available()) {
      enqueue_battery();
    }
    next_battery_us_ += 500000;
  }
  while (now_us_ >= next_gps_us_) {
    if (link_available()) {
      enqueue_gps();
    }
    next_gps_us_ += 1000000;
  }
}

bool VirtualElrsModule::write(const uint8_t* data, std::size_t size)
{
  ++stats_.radio_write_attempts;
  if (!link_available()) {
    ++stats_.failed_writes;
    return false;
  }
  if (data == nullptr || size < 4 ||
      size > crsf::kMaximumFrameSize ||
      data[1] + 2U != size) {
    ++stats_.invalid_radio_frames;
    return false;
  }
  const uint8_t expected =
      crsf::crc8_dvb_s2(data + 2, size - 3);
  if (data[size - 1] != expected) {
    ++stats_.invalid_radio_frames;
    return false;
  }
  ++stats_.radio_frames_received;
  handle_radio_frame(data, size);
  return true;
}

void VirtualElrsModule::handle_radio_frame(const uint8_t* data,
                                           std::size_t size)
{
  const uint8_t type = data[2];
  if (type == crsf::kFrameRcChannelsPacked && size == 26) {
    uint32_t accumulator = 0;
    uint8_t available_bits = 0;
    std::size_t input = 3;
    for (auto& channel : channels_) {
      while (available_bits < 11 && input < size - 1) {
        accumulator |=
            static_cast<uint32_t>(data[input++]) << available_bits;
        available_bits = static_cast<uint8_t>(available_bits + 8);
      }
      const int32_t crsf_value =
          static_cast<int32_t>(accumulator & 0x7FFU);
      accumulator >>= 11U;
      available_bits = static_cast<uint8_t>(available_bits - 11);
      channel = static_cast<int16_t>(
          clamp<int32_t>(-kResolution,
                         ((crsf_value - 992) * kResolution) / 820,
                         kResolution));
    }
    ++stats_.channel_frames_received;
  } else if (type == crsf::kFrameCommand && size == 10 &&
             data[5] == 0x10 && data[6] == 0x05 &&
             data[8] == crsf::crc8_ba(data + 2, 6)) {
    model_id_ = data[7];
    ++stats_.model_id_frames_received;
  } else if (type == crsf::kFrameCommand && size == 9 &&
             data[5] == 0x10 && data[6] == 0x01 &&
             data[7] == crsf::crc8_ba(data + 2, 5)) {
    ++stats_.bind_commands_received;
  } else if (type == crsf::kFrameDevicePing) {
    ++stats_.device_pings_received;
    enqueue_device_info();
  }
}

std::size_t VirtualElrsModule::read(uint8_t* data, std::size_t capacity)
{
  if (data == nullptr || capacity == 0 || !link_available()) {
    return 0;
  }
  const std::size_t configured =
      faults_.maximum_read_chunk == 0
          ? capacity
          : faults_.maximum_read_chunk;
  const std::size_t count =
      std::min({capacity, configured, receive_queue_.size()});
  for (std::size_t i = 0; i < count; ++i) {
    data[i] = receive_queue_.front();
    receive_queue_.pop_front();
  }
  return count;
}

void VirtualElrsModule::set_baud_rate(uint32_t baud)
{
  baud_rate_ = baud;
}

void VirtualElrsModule::reset_module()
{
  ++stats_.resets;
  receive_queue_.clear();
  next_link_statistics_us_ = now_us_ + 100000;
  next_battery_us_ = now_us_ + 250000;
  next_gps_us_ = now_us_ + 500000;
}

void VirtualElrsModule::enqueue_frame(uint8_t type, const uint8_t* payload,
                                      std::size_t payload_size,
                                      bool telemetry)
{
  std::vector<uint8_t> frame(payload_size + 4);
  frame[0] = crsf::kAddressRadio;
  frame[1] = static_cast<uint8_t>(payload_size + 2);
  frame[2] = type;
  if (payload_size != 0) {
    std::copy(payload, payload + payload_size, frame.begin() + 3);
  }
  frame.back() =
      crsf::crc8_dvb_s2(frame.data() + 2, frame.size() - 3);

  if (telemetry) {
    ++stats_.telemetry_frames_generated;
    const uint32_t sequence = stats_.telemetry_frames_generated;
    if (faults_.drop_every_nth_telemetry_frame != 0 &&
        sequence % faults_.drop_every_nth_telemetry_frame == 0) {
      ++stats_.telemetry_frames_dropped;
      return;
    }
    if (faults_.corrupt_every_nth_telemetry_frame != 0 &&
        sequence % faults_.corrupt_every_nth_telemetry_frame == 0) {
      frame.back() ^= 0x01U;
      ++stats_.telemetry_frames_corrupted;
    }
  }

  if (receive_queue_.size() + frame.size() > kMaximumQueuedBytes) {
    ++stats_.queue_overflows;
    return;
  }
  receive_queue_.insert(receive_queue_.end(), frame.begin(), frame.end());
}

void VirtualElrsModule::enqueue_link_statistics()
{
  const std::array<uint8_t, 10> payload{
      50, 55, 96, 8, 0, 7, 3, 60, 94, 5};
  enqueue_frame(crsf::kFrameLinkStatistics, payload.data(), payload.size(),
                true);
}

void VirtualElrsModule::enqueue_battery()
{
  std::array<uint8_t, 8> payload{};
  write_u16_be(payload.data(), 38);
  write_u16_be(payload.data() + 2, 12);
  write_u24_be(payload.data() + 4, 500);
  payload[7] = 87;
  enqueue_frame(crsf::kFrameBattery, payload.data(), payload.size(), true);
}

void VirtualElrsModule::enqueue_gps()
{
  std::array<uint8_t, 15> payload{};
  write_u32_be(payload.data(), static_cast<uint32_t>(523672345));
  write_u32_be(payload.data() + 4, static_cast<uint32_t>(48912345));
  write_u16_be(payload.data() + 8, 72);
  write_u16_be(payload.data() + 10, 9000);
  write_u16_be(payload.data() + 12, 1123);
  payload[14] = 14;
  enqueue_frame(crsf::kFrameGps, payload.data(), payload.size(), true);
}

void VirtualElrsModule::enqueue_device_info()
{
  const std::array<uint8_t, 8> payload{
      crsf::kAddressRadio, crsf::kAddressModule,
      'V', 'E', 'L', 'R', 'S', 0};
  enqueue_frame(crsf::kFrameDeviceInfo, payload.data(), payload.size(),
                false);
}

const VirtualElrsStats& VirtualElrsModule::stats() const
{
  return stats_;
}

const std::array<int16_t, kChannelCount>&
VirtualElrsModule::channels() const
{
  return channels_;
}

uint8_t VirtualElrsModule::model_id() const
{
  return model_id_;
}

uint32_t VirtualElrsModule::baud_rate() const
{
  return baud_rate_;
}

PbmDisplay::PbmDisplay(DisplayCapabilities capabilities,
                       std::string output_path)
    : capabilities_(capabilities),
      output_path_(std::move(output_path))
{
}

const DisplayCapabilities& PbmDisplay::capabilities() const
{
  return capabilities_;
}

bool PbmDisplay::flush(const MonoCanvas& canvas)
{
  if (canvas.width() != capabilities_.width ||
      canvas.height() != capabilities_.height) {
    return false;
  }
  std::ofstream file(output_path_, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file << "P4\n" << canvas.width() << " " << canvas.height() << "\n";
  last_lit_pixels_ = 0;
  for (uint16_t y = 0; y < canvas.height(); ++y) {
    uint8_t byte = 0;
    uint8_t bits = 0;
    for (uint16_t x = 0; x < canvas.width(); ++x) {
      const bool on = canvas.pixel_at(x, y);
      last_lit_pixels_ += on ? 1U : 0U;
      byte = static_cast<uint8_t>((byte << 1U) | (on ? 1U : 0U));
      ++bits;
      if (bits == 8) {
        file.put(static_cast<char>(byte));
        byte = 0;
        bits = 0;
      }
    }
    if (bits != 0) {
      file.put(static_cast<char>(byte << (8U - bits)));
    }
  }
  ++flushes_;
  return file.good();
}

uint32_t PbmDisplay::flushes() const
{
  return flushes_;
}

std::size_t PbmDisplay::last_lit_pixels() const
{
  return last_lit_pixels_;
}

const std::string& PbmDisplay::output_path() const
{
  return output_path_;
}

}  // namespace rivettx::sim
