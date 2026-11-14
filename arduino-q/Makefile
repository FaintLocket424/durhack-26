CC       ?= aarch64-unknown-linux-musl-cc
CXX      ?= aarch64-unknown-linux-musl-c++
CFLAGS   ?= -Wall -Wextra -O2 -g -mcpu=cortex-a53
CXXFLAGS ?= -Wall -Wextra -O2 -g -mcpu=cortex-a53
LDFLAGS  ?=

TARGET   := bin/app
SRCS     := $(wildcard src/*.c src/*.cpp)
OBJS     := $(patsubst src/%, build/%.o, $(basename $(SRCS)))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(if $(filter %.cpp,$(SRCS)),$(CXX),$(CC)) $(OBJS) -o $@ $(LDFLAGS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build bin
