# Thorn - Makefile (raylib 6.0, vendored as a static archive so the build is
# self-contained). Brew ships only raylib 5.5, so `make raylib6` builds 6.0 once.

NAME    := thorn
BUILD   := build
BIN     := $(BUILD)/$(NAME)
SRC     := src/main.c
HDRS    := $(wildcard src/*.h)

CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DGL_SILENCE_DEPRECATION
UNAME_S := $(shell uname -s)

RAYLIB_DIR := vendor/raylib
INCLUDES   := -I$(RAYLIB_DIR)/include
LIBS       := $(RAYLIB_DIR)/lib/libraylib.a

ifeq ($(UNAME_S),Darwin)
  LIBS += -framework OpenGL -framework Cocoa -framework IOKit \
          -framework CoreVideo -framework CoreAudio
endif
ifeq ($(UNAME_S),Linux)
  LIBS += -lm -lpthread -ldl -lrt -lX11
endif

.PHONY: all run debug test clean raylib6 web
all: $(BIN)

# Incremental: only relink when a source, header, or the raylib archive changes.
$(BIN): $(SRC) $(HDRS) $(RAYLIB_DIR)/lib/libraylib.a | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRC) $(LIBS)

$(BUILD):
	mkdir -p $(BUILD)

run: $(BIN)
	./$(BIN)

debug: $(BIN)
	./$(BIN) --debug

# Headless regression check: room graph + a full four-area playthrough.
test: $(BIN)
	@bash tests/playthrough.sh

clean:
	rm -rf $(BUILD)

# One-time: build a vendored raylib 6.0 static archive from the upstream branch.
raylib6:
	@if [ ! -d vendor/raylib-src ]; then \
	  git clone --depth 1 --branch 6.0 https://github.com/raysan5/raylib vendor/raylib-src; fi
	$(MAKE) -C vendor/raylib-src/src PLATFORM=PLATFORM_DESKTOP -j4
	mkdir -p $(RAYLIB_DIR)/include $(RAYLIB_DIR)/lib
	cp vendor/raylib-src/src/raylib.h vendor/raylib-src/src/raymath.h vendor/raylib-src/src/rlgl.h $(RAYLIB_DIR)/include/
	cp vendor/raylib-src/src/libraylib.a $(RAYLIB_DIR)/lib/libraylib.a

# ---- Web build (WebAssembly via emscripten) --------------------------------
# `make web` -> build/web/thorn.{html,js,wasm,data}. Needs emscripten
# (brew install emscripten). raylib is rebuilt for PLATFORM_WEB into a SEPARATE
# archive (libraylib.web.a); the desktop archive is left untouched. emscripten's
# python shim resolves `env python3`, so keep Homebrew's (3.10+) ahead of the
# system/Xcode python on PATH.
RAYLIB_SRC := vendor/raylib-src/src
RAYLIB_WEB := $(RAYLIB_SRC)/libraylib.web.a
WEB_OUT    := $(BUILD)/web
WEB_PATH   := /opt/homebrew/bin:$(PATH)
EMCC       ?= emcc

$(RAYLIB_WEB):
	@if [ ! -d vendor/raylib-src ]; then \
	  git clone --depth 1 --branch 6.0 https://github.com/raysan5/raylib vendor/raylib-src; fi
	PATH="$(WEB_PATH)" $(MAKE) -C $(RAYLIB_SRC) PLATFORM=PLATFORM_WEB -B

web: $(SRC) src/shell.html $(RAYLIB_WEB)
	mkdir -p $(WEB_OUT)
	PATH="$(WEB_PATH)" $(EMCC) -o $(WEB_OUT)/thorn.html $(SRC) -std=c11 -Os -Wall \
	  -Wno-unused-parameter -Wno-missing-field-initializers \
	  -I$(RAYLIB_SRC) $(RAYLIB_WEB) \
	  -s USE_GLFW=3 -s ALLOW_MEMORY_GROWTH=1 -s FORCE_FILESYSTEM=1 \
	  -s STACK_SIZE=4194304 \
	  --preload-file levels --preload-file assets \
	  --shell-file src/shell.html
	@echo "built $(WEB_OUT)/thorn.html — serve with:  python3 -m http.server -d $(WEB_OUT)"
