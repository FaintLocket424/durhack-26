CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

SRCS = $(wildcard src/*.c)
TARGET = app

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p bin
	$(CC) $(CFLAGS) $^ -o bin/$(TARGET)

clean:
	rm -rf bin/ $(TARGET)

.PHONY: all clean
