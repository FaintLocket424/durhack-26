CXX ?= aarch64-unknown-linux-musl-c++
CXXFLAGS = -std=c++17 -O2 -mcpu=cortex-a53 -Wall -Wextra
LDFLAGS ?= -static

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

TARGET = $(BIN_DIR)/app
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

.PHONY: all clean
