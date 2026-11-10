.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif

TOPDIR ?= $(CURDIR)
FILE_PREFIX_MAP := $(patsubst %/,%,$(shell cygpath -m "$(TOPDIR)" 2>/dev/null))
ifeq ($(strip $(FILE_PREFIX_MAP)),)
FILE_PREFIX_MAP := $(TOPDIR)
endif
APP_ICON := $(TOPDIR)/icon.png
include $(DEVKITARM)/3ds_rules

TARGET      := MULTI-Forwarder
BUILD       := build
SOURCES     := source third_party/ndsForwarder/source
INCLUDES    := source third_party/ndsForwarder/include
ROMFS       := romfs
ROMFS_FILES := $(shell find "$(TOPDIR)/$(ROMFS)" -type f)
APP_TITLE   := MULTI-Forwarder
APP_AUTHOR  :=
APP_DESCRIPTION := MULTI-Forwarder

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections \
	-ffile-prefix-map=$(FILE_PREFIX_MAP)=. $(ARCH) $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti
LDFLAGS = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lctru -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lpng -ljpeg -lz -lm -lctru
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(if $(CPPFILES),$(CXX),$(CC))
export OFILES := $(CFILES:.c=.o) $(CPPFILES:.cpp=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(DEVKITPRO)/portlibs/3ds/include/libpng16 -I$(CURDIR)/$(BUILD)
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
$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh $(ROMFS_FILES)
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
