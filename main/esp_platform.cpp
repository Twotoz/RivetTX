#include "esp_platform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#if !CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S3
#error "RivetTX supports only ESP32-C3 and ESP32-S3"
#endif

namespace rivettx::esp32 {

namespace {

constexpr char kTag[] = "rivettx-platform";
wl_handle_t filesystem_wl = WL_INVALID_HANDLE;

bool configure_button(int gpio_number)
{
  if (!GPIO_IS_VALID_GPIO(gpio_number)) {
    return false;
  }
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << gpio_number;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config) == ESP_OK;
}

bool configure_optional_switch(int gpio_number)
{
  return gpio_number < 0 || configure_button(gpio_number);
}

bool configure_output(int gpio_number, int initial_level)
{
  if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio_number)) {
    return false;
  }
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << gpio_number;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config) == ESP_OK &&
         gpio_set_level(static_cast<gpio_num_t>(gpio_number),
                        initial_level) == ESP_OK;
}

bool active_low_input(int gpio_number)
{
  return gpio_number >= 0 &&
         gpio_get_level(static_cast<gpio_num_t>(gpio_number)) == 0;
}

int8_t switch_position(int high_gpio, int low_gpio, bool& valid)
{
  const bool high = active_low_input(high_gpio);
  if (low_gpio < 0) {
    return high ? 1 : -1;
  }
  const bool low = active_low_input(low_gpio);
  if (high && low) {
    valid = false;
    return 0;
  }
  return high ? 1 : (low ? -1 : 0);
}

}  // namespace

TimeUs now_us()
{
  return static_cast<TimeUs>(esp_timer_get_time());
}

bool validate_pin_configuration()
{
  std::array<int, 64> pins{};
  pins.fill(-1);
  std::size_t pin_count = 0;
  const auto add_pin = [&pins, &pin_count](int pin) {
    if (pin_count < pins.size()) {
      pins[pin_count++] = pin;
    }
  };
  const std::array<int, 34> common_pins{
      CONFIG_RIVETTX_AXIS0_GPIO,      CONFIG_RIVETTX_AXIS1_GPIO,
      CONFIG_RIVETTX_AXIS2_GPIO,      CONFIG_RIVETTX_AXIS3_GPIO,
      CONFIG_RIVETTX_AXIS4_GPIO,      CONFIG_RIVETTX_AXIS5_GPIO,
      CONFIG_RIVETTX_AXIS6_GPIO,      CONFIG_RIVETTX_AXIS7_GPIO,
      CONFIG_RIVETTX_CRSF_TX,         CONFIG_RIVETTX_CRSF_RX,
      CONFIG_RIVETTX_BUTTON_UP,       CONFIG_RIVETTX_BUTTON_DOWN,
      CONFIG_RIVETTX_BUTTON_ENTER,    CONFIG_RIVETTX_BUTTON_BACK,
      CONFIG_RIVETTX_AUX1_GPIO,       CONFIG_RIVETTX_AUX2_GPIO,
      CONFIG_RIVETTX_AUX3_GPIO,       CONFIG_RIVETTX_AUX4_GPIO,
      CONFIG_RIVETTX_AUX2_LOW_GPIO,   CONFIG_RIVETTX_AUX3_LOW_GPIO,
      CONFIG_RIVETTX_AUX4_LOW_GPIO,   CONFIG_RIVETTX_ENCODER_A_GPIO,
      CONFIG_RIVETTX_ENCODER_B_GPIO,  CONFIG_RIVETTX_ENCODER_PRESS_GPIO,
      CONFIG_RIVETTX_TRIM_AIL_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_AIL_POS_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_POS_GPIO,
      CONFIG_RIVETTX_TRIM_THR_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_THR_POS_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_POS_GPIO,
      CONFIG_RIVETTX_BATTERY_GPIO,    CONFIG_RIVETTX_BUZZER_GPIO};
  for (const int pin : common_pins) {
    add_pin(pin);
  }
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  add_pin(CONFIG_RIVETTX_AT7456E_SCLK_GPIO);
  add_pin(CONFIG_RIVETTX_AT7456E_MOSI_GPIO);
  add_pin(CONFIG_RIVETTX_AT7456E_MISO_GPIO);
  add_pin(CONFIG_RIVETTX_AT7456E_CS_GPIO);
  add_pin(CONFIG_RIVETTX_AT7456E_RESET_GPIO);
  if (CONFIG_RIVETTX_AT7456E_SCLK_GPIO < 0 ||
      CONFIG_RIVETTX_AT7456E_MOSI_GPIO < 0 ||
      CONFIG_RIVETTX_AT7456E_MISO_GPIO < 0 ||
      CONFIG_RIVETTX_AT7456E_CS_GPIO < 0) {
    ESP_LOGE(kTag, "OpenPocket requires SCLK, MOSI, MISO and CS GPIOs");
    return false;
  }
#else
  add_pin(CONFIG_RIVETTX_I2C_SDA);
  add_pin(CONFIG_RIVETTX_I2C_SCL);
#endif
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  const std::array<int, 15> rev_a_pins{
      CONFIG_RIVETTX_BOARD_I2C_SDA_GPIO,
      CONFIG_RIVETTX_BOARD_I2C_SCL_GPIO,
      CONFIG_RIVETTX_5V_VIDEO_ENABLE_GPIO,
      CONFIG_RIVETTX_5V_DISPLAY_ENABLE_GPIO,
      CONFIG_RIVETTX_5V_ELRS_ENABLE_GPIO,
      CONFIG_RIVETTX_BACKLIGHT_PWM_GPIO,
      CONFIG_RIVETTX_AMT630A_RESET_GPIO,
      CONFIG_RIVETTX_AMT630A_FLASH_OWNER_GPIO,
      CONFIG_RIVETTX_AMT630A_FLASH_CS_GPIO,
      CONFIG_RIVETTX_VBUS_DETECT_GPIO,
      CONFIG_RIVETTX_CONTROL_EXPANDER_INT_GPIO,
      CONFIG_RIVETTX_CHARGER_INT_GPIO,
      CONFIG_RIVETTX_FUEL_GAUGE_ALERT_GPIO,
      19, 20};  // Native USB D-/D+ are reserved by the board.
  for (const int pin : rev_a_pins) add_pin(pin);
#endif
  for (std::size_t index = 0; index < pin_count; ++index) {
    const int pin = pins[index];
    if (pin < 0) {
      continue;
    }
    if (!GPIO_IS_VALID_GPIO(pin)) {
      ESP_LOGE(kTag, "GPIO %d is invalid for target %s", pin,
               CONFIG_IDF_TARGET);
      return false;
    }
    for (std::size_t other = index + 1; other < pin_count; ++other) {
      if (pins[other] == pin) {
        ESP_LOGE(kTag, "GPIO %d is assigned more than once", pin);
        return false;
      }
    }
  }

  const std::array<int, 4> optional_axes{
      CONFIG_RIVETTX_AXIS4_GPIO, CONFIG_RIVETTX_AXIS5_GPIO,
      CONFIG_RIVETTX_AXIS6_GPIO, CONFIG_RIVETTX_AXIS7_GPIO};
  bool saw_disabled_axis = false;
  for (const int pin : optional_axes) {
    if (pin < 0) {
      saw_disabled_axis = true;
    } else if (saw_disabled_axis) {
      ESP_LOGE(kTag, "optional ADC axes must be enabled contiguously");
      return false;
    }
  }
  const std::array<std::pair<int, int>, 3> three_position_pairs{{
      {CONFIG_RIVETTX_AUX2_GPIO, CONFIG_RIVETTX_AUX2_LOW_GPIO},
      {CONFIG_RIVETTX_AUX3_GPIO, CONFIG_RIVETTX_AUX3_LOW_GPIO},
      {CONFIG_RIVETTX_AUX4_GPIO, CONFIG_RIVETTX_AUX4_LOW_GPIO},
  }};
  for (const auto& pair : three_position_pairs) {
    if (pair.second >= 0 && pair.first < 0) {
      ESP_LOGE(kTag, "three-position LOW contact needs its AUX GPIO");
      return false;
    }
  }
  if ((CONFIG_RIVETTX_ENCODER_A_GPIO < 0) !=
      (CONFIG_RIVETTX_ENCODER_B_GPIO < 0)) {
    ESP_LOGE(kTag, "encoder A and B must both be configured or disabled");
    return false;
  }

  std::array<int, 16> output_pins{
      CONFIG_RIVETTX_CRSF_TX, CONFIG_RIVETTX_BUZZER_GPIO,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  output_pins[2] = CONFIG_RIVETTX_AT7456E_SCLK_GPIO;
  output_pins[3] = CONFIG_RIVETTX_AT7456E_MOSI_GPIO;
  output_pins[4] = CONFIG_RIVETTX_AT7456E_CS_GPIO;
  output_pins[5] = CONFIG_RIVETTX_AT7456E_RESET_GPIO;
#else
  output_pins[2] = CONFIG_RIVETTX_I2C_SDA;
  output_pins[3] = CONFIG_RIVETTX_I2C_SCL;
#endif
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  output_pins[6] = CONFIG_RIVETTX_BOARD_I2C_SDA_GPIO;
  output_pins[7] = CONFIG_RIVETTX_BOARD_I2C_SCL_GPIO;
  output_pins[8] = CONFIG_RIVETTX_5V_VIDEO_ENABLE_GPIO;
  output_pins[9] = CONFIG_RIVETTX_5V_DISPLAY_ENABLE_GPIO;
  output_pins[10] = CONFIG_RIVETTX_5V_ELRS_ENABLE_GPIO;
  output_pins[11] = CONFIG_RIVETTX_BACKLIGHT_PWM_GPIO;
  output_pins[12] = CONFIG_RIVETTX_AMT630A_RESET_GPIO;
  output_pins[13] = CONFIG_RIVETTX_AMT630A_FLASH_OWNER_GPIO;
  output_pins[14] = CONFIG_RIVETTX_AMT630A_FLASH_CS_GPIO;
#endif
  for (const int pin : output_pins) {
    if (pin >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      ESP_LOGE(kTag, "GPIO %d cannot drive an output on target %s", pin,
               CONFIG_IDF_TARGET);
      return false;
    }
  }
  return true;
}

bool validate_rx5808_configuration()
{
#if !CONFIG_RIVETTX_RX5808_ENABLED
  return false;
#else
  const std::array<int, 4> vrx_pins{
      CONFIG_RIVETTX_RX5808_DATA_GPIO,
      CONFIG_RIVETTX_RX5808_CLK_GPIO,
      CONFIG_RIVETTX_RX5808_LE_GPIO,
      CONFIG_RIVETTX_RX5808_RSSI_GPIO};
  for (const int pin : vrx_pins) {
    if (pin < 0) {
      ESP_LOGE(kTag,
               "RX5808 disabled: DATA, CLK, LE and RSSI GPIOs are required");
      return false;
    }
    if (!GPIO_IS_VALID_GPIO(pin)) {
      ESP_LOGE(kTag, "RX5808 disabled: GPIO %d is invalid for %s", pin,
               CONFIG_IDF_TARGET);
      return false;
    }
  }
  const std::array<int, 3> output_pins{
      CONFIG_RIVETTX_RX5808_DATA_GPIO,
      CONFIG_RIVETTX_RX5808_CLK_GPIO,
      CONFIG_RIVETTX_RX5808_LE_GPIO};
  for (const int pin : output_pins) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      ESP_LOGE(kTag, "RX5808 disabled: GPIO %d cannot drive an output", pin);
      return false;
    }
  }
  adc_unit_t rssi_unit{};
  adc_channel_t rssi_channel{};
  if (adc_oneshot_io_to_channel(CONFIG_RIVETTX_RX5808_RSSI_GPIO,
                                &rssi_unit, &rssi_channel) != ESP_OK ||
      rssi_unit != ADC_UNIT_1) {
    ESP_LOGE(kTag, "RX5808 disabled: RSSI GPIO %d is not an ADC1 input",
             CONFIG_RIVETTX_RX5808_RSSI_GPIO);
    return false;
  }
  if (CONFIG_RIVETTX_RX5808_RSSI_MIN_ADC >=
      CONFIG_RIVETTX_RX5808_RSSI_MAX_ADC) {
    ESP_LOGE(kTag,
             "RX5808 disabled: RSSI minimum ADC must be below maximum");
    return false;
  }
  for (std::size_t first = 0; first < vrx_pins.size(); ++first) {
    for (std::size_t second = first + 1; second < vrx_pins.size(); ++second) {
      if (vrx_pins[first] == vrx_pins[second]) {
        ESP_LOGE(kTag, "RX5808 disabled: GPIO %d is assigned twice",
                 vrx_pins[first]);
        return false;
      }
    }
  }

  const std::array<int, 52> occupied{
      CONFIG_RIVETTX_AXIS0_GPIO,      CONFIG_RIVETTX_AXIS1_GPIO,
      CONFIG_RIVETTX_AXIS2_GPIO,      CONFIG_RIVETTX_AXIS3_GPIO,
      CONFIG_RIVETTX_AXIS4_GPIO,      CONFIG_RIVETTX_AXIS5_GPIO,
      CONFIG_RIVETTX_AXIS6_GPIO,      CONFIG_RIVETTX_AXIS7_GPIO,
      CONFIG_RIVETTX_CRSF_TX,         CONFIG_RIVETTX_CRSF_RX,
      CONFIG_RIVETTX_BUTTON_UP,       CONFIG_RIVETTX_BUTTON_DOWN,
      CONFIG_RIVETTX_BUTTON_ENTER,    CONFIG_RIVETTX_BUTTON_BACK,
      CONFIG_RIVETTX_AUX1_GPIO,       CONFIG_RIVETTX_AUX2_GPIO,
      CONFIG_RIVETTX_AUX3_GPIO,       CONFIG_RIVETTX_AUX4_GPIO,
      CONFIG_RIVETTX_AUX2_LOW_GPIO,   CONFIG_RIVETTX_AUX3_LOW_GPIO,
      CONFIG_RIVETTX_AUX4_LOW_GPIO,   CONFIG_RIVETTX_ENCODER_A_GPIO,
      CONFIG_RIVETTX_ENCODER_B_GPIO,  CONFIG_RIVETTX_ENCODER_PRESS_GPIO,
      CONFIG_RIVETTX_TRIM_AIL_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_AIL_POS_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_POS_GPIO,
      CONFIG_RIVETTX_TRIM_THR_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_THR_POS_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_POS_GPIO,
      CONFIG_RIVETTX_BATTERY_GPIO,    CONFIG_RIVETTX_BUZZER_GPIO,
      CONFIG_RIVETTX_AT7456E_SCLK_GPIO,
      CONFIG_RIVETTX_AT7456E_MOSI_GPIO,
      CONFIG_RIVETTX_AT7456E_MISO_GPIO,
      CONFIG_RIVETTX_AT7456E_CS_GPIO,
      CONFIG_RIVETTX_AT7456E_RESET_GPIO,
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
      CONFIG_RIVETTX_BOARD_I2C_SDA_GPIO,
      CONFIG_RIVETTX_BOARD_I2C_SCL_GPIO,
      CONFIG_RIVETTX_5V_VIDEO_ENABLE_GPIO,
      CONFIG_RIVETTX_5V_DISPLAY_ENABLE_GPIO,
      CONFIG_RIVETTX_5V_ELRS_ENABLE_GPIO,
      CONFIG_RIVETTX_BACKLIGHT_PWM_GPIO,
      CONFIG_RIVETTX_AMT630A_RESET_GPIO,
      CONFIG_RIVETTX_AMT630A_FLASH_OWNER_GPIO,
      CONFIG_RIVETTX_AMT630A_FLASH_CS_GPIO,
      CONFIG_RIVETTX_VBUS_DETECT_GPIO,
      CONFIG_RIVETTX_CONTROL_EXPANDER_INT_GPIO,
      CONFIG_RIVETTX_CHARGER_INT_GPIO,
      CONFIG_RIVETTX_FUEL_GAUGE_ALERT_GPIO
#else
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
#endif
  };
  for (const int vrx_pin : vrx_pins) {
    for (const int used_pin : occupied) {
      if (used_pin >= 0 && vrx_pin == used_pin) {
        ESP_LOGE(kTag,
                 "RX5808 disabled: GPIO %d conflicts with another device",
                 vrx_pin);
        return false;
      }
    }
  }
  return true;
#endif
}

bool EspAt7456eSpi::initialize()
{
#if !CONFIG_RIVETTX_OPENPOCKET_OSD
  return false;
#else
  if (device_ != nullptr) {
    return true;
  }
  if (CONFIG_RIVETTX_AT7456E_RESET_GPIO >= 0) {
    gpio_config_t reset_config{};
    reset_config.pin_bit_mask =
        1ULL << CONFIG_RIVETTX_AT7456E_RESET_GPIO;
    reset_config.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&reset_config) != ESP_OK ||
        gpio_set_level(static_cast<gpio_num_t>(
                           CONFIG_RIVETTX_AT7456E_RESET_GPIO),
                       0) != ESP_OK) {
      return false;
    }
    esp_rom_delay_us(50000);
    if (gpio_set_level(static_cast<gpio_num_t>(
                           CONFIG_RIVETTX_AT7456E_RESET_GPIO),
                       1) != ESP_OK) {
      return false;
    }
    esp_rom_delay_us(100);
  }

  spi_bus_config_t bus{};
  bus.mosi_io_num = CONFIG_RIVETTX_AT7456E_MOSI_GPIO;
  bus.miso_io_num = CONFIG_RIVETTX_AT7456E_MISO_GPIO;
  bus.sclk_io_num = CONFIG_RIVETTX_AT7456E_SCLK_GPIO;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  const esp_err_t bus_result =
      spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
  if (bus_result != ESP_OK) {
    ESP_LOGE(kTag, "AT7456E SPI bus init failed: %s",
             esp_err_to_name(bus_result));
    return false;
  }

  spi_device_interface_config_t device_config{};
  device_config.mode = 0;
  device_config.clock_speed_hz = CONFIG_RIVETTX_AT7456E_SPI_CLOCK_HZ;
  device_config.spics_io_num = CONFIG_RIVETTX_AT7456E_CS_GPIO;
  device_config.queue_size = 1;
  device_config.cs_ena_pretrans =
      CONFIG_RIVETTX_AT7456E_CS_SETUP_CYCLES;
  device_config.cs_ena_posttrans =
      CONFIG_RIVETTX_AT7456E_CS_HOLD_CYCLES;
  const esp_err_t device_result =
      spi_bus_add_device(SPI2_HOST, &device_config, &device_);
  if (device_result != ESP_OK) {
    ESP_LOGE(kTag, "AT7456E SPI device init failed: %s",
             esp_err_to_name(device_result));
    device_ = nullptr;
    return false;
  }
  return true;
#endif
}

bool EspAt7456eSpi::queue(const uint8_t* transmit, uint8_t* receive,
                          std::size_t size)
{
  if (device_ == nullptr || queued_ || transmit == nullptr ||
      receive == nullptr || size == 0 || size > 64) {
    return false;
  }
  transaction_ = {};
  transaction_.length = size * 8;
  transaction_.tx_buffer = transmit;
  transaction_.rx_buffer = receive;
  const esp_err_t result =
      spi_device_queue_trans(device_, &transaction_, 0);
  queued_ = result == ESP_OK;
  return queued_;
}

At7456eTransferState EspAt7456eSpi::poll()
{
  if (device_ == nullptr || !queued_) {
    return At7456eTransferState::Failed;
  }
  spi_transaction_t* completed = nullptr;
  const esp_err_t result =
      spi_device_get_trans_result(device_, &completed, 0);
  if (result == ESP_ERR_TIMEOUT) {
    return At7456eTransferState::Busy;
  }
  queued_ = false;
  return result == ESP_OK && completed == &transaction_
             ? At7456eTransferState::Complete
             : At7456eTransferState::Failed;
}

void EspAt7456eSpi::abort()
{
  if (device_ == nullptr || !queued_) {
    return;
  }
  spi_transaction_t* completed = nullptr;
  if (spi_device_get_trans_result(device_, &completed, 0) == ESP_OK) {
    queued_ = false;
  }
}

bool EspBoard::configure_adc_gpio(int gpio, AdcInput& input)
{
  if (gpio < 0) {
    return false;
  }
  adc_unit_t unit{};
  adc_channel_t channel{};
  if (adc_oneshot_io_to_channel(gpio, &unit, &channel) != ESP_OK ||
      unit != ADC_UNIT_1) {
    return false;
  }
  adc_oneshot_chan_cfg_t channel_config{};
  channel_config.atten = ADC_ATTEN_DB_12;
  channel_config.bitwidth = ADC_BITWIDTH_12;
  if (adc_oneshot_config_channel(adc_, channel, &channel_config) != ESP_OK) {
    return false;
  }
  input.unit = unit;
  input.channel = channel;
  input.configured = true;
  return true;
}

bool EspBoard::read_adc(const AdcInput& input, int& value) const
{
  value = 0;
  if (!input.configured) {
    return false;
  }
  return adc_oneshot_read(adc_, input.channel, &value) == ESP_OK;
}

bool EspBoard::configure_vrx_rssi(int gpio)
{
  if (adc_ == nullptr || vrx_rssi_.configured) {
    return vrx_rssi_.configured;
  }
  return configure_adc_gpio(gpio, vrx_rssi_);
}

bool EspBoard::read_vrx_rssi(int& value) const
{
  return read_adc(vrx_rssi_, value);
}

EspRx5808Io::EspRx5808Io(EspBoard& board) : board_(board) {}

bool EspRx5808Io::initialize()
{
#if !CONFIG_RIVETTX_RX5808_ENABLED
  return false;
#else
  if (initialized_) {
    return true;
  }
  gpio_config_t outputs{};
  outputs.pin_bit_mask =
      (1ULL << CONFIG_RIVETTX_RX5808_DATA_GPIO) |
      (1ULL << CONFIG_RIVETTX_RX5808_CLK_GPIO) |
      (1ULL << CONFIG_RIVETTX_RX5808_LE_GPIO);
  outputs.mode = GPIO_MODE_OUTPUT;
  outputs.pull_up_en = GPIO_PULLUP_DISABLE;
  outputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
  outputs.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&outputs) != ESP_OK ||
      !board_.configure_vrx_rssi(CONFIG_RIVETTX_RX5808_RSSI_GPIO) ||
      gpio_set_level(static_cast<gpio_num_t>(
                         CONFIG_RIVETTX_RX5808_DATA_GPIO),
                     0) != ESP_OK ||
      gpio_set_level(static_cast<gpio_num_t>(
                         CONFIG_RIVETTX_RX5808_CLK_GPIO),
                     0) != ESP_OK ||
      gpio_set_level(static_cast<gpio_num_t>(
                         CONFIG_RIVETTX_RX5808_LE_GPIO),
                     1) != ESP_OK) {
    ESP_LOGE(kTag, "RX5808 GPIO or RSSI ADC initialization failed");
    return false;
  }
  initialized_ = true;
  return true;
#endif
}

bool EspRx5808Io::set_data(bool high)
{
#if CONFIG_RIVETTX_RX5808_ENABLED
  return initialized_ &&
         gpio_set_level(static_cast<gpio_num_t>(
                            CONFIG_RIVETTX_RX5808_DATA_GPIO),
                        high ? 1 : 0) == ESP_OK;
#else
  (void)high;
  return false;
#endif
}

bool EspRx5808Io::set_clock(bool high)
{
#if CONFIG_RIVETTX_RX5808_ENABLED
  return initialized_ &&
         gpio_set_level(static_cast<gpio_num_t>(
                            CONFIG_RIVETTX_RX5808_CLK_GPIO),
                        high ? 1 : 0) == ESP_OK;
#else
  (void)high;
  return false;
#endif
}

bool EspRx5808Io::set_latch(bool high)
{
#if CONFIG_RIVETTX_RX5808_ENABLED
  return initialized_ &&
         gpio_set_level(static_cast<gpio_num_t>(
                            CONFIG_RIVETTX_RX5808_LE_GPIO),
                        high ? 1 : 0) == ESP_OK;
#else
  (void)high;
  return false;
#endif
}

bool EspRx5808Io::read_rssi(int& raw_adc)
{
  return initialized_ && board_.read_vrx_rssi(raw_adc);
}

bool EspBoardPowerIo::set_output(int gpio, bool enabled)
{
  return initialized_ && GPIO_IS_VALID_OUTPUT_GPIO(gpio) &&
         gpio_set_level(static_cast<gpio_num_t>(gpio),
                        enabled ? 1 : 0) == ESP_OK;
}

bool EspBoardPowerIo::read_register(i2c_master_dev_handle_t device,
                                    uint8_t reg, uint8_t* data,
                                    std::size_t size)
{
  return initialized_ && device != nullptr && data != nullptr && size != 0 &&
         i2c_master_transmit_receive(device, &reg, 1, data, size, 10) == ESP_OK;
}

bool EspBoardPowerIo::initialize()
{
#if !CONFIG_RIVETTX_OPENPOCKET_REV_A
  return false;
#else
  if (initialized_) {
    return true;
  }
  const std::array<std::pair<int, int>, 6> safe_outputs{{
      {CONFIG_RIVETTX_5V_VIDEO_ENABLE_GPIO, 0},
      {CONFIG_RIVETTX_5V_DISPLAY_ENABLE_GPIO, 0},
      {CONFIG_RIVETTX_5V_ELRS_ENABLE_GPIO, 0},
      {CONFIG_RIVETTX_BACKLIGHT_PWM_GPIO, 0},
      {CONFIG_RIVETTX_AMT630A_RESET_GPIO, 0},
      {CONFIG_RIVETTX_AMT630A_FLASH_OWNER_GPIO, 0},
  }};
  for (const auto& output : safe_outputs) {
    if (!configure_output(output.first, output.second)) {
      ESP_LOGE(kTag, "Rev-A power GPIO %d initialization failed",
               output.first);
      return false;
    }
  }
  if (!GPIO_IS_VALID_GPIO(CONFIG_RIVETTX_VBUS_DETECT_GPIO)) {
    return false;
  }
  gpio_config_t vbus_config{};
  vbus_config.pin_bit_mask = 1ULL << CONFIG_RIVETTX_VBUS_DETECT_GPIO;
  vbus_config.mode = GPIO_MODE_INPUT;
  vbus_config.pull_up_en = GPIO_PULLUP_DISABLE;
  vbus_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
  if (gpio_config(&vbus_config) != ESP_OK) {
    return false;
  }

  i2c_master_bus_config_t bus_config{};
  bus_config.i2c_port = -1;
  bus_config.sda_io_num =
      static_cast<gpio_num_t>(CONFIG_RIVETTX_BOARD_I2C_SDA_GPIO);
  bus_config.scl_io_num =
      static_cast<gpio_num_t>(CONFIG_RIVETTX_BOARD_I2C_SCL_GPIO);
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = false;
  if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
    return false;
  }
  const auto add_device = [this](uint8_t address,
                                 i2c_master_dev_handle_t& device) {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(bus_, &config, &device) == ESP_OK;
  };
  if (!add_device(0x6A, charger_) || !add_device(0x36, gauge_) ||
      !add_device(CONFIG_RIVETTX_CONTROL_EXPANDER0_ADDRESS,
                  expanders_[0]) ||
      !add_device(CONFIG_RIVETTX_CONTROL_EXPANDER1_ADDRESS,
                  expanders_[1])) {
    return false;
  }

  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_1;
  timer.freq_hz = 20000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ledc_channel_config_t channel{};
  channel.gpio_num = CONFIG_RIVETTX_BACKLIGHT_PWM_GPIO;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_1;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = LEDC_TIMER_1;
  channel.duty = 0;
  channel.hpoint = 0;
  initialized_ = ledc_timer_config(&timer) == ESP_OK &&
                 ledc_channel_config(&channel) == ESP_OK;
  return initialized_;
#endif
}

bool EspBoardPowerIo::set_video_5v(bool enabled)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return set_output(CONFIG_RIVETTX_5V_VIDEO_ENABLE_GPIO, enabled);
#else
  (void)enabled;
  return false;
#endif
}

bool EspBoardPowerIo::set_display_5v(bool enabled)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return set_output(CONFIG_RIVETTX_5V_DISPLAY_ENABLE_GPIO, enabled);
#else
  (void)enabled;
  return false;
#endif
}

bool EspBoardPowerIo::set_elrs_5v(bool enabled)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return set_output(CONFIG_RIVETTX_5V_ELRS_ENABLE_GPIO, enabled);
#else
  (void)enabled;
  return false;
#endif
}

bool EspBoardPowerIo::set_backlight(uint8_t percent)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  if (!initialized_ || percent > 100) {
    return false;
  }
  const uint32_t duty = static_cast<uint32_t>(percent) * 1023U / 100U;
  return ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty) == ESP_OK &&
         ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1) == ESP_OK;
#else
  (void)percent;
  return false;
#endif
}

bool EspBoardPowerIo::set_amt630a_reset(bool asserted)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return set_output(CONFIG_RIVETTX_AMT630A_RESET_GPIO, !asserted);
#else
  (void)asserted;
  return false;
#endif
}

bool EspBoardPowerIo::set_amt630a_flash_owner(bool esp_owns_bus)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return set_output(CONFIG_RIVETTX_AMT630A_FLASH_OWNER_GPIO, esp_owns_bus);
#else
  (void)esp_owns_bus;
  return false;
#endif
}

bool EspBoardPowerIo::read_vbus_present(bool& present)
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  if (!initialized_) return false;
  present = gpio_get_level(
                static_cast<gpio_num_t>(CONFIG_RIVETTX_VBUS_DETECT_GPIO)) != 0;
  return true;
#else
  present = false;
  return false;
#endif
}

ChargerTelemetry EspBoardPowerIo::read_charger()
{
  ChargerTelemetry telemetry{};
  uint8_t status = 0;
  uint8_t fault = 0;
  uint8_t battery = 0;
  if (!read_register(charger_, 0x0B, &status, 1) ||
      !read_register(charger_, 0x0C, &fault, 1) ||
      !read_register(charger_, 0x0E, &battery, 1)) {
    telemetry.state = initialized_ ? BoardSensorState::Fault
                                   : BoardSensorState::Unavailable;
    telemetry.charge = ChargeState::Fault;
    return telemetry;
  }
  telemetry.state = fault == 0 ? BoardSensorState::Valid
                               : BoardSensorState::Fault;
  const uint8_t vbus_state = static_cast<uint8_t>((status >> 5U) & 0x07U);
  const uint8_t charge_state = static_cast<uint8_t>((status >> 3U) & 0x03U);
  telemetry.vbus_present = vbus_state != 0;
  telemetry.battery_present = (battery & 0x7FU) != 0;
  telemetry.battery_mv = static_cast<uint16_t>(
      2304U + static_cast<uint16_t>(battery & 0x7FU) * 20U);
  telemetry.thermal_regulation = (status & 0x02U) != 0;
  if (fault != 0) {
    telemetry.charge = ChargeState::Fault;
  } else if (!telemetry.vbus_present) {
    telemetry.charge = ChargeState::Disconnected;
  } else if (charge_state == 3) {
    telemetry.charge = ChargeState::Full;
  } else if (charge_state != 0) {
    telemetry.charge = ChargeState::Charging;
  } else {
    telemetry.charge = ChargeState::Disconnected;
  }
  return telemetry;
}

FuelGaugeTelemetry EspBoardPowerIo::read_fuel_gauge()
{
  FuelGaugeTelemetry telemetry{};
  std::array<uint8_t, 2> vcell{};
  std::array<uint8_t, 2> soc{};
  std::array<uint8_t, 2> config{};
  if (!read_register(gauge_, 0x02, vcell.data(), vcell.size()) ||
      !read_register(gauge_, 0x04, soc.data(), soc.size()) ||
      !read_register(gauge_, 0x0C, config.data(), config.size())) {
    telemetry.state = initialized_ ? BoardSensorState::Fault
                                   : BoardSensorState::Unavailable;
    return telemetry;
  }
  const uint16_t raw_vcell = static_cast<uint16_t>(
      (static_cast<uint16_t>(vcell[0]) << 8U) | vcell[1]);
  telemetry.cell_mv = static_cast<uint16_t>((raw_vcell >> 4U) * 5U / 4U);
  telemetry.state_of_charge = std::min<uint8_t>(100, soc[0]);
  telemetry.alert = (config[1] & 0x20U) != 0;
  telemetry.state = BoardSensorState::Valid;
  return telemetry;
}

bool EspBoardPowerIo::read_expanded_controls(uint32_t& active_low_bits)
{
  std::array<uint8_t, 2> first{};
  std::array<uint8_t, 2> second{};
  if (!read_register(expanders_[0], 0x00, first.data(), first.size()) ||
      !read_register(expanders_[1], 0x00, second.data(), second.size())) {
    return false;
  }
  const uint32_t raw = static_cast<uint32_t>(first[0]) |
                       (static_cast<uint32_t>(first[1]) << 8U) |
                       (static_cast<uint32_t>(second[0]) << 16U) |
                       (static_cast<uint32_t>(second[1]) << 24U);
  active_low_bits = ~raw;
  return true;
}

bool EspBoard::initialize()
{
  adc_oneshot_unit_init_cfg_t unit_config{};
  unit_config.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit_config, &adc_) != ESP_OK) {
    return false;
  }

  const std::array<int, kMaxAxes> pins{
      CONFIG_RIVETTX_AXIS0_GPIO, CONFIG_RIVETTX_AXIS1_GPIO,
      CONFIG_RIVETTX_AXIS2_GPIO, CONFIG_RIVETTX_AXIS3_GPIO,
      CONFIG_RIVETTX_AXIS4_GPIO, CONFIG_RIVETTX_AXIS5_GPIO,
      CONFIG_RIVETTX_AXIS6_GPIO, CONFIG_RIVETTX_AXIS7_GPIO};
  configured_axis_count_ = 4;
  for (std::size_t i = 0; i < pins.size(); ++i) {
    if (pins[i] < 0 && i >= 4) {
      continue;
    }
    if (!configure_adc_gpio(pins[i], axes_[i])) {
      ESP_LOGE(kTag, "ADC axis %u is invalid", static_cast<unsigned>(i));
      return false;
    }
    configured_axis_count_ = static_cast<uint8_t>(i + 1);
  }
  if (CONFIG_RIVETTX_BATTERY_GPIO >= 0) {
    if (!configure_adc_gpio(CONFIG_RIVETTX_BATTERY_GPIO, battery_)) {
      ESP_LOGE(kTag, "battery ADC GPIO is invalid");
      return false;
    }
  }
  adc_cali_curve_fitting_config_t calibration_config{};
  calibration_config.unit_id = ADC_UNIT_1;
  calibration_config.atten = ADC_ATTEN_DB_12;
  calibration_config.bitwidth = ADC_BITWIDTH_12;
  if (adc_cali_create_scheme_curve_fitting(
          &calibration_config, &adc_calibration_) != ESP_OK) {
    adc_calibration_ = nullptr;
    ESP_LOGW(kTag, "ADC eFuse calibration unavailable");
  }

  bool digital_inputs_ok = true;
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  digital_inputs_ok =
      configure_optional_switch(CONFIG_RIVETTX_ENCODER_A_GPIO) &&
      configure_optional_switch(CONFIG_RIVETTX_ENCODER_B_GPIO);
#else
  digital_inputs_ok = configure_button(CONFIG_RIVETTX_BUTTON_UP) &&
         configure_button(CONFIG_RIVETTX_BUTTON_DOWN) &&
         configure_button(CONFIG_RIVETTX_BUTTON_ENTER) &&
         configure_button(CONFIG_RIVETTX_BUTTON_BACK) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX1_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX2_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX3_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX4_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX2_LOW_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX3_LOW_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_AUX4_LOW_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_ENCODER_A_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_ENCODER_B_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_ENCODER_PRESS_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_AIL_NEG_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_AIL_POS_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_ELE_NEG_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_ELE_POS_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_THR_NEG_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_THR_POS_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_RUD_NEG_GPIO) &&
         configure_optional_switch(CONFIG_RIVETTX_TRIM_RUD_POS_GPIO);
#endif
  return digital_inputs_ok;
}

RawInputs EspBoard::sample_inputs(TimeUs sample_time_us)
{
  RawInputs inputs{};
  inputs.valid = true;
  inputs.sampled_at_us = sample_time_us;
  for (std::size_t i = 0; i < axes_.size(); ++i) {
    if (!axes_[i].configured) {
      continue;
    }
    int value = 0;
    if (!read_adc(axes_[i], value)) {
      inputs.valid = false;
      continue;
    }
    inputs.axes[i] = static_cast<int16_t>(value);
  }
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  const uint32_t expanded = rev_a_controls_.load(std::memory_order_acquire);
  const TimeUs expanded_at =
      rev_a_controls_sampled_at_.load(std::memory_order_acquire);
  const bool expanded_valid =
      rev_a_controls_valid_.load(std::memory_order_acquire) &&
      expanded_at != 0 && sample_time_us >= expanded_at &&
      sample_time_us - expanded_at <= 50000;
  inputs.valid = inputs.valid && expanded_valid;
  const auto expanded_input = [expanded](uint8_t bit) {
    return (expanded & (1UL << bit)) != 0;
  };
  inputs.switches[0] = expanded_input(0);
  inputs.switches[1] = expanded_input(1);
  inputs.switches[2] = expanded_input(2);
  inputs.switches[3] = expanded_input(3);
  inputs.switches[4] = expanded_input(4);
#else
  inputs.switches[0] = active_low_input(CONFIG_RIVETTX_BUTTON_UP);
  inputs.switches[1] = active_low_input(CONFIG_RIVETTX_BUTTON_DOWN);
  inputs.switches[2] = active_low_input(CONFIG_RIVETTX_BUTTON_ENTER);
  inputs.switches[3] = active_low_input(CONFIG_RIVETTX_BUTTON_BACK);
  inputs.switches[4] = active_low_input(CONFIG_RIVETTX_AUX1_GPIO);
#endif
  inputs.switch_positions_valid = true;
  for (std::size_t i = 0; i < kNavigationButtonCount; ++i) {
    inputs.switch_positions[i] = inputs.switches[i] ? 1 : -1;
  }
  inputs.switch_positions[4] = inputs.switches[4] ? 1 : -1;
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  const auto expanded_position = [&expanded_input](uint8_t high_bit,
                                                   uint8_t low_bit) {
    const bool high = expanded_input(high_bit);
    const bool low = expanded_input(low_bit);
    return high == low ? (high ? int8_t{0} : int8_t{0})
                       : (high ? int8_t{1} : int8_t{-1});
  };
  if ((expanded_input(5) && expanded_input(6)) ||
      (expanded_input(7) && expanded_input(8)) ||
      (expanded_input(9) && expanded_input(10))) {
    inputs.valid = false;
    inputs.switch_positions_valid = false;
  }
  inputs.switch_positions[5] = expanded_position(5, 6);
  inputs.switch_positions[6] = expanded_position(7, 8);
  inputs.switch_positions[7] = expanded_position(9, 10);
#else
  inputs.switch_positions[5] = switch_position(
      CONFIG_RIVETTX_AUX2_GPIO, CONFIG_RIVETTX_AUX2_LOW_GPIO, inputs.valid);
  inputs.switch_positions[6] = switch_position(
      CONFIG_RIVETTX_AUX3_GPIO, CONFIG_RIVETTX_AUX3_LOW_GPIO, inputs.valid);
  inputs.switch_positions[7] = switch_position(
      CONFIG_RIVETTX_AUX4_GPIO, CONFIG_RIVETTX_AUX4_LOW_GPIO, inputs.valid);
#endif
  for (std::size_t i = 5; i < 8; ++i) {
    inputs.switches[i] = inputs.switch_positions[i] > 0;
  }

#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  for (std::size_t i = 0; i < kTrimSwitchCount; ++i) {
    const std::size_t index = kFirstTrimSwitch + i;
    inputs.switches[index] = expanded_input(static_cast<uint8_t>(12 + i));
    inputs.switch_positions[index] = inputs.switches[index] ? 1 : -1;
  }
#else
  const std::array<int, kTrimSwitchCount> trim_pins{
      CONFIG_RIVETTX_TRIM_AIL_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_AIL_POS_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_ELE_POS_GPIO,
      CONFIG_RIVETTX_TRIM_THR_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_THR_POS_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_NEG_GPIO,
      CONFIG_RIVETTX_TRIM_RUD_POS_GPIO};
  for (std::size_t i = 0; i < trim_pins.size(); ++i) {
    const std::size_t index = kFirstTrimSwitch + i;
    inputs.switches[index] = active_low_input(trim_pins[i]);
    inputs.switch_positions[index] = inputs.switches[index] ? 1 : -1;
  }
#endif

  if (CONFIG_RIVETTX_ENCODER_A_GPIO >= 0) {
    inputs.encoder_delta = encoder_decoder_.update(
        active_low_input(CONFIG_RIVETTX_ENCODER_A_GPIO),
        active_low_input(CONFIG_RIVETTX_ENCODER_B_GPIO));
  }
  inputs.encoder_pressed =
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
      expanded_input(11);
#else
      active_low_input(CONFIG_RIVETTX_ENCODER_PRESS_GPIO);
#endif
  return inputs;
}

void EspBoard::publish_rev_a_controls(uint32_t active_low_bits,
                                      TimeUs sampled_at_us, bool valid)
{
  rev_a_controls_.store(active_low_bits, std::memory_order_relaxed);
  rev_a_controls_sampled_at_.store(sampled_at_us,
                                   std::memory_order_relaxed);
  rev_a_controls_valid_.store(valid, std::memory_order_release);
}

uint8_t EspBoard::configured_axis_count() const
{
  return configured_axis_count_;
}

BatterySensorSample EspBoard::sample_battery()
{
  BatterySensorSample sample{};
  sample.configured = battery_.configured;
  if (!battery_.configured) {
    sample.valid = true;
    return sample;
  }
  int raw = 0;
  if (!read_adc(battery_, raw)) {
    return sample;
  }
  int calibrated_mv = 0;
  const uint32_t pin_mv =
      adc_calibration_ != nullptr &&
              adc_cali_raw_to_voltage(adc_calibration_, raw,
                                      &calibrated_mv) == ESP_OK
          ? static_cast<uint32_t>(std::max(0, calibrated_mv))
          : static_cast<uint32_t>(raw) * 3300U / 4095U;
  sample.millivolts = static_cast<uint16_t>(std::min<uint32_t>(
      UINT16_MAX,
      pin_mv * CONFIG_RIVETTX_BATTERY_DIVIDER_MILLI / 1000U));
  // Validity describes the ADC/configuration operation, not the voltage.
  // A successfully sampled 0 V is a valid critical reading and must fail
  // closed through the battery safety policy.
  sample.valid = true;
  return sample;
}

bool EspBoard::recovery_button_pressed() const
{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  return (rev_a_controls_.load(std::memory_order_acquire) & (1UL << 3U)) != 0;
#else
  return gpio_get_level(
             static_cast<gpio_num_t>(CONFIG_RIVETTX_BUTTON_BACK)) == 0;
#endif
}

bool EspCrsfTransport::initialize()
{
  uart_config_t config{};
  config.baud_rate = 400000;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  return uart_driver_install(port_, 512, 512, 0, nullptr, 0) == ESP_OK &&
         uart_param_config(port_, &config) == ESP_OK &&
         uart_set_pin(port_, CONFIG_RIVETTX_CRSF_TX,
                      CONFIG_RIVETTX_CRSF_RX, UART_PIN_NO_CHANGE,
                      UART_PIN_NO_CHANGE) == ESP_OK;
}

bool EspCrsfTransport::write(const uint8_t* data, std::size_t size)
{
  return uart_write_bytes(port_, data, size) ==
         static_cast<int>(size);
}

std::size_t EspCrsfTransport::read(uint8_t* data, std::size_t capacity)
{
  const int result =
      uart_read_bytes(port_, data, capacity, 0);
  return result > 0 ? static_cast<std::size_t>(result) : 0;
}

void EspCrsfTransport::set_baud_rate(uint32_t baud)
{
  (void)uart_set_baudrate(port_, baud);
}

void EspCrsfTransport::reset_module()
{
  (void)uart_flush(port_);
}

bool Ssd1306Display::command(const uint8_t* bytes, std::size_t size)
{
  std::vector<uint8_t> transfer(size + 1);
  transfer[0] = 0x00;
  std::copy(bytes, bytes + size, transfer.begin() + 1);
  return i2c_master_transmit(device_, transfer.data(), transfer.size(),
                             100) == ESP_OK;
}

bool Ssd1306Display::initialize()
{
  capabilities_.width = 128;
  capabilities_.height = 64;
  capabilities_.color_depth = 1;
  capabilities_.touch = false;
  capabilities_.partial_refresh = false;
  capabilities_.preferred_font_height = 8;

  i2c_master_bus_config_t bus_config{};
  bus_config.i2c_port = -1;
  bus_config.sda_io_num =
      static_cast<gpio_num_t>(CONFIG_RIVETTX_I2C_SDA);
  bus_config.scl_io_num =
      static_cast<gpio_num_t>(CONFIG_RIVETTX_I2C_SCL);
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
    return false;
  }

  i2c_device_config_t device_config{};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = CONFIG_RIVETTX_OLED_ADDRESS;
  device_config.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus_, &device_config, &device_) != ESP_OK) {
    return false;
  }

  constexpr std::array<uint8_t, 25> init{
      0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D,
      0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF,
      0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
  return command(init.data(), init.size());
}

const DisplayCapabilities& Ssd1306Display::capabilities() const
{
  return capabilities_;
}

bool Ssd1306Display::flush(const MonoCanvas& canvas)
{
  if (canvas.width() != capabilities_.width ||
      canvas.height() != capabilities_.height) {
    return false;
  }
  constexpr std::array<uint8_t, 6> address{
      0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
  if (!command(address.data(), address.size())) {
    return false;
  }

  std::vector<uint8_t> transfer(1 + 128 * 8);
  transfer[0] = 0x40;
  for (uint16_t page = 0; page < 8; ++page) {
    for (uint16_t x = 0; x < 128; ++x) {
      uint8_t value = 0;
      for (uint8_t bit = 0; bit < 8; ++bit) {
        if (canvas.pixel_at(x, static_cast<int16_t>(page * 8 + bit))) {
          value |= static_cast<uint8_t>(1U << bit);
        }
      }
      transfer[1 + page * 128 + x] = value;
    }
  }
  return i2c_master_transmit(device_, transfer.data(), transfer.size(),
                             200) == ESP_OK;
}

void EspWatchdog::kick()
{
  const esp_err_t result = esp_task_wdt_reset();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "watchdog kick failed: %s",
             esp_err_to_name(result));
  }
}

void EspToneOutput::stop_callback(void* context)
{
  static_cast<EspToneOutput*>(context)->stop_tone();
}

bool EspToneOutput::initialize()
{
  if (CONFIG_RIVETTX_BUZZER_GPIO < 0) {
    return false;
  }
  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 1000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ledc_channel_config_t channel{};
  channel.gpio_num = CONFIG_RIVETTX_BUZZER_GPIO;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = 0;
  channel.hpoint = 0;
  esp_timer_create_args_t timer_args{};
  timer_args.callback = stop_callback;
  timer_args.arg = this;
  timer_args.name = "rivet-tone";
  initialized_ =
      ledc_timer_config(&timer) == ESP_OK &&
      ledc_channel_config(&channel) == ESP_OK &&
      esp_timer_create(&timer_args, &stop_timer_) == ESP_OK;
  return initialized_;
}

bool EspToneOutput::play_tone(uint16_t frequency_hz,
                              uint16_t duration_ms)
{
  if (!initialized_ || frequency_hz < 100 || duration_ms == 0) {
    return false;
  }
  const uint32_t duty =
      1023U * CONFIG_RIVETTX_BUZZER_VOLUME / 100U;
  (void)esp_timer_stop(stop_timer_);
  if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0,
                    std::min<uint16_t>(frequency_hz, 5000)) == 0 ||
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty) != ESP_OK ||
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK) {
    return false;
  }
  return esp_timer_start_once(
             stop_timer_, static_cast<uint64_t>(duration_ms) * 1000U) ==
         ESP_OK;
}

void EspToneOutput::stop_tone()
{
  if (initialized_) {
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  }
}

bool EspToneOutput::available() const
{
  return initialized_;
}

bool EspOtaBackend::running_image_pending_verification() const
{
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state{};
  return running != nullptr &&
         esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool EspOtaBackend::mark_running_image_valid()
{
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool EspOtaBackend::request_rollback()
{
  return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

bool EspOtaBackend::begin_https_update(const std::string& url)
{
  esp_http_client_config_t http_config{};
  http_config.url = url.c_str();
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.timeout_ms = 15000;
  http_config.keep_alive_enable = true;
  esp_https_ota_config_t ota_config{};
  ota_config.http_config = &http_config;
  return esp_https_ota(&ota_config) == ESP_OK;
}

bool NvsCrashStore::initialize()
{
  return nvs_open("rivettx", NVS_READWRITE, &handle_) == ESP_OK;
}

bool NvsCrashStore::write(const CrashSnapshot& snapshot)
{
  return handle_ != 0 &&
         nvs_set_blob(handle_, "crash", &snapshot, sizeof(snapshot)) ==
             ESP_OK &&
         nvs_commit(handle_) == ESP_OK;
}

bool NvsCrashStore::read(CrashSnapshot& snapshot)
{
  std::size_t size = sizeof(snapshot);
  return handle_ != 0 &&
         nvs_get_blob(handle_, "crash", &snapshot, &size) == ESP_OK &&
         size == sizeof(snapshot) && snapshot.magic == 0x52564352U;
}

void NvsCrashStore::clear()
{
  if (handle_ != 0) {
    (void)nvs_erase_key(handle_, "crash");
    (void)nvs_commit(handle_);
  }
}

bool NvsBootState::initialize()
{
  if (nvs_open("rivetboot", NVS_READWRITE, &handle_) != ESP_OK) {
    return false;
  }
  if (nvs_get_u32(handle_, "attempts", &attempts_) != ESP_OK) {
    attempts_ = 0;
  }
  return true;
}

uint32_t NvsBootState::begin_attempt()
{
  if (handle_ == 0) {
    return attempts_;
  }
  ++attempts_;
  if (nvs_set_u32(handle_, "attempts", attempts_) != ESP_OK ||
      nvs_commit(handle_) != ESP_OK) {
    return attempts_;
  }
  return attempts_;
}

bool NvsBootState::mark_success()
{
  attempts_ = 0;
  return handle_ != 0 &&
         nvs_set_u32(handle_, "attempts", 0) == ESP_OK &&
         nvs_commit(handle_) == ESP_OK;
}

uint32_t NvsBootState::failed_attempts() const
{
  return attempts_;
}

CsvTelemetrySink::CsvTelemetrySink(std::string path,
                                   std::size_t maximum_bytes)
    : path_(std::move(path)),
      maximum_bytes_(std::max<std::size_t>(4096, maximum_bytes))
{
}

CsvTelemetrySink::~CsvTelemetrySink()
{
  if (file_ != nullptr) {
    FILE* stream = static_cast<FILE*>(file_);
    (void)std::fflush(stream);
    (void)std::fclose(stream);
    file_ = nullptr;
  }
}

bool CsvTelemetrySink::append(TimeUs time_us, uint16_t sensor_id,
                              int32_t value)
{
  if (file_ == nullptr) {
    file_ = std::fopen(path_.c_str(), "a");
  }
  if (file_ == nullptr) {
    return false;
  }
  FILE* stream = static_cast<FILE*>(file_);
  const long position = std::ftell(stream);
  if (position >= 0 &&
      static_cast<std::size_t>(position) >= maximum_bytes_) {
    const bool flushed = std::fflush(stream) == 0;
    const bool closed = std::fclose(stream) == 0;
    file_ = nullptr;
    if (!flushed || !closed) {
      return false;
    }
    const std::string rotated = path_ + ".1";
    if (std::remove(rotated.c_str()) != 0 && errno != ENOENT) {
      return false;
    }
    if (std::rename(path_.c_str(), rotated.c_str()) != 0) {
      return false;
    }
    file_ = std::fopen(path_.c_str(), "w");
    if (file_ == nullptr) {
      return false;
    }
    stream = static_cast<FILE*>(file_);
  }
  const int written =
      std::fprintf(stream, "%llu,%u,%ld\n",
                   static_cast<unsigned long long>(time_us),
                   static_cast<unsigned>(sensor_id),
                   static_cast<long>(value));
  return written > 0 && std::ferror(stream) == 0;
}

bool CsvTelemetrySink::flush()
{
  if (file_ == nullptr) {
    return true;
  }
  FILE* stream = static_cast<FILE*>(file_);
  const bool flushed = std::fflush(stream) == 0;
  const bool closed = std::fclose(stream) == 0;
  file_ = nullptr;
  return flushed && closed;
}

WifiBackupPortal::WifiBackupPortal(TransactionalModelStore& models,
                                   ModelLibrary& library,
                                   SafetyManager& safety)
    : models_(models), library_(library), safety_(safety)
{
}

esp_err_t WifiBackupPortal::get_index(httpd_req_t* request)
{
  static constexpr char page[] =
      "<!doctype html><meta name=viewport "
      "content='width=device-width,initial-scale=1'>"
      "<title>RivetTX Config</title>"
      "<h1>RivetTX</h1>"
      "<p>Outputs are locked while this page is active.</p>"
      "<p><a href=/backup>Download active model</a></p>"
      "<form method=post action=/restore enctype=application/octet-stream>"
      "<label>Restore .rvm: <input type=file id=f></label>"
      "<button type=button onclick='fetch(\"/restore\","
      "{method:\"POST\",body:f.files[0]}).then(r=>r.text())"
      ".then(alert)'>Verify and restore</button></form>"
      "<p><a href=/status>Device status</a></p>";
  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiBackupPortal::get_backup(httpd_req_t* request)
{
  auto* portal = static_cast<WifiBackupPortal*>(request->user_ctx);
  if (!portal->safety_.maintenance_allowed()) {
    httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                        "lock transmitter before backup");
    return ESP_FAIL;
  }
  std::vector<uint8_t> data;
  if (!portal->library_.export_active(data)) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no valid active model");
    return ESP_FAIL;
  }
  httpd_resp_set_type(request, "application/octet-stream");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=active.rvm");
  return httpd_resp_send(
      request, reinterpret_cast<const char*>(data.data()), data.size());
}

esp_err_t WifiBackupPortal::post_restore(httpd_req_t* request)
{
  auto* portal = static_cast<WifiBackupPortal*>(request->user_ctx);
  if (!portal->safety_.maintenance_allowed()) {
    httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                        "lock transmitter before restore");
    return ESP_FAIL;
  }
  if (request->content_len <= 0 || request->content_len > 32 * 1024) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                        "invalid model size");
    return ESP_FAIL;
  }
  std::vector<uint8_t> data(static_cast<std::size_t>(request->content_len));
  std::size_t received = 0;
  while (received < data.size()) {
    const int result = httpd_req_recv(
        request, reinterpret_cast<char*>(data.data() + received),
        data.size() - received);
    if (result <= 0) {
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "upload interrupted");
      return ESP_FAIL;
    }
    received += static_cast<std::size_t>(result);
  }
  std::string error;
  if (!portal->library_.import_active(data, error)) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, error.c_str());
    return ESP_FAIL;
  }
  std::string mirror_error;
  if (!portal->models_.import_candidate(data, mirror_error)) {
    ESP_LOGW(kTag, "restored model mirror failed: %s",
             mirror_error.c_str());
  }
  return httpd_resp_sendstr(
      request, "model verified and restored; leave Wi-Fi mode to activate");
}

esp_err_t WifiBackupPortal::get_status(httpd_req_t* request)
{
  auto* portal = static_cast<WifiBackupPortal*>(request->user_ctx);
  const char* state =
      portal->safety_.maintenance_allowed() ? "maintenance" : "enabled";
  httpd_resp_set_type(request, "application/json");
  std::string response =
      std::string("{\"project\":\"RivetTX\",\"state\":\"") + state +
      "\",\"schema\":" + std::to_string(Model::kSchemaVersion) + "}";
  return httpd_resp_sendstr(request, response.c_str());
}

bool WifiBackupPortal::start()
{
#if CONFIG_RIVETTX_WIFI_BACKUP
  if (server_ != nullptr) {
    return true;
  }
  if (!safety_.begin_maintenance()) {
    return false;
  }
  const auto fail = [this]() {
    safety_.end_maintenance();
    return false;
  };
  if (std::strlen(CONFIG_RIVETTX_WIFI_PASSWORD) < 8 ||
      std::strlen(CONFIG_RIVETTX_WIFI_PASSWORD) > 63) {
    ESP_LOGE(kTag, "recovery Wi-Fi password must contain 8..63 bytes");
    return fail();
  }
  if (esp_netif_init() != ESP_OK) {
    return fail();
  }
  const esp_err_t loop_result = esp_event_loop_create_default();
  if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
    return fail();
  }
  if (esp_netif_create_default_wifi_ap() == nullptr) {
    return fail();
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&init) != ESP_OK ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
      esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) {
    return fail();
  }
  wifi_config_t wifi{};
  std::strncpy(reinterpret_cast<char*>(wifi.ap.ssid),
               "RivetTX-Recovery", sizeof(wifi.ap.ssid));
  std::strncpy(reinterpret_cast<char*>(wifi.ap.password),
               CONFIG_RIVETTX_WIFI_PASSWORD, sizeof(wifi.ap.password));
  wifi.ap.ssid_len = 0;
  wifi.ap.channel = 1;
  wifi.ap.max_connection = 2;
  wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
  if (esp_wifi_set_config(WIFI_IF_AP, &wifi) != ESP_OK ||
      esp_wifi_start() != ESP_OK) {
    return fail();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 5;
  if (httpd_start(&server_, &config) != ESP_OK) {
    return fail();
  }
  httpd_uri_t index{};
  index.uri = "/";
  index.method = HTTP_GET;
  index.handler = get_index;
  index.user_ctx = this;
  httpd_uri_t backup{};
  backup.uri = "/backup";
  backup.method = HTTP_GET;
  backup.handler = get_backup;
  backup.user_ctx = this;
  httpd_uri_t restore{};
  restore.uri = "/restore";
  restore.method = HTTP_POST;
  restore.handler = post_restore;
  restore.user_ctx = this;
  httpd_uri_t status{};
  status.uri = "/status";
  status.method = HTTP_GET;
  status.handler = get_status;
  status.user_ctx = this;
  const bool registered =
      httpd_register_uri_handler(server_, &index) == ESP_OK &&
      httpd_register_uri_handler(server_, &backup) == ESP_OK &&
      httpd_register_uri_handler(server_, &restore) == ESP_OK &&
      httpd_register_uri_handler(server_, &status) == ESP_OK;
  if (!registered) {
    (void)httpd_stop(server_);
    server_ = nullptr;
    return fail();
  }
  return true;
#else
  return false;
#endif
}

void WifiBackupPortal::stop()
{
  if (server_ != nullptr) {
    const esp_err_t server_result = httpd_stop(server_);
    if (server_result != ESP_OK) {
      ESP_LOGE(kTag, "web server stop failed: %s",
               esp_err_to_name(server_result));
    }
    server_ = nullptr;
    safety_.end_maintenance();
  }
#if CONFIG_RIVETTX_WIFI_BACKUP
  const esp_err_t wifi_result = esp_wifi_stop();
  if (wifi_result != ESP_OK && wifi_result != ESP_ERR_WIFI_NOT_INIT) {
    ESP_LOGE(kTag, "Wi-Fi stop failed: %s",
             esp_err_to_name(wifi_result));
  }
#endif
}

bool WifiBackupPortal::running() const
{
  return server_ != nullptr;
}

bool mount_model_filesystem(bool format_if_mount_failed)
{
  esp_vfs_fat_mount_config_t config{};
  config.format_if_mount_failed = format_if_mount_failed;
  config.max_files = 8;
  config.allocation_unit_size = 4096;
  return esp_vfs_fat_spiflash_mount_rw_wl(
             "/models", "models", &config, &filesystem_wl) == ESP_OK;
}

}  // namespace rivettx::esp32
