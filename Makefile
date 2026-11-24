#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := PureMusic
BUILD := build
SOURCES := src
INCLUDES := src deps

APP_TITLE := Sirène
APP_DESCRIPTION := Lecteur musical minimaliste pour Switch
APP_AUTHOR := Sirène
APP_VERSION := 1.2.5
APP_ICON := icon.jpg

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -O3 -fno-math-errno -Wall -ffunction-sections $(ARCH)

CFLAGS += $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS := -g $(ARCH)

LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS := -lSDL2 -lEGL -lstdc++ -lglapi -ldrm_nouveau -lnx -lpthread -lm

PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBDIRS := $(PORTLIBS) $(DEVKITPRO)/libnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)) $(notdir $(wildcard $(dir)/*.S)))

export LD := $(CC)

export OFILES := $(CFILES:.c=.o) $(patsubst %.S,%.o,$(SFILES:.s=.o))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS += --icon=$(CURDIR)/$(APP_ICON)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) all clean

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).nro $(OUTPUT).nacp $(OUTPUT).lst

#---------------------------------------------------------------------------------
else

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
	@echo built ... $(notdir $@)
	@elf2nro $< $@ $(NROFLAGS)

$(OUTPUT).nacp: $(OUTPUT).elf
	@echo generating ... $(notdir $@)
	@nacptool --create "Sirene" "Sirene" "$(APP_VERSION)" $@
	@for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do \
		printf 'Sir\303\250ne\0\0\0\0' | dd of=$@ bs=1 seek=$$((i*768)) conv=notrunc status=none; \
		printf 'Sir\303\250ne\0\0\0\0' | dd of=$@ bs=1 seek=$$((i*768+512)) conv=notrunc status=none; \
	done

$(OUTPUT).elf: $(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
