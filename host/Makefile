# Host (x86_64 Linux) build of both bridge halves, for testing against the desktop's own Vulkan
# driver: the server loads libvulkan.so.1 (pick a driver with VK_DRIVER_FILES in ITS environment),
# the client ICD is what test programs load (VK_DRIVER_FILES=$(OUT)/vkbridge_icd.json).
#   make -C host            build the server, the client ICD and the test programs
#   make -C host test       run the regression suite (run_tests.sh)
VKB := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
SRV := $(VKB)/server
#   make -C host TSAN=1     the same with ThreadSanitizer, into host/out-tsan (OUT= to move it)
OUT ?= $(VKB)/host/out$(if $(TSAN),-tsan)
CC ?= gcc
CFLAGS ?= -O1 -g
SAN := $(if $(TSAN),-fsanitize=thread -g)
# The bridge build named in the startup probe's device profile and the session report.
VKB_GIT := $(shell git -C $(VKB) describe --always --dirty 2>/dev/null || echo unknown)
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare
INC := -I$(VKB)/third_party/Vulkan-Headers-1.4.341/include -I$(VKB)/common -I$(VKB)/common/gen
COMMON_SRC := $(VKB)/common/vkb_wire.c $(VKB)/common/gen/vkb_gen_structs.c
SERVER_SRC := $(wildcard $(SRV)/*.c) $(VKB)/common/gen/vkb_gen_server.c $(COMMON_SRC)
CLIENT_SRC := $(wildcard $(VKB)/client/*.c) $(VKB)/common/gen/vkb_gen_client.c $(COMMON_SRC)

TESTS := vkbridge-smoke bcn_check opaque_check dmabuf_image_check recycle_check desc_traffic_check query_check placed_check crash_contain_check desc_limits_check flicker_check nulldesc_check pcache_check divisor_check gsfold_check view3d_check viewchurn_check wait_check bitwise_check bitwise_corpus dynstate_check vbstride_check viewport_check flip_corpus clampzero_check clampzero_corpus cached_check

all: $(OUT)/vkbridge-server $(OUT)/libvulkan_droidbridge.so $(OUT)/vkbridge_icd.json $(addprefix $(OUT)/,$(TESTS))

$(OUT)/vkbridge-server: $(SERVER_SRC) $(wildcard $(SRV)/*.h $(SRV)/*.inc $(VKB)/common/*.h $(VKB)/common/gen/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -DVKB_GIT='"$(VKB_GIT)"' -o $@ $(SERVER_SRC) -ldl -lpthread -lm -rdynamic

$(OUT)/libvulkan_droidbridge.so: $(CLIENT_SRC) $(wildcard $(VKB)/client/*.h $(VKB)/common/*.h $(VKB)/common/gen/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -fPIC -shared -Wl,-z,nodelete -fvisibility=hidden -DVKB_CLIENT $(INC) -I$(VKB)/client -I$(VKB)/client/compat \
		-o $@ $(CLIENT_SRC) -ldl -lpthread -Wl,--no-undefined

$(OUT)/vkbridge_icd.json:
	@mkdir -p $(OUT)
	printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s","api_version":"1.4.341"}}\n' "$(OUT)/libvulkan_droidbridge.so" > $@

# The test programs load whatever Vulkan driver VK_DRIVER_FILES names (the bridge or a native one).
$(OUT)/vkbridge-smoke: $(VKB)/tests/smoke.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl

# Unsigned char as on the AArch64 guest the checks also run on: x86's signed char hid bcdec's
# plain-char sign extension of BC4/BC5 SNORM endpoints, which failed bcn_check on the tablet.
$(OUT)/%_check: $(VKB)/tests/%_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lm

$(OUT)/bcn_check: $(VKB)/server/bc_spv.h

$(OUT)/divisor_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/vbstride_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/dynstate_check: $(VKB)/tests/dynstate_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/gsfold_check: $(VKB)/tests/gsfold_vert.inc $(VKB)/tests/gsfold_geom.inc $(VKB)/tests/gsfold_frag.inc $(VKB)/tests/gsfold_lut.inc

$(OUT)/bitwise_check: $(VKB)/tests/bitwise_comp.inc

$(OUT)/clampzero_check: $(VKB)/tests/clampzero_comp.inc

$(OUT)/viewport_check: $(VKB)/tests/viewport_vert.inc $(VKB)/tests/viewport_vert_mv.inc $(VKB)/tests/viewport_geom.inc $(VKB)/tests/viewport_frag.inc

# The MK-02 SPIR-V pass alone, over a corpus (host/viewport_test.sh)
$(OUT)/flip_corpus: $(VKB)/tests/flip_corpus.c $(SRV)/spirv_flip.c $(SRV)/spirv_flip.h $(SRV)/spirv_edit.c $(SRV)/spirv_edit.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/flip_corpus.c $(SRV)/spirv_flip.c $(SRV)/spirv_edit.c

# The MK-03 SPIR-V pass alone, over a corpus (host/bitwise_test.sh)
$(OUT)/bitwise_corpus: $(VKB)/tests/bitwise_corpus.c $(SRV)/spirv_bitwise.c $(SRV)/spirv_bitwise.h $(SRV)/spirv_opclass.inc
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/bitwise_corpus.c $(SRV)/spirv_bitwise.c

# The MK-32 SPIR-V pass alone, over a corpus (host/clampzero_test.sh)
$(OUT)/clampzero_corpus: $(VKB)/tests/clampzero_corpus.c $(SRV)/spirv_clampzero.c $(SRV)/spirv_clampzero.h $(SRV)/spirv_edit.c $(SRV)/spirv_edit.h $(SRV)/spirv_opclass.inc
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/clampzero_corpus.c $(SRV)/spirv_clampzero.c $(SRV)/spirv_edit.c

$(OUT)/flicker_check: $(VKB)/tests/flicker_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lm -lpthread

test: all
	$(VKB)/host/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all test clean
