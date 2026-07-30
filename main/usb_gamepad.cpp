#include "esp_platform.hpp"

#include "esp_log.h"

#include <array>
#include <cstdint>

#if CONFIG_IDF_TARGET_ESP32S3
#include "class/hid/hid_device.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#endif

namespace rivettx::esp32 {

namespace {

constexpr char kTag[] = "rivettx-usb";

#if CONFIG_IDF_TARGET_ESP32S3

constexpr uint8_t kHidEndpoint = 0x81;
constexpr uint16_t kUsbConfigurationLength =
    TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN;

// Eight signed 16-bit Generic Desktop axes followed by 32 buttons. The
// standard TinyUSB gamepad report has only six axes, which would discard the
// two OpenPocket scroll/pot controls.
const uint8_t kHidReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x30,        // Usage (X)
    0x09, 0x31,        // Usage (Y)
    0x09, 0x32,        // Usage (Z)
    0x09, 0x33,        // Usage (Rx)
    0x09, 0x34,        // Usage (Ry)
    0x09, 0x35,        // Usage (Rz)
    0x09, 0x36,        // Usage (Slider)
    0x09, 0x37,        // Usage (Dial)
    0x16, 0x01, 0x80,  // Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  // Logical Maximum (32767)
    0x75, 0x10,        // Report Size (16)
    0x95, 0x08,        // Report Count (8)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x05, 0x09,        // Usage Page (Button)
    0x19, 0x01,        // Usage Minimum (Button 1)
    0x29, 0x20,        // Usage Maximum (Button 32)
    0x15, 0x00,        // Logical Minimum (0)
    0x25, 0x01,        // Logical Maximum (1)
    0x75, 0x01,        // Report Size (1)
    0x95, 0x20,        // Report Count (32)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0xC0,              // End Collection
};

const uint8_t kHidConfigurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1, 1, 0, kUsbConfigurationLength,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(
        0, 4, false, sizeof(kHidReportDescriptor),
        kHidEndpoint, CFG_TUD_HID_EP_BUFSIZE, 4),
};

const char* kUsbStrings[] = {
    reinterpret_cast<const char*>(u"\x0409"),
    "RivetTX",
    "OpenPocket FPV Controller",
    "RIVETTX",
    "OpenPocket Gamepad",
};

int16_t hid_axis(int16_t value)
{
  return static_cast<int16_t>(
      clamp<int32_t>(-32767,
                     static_cast<int32_t>(value) * 32767 / kResolution,
                     32767));
}

struct HidOpenPocketReport {
  std::array<int16_t, 8> axes{};
  uint32_t buttons = 0;
};

static_assert(sizeof(HidOpenPocketReport) == 20);
static_assert(sizeof(HidOpenPocketReport) <= CFG_TUD_HID_EP_BUFSIZE);

#endif

}  // namespace

#if CONFIG_IDF_TARGET_ESP32S3

extern "C" const uint8_t* tud_hid_descriptor_report_cb(uint8_t)
{
  return kHidReportDescriptor;
}

extern "C" uint16_t tud_hid_get_report_cb(
    uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t)
{
  return 0;
}

extern "C" void tud_hid_set_report_cb(
    uint8_t, uint8_t, hid_report_type_t, const uint8_t*, uint16_t)
{
}

#endif

bool EspUsbGamepad::initialize()
{
#if CONFIG_IDF_TARGET_ESP32S3
  tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
  config.descriptor.device = nullptr;
  config.descriptor.full_speed_config = kHidConfigurationDescriptor;
  config.descriptor.string = kUsbStrings;
  config.descriptor.string_count =
      sizeof(kUsbStrings) / sizeof(kUsbStrings[0]);
#if TUD_OPT_HIGH_SPEED
  config.descriptor.high_speed_config = kHidConfigurationDescriptor;
#endif
  const esp_err_t result = tinyusb_driver_install(&config);
  initialized_ = result == ESP_OK;
  if (!initialized_) {
    ESP_LOGE(kTag, "TinyUSB HID initialization failed: %s",
             esp_err_to_name(result));
  }
  return initialized_;
#else
  ESP_LOGW(kTag, "USB HID requires ESP32-S3");
  return false;
#endif
}

bool EspUsbGamepad::send(const UsbGamepadReport& source)
{
#if CONFIG_IDF_TARGET_ESP32S3
  if (!initialized_ || !tud_mounted() || !tud_hid_ready()) {
    return false;
  }
  HidOpenPocketReport report{};
  for (std::size_t axis = 0; axis < report.axes.size(); ++axis) {
    report.axes[axis] = hid_axis(source.axes[axis]);
  }
  report.buttons = source.buttons;
  return tud_hid_report(0, &report, sizeof(report));
#else
  (void)source;
  return false;
#endif
}

bool EspUsbGamepad::supported() const
{
#if CONFIG_IDF_TARGET_ESP32S3
  return true;
#else
  return false;
#endif
}

bool EspUsbGamepad::mounted() const
{
#if CONFIG_IDF_TARGET_ESP32S3
  return initialized_ && tud_mounted();
#else
  return false;
#endif
}

}  // namespace rivettx::esp32
