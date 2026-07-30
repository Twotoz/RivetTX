CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Icomponents/rivet_core/include -Isimulator
BUILD := build
CORE := \
	components/rivet_core/audio.cpp \
	components/rivet_core/core.cpp \
	components/rivet_core/crsf.cpp \
	components/rivet_core/elrs.cpp \
	components/rivet_core/services.cpp \
	components/rivet_core/storage.cpp \
	components/rivet_core/ui.cpp
SIMULATION := simulator/virtual_hardware.cpp
SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test sanitize clean

all: $(BUILD)/rivettx-sim $(BUILD)/rivettx-tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rivettx-sim: $(CORE) $(SIMULATION) simulator/main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/rivettx-tests: $(CORE) $(SIMULATION) tests/test_main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/rivettx-tests
	./$(BUILD)/rivettx-tests

$(BUILD)/rivettx-tests-sanitize: $(CORE) $(SIMULATION) tests/test_main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZER_FLAGS) $^ -o $@

$(BUILD)/rivettx-sim-sanitize: $(CORE) $(SIMULATION) simulator/main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZER_FLAGS) $^ -o $@

sanitize: $(BUILD)/rivettx-tests-sanitize $(BUILD)/rivettx-sim-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./$(BUILD)/rivettx-tests-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./$(BUILD)/rivettx-sim-sanitize

clean:
	rm -rf $(BUILD)
