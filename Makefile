# Makefile — Fractint WASM build via Emscripten
#
# Prerequisites:
#   emsdk installed and activated:
#     git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
#     cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
#     source ~/emsdk/emsdk_env.sh
#
# Usage:
#   source ~/emsdk/emsdk_env.sh
#   emmake make
#
# Output: web/fractint.js  web/fractint.wasm

SRCDIR := xfractint-20.04p16
COMDIR := $(SRCDIR)/common
UDIR   := $(SRCDIR)/unix
HFD    := $(SRCDIR)/headers

# Our WASM driver and WASM-safe include override
WASM_DRIVER := src/driver/d_wasm.c
WASM_INC    := src/include

OUTDIR := web
TARGET := $(OUTDIR)/fractint.js

# -----------------------------------------------------------------------
# Compiler and flags
# -----------------------------------------------------------------------

CC = emcc

# Core defines matching the upstream Linux/XFRACT build
DEFINES  = -DXFRACT -DLINUX -DBIG_ANSI_C -DNOBSTRING -DWITH_XFT=0
# -DWASM_BUILD is our own guard for WASM-specific ifdefs
DEFINES += -DWASM_BUILD
# SRCDIR: where fractint looks for .hlp, .par, .frm etc at runtime
DEFINES += -DSRCDIR=\"/data\"

# Include paths:
#   -iquote $(HFD): makes "port.h" style includes from common/*.c resolve
#                   to headers/port.h (clang/emcc searches -iquote dirs for "" includes)
#   -I$(WASM_INC):  MUST come before -I$(HFD) so our xfcurses.h + helpdefs.h
#                   shadow the upstream X11-dependent versions
# -iquote $(WASM_INC) MUST come first so our xfcurses.h + helpdefs.h
# shadow the upstream X11-dependent versions for all "..." includes
INCLUDES  = -iquote $(WASM_INC)
INCLUDES += -iquote $(HFD)
INCLUDES += -I$(WASM_INC)
INCLUDES += -I$(HFD)
INCLUDES += -I$(UDIR)
INCLUDES += -I$(COMDIR)

# Warning suppression for old C code (K&R style, implicit functions, etc.)
WARN = -Wno-implicit-function-declaration \
       -Wno-implicit-int \
       -Wno-return-type \
       -Wno-incompatible-pointer-types \
       -Wno-int-conversion \
       -Wno-deprecated-non-prototype

CFLAGS = $(DEFINES) $(INCLUDES) $(WARN) -O3 -msimd128 -mbulk-memory -fno-builtin

# Extra flags for hot fractal calculation paths (ffast-math safe here)
CALC_CFLAGS = $(CFLAGS) -ffast-math

# -----------------------------------------------------------------------
# Emscripten link flags
# -----------------------------------------------------------------------

EM_FLAGS  = -s USE_SDL=2
EM_FLAGS += -s ALLOW_MEMORY_GROWTH=1
EM_FLAGS += -s INITIAL_MEMORY=33554432
EM_FLAGS += -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPU32"]'
EM_FLAGS += -s EXPORTED_FUNCTIONS='["_main","_wasm_push_key","_wasm_get_pixel_buf","_wasm_get_rgba_lut","_wasm_get_screen_dims","_wasm_toggle_cycle","_wasm_set_cycle_speed","_wasm_set_fractype","_wasm_get_fractype","_wasm_resize","_wasm_zoom_to_rect","_wasm_zoom_at_point"]'
EM_FLAGS += -s FORCE_FILESYSTEM=1
EM_FLAGS += -s ENVIRONMENT=web

# Preload data files when the directory is populated
DATA_FILES := $(wildcard data/*)
ifneq ($(DATA_FILES),)
EM_FLAGS += --preload-file data@/data
endif

# -----------------------------------------------------------------------
# Source lists
# -----------------------------------------------------------------------

# All common/*.c files except DOS-specific ones:
#   diskvid.c  — uses FARMEM duplicate case; replaced by unix/diskvidu.c
#   yourvid.c  — uses union REGS (DOS BIOS), not needed for WASM
COMMON_SRCS := $(filter-out $(COMDIR)/diskvid.c $(COMDIR)/yourvid.c, \
                             $(wildcard $(COMDIR)/*.c))

# unix/ files that are safe for WASM (no X11, no curses, no ioctl)
# Include: general.c unix.c calcmand.c calmanfp.c diskvidu.c fpu087.c fracsuba.c
# Exclude: unixscr.c (X11 graphics), xfcurses.c (X11 text window)
UNIX_SRCS := \
    $(UDIR)/general.c \
    $(UDIR)/unix.c \
    $(UDIR)/calcmand.c \
    $(UDIR)/calmanfp.c \
    $(UDIR)/diskvidu.c \
    $(UDIR)/fpu087.c \
    $(UDIR)/fracsuba.c \
    $(UDIR)/video.c

# Our WASM driver replaces unixscr.c + xfcurses.c
DRIVER_SRCS := $(WASM_DRIVER)

ALL_SRCS := $(COMMON_SRCS) $(UNIX_SRCS) $(DRIVER_SRCS)

# -----------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------

.PHONY: all clean dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OUTDIR)

$(TARGET): $(ALL_SRCS) Makefile
	$(CC) $(CFLAGS) $(EM_FLAGS) \
		$(ALL_SRCS) \
		-o $(TARGET) \
		2>&1
	@echo ""
	@echo "Build complete: $(OUTDIR)/fractint.js + $(OUTDIR)/fractint.wasm"

clean:
	rm -f $(OUTDIR)/fractint.js \
	      $(OUTDIR)/fractint.wasm \
	      $(OUTDIR)/fractint.data

# -----------------------------------------------------------------------
# Convenience: copy static assets into web/ for testing
# -----------------------------------------------------------------------

deploy: all
	cp src/index.html  $(OUTDIR)/
	cp -r src/css      $(OUTDIR)/
	cp -r src/js       $(OUTDIR)/
	@echo "Deployed to $(OUTDIR)/"
