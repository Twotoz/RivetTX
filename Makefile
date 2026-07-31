CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Icomponents/rivet_core/include -Isimulator
BUILD := build
SANITIZE_BUILD := $(BUILD)/sanitize
CORE := \
	components/rivet_core/at7456e.cpp \
	components/rivet_core/audio.cpp \
	components/rivet_core/core.cpp \
	components/rivet_core/crsf.cpp \
	components/rivet_core/elrs.cpp \
	components/rivet_core/product.cpp \
	components/rivet_core/services.cpp \
	components/rivet_core/storage.cpp \
	components/rivet_core/ui.cpp
SIMULATION := simulator/virtual_hardware.cpp
COMMON_SOURCES := $(CORE) $(SIMULATION)
COMMON_OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(COMMON_SOURCES))
SANITIZE_OBJECTS := \
	$(patsubst %.cpp,$(SANITIZE_BUILD)/%.o,$(COMMON_SOURCES))
TEST_OBJECT := $(BUILD)/tests/test_main.o
SIM_OBJECT := $(BUILD)/simulator/main.o
SANITIZE_TEST_OBJECT := $(SANITIZE_BUILD)/tests/test_main.o
SANITIZE_SIM_OBJECT := $(SANITIZE_BUILD)/simulator/main.o
OBJECTS := \
	$(COMMON_OBJECTS) $(TEST_OBJECT) $(SIM_OBJECT) \
	$(SANITIZE_OBJECTS) $(SANITIZE_TEST_OBJECT) $(SANITIZE_SIM_OBJECT)
DEPENDENCIES := $(OBJECTS:.o=.d)
SANITIZER_FLAGS := \
	-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test sanitize clean

all: $(BUILD)/rivettx-sim $(BUILD)/rivettx-tests

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(SANITIZE_BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZER_FLAGS) \
		-MMD -MP -c $< -o $@

$(BUILD)/rivettx-sim: $(COMMON_OBJECTS) $(SIM_OBJECT)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/rivettx-tests: $(COMMON_OBJECTS) $(TEST_OBJECT)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/rivettx-tests
	./$(BUILD)/rivettx-tests

$(BUILD)/rivettx-tests-sanitize: \
		$(SANITIZE_OBJECTS) $(SANITIZE_TEST_OBJECT)
	$(CXX) $(CXXFLAGS) $(SANITIZER_FLAGS) $^ -o $@

$(BUILD)/rivettx-sim-sanitize: \
		$(SANITIZE_OBJECTS) $(SANITIZE_SIM_OBJECT)
	$(CXX) $(CXXFLAGS) $(SANITIZER_FLAGS) $^ -o $@

sanitize: $(BUILD)/rivettx-tests-sanitize \
		$(BUILD)/rivettx-sim-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./$(BUILD)/rivettx-tests-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./$(BUILD)/rivettx-sim-sanitize

clean:
	rm -rf $(BUILD)

-include $(DEPENDENCIES)
