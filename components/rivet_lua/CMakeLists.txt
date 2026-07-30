idf_component_register(
  SRCS "lua_vm.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    rivet_core
    esp_timer
  PRIV_REQUIRES
    lua
)

idf_component_get_property(lua_component lua COMPONENT_LIB)
target_compile_definitions(${lua_component} PUBLIC LUA_32BITS=1)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
