CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Icomponents/rivet_core/include
BUILD := build
CORE := \
	components/rivet_core/core.cpp \
	components/rivet_core/crsf.cpp \
	components/rivet_core/services.cpp \
	components/rivet_core/storage.cpp \
	components/rivet_core/ui.cpp

.PHONY: all test clean

all: $(BUILD)/rivettx-sim $(BUILD)/rivettx-tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/rivettx-sim: $(CORE) simulator/main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILD)/rivettx-tests: $(CORE) tests/test_main.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/rivettx-tests
	./$(BUILD)/rivettx-tests

clean:
	rm -rf $(BUILD)
