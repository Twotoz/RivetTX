#pragma once

#include "rivettx/core.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rivettx {

namespace crsf {

constexpr uint8_t kAddressBroadcast = 0x00;
constexpr uint8_t kAddressModule = 0xEE;
constexpr uint8_t kAddressRadio = 0xEA;
constexpr uint8_t kFrameGps = 0x02;
constexpr uint8_t kFrameBattery = 0x08;
constexpr uint8_t kFrameLinkStatistics = 0x14;
constexpr uint8_t kFrameRcChannelsPacked = 0x16;
constexpr uint8_t kFrameDevicePing = 0x28;
constexpr uint8_t kFrameDeviceInfo = 0x29;
constexpr uint8_t kFrameParameterEntry = 0x2B;
constexpr uint8_t kFrameParameterRead = 0x2C;
constexpr uint8_t kFrameParameterWrite = 0x2D;
constexpr uint8_t kFrameCommand = 0x32;
constexpr std::size_t kMaximumFrameSize = 64;

enum SensorId : uint16_t {
  SensorUplinkRssi1 = 1,
  SensorUplinkRssi2,
  SensorUplinkLinkQuality,
  SensorUplinkSnr,
  SensorRfMode,
  SensorTxPower,
  SensorDownlinkRssi,
  SensorDownlinkLinkQuality,
  SensorDownlinkSnr,
  SensorBatteryVoltage,
  SensorBatteryCurrent,
  SensorBatteryCapacity,
  SensorBatteryRemaining,
  SensorGpsLatitude,
  SensorGpsLongitude,
  SensorGpsSpeed,
  SensorGpsHeading,
  SensorGpsAltitude,
  SensorGpsSatellites,
};

uint8_t crc8_dvb_s2(const uint8_t* data, std::size_t size);
uint8_t crc8_ba(const uint8_t* data, std::size_t size);

struct Frame {
  std::array<uint8_t, kMaximumFrameSize> bytes{};
  uint8_t size = 0;
};

Frame make_channels_frame(const ChannelFrame& channels);
Frame make_model_id_frame(uint8_t model_id);
Frame make_bind_frame(bool unbind);
Frame make_device_ping();

}  // namespace crsf

enum class TelemetryUnit : uint8_t {
  Raw,
  Percent,
  Dbm,
  Db,
  Millivolt,
  Milliamp,
  MilliampHour,
  DegreesE7,
  Centimeters,
  CentimetersPerSecond,
};

struct TelemetryEntry {
  uint16_t id = 0;
  int32_t value = 0;
  int32_t minimum = 0;
  int32_t maximum = 0;
  TimeUs updated_at_us = 0;
  TelemetryUnit unit = TelemetryUnit::Raw;
  bool discovered = false;
};

class TelemetryRegistry final : public ITelemetrySource {
 public:
  bool value(uint16_t sensor_id, int32_t& result) const override;
  void update(uint16_t sensor_id, int32_t value, TelemetryUnit unit,
              TimeUs now_us);
  const TelemetryEntry* find(uint16_t sensor_id) const;
  const std::array<TelemetryEntry, kMaxTelemetrySensors>& entries() const;
  void clear();

 private:
  std::array<TelemetryEntry, kMaxTelemetrySensors> entries_{};
};

struct CrsfParserStats {
  uint32_t valid_frames = 0;
  uint32_t crc_errors = 0;
  uint32_t length_errors = 0;
  uint32_t dropped_bytes = 0;
};

class CrsfParser {
 public:
  explicit CrsfParser(TelemetryRegistry& telemetry);

  bool feed(uint8_t byte, TimeUs now_us);
  void reset();
  const CrsfParserStats& stats() const;
  TimeUs last_valid_frame_us() const;
  uint8_t last_frame_type() const;
  bool pop_frame(crsf::Frame& frame);

 private:
  void process_frame(TimeUs now_us);
  static uint16_t read_u16_be(const uint8_t* data);
  static uint32_t read_u24_be(const uint8_t* data);
  static int32_t read_i32_be(const uint8_t* data);

  TelemetryRegistry& telemetry_;
  std::array<uint8_t, crsf::kMaximumFrameSize> buffer_{};
  uint8_t position_ = 0;
  uint8_t expected_size_ = 0;
  uint8_t last_frame_type_ = 0;
  TimeUs last_valid_frame_us_ = 0;
  CrsfParserStats stats_{};
  std::array<crsf::Frame, 4> received_frames_{};
  std::atomic<uint32_t> received_read_{0};
  std::atomic<uint32_t> received_write_{0};
};

class ICrsfTransport {
 public:
  virtual ~ICrsfTransport() = default;
  virtual bool write(const uint8_t* data, std::size_t size) = 0;
  virtual std::size_t read(uint8_t* data, std::size_t capacity) = 0;
  virtual void set_baud_rate(uint32_t baud) = 0;
  virtual void reset_module() = 0;
};

}  // namespace rivettx
