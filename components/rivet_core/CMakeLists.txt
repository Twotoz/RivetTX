idf_component_register(
  SRCS
    "audio.cpp"
    "core.cpp"
    "crsf.cpp"
    "elrs.cpp"
    "product.cpp"
    "services.cpp"
    "storage.cpp"
    "ui.cpp"
  INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
