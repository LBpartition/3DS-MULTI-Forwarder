.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif

TOPDIR ?= $(CURDIR)
FILE_PREFIX_MAP := $(patsubst %/,%,$(shell cygpath -m "$(TOPDIR)" 2>/dev/null))
ifeq ($(strip $(FILE_PREFIX_MAP)),)
FILE_PREFIX_MAP := $(TOPDIR)
endif
include $(DEVKITARM)/3ds_rules

TARGET      := 3dsx-iconmaker
BUILD       := build
SOURCES     := source
INCLUDES    := source
ROMFS       := romfs
ICON        := $(CURDIR)/icon.png
APP_TITLE   := 3DSX Icon Maker
APP_AUTHOR  := 3DSX Icon Maker contributors
APP_DESCRIPTION := Create HOME Menu forwarders for 3DSX apps

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections \
	-ffile-prefix-map=$(FILE_PREFIX_MAP)=. $(ARCH) $(INCLUDE) -D__3DS__
LDFLAGS = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lctru -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lpng -ljpeg -lz -lm -lctru
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh \
	--romfs=$(CURDIR)/$(ROMFS)
.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh
else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
