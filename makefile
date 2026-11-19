EXE = txtcnv

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests

SRC := $(wildcard $(SRC_DIR)/*.cpp)
OBJ := $(SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

LIB_OBJ := $(filter-out $(OBJ_DIR)/main.o,$(OBJ))

CXX = g++
CFLAGS = -g -Wall -Iinclude -std=c++20
LDFLAGS =
LDLIBS =

CMP_CMD = compile_commands.json

ifeq ($(OS),Windows_NT)
	LDFLAGS += -static -static-libstdc++
	EXT = .exe
endif

EXE_PATH = $(BIN_DIR)/$(EXE)$(EXT)

.PHONY: all test clean

all: $(EXE_PATH) $(CMP_CMD)
	@echo "Build complete"

$(CMP_CMD): $(EXE_PATH)
	@if command -v compiledb >/dev/null 2>&1; then \
		echo "Running compiledb..."; \
		compiledb -n make; \
	else \
		echo "compiledb not found, skipping."; \
	fi

$(EXE_PATH): $(OBJ)
	mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(wildcard include/*.hpp)
	mkdir -p $(@D)
	$(CXX) $(CFLAGS) -c $< -o $@

test: $(LIB_OBJ) $(TEST_DIR)/tests.cpp | $(EXE_PATH)
	$(CXX) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $(TEST_DIR)/tests$(EXT)
	./$(TEST_DIR)/tests$(EXT)

clean:
	$(RM) $(OBJ) $(EXE_PATH) $(TEST_DIR)/tests$(EXT) $(CMP_CMD)
