#pragma once

#include "rivettx/core.hpp"
#include "rivettx/at7456e.hpp"
#include "rivettx/board_power.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/product.hpp"
#include "rivettx/services.hpp"
#include "rivettx/speaker.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/amt630a.hpp"
#include "rivettx/ui.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"

namespace rivettx::esp32 {

class EspBoard {
 public:
  bool initialize();
  bool configure_vrx_rssi(int gpio);
  bool read_vrx_rssi(int& value) const;
  RawInputs sample_inputs(TimeUs now_us);
  BatterySensorSample sample_battery();
  bool recovery_button_pressed() const;
  uint8_t configured_axis_count() const;
  void publish_rev_a_controls(uint32_t active_low_bits, TimeUs sampled_at_us,
                              bool valid);

 private:
  struct AdcInput {
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    bool configured = false;
  };

  bool configure_adc_gpio(int gpio, AdcInput& input);
  bool read_adc(const AdcInput& input, int& value) const;

  adc_oneshot_unit_handle_t adc_ = nullptr;
  adc_cali_handle_t adc_calibration_ = nullptr;
  std::array<AdcInput, kMaxAxes> axes_{};
  AdcInput battery_{};
  AdcInput vrx_rssi_{};
  uint8_t configured_axis_count_ = 4;
  RotaryEncoderDecoder encoder_decoder_{};
  std::atomic<uint32_t> rev_a_controls_{0};
  std::atomic<TimeUs> rev_a_controls_sampled_at_{0};
  std::atomic<bool> rev_a_controls_valid_{false};
};

class EspBoardPowerIo final : public IBoardPowerHardware,
                              public IAmt630aHardware {
 public:
  bool initialize() override;
  bool set_video_5v(bool enabled) override;
  bool set_display_5v(bool enabled) override;
  bool set_elrs_5v(bool enabled) override;
  bool set_backlight(uint8_t percent) override;
  bool set_display_controller_reset(bool asserted) override;
  bool read_vbus_present(bool& present) override;
  ChargerTelemetry read_charger() override;
  FuelGaugeTelemetry read_fuel_gauge() override;
  bool read_expanded_controls(uint32_t& active_low_bits);
  bool read_identity(uint16_t& identity) override;
  bool acquire_flash(uint32_t& jedec_identity) override;
  bool flash_write_enable() override;
  bool flash_chip_erase() override;
  bool flash_busy(bool& busy) override;
  bool flash_program_page(uint32_t address, const uint8_t* data,
                          std::size_t size) override;
  bool flash_read(uint32_t address, uint8_t* data,
                  std::size_t size) override;
  bool release_flash_and_reset() override;
  bool read_runtime_status(Amt630aRuntimeStatus& status) override;

 private:
  bool set_output(int gpio, bool enabled);
  bool read_register(i2c_master_dev_handle_t device, uint8_t reg,
                     uint8_t* data, std::size_t size);
  bool write_register(i2c_master_dev_handle_t device, uint8_t reg,
                      const uint8_t* data, std::size_t size);
  bool amt630a_read(uint8_t reg, uint8_t& value);
  bool flash_command(uint8_t command);
  bool flash_transfer(const uint8_t* transmit, uint8_t* receive,
                      std::size_t size);

  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t charger_ = nullptr;
  i2c_master_dev_handle_t gauge_ = nullptr;
  i2c_master_dev_handle_t amt630a_ = nullptr;
  std::array<i2c_master_dev_handle_t, 2> expanders_{};
  spi_device_handle_t amt_flash_ = nullptr;
  bool flash_owned_ = false;
  bool initialized_ = false;
};

class EspRx5808Io final : public IRtc6715Io {
 public:
  explicit EspRx5808Io(EspBoard& board);

  bool initialize() override;
  bool set_data(bool high) override;
  bool set_clock(bool high) override;
  bool set_latch(bool high) override;
  bool read_rssi(int& raw_adc) override;

 private:
  EspBoard& board_;
  bool initialized_ = false;
};

class EspCrsfTransport final : public ICrsfTransport {
 public:
  bool initialize();
  bool write(const uint8_t* data, std::size_t size) override;
  std::size_t read(uint8_t* data, std::size_t capacity) override;
  void set_baud_rate(uint32_t baud) override;
  void reset_module() override;

 private:
  uart_port_t port_ = UART_NUM_1;
};

class Ssd1306Display final : public IDisplaySink {
 public:
  bool initialize();
  const DisplayCapabilities& capabilities() const override;
  bool flush(const MonoCanvas& canvas) override;

 private:
  bool command(const uint8_t* bytes, std::size_t size);
  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t device_ = nullptr;
  DisplayCapabilities capabilities_{};
};

class EspAt7456eSpi final : public IAt7456eSpi {
 public:
  bool initialize();
  bool queue(const uint8_t* transmit, uint8_t* receive,
             std::size_t size) override;
  At7456eTransferState poll() override;
  void abort() override;

 private:
  spi_device_handle_t device_ = nullptr;
  spi_transaction_t transaction_{};
  bool queued_ = false;
};

class EspWatchdog final : public IWatchdog {
 public:
  void kick() override;
};

class EspUsbGamepad {
 public:
  bool initialize();
  bool send(const UsbGamepadReport& report);
  bool supported() const;
  bool mounted() const;

 private:
  bool initialized_ = false;
};

class EspToneOutput final : public IToneOutput {
 public:
  bool initialize();
  bool play_tone(uint16_t frequency_hz,
                 uint16_t duration_ms) override;
  void stop_tone() override;
  bool available() const override;
  bool set_intensity(uint8_t percent) override;

 private:
  static void stop_callback(void* context);

  esp_timer_handle_t stop_timer_ = nullptr;
  bool initialized_ = false;
  uint8_t intensity_percent_ = 60;
};

class EspPcmOutput final : public IPcmOutput {
 public:
  bool initialize(uint32_t sample_rate_hz) override;
  bool set_amplifier_enabled(bool enabled) override;
  bool write_nonblocking(const int16_t* samples, std::size_t count,
                         std::size_t& written) override;

 private:
  i2s_chan_handle_t channel_ = nullptr;
  bool initialized_ = false;
};

class EspOtaBackend final : public IOtaBackend {
 public:
  bool running_image_pending_verification() const override;
  bool mark_running_image_valid() override;
  bool request_rollback() override;
  bool begin_https_update(const std::string& url) override;
};

class NvsCrashStore final : public ICrashStore {
 public:
  bool initialize();
  bool write(const CrashSnapshot& snapshot) override;
  bool read(CrashSnapshot& snapshot) override;
  void clear() override;

 private:
  nvs_handle_t handle_ = 0;
};

class NvsBootState {
 public:
  bool initialize();
  uint32_t begin_attempt();
  bool mark_success();
  uint32_t failed_attempts() const;

 private:
  nvs_handle_t handle_ = 0;
  uint32_t attempts_ = 0;
};

class CsvTelemetrySink final : public ITelemetryLogSink {
 public:
  explicit CsvTelemetrySink(std::string path,
                            std::size_t maximum_bytes = 64 * 1024);
  ~CsvTelemetrySink() override;
  bool append(TimeUs time_us, uint16_t sensor_id, int32_t value) override;
  bool flush() override;

 private:
  std::string path_;
  std::size_t maximum_bytes_;
  void* file_ = nullptr;
};

class WifiBackupPortal {
 public:
  WifiBackupPortal(TransactionalModelStore& models,
                   ModelLibrary& library,
                   SafetyManager& safety);
  bool start();
  void stop();
  bool running() const;

 private:
  static esp_err_t get_index(httpd_req_t* request);
  static esp_err_t get_backup(httpd_req_t* request);
  static esp_err_t post_restore(httpd_req_t* request);
  static esp_err_t get_status(httpd_req_t* request);

  TransactionalModelStore& models_;
  ModelLibrary& library_;
  SafetyManager& safety_;
  httpd_handle_t server_ = nullptr;
};

bool mount_model_filesystem(bool format_if_mount_failed = false);
bool validate_pin_configuration();
bool validate_rx5808_configuration();
TimeUs now_us();

}  // namespace rivettx::esp32
