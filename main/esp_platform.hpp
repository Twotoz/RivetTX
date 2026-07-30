#pragma once

#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/product.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"

namespace rivettx::esp32 {

struct BatterySample {
  uint16_t millivolts = 0;
  bool configured = false;
  bool valid = false;
};

class EspBoard {
 public:
  bool initialize();
  RawInputs sample_inputs(TimeUs now_us);
  BatterySample sample_battery();
  bool recovery_button_pressed() const;
  uint8_t configured_axis_count() const;

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
  uint8_t configured_axis_count_ = 4;
  RotaryEncoderDecoder encoder_decoder_{};
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

 private:
  static void stop_callback(void* context);

  esp_timer_handle_t stop_timer_ = nullptr;
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
TimeUs now_us();

}  // namespace rivettx::esp32
