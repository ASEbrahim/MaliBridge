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
VKB_GIT := $(shell git -C $(VKB) describe --tags --match 'v[0-9]*' --always --dirty 2>/dev/null || echo unknown)
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare
INC := -I$(VKB)/third_party/Vulkan-Headers-1.4.341/include -I$(VKB)/common -I$(VKB)/common/gen
COMMON_SRC := $(VKB)/common/vkb_wire.c $(VKB)/common/gen/vkb_gen_structs.c
SERVER_SRC := $(wildcard $(SRV)/*.c) $(VKB)/common/gen/vkb_gen_server.c $(COMMON_SRC)
CLIENT_SRC := $(wildcard $(VKB)/client/*.c) $(VKB)/common/gen/vkb_gen_client.c $(COMMON_SRC)

TESTS := vkbridge-smoke poolfull_check bcn_check opaque_check dmabuf_image_check recycle_check desc_traffic_check query_check placed_check crash_contain_check desc_limits_check flicker_check nulldesc_check pcache_check divisor_check gsfold_check view3d_check viewchurn_check wait_check bitwise_check bitwise_corpus dynstate_check vbstride_check viewport_check flip_corpus clampzero_check clampzero_corpus begin_check shlbarrier_corpus cullmode_check dedimport_check cached_check polygon_check gpuview_check dedhint_check planar_check fdprops_check memclass_check procaddr_check exitclean_check effects_corpus asyncpipe_check texcap_check robust2_check fp16_check gshide_check featclamp_check promote_check downgrade_corpus xfb_check shmbarrier_check shmbarrier_corpus stencillayout_check dynrend_check wave32_check wave32_corpus vidyn_check ycbcr_compute_check gsexport_check present_late_check hotpath_bench hotpath_bench_old syncfd_check

TESTS += eventstatus_check
TESTS += querystuck_check

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

# hotpath_bench_old: the same with the arena and the null-descriptor set map as before 2026-10-21.
$(OUT)/hotpath_bench: $(VKB)/tests/hotpath_bench.c $(COMMON_SRC) $(VKB)/server/nd_map.h
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) -Wall $(INC) -I$(SRV) -o $@ $(VKB)/tests/hotpath_bench.c $(COMMON_SRC)

$(OUT)/hotpath_bench_old: $(VKB)/tests/hotpath_bench.c $(COMMON_SRC) $(VKB)/server/nd_map.h
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) -Wall -Wno-type-limits $(INC) -I$(SRV) -DARENA_SPARE_MAX=0 -DMAP_MAX_BUCKETS=8192 -o $@ $(VKB)/tests/hotpath_bench.c $(COMMON_SRC)

$(OUT)/texcap_check: $(VKB)/tests/texcap_comp.inc

$(OUT)/divisor_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/vbstride_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/dynstate_check: $(VKB)/tests/dynstate_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/cullmode_check: $(VKB)/tests/cullmode_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/gsfold_check: $(VKB)/tests/gsfold_vert.inc $(VKB)/tests/gsfold_geom.inc $(VKB)/tests/gsfold_frag.inc $(VKB)/tests/gsfold_lut.inc

$(OUT)/bitwise_check: $(VKB)/tests/bitwise_comp.inc

$(OUT)/ycbcr_compute_check: $(VKB)/tests/ycbcr_comp.inc

$(OUT)/xfb_check: $(VKB)/tests/xfb_xfb_vert.inc $(VKB)/tests/xfb_plain_vert.inc $(VKB)/tests/xfb_so_geom.inc

$(OUT)/asyncpipe_check: $(VKB)/tests/cullmode_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/polygon_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/dynrend_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/vidyn_check: $(VKB)/tests/vidyn_vert.inc $(VKB)/tests/vidyn_frag.inc

$(OUT)/clampzero_check: $(VKB)/tests/clampzero_comp.inc

$(OUT)/shmbarrier_check: $(VKB)/tests/shmbarrier_comp.inc

$(OUT)/wave32_check: $(VKB)/tests/wave32_comp.inc

$(OUT)/viewport_check: $(VKB)/tests/viewport_vert.inc $(VKB)/tests/viewport_vert_mv.inc $(VKB)/tests/viewport_geom.inc $(VKB)/tests/viewport_frag.inc

$(OUT)/promote_check: $(VKB)/tests/promote_comp15.inc $(VKB)/tests/promote_comp16.inc

# Version promotion's SPIR-V lowering alone, over a corpus (host/promote_test.sh)
$(OUT)/downgrade_corpus: $(VKB)/tests/downgrade_corpus.c $(SRV)/spirv_downgrade.c $(SRV)/spirv_downgrade.h $(SRV)/spirv_edit.c $(SRV)/spirv_edit.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/downgrade_corpus.c $(SRV)/spirv_downgrade.c $(SRV)/spirv_edit.c

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

# The shl_barrier SPIR-V pass alone, over a corpus (host/shlbarrier_test.sh)
$(OUT)/shlbarrier_corpus: $(VKB)/tests/shlbarrier_corpus.c $(SRV)/spirv_shlbarrier.c $(SRV)/spirv_shlbarrier.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/shlbarrier_corpus.c $(SRV)/spirv_shlbarrier.c

# The shm_barrier SPIR-V pass alone, over a corpus (host/shmbarrier_test.sh)
$(OUT)/shmbarrier_corpus: $(VKB)/tests/shmbarrier_corpus.c $(SRV)/spirv_shmbarrier.c $(SRV)/spirv_shmbarrier.h $(SRV)/spirv_uniform.c $(SRV)/spirv_uniform.h $(SRV)/spirv_lockstep.c $(SRV)/spirv_lockstep.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/shmbarrier_corpus.c $(SRV)/spirv_shmbarrier.c $(SRV)/spirv_uniform.c $(SRV)/spirv_lockstep.c

# The wave32 SPIR-V pass alone, over a corpus (host/wave32_test.sh)
$(OUT)/wave32_corpus: $(VKB)/tests/wave32_corpus.c $(SRV)/spirv_wave32.c $(SRV)/spirv_wave32.h $(SRV)/spirv_uniform.c $(SRV)/spirv_uniform.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/wave32_corpus.c $(SRV)/spirv_wave32.c $(SRV)/spirv_uniform.c

# The asynchronous compile's shader scan alone (host/asyncpipe_test.sh)
$(OUT)/effects_corpus: $(VKB)/tests/effects_corpus.c $(SRV)/spirv_effects.c $(SRV)/spirv_effects.h $(SRV)/spirv_opclass.inc \
		$(VKB)/tests/bitwise_comp.inc $(VKB)/tests/clampzero_comp.inc $(VKB)/tests/dynstate_frag.inc $(VKB)/tests/cullmode_vert.inc
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -I$(VKB)/tests -o $@ $(VKB)/tests/effects_corpus.c $(SRV)/spirv_effects.c

$(OUT)/flicker_check: $(VKB)/tests/flicker_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lm -lpthread

# The device-profile check's layer beneath the Khronos Profiles layer (devprofile_check.sh; host only)
VKH := $(VKB)/third_party/Vulkan-Headers-1.4.341
DP := $(VKB)/host/devprofile
$(OUT)/devprofile/devclamp_table.h: $(DP)/gen_devclamp_table.py $(VKH)/registry/vk.xml
	@mkdir -p $(OUT)/devprofile
	python3 $(DP)/gen_devclamp_table.py $(VKH)/registry/vk.xml $(VKH)/include/vulkan/vulkan_core.h $@

$(OUT)/devprofile/libVkLayer_malibridge_devclamp.so: $(DP)/devclamp_layer.c $(OUT)/devprofile/devclamp_table.h
	$(CC) $(CFLAGS) $(WARN) -fPIC -shared -fvisibility=hidden -DVK_ENABLE_BETA_EXTENSIONS $(INC) -I$(OUT)/devprofile -o $@ $< -lpthread -Wl,--no-undefined

$(OUT)/devprofile/VkLayer_malibridge_devclamp.json:
	@mkdir -p $(OUT)/devprofile
	printf '{"file_format_version":"1.2.0","layer":{"name":"VK_LAYER_MALIBRIDGE_devclamp","type":"GLOBAL","library_path":"./libVkLayer_malibridge_devclamp.so","api_version":"1.4.341","implementation_version":"1","description":"devprofile_check: features and extensions the desktop driver lacks taken off at vkCreateDevice"}}\n' > $@

devprofile: all $(OUT)/devprofile/libVkLayer_malibridge_devclamp.so $(OUT)/devprofile/VkLayer_malibridge_devclamp.json

test: all
	$(VKB)/host/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all test clean devprofile

$(OUT)/querystuck_check: $(VKB)/tests/querystuck_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lpthread
