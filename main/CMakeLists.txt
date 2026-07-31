set(rivettx_private_requires
  app_update
  mbedtls
)

if(CONFIG_IDF_TARGET_ESP32S3)
  list(APPEND rivettx_private_requires esp_tinyusb)
endif()

idf_component_register(
  SRCS
    "app_main.cpp"
    "esp_platform.cpp"
    "usb_gamepad.cpp"
  INCLUDE_DIRS "."
  REQUIRES
    rivet_core
    rivet_lua
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_sdmmc
    esp_driver_spi
    esp_driver_uart
    esp_https_ota
    esp_http_server
    esp_wifi
    esp_timer
    fatfs
    nvs_flash
    wear_levelling
  PRIV_REQUIRES
    ${rivettx_private_requires}
)

if(CONFIG_RIVETTX_OPENPOCKET_REV_A)
  target_add_binary_data(${COMPONENT_LIB}
    "firmware/amt630a-openpocket-er-tft050a3-2.bin" BINARY)
endif()
