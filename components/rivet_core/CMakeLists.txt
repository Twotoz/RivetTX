idf_component_register(
  SRCS
    "at7456e.cpp"
    "audio.cpp"
    "board_power.cpp"
    "core.cpp"
    "crsf.cpp"
    "elrs.cpp"
    "product.cpp"
    "rx5808.cpp"
    "services.cpp"
    "speaker.cpp"
    "storage.cpp"
    "amt630a.cpp"
    "ui.cpp"
  INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
