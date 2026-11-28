#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := sys-sirene
BUILD       := build
CONFIG_JSON := sys-sirene.json

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -O2 -fno-math-errno -Wall -ffunction-sections -fdata-sections $(ARCH) \
          -D__SWITCH__ -DSIRENE_SYSMODULE \
          -I$(TOPDIR)/source -I$(TOPDIR)/../common -I$(TOPDIR)/../src -I$(TOPDIR)/../deps \
          -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include

ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx -lm
LIBDIRS := $(PORTLIBS) $(DEVKITPRO)/libnx

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export APP_JSON := $(CURDIR)/$(CONFIG_JSON)
export VPATH   := $(CURDIR)/source:$(CURDIR)/../src
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := main.c decode.c util.c
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) all clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean sys-sirene ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf $(TARGET).lst exefs

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
