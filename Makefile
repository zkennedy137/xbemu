CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc
MINGW_CXX ?= x86_64-w64-mingw32-g++
DLLTOOL   ?= x86_64-w64-mingw32-dlltool

CFLAGS    ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -MMD -MP

SRC  := src/main.c src/pe.c src/w64abi.c src/teb.c \
        src/host/hostlib.c src/host/kernel32.c src/host/kernel32_sync.c \
        src/host/kernel32_loader.c src/host/kernelx.c src/host/msvcrt.c \
        src/host/vulkan.c src/host/paths.c src/host/advapi32.c src/host/user32.c \
        src/host/xdisplay.c src/host/seh.c src/host/kernel32_file.c src/host/audio_out.c src/host/xaudio2.c src/host/gdi32.c src/host/setupapi.c src/host/display.c src/host/rawinput.c src/host/imm32.c
OBJ  := $(SRC:.c=.o)
-include $(OBJ:.o=.d)

# Win64 test programs (built with MinGW; run by tests/run_tests.sh and tests/dxvk/*).
WIN_TESTS := tests/hello/hello.exe tests/dxvk/probe.exe tests/dxvk/vkprobe.exe \
             tests/dxvk/present.exe tests/dxvk/present_vsync.exe tests/dxvk/adapters.exe tests/eh/throw.exe \
             tests/input/input.exe tests/input/raw.exe tests/game/game.exe tests/crt/hello_crt.exe tests/d3d11/triangle.exe tests/audio/tone.exe \
             tests/audio/routing.exe tests/audio/two_engines.exe tests/threads/sync.exe tests/registry/reg.exe tests/vulkan/float.exe \
             tests/eh/wndproc.exe

all: xbemu $(WIN_TESTS) tests/input/sendevents tests/input/sendraw

xbemu: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lxcb -lasound -lX11 -lXrandr -lXi

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Import library for kernelx.dll, so MinGW can link against it.
tests/hello/libkernelx.a: tests/hello/kernelx.def
	$(DLLTOOL) -d $< -l $@

# Test PE: no CRT, entry point `entry`, imports from kernel32 and kernelx.
tests/hello/hello.exe: tests/hello/hello.c tests/hello/libkernelx.a
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ tests/hello/hello.c -Ltests/hello -lkernelx -lkernel32

# Freestanding Win64 programs: no CRT, entry point `entry`.
tests/dxvk/%.exe: tests/dxvk/%.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ $< -lkernel32 -luser32 -luuid

# Programs that use the MinGW headers (COM interfaces, window classes).
tests/dxvk/present.exe tests/dxvk/adapters.exe: tests/dxvk/%.exe: tests/dxvk/%.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ $< -lkernel32 -luser32 -luuid

# Homebrew test game: D3D11 with shaders, XAudio2, and keyboard input (tests/game).
tests/game/game.exe: tests/game/game.c tests/d3d11/shaders.h
	$(MINGW_CC) -O2 -Wall -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Itests/d3d11 -Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ $< -lkernel32 -luser32 -luuid

# The same program with vblank-synchronized presentation (FIFO).
tests/dxvk/present_vsync.exe: tests/dxvk/present.c
	$(MINGW_CC) -O2 -DPRESENT_SYNC=1 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ $< -lkernel32 -luser32 -luuid

tests/input/input.exe: tests/input/input.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32 -luser32

tests/input/sendevents: tests/input/sendevents.c
	$(CC) -O1 -o $@ $< -lxcb

tests/input/sendraw: tests/input/sendraw.c
	$(CC) -O1 -o $@ $< -lX11 -lXtst

tests/input/raw.exe: tests/input/raw.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32 -luser32

# C++ exception test (MinGW's SEH-based C++ runtime). Freestanding: our own entry point.
tests/eh/wndproc.exe: tests/eh/wndproc.cpp
	$(MINGW_CXX) -O1 -std=c++17 -fno-stack-protector -nostartfiles -Wl,-e,entry \
		-Wl,--subsystem,console -o $@ $< -static-libgcc -static-libstdc++ -lkernel32 -luser32 -lmsvcrt

tests/eh/throw.exe: tests/eh/throw.cpp
	$(MINGW_CXX) -O1 -std=c++17 -fno-stack-protector -nostartfiles -Wl,-e,entry \
		-Wl,--subsystem,console -o $@ $< -static-libgcc -static-libstdc++ -lkernel32 -luser32 -lmsvcrt

tests/audio/two_engines.exe: tests/audio/two_engines.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32

tests/vulkan/float.exe: tests/vulkan/float.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Itests/vulkan/include \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32

tests/audio/routing.exe: tests/audio/routing.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32

tests/threads/sync.exe: tests/threads/sync.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32

tests/registry/reg.exe: tests/registry/reg.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -ladvapi32 -lkernel32

tests/audio/tone.exe: tests/audio/tone.c
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase -o $@ $< -lkernel32

# Ordinary C program with the default C runtime startup.
tests/crt/hello_crt.exe: tests/crt/hello_crt.c
	$(MINGW_CC) -O2 -o $@ $<

# Shader bytecode for the draw test, generated (no D3D shader compiler is needed).
tests/d3d11/shaders.h: tests/d3d11/make_shaders.py
	python3 $< $@

tests/d3d11/triangle.exe: tests/d3d11/triangle.c tests/d3d11/shaders.h
	$(MINGW_CC) -O2 -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
		-Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
		-o $@ tests/d3d11/triangle.c -Itests/d3d11 -lkernel32 -luser32 -luuid

test: all
	./tests/run_tests.sh

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) xbemu tests/input/sendevents tests/input/sendraw
	rm -f $(WIN_TESTS) tests/d3d11/shaders.h

.PHONY: all test clean
