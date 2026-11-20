.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Define DEVKITPRO con la ruta de devkitPro y Ounce Devkit)
endif

APP_TITLE := NovaSave2
APP_AUTHOR := Nova
APP_VERSION := 0.4.16

include $(DEVKITPRO)/libounce/switch2_rules

TARGET := NovaSave2
BUILD := build
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := -O2 -Wall -Wextra -std=gnu11 -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE)
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--gc-sections
LIBS := -lz -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CC)
export OFILES := main.o backup_log.o archive.o catalog.o titledata.o progress.o zipio.o
export INCLUDE := -I$(LIBNX)/include -I$(PORTLIBS)/include
export LIBPATHS := -L$(LIBNX)/lib -L$(PORTLIBS)/lib
export NROFLAGS := --icon=$(CURDIR)/icon.jpg --nacp=$(OUTPUT).nacp

.PHONY: all clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp
else
.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
