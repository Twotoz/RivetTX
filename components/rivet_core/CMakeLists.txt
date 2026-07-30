idf_component_register(
  SRCS
    "core.cpp"
    "crsf.cpp"
    "services.cpp"
    "storage.cpp"
    "ui.cpp"
  INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
