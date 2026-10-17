# Read-only live Wii U radio-state probe. Uses the repo-local wut toolchain.
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment.")
endif

TOPDIR ?= $(CURDIR)
APP_NAME := ZiiU Live Probe
APP_SHORTNAME := ZiiU Live Probe
APP_AUTHOR := ZiiUScreen
include $(DEVKITPRO)/wut/share/wut_rules

TARGET := ziiu-live-probe
BUILD := build
SOURCES := source
DATA :=
INCLUDES :=
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__
ASFLAGS := -g $(ARCH)
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)
LIBS := -lwut
LIBDIRS := $(PORTLIBS) $(WUT_ROOT)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf
else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).wuhb
$(OUTPUT).wuhb: $(OUTPUT).rpx
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
