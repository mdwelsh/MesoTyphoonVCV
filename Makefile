RACK_DIR ?= ../build/rack-sdk/Rack-SDK

FIRMWARE := $(abspath ..)
FLAGS += -DTEST -I"$(FIRMWARE)"
SOURCES += src/Superparasites.cpp
SOURCES += $(wildcard ../supercell/dsp/*.cc ../supercell/dsp/pvoc/*.cc)
SOURCES += ../supercell/resources.cc ../stmlib/dsp/atan.cc
SOURCES += ../stmlib/dsp/units.cc ../stmlib/utils/random.cc

# Keep the SDK's objects for the shared firmware sources inside rack/build.
SOURCES := $(patsubst ../%,firmware/%,$(SOURCES))
DISTRIBUTABLES += LICENSE

include $(RACK_DIR)/plugin.mk

build/firmware/%.cc.o: ../%.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
