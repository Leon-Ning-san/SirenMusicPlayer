#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := Sirène Overlay
APP_AUTHOR  := Sirène
APP_VERSION := 1.2.10

TARGET   := SireneOverlay
BUILD    := build
SOURCES  := source ../common
INCLUDES := source ../common libs/libtesla/include
NO_ICON  := 1

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
            $(INCLUDE) -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include
CXXFLAGS := $(CFLAGS) -fno-exceptions -std=c++20
ASFLAGS  := -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx -lm
LIBDIRS := $(PORTLIBS) $(DEVKITPRO)/libnx

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(CURDIR)/source:$(CURDIR)/../common
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := sirene_ipc_client.c
CPPFILES := main.cpp

export LD      := $(CXX)
export OFILES  := $(CFILES:.c=.o) $(CPPFILES:.cpp=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).lst

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl)"

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
