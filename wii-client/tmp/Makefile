.SUFFIXES:

# ------------------------------------------------------------------------------
#   make          Wii build  -> app.dol      (needs DEVKITPPC, e.g. nix develop)
#   make host     native PC build -> ./node_host   (no devkitPro needed)
#   make clean
#
# Layout:  core/           portable: protocol, render, handlers, node, platform.h
#          platform/bsd/   BSD-socket backend, shared by Wii / PC / (Wii U, DS)
#          platform/wii/   Wii main(), libogc glue, tile preview
#          platform/posix/ PC main()
# ------------------------------------------------------------------------------

HOST_GOALS := host

ifneq ($(filter $(HOST_GOALS),$(MAKECMDGOALS)),)
# ==============================================================================
# Native build. Deliberately does NOT include wii_rules (it would switch CC to
# powerpc-eabi-gcc), so it works without DEVKITPPC.
# ==============================================================================
HOSTCC        ?= cc
HOSTCFLAGS    ?= -O2 -g -Wall -Wextra
HOST_SOURCES  := core platform/bsd platform/posix
HOST_INCLUDES := -Icore
HOST_CFILES   := $(foreach dir,$(HOST_SOURCES),$(wildcard $(dir)/*.c))
HOST_HFILES   := $(foreach dir,$(HOST_SOURCES),$(wildcard $(dir)/*.h))

.PHONY: host clean
host: node_host

node_host: $(HOST_CFILES) $(HOST_HFILES)
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_INCLUDES) $(HOST_CFILES) -lm -o $@

clean:
	@rm -f node_host

else
# ==============================================================================
# Wii build (devkitPPC / libogc)
# ==============================================================================

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := app
BUILD    := build
SOURCES  := core platform/bsd platform/wii
DATA     :=
INCLUDES := core

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lwiiuse -lbte -logc -lm
LIBDIRS := $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(subst .,_,$(BINFILES)))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(LIBOGC_INC) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).dol $(TARGET).elf *.map node_host

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).dol

$(OUTPUT).dol: $(OUTPUT).elf
$(OFILES_SRC): $(HFILES_BIN)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
endif
