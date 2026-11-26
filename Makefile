ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO to a devkitPro installation containing ounce-devkit")
endif

TARGET  := KaiDump
BUILD   := build
ICON    := assets/icon.jpg
PREFIX  := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-

include $(DEVKITPRO)/libounce/switch2_cpu.mk

ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS   := $(ARCH) -std=gnu11 -O2 -g -Wall -Wextra \
            -ffunction-sections -fdata-sections \
            -D__SWITCH__ -D__SWITCH2__ \
            -Isource/mtp \
            -I$(DEVKITPRO)/libounce/include \
            -I$(DEVKITPRO)/portlibs/switch/include/SDL2 \
            -I$(DEVKITPRO)/portlibs/switch/include \
            -I$(DEVKITPRO)/portlibs/switch2/include
LDFLAGS  := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs \
            -Wl,--gc-sections,-Map,$(BUILD)/$(TARGET).map
LIBS     := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib \
            -L$(DEVKITPRO)/portlibs/switch/lib \
            -Wl,--start-group -lSDL2_ttf -lSDL2 -lEGL -lglapi \
            -ldrm_nouveau -lharfbuzz -lfreetype -lbz2 -lpng16 -ljpeg \
            -lounce_core -lz -lstdc++ -lpthread -lm \
            -Wl,--end-group

APP_SOURCES := main.c app.c catalog.c screens.c content_access.c \
               content_export.c export.c program_select.c source_io.c \
               ui.c nup.c tar.c ftp.c virtual_file.c virtual_tree.c
MTP_SOURCES := mtp/dataset.c mtp/mtp.c mtp/objdb.c mtp/status.c \
               mtp/usb_transport.c
SOURCES     := $(APP_SOURCES) $(MTP_SOURCES)
OBJECTS     := $(SOURCES:%.c=$(BUILD)/%.o)

APP_TITLE   := KaiDump
APP_AUTHOR  := TomDom
APP_VERSION := 1.0.8
CFLAGS      += -DKAIDUMP_VERSION=\"$(APP_VERSION)\"

all: $(TARGET).nro

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: source/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(PREFIX)gcc $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: Makefile | $(BUILD)
	$(DEVKITPRO)/tools/bin/nacptool --create \
		"$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp $(ICON)
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ \
		--icon=$(ICON) --nacp=$(BUILD)/$(TARGET).nacp

clean:
	rm -rf $(BUILD) $(TARGET).nro

-include $(OBJECTS:.o=.d)

.PHONY: all clean
