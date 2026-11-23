RACK_DIR ?= build/rack-sdk/Rack-SDK
FIRMWARE_DIR ?= firmware
FIRMWARE := $(abspath $(FIRMWARE_DIR))

FLAGS += -DTEST -I"$(FIRMWARE)"
SOURCES += src/Superparasites.cpp
FIRMWARE_SOURCES := $(wildcard $(FIRMWARE)/supercell/dsp/*.cc $(FIRMWARE)/supercell/dsp/pvoc/*.cc)
FIRMWARE_SOURCES += $(FIRMWARE)/supercell/resources.cc $(FIRMWARE)/stmlib/dsp/atan.cc
FIRMWARE_SOURCES += $(FIRMWARE)/stmlib/dsp/units.cc $(FIRMWARE)/stmlib/utils/random.cc

# Keep the SDK's objects for the shared firmware sources inside build.
SOURCES += $(patsubst $(FIRMWARE)/%,firmware/%,$(FIRMWARE_SOURCES))
DISTRIBUTABLES += LICENSE

include $(RACK_DIR)/plugin.mk

build/firmware/%.cc.o: $(FIRMWARE)/%.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
