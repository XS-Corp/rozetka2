CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -O2 -g
CPPFLAGS ?= -Iinclude -Isrc
LDFLAGS ?=
LDLIBS ?= -lffi

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rozetka2
NANO := $(BUILD_DIR)/nano/nano-9.3/src/nano
DOOM := $(BUILD_DIR)/doom/doom-x86_64
QUAKE := $(BUILD_DIR)/quake/quake-x86_64

SRCS := \
	src/a64_jit.c \
	src/decode.c \
	src/dyld.c \
	src/interp.c \
	src/launch.c \
	src/main.c \
	src/native.c \
	src/macho.c \
	src/util.c

OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test nano test-nano doom doom-app run-doom test-doom test-doom-audio quake quake-app run-quake test-quake cxxabi test-cxxabi jvm-probe test-jvm test-jimage test-jvm-create test-jvm-hot test-graphics test-lwjgl test-jna
.PHONY: minecraft-app minecraft-app-c2 test-sort test-stb test-opengl test-minecraft-opengl test-objc-native test-objc test-freetype test-minecraft-shaders test-minecraft-graphics test-thread-signals test-vulkan-callbacks test-vulkan-frame test-platform test-callbacks test-java-callbacks test-openal-playback libcxx test-libcxx test-shaders run-minecraft test-oshi test-java-files test-openal test-java-nio test-java-tls

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

test: all
	./tests/run_tests.sh $(TARGET)

nano: all $(NANO)

$(NANO): scripts/build_nano.sh
	./scripts/build_nano.sh

test-nano: all $(NANO)
	python3 tests/test_nano_pty.py $(TARGET) $(NANO) --unicode

doom: all $(DOOM)

$(DOOM): scripts/build_doom.sh ports/doom_sdl.c ports/doom_audio.c ports/doom_audio.h
	./scripts/build_doom.sh

doom-app: doom
	./scripts/package_doom.sh

run-doom: doom
	cd $(BUILD_DIR)/doom && ../rozetka2 launch --host-libs ./doom-x86_64 -- -iwad freedoom-0.13.0/freedoom1.wad -warp 1 -skill 1

test-doom: doom
	python3 tests/test_doom.py $(TARGET) $(DOOM) $(BUILD_DIR)/doom/freedoom-0.13.0/freedoom1.wad

test-doom-audio: test-doom
	python3 tests/test_doom_audio_bridge.py $(TARGET)
	python3 tests/test_doom.py $(TARGET) $(DOOM) $(BUILD_DIR)/doom/freedoom-0.13.0/freedoom1.wad --sound-mode nomusic
	python3 tests/test_doom.py $(TARGET) $(DOOM) $(BUILD_DIR)/doom/freedoom-0.13.0/freedoom1.wad --sound-mode nosfx
	python3 tests/test_doom.py $(TARGET) $(DOOM) $(BUILD_DIR)/doom/freedoom-0.13.0/freedoom1.wad --sound-mode nosound
	python3 tests/test_doom.py $(TARGET) $(DOOM) $(BUILD_DIR)/doom/freedoom-0.13.0/freedoom1.wad --mus-fixture

quake: all $(QUAKE)

$(QUAKE): scripts/build_quake.sh ports/quake_sdl.c ports/quake_audio.c
	./scripts/build_quake.sh

run-quake: quake
	cd $(BUILD_DIR)/quake && ../rozetka2 launch --host-libs ./quake-x86_64 -- -basedir data -noconinput -noudp -noextmusic +viewsize 90 +map e1m1

quake-app: quake
	./scripts/package_quake.sh

test-quake: quake
	python3 tests/test_quake.py $(TARGET) $(QUAKE) $(BUILD_DIR)/quake/data

cxxabi: all
	sh scripts/build_cxxabi.sh

test-cxxabi: cxxabi
	python3 tests/test_cxxabi.py $(TARGET) $(BUILD_DIR)/minecraft/cxxabi/librozetka2-cxxabi.dylib

jvm-probe: cxxabi
	sh scripts/build_jvm_probe.sh

test-jvm: jvm-probe
	python3 tests/test_jvm.py $(TARGET) $(BUILD_DIR)/minecraft/jvm-probe-x86_64

test-jimage: jvm-probe
	python3 tests/test_jvm.py $(TARGET) $(BUILD_DIR)/minecraft/jvm-probe-x86_64 --image

# Requires actual VM creation, Java execution and normal shutdown.
test-jvm-create: jvm-probe
	python3 tests/test_jvm.py $(TARGET) $(BUILD_DIR)/minecraft/jvm-probe-x86_64 --create

test-jvm-hot: jvm-probe
	python3 tests/test_jvm_hot.py $(TARGET) $(BUILD_DIR)/minecraft/java-x86_64

test-graphics: all
	clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains tests/graphics_guest.c -o $(BUILD_DIR)/graphics-guest-x86_64
	python3 tests/test_graphics.py $(TARGET) $(BUILD_DIR)/graphics-guest-x86_64

test-lwjgl: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET)

test-jna: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --jna

test-oshi: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --oshi

test-java-files: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --files

test-java-nio: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --nio

test-java-tls: jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --nio --tls

libcxx: cxxabi
	sh scripts/build_libcxx.sh

test-libcxx: libcxx
	python3 tests/test_cxxabi.py $(TARGET) $(BUILD_DIR)/minecraft/libcxx/librozetka2-cxxabi.dylib --stdlib
	python3 tests/test_libcxx.py $(TARGET) $(BUILD_DIR)/minecraft/libcxx/librozetka2-cxxabi.dylib

test-shaders: libcxx
	python3 tests/test_libcxx.py $(TARGET) $(BUILD_DIR)/minecraft/libcxx/librozetka2-cxxabi.dylib --shaders

test-openal: libcxx jvm-probe
	python3 tests/test_lwjgl.py $(TARGET) --openal

test-opengl: jvm-probe
	clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains -Wno-deprecated-declarations tests/opengl_guest.c -o $(BUILD_DIR)/opengl-guest
	python3 tests/test_opengl.py $(TARGET) $(BUILD_DIR)/opengl-guest
	python3 tests/test_lwjgl.py $(TARGET) --opengl

test-freetype: libcxx
	python3 tests/test_freetype.py $(TARGET)

test-minecraft-shaders: libcxx
	python3 tests/test_minecraft_shaders.py $(TARGET)

test-minecraft-graphics: test-minecraft-shaders
	python3 tests/test_vulkan_frame.py $(TARGET) --game-shaders

test-minecraft-opengl: test-minecraft-shaders
	clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains -Wno-deprecated-declarations tests/opengl_guest.c -o $(BUILD_DIR)/opengl-guest
	python3 tests/test_opengl.py $(TARGET) $(BUILD_DIR)/opengl-guest --game-shaders

test-thread-signals: all
	python3 tests/test_thread_signals.py $(TARGET)

test-vulkan-callbacks: all
	python3 tests/test_vulkan_callbacks.py $(TARGET)

test-vulkan-frame: all
	python3 tests/test_vulkan_frame.py $(TARGET)

test-platform: all
	python3 tests/test_platform.py $(TARGET)

test-objc-native: all
	python3 tests/test_objc_native.py $(TARGET)

test-objc: libcxx jvm-probe test-objc-native
	python3 tests/test_objc.py $(TARGET)

test-callbacks: all
	python3 tests/test_callbacks.py $(TARGET)
	python3 tests/test_audio_callbacks.py $(TARGET)
	python3 tests/test_sort.py $(TARGET)

test-sort: all
	python3 tests/test_sort.py $(TARGET)

test-stb: all
	python3 tests/test_stb.py $(TARGET)
	python3 tests/test_stb_resize.py $(TARGET)

test-java-callbacks: jvm-probe test-callbacks
	python3 tests/test_lwjgl_callbacks.py $(TARGET)
	python3 tests/test_lwjgl_callbacks.py $(TARGET) --backend $(BUILD_DIR)/minecraft/callback-test/native-thread.dylib

test-openal-playback: libcxx
	python3 tests/test_openal_playback.py $(TARGET)
	python3 tests/test_openal_playback.py $(TARGET) --loopback

run-minecraft: libcxx jvm-probe
	python3 scripts/run_minecraft.py

minecraft-app: libcxx jvm-probe
	python3 scripts/run_minecraft.py --prepare-only
	python3 scripts/build_minecraft_app.py

minecraft-app-c2: libcxx jvm-probe
	python3 scripts/run_minecraft.py --prepare-only
	python3 scripts/build_minecraft_app.py --c2

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
