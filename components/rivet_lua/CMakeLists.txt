idf_component_register(
  SRCS "lua_vm.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    rivet_core
    esp_timer
  PRIV_REQUIRES
    lua
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
