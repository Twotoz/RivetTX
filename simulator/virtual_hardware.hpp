#pragma once

#include "rivettx/crsf.hpp"
#include "rivettx/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace rivettx::sim {

struct LinkFaultPlan {
  uint32_t drop_every_nth_telemetry_frame = 0;
  uint32_t corrupt_every_nth_telemetry_frame = 0;
  TimeUs disconnect_start_us = 0;
  TimeUs disconnect_end_us = 0;
  std::size_t maximum_read_chunk = 7;
};

struct VirtualElrsStats {
  uint32_t radio_write_attempts = 0;
  uint32_t radio_frames_received = 0;
  uint32_t invalid_radio_frames = 0;
  uint32_t channel_frames_received = 0;
  uint32_t model_id_frames_received = 0;
  uint32_t bind_commands_received = 0;
  uint32_t device_pings_received = 0;
  uint32_t parameter_reads_received = 0;
  uint32_t parameter_writes_received = 0;
  uint32_t wifi_commands_received = 0;
  uint32_t failed_writes = 0;
  uint32_t telemetry_frames_generated = 0;
  uint32_t telemetry_frames_dropped = 0;
  uint32_t telemetry_frames_corrupted = 0;
  uint32_t queue_overflows = 0;
  uint32_t resets = 0;
};

class VirtualElrsModule final : public ICrsfTransport {
 public:
  explicit VirtualElrsModule(LinkFaultPlan faults = {});

  void set_fault_plan(LinkFaultPlan faults);
  void advance(TimeUs now_us);

  bool write(const uint8_t* data, std::size_t size) override;
  std::size_t read(uint8_t* data, std::size_t capacity) override;
  void set_baud_rate(uint32_t baud) override;
  void reset_module() override;

  const VirtualElrsStats& stats() const;
  const std::array<int16_t, kChannelCount>& channels() const;
  uint8_t model_id() const;
  uint32_t baud_rate() const;
  bool link_available() const;
  uint8_t power_option() const;
  uint8_t dynamic_power_option() const;
  uint8_t switch_mode_option() const;
  uint8_t telemetry_ratio_option() const;
  bool wifi_update_mode() const;

 private:
  void handle_radio_frame(const uint8_t* data, std::size_t size);
  void enqueue_frame(uint8_t type, const uint8_t* payload,
                     std::size_t payload_size, bool telemetry);
  void enqueue_link_statistics();
  void enqueue_battery();
  void enqueue_gps();
  void enqueue_device_info();
  void enqueue_parameter(uint8_t field_id, uint8_t command_state = 0,
                         uint8_t requested_chunk = 0,
                         bool fragmented = false);
  void enqueue_parameter_entry(uint8_t field_id,
                               const std::vector<uint8_t>& data,
                               uint8_t requested_chunk = 0,
                               bool fragmented = false);
  bool fault_window_active() const;

  LinkFaultPlan faults_{};
  VirtualElrsStats stats_{};
  std::deque<uint8_t> receive_queue_{};
  std::array<int16_t, kChannelCount> channels_{};
  TimeUs now_us_ = 0;
  TimeUs next_link_statistics_us_ = 100000;
  TimeUs next_battery_us_ = 250000;
  TimeUs next_gps_us_ = 500000;
  uint32_t baud_rate_ = 0;
  uint8_t model_id_ = 0;
  uint8_t power_option_ = 3;
  uint8_t dynamic_power_option_ = 0;
  uint8_t switch_mode_option_ = 0;
  uint8_t telemetry_ratio_option_ = 0;
  bool wifi_update_mode_ = false;
};

class PbmDisplay final : public IDisplaySink {
 public:
  PbmDisplay(DisplayCapabilities capabilities, std::string output_path);

  const DisplayCapabilities& capabilities() const override;
  bool flush(const MonoCanvas& canvas) override;

  uint32_t flushes() const;
  std::size_t last_lit_pixels() const;
  const std::string& output_path() const;

 private:
  DisplayCapabilities capabilities_{};
  std::string output_path_;
  uint32_t flushes_ = 0;
  std::size_t last_lit_pixels_ = 0;
};

}  // namespace rivettx::sim
