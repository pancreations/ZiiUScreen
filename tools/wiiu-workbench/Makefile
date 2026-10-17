.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is required; use build.ps1")
endif
WUPS_ROOT ?= $(DEVKITPRO)/wups
include $(WUPS_ROOT)/share/wups_rules
TARGET := ziiu-workbench
BUILD := build
SOURCES := source
WUMS_ROOT := $(DEVKITPRO)/wums
LIBDIRS := $(PORTLIBS) $(WUPS_ROOT) $(WUT_ROOT) $(WUMS_ROOT)
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__ -D__WUPS__
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) -T$(WUMS_ROOT)/share/libmappedmemory.ld $(WUPSSPECS)
LIBS := -lwups -lwut -lmappedmemory
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: all $(BUILD) clean
all: $(BUILD)
$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.d $(TARGET).wps $(TARGET).elf
else
.PHONY: all
all: $(OUTPUT).wps
$(OUTPUT).wps: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
