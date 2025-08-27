# Output binary
TARGET := gizmo.exe

# Tools
CC := gcc

# Build Type (debug | release)
BUILD ?= release

# Resource
RES := res/gizmo_res.o

# build the resource object
#res/gizmo_res.o: res/gizmo.rc res/gizmo.ico
#	windres -i res/gizmo.rc -o res/gizmo_res.o -O coff

# Compile Flags
CFLAGS_COMMON  := -std=c99 -Iinclude $(shell sdl2-config --cflags) -mconsole
CFLAGS_debug   := -g
CFLAGS_release := -Ofast -s -DNDEBUG
CFLAGS 		   := $(CFLAGS_COMMON) $(CFLAGS_$(BUILD))

# Link
LDFLAGS := -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
LDLIBS  := $(shell sdl2-config --libs) -lole32 -luuid -lcomdlg32 -lshell32 -luser32

# Sources
SRC := $(wildcard src/core/*.c src/util/*.c src/external/*.c src/*.c)

# Default rule
all: $(TARGET)

$(TARGET): $(SRC) $(RES) 
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

DIST := dist

bundle: $(TARGET)
	mkdir -p $(DIST)
	cp -f $(TARGET) $(DIST)/
	@command -v ntldd >/dev/null 2>&1 || { \
	  echo "Install ntldd (MSYS2 UCRT64): pacman -S mingw-w64-ucrt-x86_64-ntldd"; exit 1; }
	@DLLS="$$(ntldd -R $(TARGET) | awk '/=>/ && $$3 ~ /\.dll/ {print $$3}' \
	    | grep -iv 'windows\\system32' | sort -u)"; \
	for f in $$DLLS; do echo "Copy $$f"; cp -f "$$f" $(DIST)/ ; done
	@echo "Bundle created in $(DIST)/"

clean:
	rm -f $(TARGET)
	rm -rf $(DIST)

.PHONY: all clean bundle