# Guest Vulkan Video encode layer (VK_LAYER_STEAMAC_video_encode) and its encoder backends. Linux.
#
#   make [ARCH=x86_64|aarch64]   build/<arch>/: libVkLayer_steamac_venc.so, libvenc_backend_vt.so, the layer json
#   make install DESTDIR=<root> [LIBDIR=/usr/lib/steamac/venc]
#   make tests                   (x86_64 build box) x26x backend, fake VideoToolbox host, backend test
#   make check                   FFmpeg's Vulkan encoders through the layer on lavapipe (needs FF=, an FFmpeg >= 7.1
#                                with Vulkan, and x264/x265): software backend, then the "vt" protocol via the fake host
#
# Where it runs: SteamVR's vrlink driver (x86_64, under FEX) and any native ARM64 Vulkan encoder in the VM. The
# x86_64 build belongs in the fex-mesa graphics provider (built with the provider's toolchain, like Venus x86:
# guest/mesa/container/build-x86.sh), the aarch64 build in the image's /usr.
# VKH: a Vulkan-Headers include directory with VK_HEADER_VERSION >= 277 (Mesa's include/ works).
ARCH ?= $(shell uname -m)
ifeq ($(ARCH),aarch64)
CROSS ?= $(if $(filter aarch64,$(shell uname -m)),,aarch64-linux-gnu-)
else
CROSS ?=
endif
CC = $(CROSS)gcc
CXX = $(CROSS)g++
VKH ?= /usr/include
LIBDIR ?= /usr/lib/steamac/venc
OUT ?= build/$(ARCH)
WMTR = ../../wmtr
VR = ../vr

CPPFLAGS = -Iinclude -I$(VKH)
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# loaded into other programs' processes (SteamVR, games): own C++ runtime, hidden symbols, no undefined
SOFLAGS = -shared -fPIC -fvisibility=hidden -Wl,--no-undefined -Wl,-z,defs
LAYER_SRC = layer/venc_layer.cpp layer/h264_rewrite.cpp layer/h265_rewrite.cpp src/venc_nal.c
VT_SRC = src/backend_vt.c $(VR)/runtime/wmtr_native.c $(WMTR)/src/transport_virtgpu.c

all: $(OUT)/libVkLayer_steamac_venc.so $(OUT)/libvenc_backend_vt.so $(OUT)/VkLayer_steamac_venc.json

$(OUT):
	mkdir -p $(OUT)

$(OUT)/check_headers: | $(OUT)
	@printf '#include <vulkan/vulkan_core.h>\n#if VK_HEADER_VERSION < 277\n#error "VKH: Vulkan-Headers >= 1.3.277 needed"\n#endif\nint main(void){return 0;}\n' | \
	  $(CC) -I$(VKH) -x c - -o $@

$(OUT)/libVkLayer_steamac_venc.so: $(LAYER_SRC) layer/*.h include/*.h | $(OUT)/check_headers
	$(CXX) -std=c++17 $(CPPFLAGS) $(CXXFLAGS) $(WARN) $(SOFLAGS) -o $@ $(LAYER_SRC) -static-libstdc++ -static-libgcc -ldl -lpthread

$(OUT)/libvenc_backend_vt.so: $(VT_SRC) include/*.h | $(OUT)
	$(CC) -std=gnu11 $(CPPFLAGS) -DVENC_VT_NATIVE -I$(VR)/runtime -I$(VR)/include -I$(WMTR)/include $(CFLAGS) $(WARN) \
	  $(SOFLAGS) -o $@ $(VT_SRC) -lpthread

$(OUT)/VkLayer_steamac_venc.json: | $(OUT)
	printf '%s\n' '{' '    "file_format_version": "1.2.0",' '    "layer": {' \
	  '        "name": "VK_LAYER_STEAMAC_video_encode",' '        "type": "GLOBAL",' \
	  '        "library_path": "$(LIBDIR)/libVkLayer_steamac_venc.so",' '        "api_version": "1.4.318",' \
	  '        "implementation_version": "1",' \
	  '        "description": "steamac: Vulkan Video encode (H.264/H.265) on Venus, encoded by the Mac (VideoToolbox)",' \
	  '        "disable_environment": { "STEAMAC_VENC_DISABLE": "1" }' '    }' '}' > $@

install: all
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)/usr/share/vulkan/implicit_layer.d
	install -m 0755 $(OUT)/libVkLayer_steamac_venc.so $(OUT)/libvenc_backend_vt.so $(DESTDIR)$(LIBDIR)/
	install -m 0644 $(OUT)/VkLayer_steamac_venc.json \
	  $(DESTDIR)/usr/share/vulkan/implicit_layer.d/VkLayer_steamac_venc.$(ARCH).json

# ---- tests (x86_64 build box) ----------------------------------------------------------------------------------
X26X ?= /usr
tests: $(OUT)/libVkLayer_steamac_venc.so $(OUT)/libvenc_backend_x26x.so $(OUT)/libvenc_backend_vt_socket.so \
       $(OUT)/vt_fake_host $(OUT)/backend_test
$(OUT)/libvenc_backend_x26x.so: src/backend_x26x.c src/venc_nal.c include/*.h | $(OUT)
	$(CC) -std=gnu11 $(CPPFLAGS) -I$(X26X)/include $(CFLAGS) $(WARN) $(SOFLAGS) -o $@ src/backend_x26x.c src/venc_nal.c \
	  -L$(X26X)/lib/x86_64-linux-gnu -l:libx264.so.164 -l:libx265.so.199
# the vt backend without the virtio-gpu transport: socket only (VENC_VT_SOCKET)
$(OUT)/libvenc_backend_vt_socket.so: src/backend_vt.c include/*.h | $(OUT)
	$(CC) -std=gnu11 $(CPPFLAGS) $(CFLAGS) $(WARN) $(SOFLAGS) -o $@ src/backend_vt.c -lpthread
$(OUT)/vt_fake_host: tests/vt_fake_host.c include/*.h | $(OUT)
	$(CC) -std=gnu11 $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ $< -ldl
$(OUT)/backend_test: tests/backend_test.c include/*.h | $(OUT)
	$(CC) -std=gnu11 $(CPPFLAGS) $(CFLAGS) -o $@ $< -ldl

check: tests
	mkdir -p $(OUT)/test-layer
	printf '%s\n' '{"file_format_version":"1.2.0","layer":{"name":"VK_LAYER_STEAMAC_video_encode","type":"GLOBAL",' \
	  '"library_path":"$(abspath $(OUT))/libVkLayer_steamac_venc.so","api_version":"1.4.318","implementation_version":"1",' \
	  '"description":"test"}}' > $(OUT)/test-layer/VkLayer_steamac_venc.json
	VENC_LAYER_DIR=$(abspath $(OUT))/test-layer VENC_BACKEND=x26x tests/run_ffmpeg_tests.sh
	rm -f /tmp/venc-check.sock; ($(OUT)/vt_fake_host /tmp/venc-check.sock $(abspath $(OUT))/libvenc_backend_x26x.so >/dev/null 2>&1 &); \
	  sleep 1; VENC_LAYER_DIR=$(abspath $(OUT))/test-layer VENC_BACKEND=$(abspath $(OUT))/libvenc_backend_vt_socket.so \
	  VENC_VT_SOCKET=/tmp/venc-check.sock OUT=/tmp/venc-check-vt tests/run_ffmpeg_tests.sh; r=$$?; \
	  pkill -f "vt_fake_host /tmp/venc-check.sock"; exit $$r

clean:
	rm -rf build

.PHONY: all install tests check clean
