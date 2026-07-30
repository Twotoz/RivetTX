#include "rivettx/crsf.hpp"

#include <algorithm>
#include <cstdlib>

namespace rivettx {
namespace crsf {

namespace {

uint8_t crc8_polynomial(const uint8_t* data, std::size_t size,
                        uint8_t polynomial)
{
  uint8_t crc = 0;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80U) != 0
                ? static_cast<uint8_t>((crc << 1U) ^ polynomial)
                : static_cast<uint8_t>(crc << 1U);
    }
  }
  return crc;
}

}  // namespace

uint8_t crc8_dvb_s2(const uint8_t* data, std::size_t size)
{
  return crc8_polynomial(data, size, 0xD5);
}

uint8_t crc8_ba(const uint8_t* data, std::size_t size)
{
  return crc8_polynomial(data, size, 0xBA);
}

Frame make_channels_frame(const ChannelFrame& channels)
{
  Frame frame{};
  frame.bytes[0] = kAddressModule;
  frame.bytes[1] = 24;
  frame.bytes[2] = kFrameRcChannelsPacked;

  uint32_t accumulator = 0;
  uint8_t available_bits = 0;
  std::size_t output = 3;
  for (const auto channel : channels.channels) {
    const int32_t crsf_value =
        clamp<int32_t>(172, 992 + (static_cast<int32_t>(channel) * 820) /
                                      kResolution,
                       1811);
    accumulator |= static_cast<uint32_t>(crsf_value) << available_bits;
    available_bits = static_cast<uint8_t>(available_bits + 11);
    while (available_bits >= 8) {
      frame.bytes[output++] = static_cast<uint8_t>(accumulator & 0xFFU);
      accumulator >>= 8U;
      available_bits = static_cast<uint8_t>(available_bits - 8);
    }
  }
  const uint8_t crc =
      crc8_dvb_s2(frame.bytes.data() + 2, output - 2);
  frame.bytes[output++] = crc;
  frame.size = static_cast<uint8_t>(output);
  return frame;
}

Frame make_model_id_frame(uint8_t model_id)
{
  Frame frame{};
  frame.bytes[0] = kAddressModule;
  frame.bytes[1] = 8;
  frame.bytes[2] = kFrameCommand;
  frame.bytes[3] = kAddressModule;
  frame.bytes[4] = kAddressRadio;
  frame.bytes[5] = 0x10;
  frame.bytes[6] = 0x05;
  frame.bytes[7] = model_id;
  frame.bytes[8] = crc8_ba(frame.bytes.data() + 2, 6);
  frame.bytes[9] = crc8_dvb_s2(frame.bytes.data() + 2, 7);
  frame.size = 10;
  return frame;
}

Frame make_bind_frame(bool unbind)
{
  Frame frame{};
  frame.bytes[0] = kAddressModule;
  frame.bytes[1] = 7;
  frame.bytes[2] = kFrameCommand;
  frame.bytes[3] = unbind ? 0xEC : kAddressModule;
  frame.bytes[4] = kAddressRadio;
  frame.bytes[5] = 0x10;
  frame.bytes[6] = 0x01;
  frame.bytes[7] = crc8_ba(frame.bytes.data() + 2, 5);
  frame.bytes[8] = crc8_dvb_s2(frame.bytes.data() + 2, 6);
  frame.size = 9;
  return frame;
}

Frame make_device_ping()
{
  Frame frame{};
  frame.bytes[0] = kAddressModule;
  frame.bytes[1] = 4;
  frame.bytes[2] = kFrameDevicePing;
  frame.bytes[3] = kAddressBroadcast;
  frame.bytes[4] = kAddressRadio;
  frame.bytes[5] = crc8_dvb_s2(frame.bytes.data() + 2, 3);
  frame.size = 6;
  return frame;
}

}  // namespace crsf

bool TelemetryRegistry::value(uint16_t sensor_id, int32_t& result) const
{
  const auto* entry = find(sensor_id);
  if (entry == nullptr || !entry->discovered) {
    return false;
  }
  result = entry->value;
  return true;
}

void TelemetryRegistry::update(uint16_t sensor_id, int32_t value,
                               TelemetryUnit unit, TimeUs now_us)
{
  TelemetryEntry* available = nullptr;
  for (auto& entry : entries_) {
    if (entry.discovered && entry.id == sensor_id) {
      available = &entry;
      break;
    }
    if (!entry.discovered && available == nullptr) {
      available = &entry;
    }
  }
  if (available == nullptr) {
    return;
  }
  if (!available->discovered) {
    available->id = sensor_id;
    available->minimum = value;
    available->maximum = value;
    available->discovered = true;
  } else {
    available->minimum = std::min(available->minimum, value);
    available->maximum = std::max(available->maximum, value);
  }
  available->value = value;
  available->unit = unit;
  available->updated_at_us = now_us;
}

const TelemetryEntry* TelemetryRegistry::find(uint16_t sensor_id) const
{
  for (const auto& entry : entries_) {
    if (entry.discovered && entry.id == sensor_id) {
      return &entry;
    }
  }
  return nullptr;
}

const std::array<TelemetryEntry, kMaxTelemetrySensors>&
TelemetryRegistry::entries() const
{
  return entries_;
}

void TelemetryRegistry::clear()
{
  entries_ = {};
}

CrsfParser::CrsfParser(TelemetryRegistry& telemetry) : telemetry_(telemetry)
{
}

void CrsfParser::reset()
{
  position_ = 0;
  expected_size_ = 0;
}

uint16_t CrsfParser::read_u16_be(const uint8_t* data)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) |
                               data[1]);
}

uint32_t CrsfParser::read_u24_be(const uint8_t* data)
{
  return (static_cast<uint32_t>(data[0]) << 16U) |
         (static_cast<uint32_t>(data[1]) << 8U) | data[2];
}

int32_t CrsfParser::read_i32_be(const uint8_t* data)
{
  const uint32_t value = (static_cast<uint32_t>(data[0]) << 24U) |
                         (static_cast<uint32_t>(data[1]) << 16U) |
                         (static_cast<uint32_t>(data[2]) << 8U) | data[3];
  return static_cast<int32_t>(value);
}

bool CrsfParser::feed(uint8_t byte, TimeUs now_us)
{
  if (position_ == 0) {
    buffer_[position_++] = byte;
    return false;
  }
  if (position_ == 1) {
    if (byte < 2 || byte > crsf::kMaximumFrameSize - 2) {
      ++stats_.length_errors;
      reset();
      return false;
    }
    buffer_[position_++] = byte;
    expected_size_ = static_cast<uint8_t>(byte + 2);
    return false;
  }
  if (position_ >= buffer_.size()) {
    ++stats_.dropped_bytes;
    reset();
    return false;
  }

  buffer_[position_++] = byte;
  if (position_ < expected_size_) {
    return false;
  }

  const uint8_t payload_length = buffer_[1];
  const uint8_t expected_crc = buffer_[expected_size_ - 1];
  const uint8_t actual_crc =
      crsf::crc8_dvb_s2(buffer_.data() + 2, payload_length - 1);
  if (expected_crc != actual_crc) {
    ++stats_.crc_errors;
    reset();
    return false;
  }

  ++stats_.valid_frames;
  last_valid_frame_us_ = now_us;
  last_frame_type_ = buffer_[2];
  const uint32_t write = received_write_.load(std::memory_order_relaxed);
  const uint32_t read = received_read_.load(std::memory_order_acquire);
  if (write - read < received_frames_.size()) {
    crsf::Frame& received =
        received_frames_[write % received_frames_.size()];
    received.size = expected_size_;
    std::copy(buffer_.begin(), buffer_.begin() + expected_size_,
              received.bytes.begin());
    received_write_.store(write + 1, std::memory_order_release);
  } else {
    ++stats_.dropped_bytes;
  }
  process_frame(now_us);
  reset();
  return true;
}

void CrsfParser::process_frame(TimeUs now_us)
{
  const uint8_t type = buffer_[2];
  const uint8_t payload_size =
      buffer_[1] >= 2 ? static_cast<uint8_t>(buffer_[1] - 2) : 0;
  const uint8_t* payload = buffer_.data() + 3;

  if (type == crsf::kFrameLinkStatistics && payload_size >= 10) {
    telemetry_.update(crsf::SensorUplinkRssi1, -payload[0],
                      TelemetryUnit::Dbm, now_us);
    telemetry_.update(crsf::SensorUplinkRssi2, -payload[1],
                      TelemetryUnit::Dbm, now_us);
    telemetry_.update(crsf::SensorUplinkLinkQuality, payload[2],
                      TelemetryUnit::Percent, now_us);
    telemetry_.update(crsf::SensorUplinkSnr,
                      static_cast<int8_t>(payload[3]), TelemetryUnit::Db,
                      now_us);
    telemetry_.update(crsf::SensorRfMode, payload[5], TelemetryUnit::Raw,
                      now_us);
    telemetry_.update(crsf::SensorTxPower, payload[6], TelemetryUnit::Raw,
                      now_us);
    telemetry_.update(crsf::SensorDownlinkRssi, -payload[7],
                      TelemetryUnit::Dbm, now_us);
    telemetry_.update(crsf::SensorDownlinkLinkQuality, payload[8],
                      TelemetryUnit::Percent, now_us);
    telemetry_.update(crsf::SensorDownlinkSnr,
                      static_cast<int8_t>(payload[9]), TelemetryUnit::Db,
                      now_us);
  } else if (type == crsf::kFrameBattery && payload_size >= 8) {
    telemetry_.update(crsf::SensorBatteryVoltage,
                      static_cast<int32_t>(read_u16_be(payload)) * 100,
                      TelemetryUnit::Millivolt, now_us);
    telemetry_.update(crsf::SensorBatteryCurrent,
                      static_cast<int32_t>(read_u16_be(payload + 2)) * 100,
                      TelemetryUnit::Milliamp, now_us);
    telemetry_.update(crsf::SensorBatteryCapacity,
                      static_cast<int32_t>(read_u24_be(payload + 4)),
                      TelemetryUnit::MilliampHour, now_us);
    telemetry_.update(crsf::SensorBatteryRemaining, payload[7],
                      TelemetryUnit::Percent, now_us);
  } else if (type == crsf::kFrameGps && payload_size >= 15) {
    telemetry_.update(crsf::SensorGpsLatitude, read_i32_be(payload),
                      TelemetryUnit::DegreesE7, now_us);
    telemetry_.update(crsf::SensorGpsLongitude, read_i32_be(payload + 4),
                      TelemetryUnit::DegreesE7, now_us);
    telemetry_.update(crsf::SensorGpsSpeed,
                      static_cast<int32_t>(read_u16_be(payload + 8)) * 25 /
                          9,
                      TelemetryUnit::CentimetersPerSecond, now_us);
    telemetry_.update(crsf::SensorGpsHeading, read_u16_be(payload + 10),
                      TelemetryUnit::Raw, now_us);
    telemetry_.update(crsf::SensorGpsAltitude,
                      (static_cast<int32_t>(read_u16_be(payload + 12)) -
                       1000) *
                          100,
                      TelemetryUnit::Centimeters, now_us);
    telemetry_.update(crsf::SensorGpsSatellites, payload[14],
                      TelemetryUnit::Raw, now_us);
  }
}

const CrsfParserStats& CrsfParser::stats() const
{
  return stats_;
}

TimeUs CrsfParser::last_valid_frame_us() const
{
  return last_valid_frame_us_;
}

uint8_t CrsfParser::last_frame_type() const
{
  return last_frame_type_;
}

bool CrsfParser::pop_frame(crsf::Frame& frame)
{
  const uint32_t read = received_read_.load(std::memory_order_relaxed);
  const uint32_t write = received_write_.load(std::memory_order_acquire);
  if (read == write) {
    return false;
  }
  frame = received_frames_[read % received_frames_.size()];
  received_read_.store(read + 1, std::memory_order_release);
  return true;
}

}  // namespace rivettx
