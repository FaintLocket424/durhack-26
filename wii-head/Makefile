.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

# Two programs share this source tree and both define main(), so a *.c wildcard
# would link them together. Pick one with ROLE:
#   make              -> head.dol  (coordinator: pings / drives the workers)
#   make ROLE=node    -> node.dol  (worker: listens on NODE_PORT, runs handlers)
#   make both         -> builds both, each in its own build directory
ROLE ?= head

ifeq ($(ROLE),head)
	TARGET   := head
	SRCFILES := main.c head.c
else ifeq ($(ROLE),node)
	TARGET   := node
	SRCFILES := main.c node.c handlers.c
else
$(error "ROLE must be 'head' or 'node' (got '$(ROLE)')")
endif

BUILD    := build_$(ROLE)
SOURCES  := src
DATA     :=
INCLUDES :=

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

CFILES   := $(filter %.c,$(SRCFILES))
CPPFILES := $(filter %.cpp,$(SRCFILES))
SFILES   := $(filter %.s,$(SRCFILES))
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
                   -I$(CURDIR)/$(SOURCES) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean all both

all: $(BUILD)

both:
	@$(MAKE) --no-print-directory ROLE=head
	@$(MAKE) --no-print-directory ROLE=node

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr build_head build_node head.dol head.elf node.dol node.elf *.map

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).dol

$(OUTPUT).dol: $(OUTPUT).elf
$(OFILES_SRC): $(HFILES_BIN)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
