CROSS   ?= x86_64-w64-mingw32-
CC       = $(CROSS)gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS   = -lhid -lsetupapi

all: build/wooting_analog_sdk.dll build/test_shim.exe

build/wooting_analog_sdk.dll: src/keychron_analog.c | build
	$(CC) $(CFLAGS) -shared -static $(LDFLAGS) -o $@ $< $(LDLIBS)

build/test_shim.exe: src/test_shim.c | build
	$(CC) $(CFLAGS) -static $(LDFLAGS) -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
