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
SERVER_SRC := $(wildcard $(SRV)/*.c) $(VKB)/common/gen/vkb_gen_server.c $(COMMON_SRC) $(VKB)/common/vkb_caps.c
CLIENT_SRC := $(wildcard $(VKB)/client/*.c) $(VKB)/common/gen/vkb_gen_client.c $(COMMON_SRC) $(VKB)/common/vkb_caps.c

TESTS := vkbridge-smoke heapleak_check exitheap_check pvrdestroy_check pldedup_check pvmutable_check pvbgra_check gpuprof_check poll_check reqcache_check wginit_check hitch_check syncmirror_check poolfull_check bcn_check opaque_check dmabuf_image_check recycle_check desc_traffic_check query_check placed_check crash_contain_check desc_limits_check flicker_check nulldesc_check pcache_check divisor_check gsfold_check view3d_check viewchurn_check wait_check bitwise_check hangdisp_check fragdepth_check bitwise_corpus dynstate_check vbstride_check viewport_check flip_corpus clampzero_check clampzero_corpus begin_check shlbarrier_corpus cullmode_check dedimport_check importrace_check cached_check polygon_check gpuview_check membudget_check dedhint_check planar_check fdprops_check memclass_check procaddr_check exitclean_check effects_corpus asyncpipe_check texcap_check robust2_check fp16_check gshide_check featclamp_check promote_check downgrade_corpus xfb_check shmbarrier_check shmbarrier_corpus stencillayout_check dynrend_check wave32_check wave32_corpus vidyn_check ycbcr_compute_check gsexport_check present_late_check hotpath_bench hotpath_bench_old syncfd_check asyncgap_check dmabufsync_check timeline_check splitvary_check splitvary_unit layer-app pvnames_check pvnames_corpus

TESTS += eventstatus_check
TESTS += compilegate_unit
TESTS += bcburst_check bcupload_check
TESTS += compileburst_check
TESTS += presentwait_check
TESTS += dabreg_unit dmabufahb_check pvbgrareg_unit
TESTS += dshape_unit dmabufsize_check
TESTS += bcauto_unit bcsmall_unit
TESTS += gpuprof_report_check
TESTS += imgstat_check
TESTS += querystuck_check
TESTS += lite_bench
TESTS += sparse_check
TESTS += present_block_check swapchain_churn_check
TESTS += stale_frames_check
TESTS += lost_check
TESTS += endcb_check
TESTS += wire_check
TESTS += flushelide_check
TESTS += imageless_check
TESTS += heapkind_check
TESTS += capgate_unit clip_pipeline
TESTS += cores_unit
TESTS += present_path_check
TESTS += pacer_check
TESTS += fmtcache_check
TESTS += fastcreate_check
TESTS += asynctimer_check
TESTS += querypoll_check asynclock_check
TESTS += cpuprof_unit
TESTS += importtype_unit
TESTS += pagesize_unit
TESTS += pv3d_check
TESTS += zinkname_check
TESTS += faultsnap_check
TESTS += pvsusp_check

all: $(OUT)/vkbridge-server $(OUT)/libvulkan_droidbridge.so $(OUT)/vkbridge_icd.json $(addprefix $(OUT)/,$(TESTS)) $(OUT)/libvkb_mali_profile.so \
	$(OUT)/pcmem/libvkb_pcache_mem.so $(OUT)/pcmem/vkb_pcache_mem.json

# precachemem_test.sh: a layer for the server's driver whose pipeline caches hold memory as Mali's do (lavapipe's hold none)
$(OUT)/pcmem/libvkb_pcache_mem.so: $(VKB)/tests/pcache_mem_layer.c
	@mkdir -p $(OUT)/pcmem
	$(CC) $(CFLAGS) -Wall -fPIC -shared -fvisibility=hidden $(INC) -o $@ $< -lpthread

$(OUT)/pcmem/vkb_pcache_mem.json: $(VKB)/host/Makefile
	@mkdir -p $(OUT)/pcmem
	printf '{"file_format_version":"1.1.2","layer":{"name":"VK_LAYER_VKB_pcache_mem","type":"GLOBAL","library_path":"%s","api_version":"1.4.341","implementation_version":"1","description":"host test: pipeline caches hold memory","functions":{"vkGetInstanceProcAddr":"vkb_pcmem_GetInstanceProcAddr","vkGetDeviceProcAddr":"vkb_pcmem_GetDeviceProcAddr"}}}\n' "$(OUT)/pcmem/libvkb_pcache_mem.so" > $@

# The suite's Mali profile (mali_profile.sh): the profile's switches added to every server's and client's own
$(OUT)/libvkb_mali_profile.so: $(VKB)/host/profiles/mali_profile_shim.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Wall -fPIC -shared -o $@ $< -lpthread

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

# The layer's tests (host/layer_check.sh): a minimal app, and the BCn emulation.
$(OUT)/layer-app: $(VKB)/tests/layer_app.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -l:libvulkan.so.1

$(OUT)/bcn_check: $(VKB)/server/bc_spv.h $(VKB)/tests/bcsink_vert.inc $(VKB)/tests/bcsink_frag.inc

# pvbgrareg_unit: the register of dma-bufs holding R8G8B8A8 for a B8G8R8A8 client image (server_pvbgra_reg.c, PV-07) on its own.
$(OUT)/pvbgrareg_unit: $(VKB)/tests/pvbgrareg_unit.c $(SRV)/server_pvbgra_reg.c $(SRV)/server_pvbgra_reg.h $(VKB)/common/vkb_dmabuf_ahb.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(VKB)/common -I$(SRV) -o $@ $(VKB)/tests/pvbgrareg_unit.c $(SRV)/server_pvbgra_reg.c -lpthread

# dabreg_unit: the register of dma-bufs over AHardwareBuffers (server_dab_reg.c) on its own.
$(OUT)/dabreg_unit: $(VKB)/tests/dabreg_unit.c $(SRV)/server_dab_reg.c $(SRV)/server_dab.h $(VKB)/common/vkb_dmabuf_ahb.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(VKB)/common -I$(SRV) -o $@ $(VKB)/tests/dabreg_unit.c $(SRV)/server_dab_reg.c -lpthread

# clip_pipeline: a vertex shader's clip distance drawn (clip_check.sh, capgate_test.sh)
$(OUT)/clip_pipeline: $(VKB)/tests/clip_pipeline.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ $< -ldl

# capgate_unit: the capability check's SPIR-V scan (spirv_caps.c) on its own.
$(OUT)/capgate_unit: $(VKB)/tests/capgate_unit.c $(SRV)/spirv_caps.c $(SRV)/spirv_caps.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -o $@ $(VKB)/tests/capgate_unit.c $(SRV)/spirv_caps.c

# dshape_unit: the register of dma-buf image shapes (server_dmabuf_shape.c) on its own.
$(OUT)/dshape_unit: $(VKB)/tests/dshape_unit.c $(SRV)/server_dmabuf_shape.c $(SRV)/server_dshape.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -o $@ $(VKB)/tests/dshape_unit.c $(SRV)/server_dmabuf_shape.c -lpthread

# bcauto_unit: the BC budget's auto rate from a GPU name (server_bc_auto.h) on its own.
$(OUT)/bcauto_unit: $(VKB)/tests/bcauto_unit.c $(SRV)/server_bc_auto.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -o $@ $(VKB)/tests/bcauto_unit.c -lm

# bcsmall_unit: the small-image rule's memory cap from the device's RAM (server_bc_small.h) on its own.
$(OUT)/bcsmall_unit: $(VKB)/tests/bcsmall_unit.c $(SRV)/server_bc_small.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -o $@ $(VKB)/tests/bcsmall_unit.c

# cores_unit: server_cores.c on its own (the fast cores for the threads a game waits on), the server's functions stubbed.
$(OUT)/cores_unit: $(VKB)/tests/cores_unit.c $(SRV)/server_cores.c $(SRV)/server_cores.h $(SRV)/vkb_server.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -o $@ $(VKB)/tests/cores_unit.c -lpthread

# compilegate_unit: server_compile_gate.c on its own, the server's functions stubbed in the test.
$(OUT)/compilegate_unit: $(VKB)/tests/compilegate_unit.c $(SRV)/server_compile_gate.c $(SRV)/vkb_server.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -o $@ $(VKB)/tests/compilegate_unit.c -lpthread

# importtype_unit: server_import_type.c on its own (the dma-buf import's memory type fallback).
$(OUT)/importtype_unit: $(VKB)/tests/importtype_unit.c $(SRV)/server_import_type.c $(SRV)/server_import_type.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -o $@ $(VKB)/tests/importtype_unit.c

# pagesize_unit: common/vkb_page.h on its own (the page size the two halves use; a 16 KB kernel imitated).
$(OUT)/pagesize_unit: $(VKB)/tests/pagesize_unit.c $(VKB)/common/vkb_page.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -o $@ $(VKB)/tests/pagesize_unit.c

# hotpath_bench_old: the same with the arena and the null-descriptor set map as before 2026-10-21.
$(OUT)/hotpath_bench: $(VKB)/tests/hotpath_bench.c $(COMMON_SRC) $(VKB)/server/nd_map.h
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) -Wall $(INC) -I$(SRV) -o $@ $(VKB)/tests/hotpath_bench.c $(COMMON_SRC)

# wire_check: the encoder/decoder primitives against a byte-wise model, and their cost against the out-of-line chain they replaced.
$(OUT)/wire_check: $(VKB)/tests/wire_check.c $(COMMON_SRC)
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) $(WARN) $(INC) -o $@ $(VKB)/tests/wire_check.c $(COMMON_SRC)

$(OUT)/hotpath_bench_old: $(VKB)/tests/hotpath_bench.c $(COMMON_SRC) $(VKB)/server/nd_map.h
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) -Wall -Wno-type-limits $(INC) -I$(SRV) -DARENA_SPARE_MAX=0 -DMAP_MAX_BUCKETS=8192 -o $@ $(VKB)/tests/hotpath_bench.c $(COMMON_SRC)

$(OUT)/texcap_check: $(VKB)/tests/texcap_comp.inc

$(OUT)/divisor_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/vbstride_check: $(VKB)/tests/divisor_vert.inc $(VKB)/tests/divisor_frag.inc

$(OUT)/dynstate_check: $(VKB)/tests/dynstate_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/cullmode_check: $(VKB)/tests/cullmode_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/splitvary_check: $(wildcard $(VKB)/tests/splitvary_*.inc)

$(OUT)/gsfold_check: $(VKB)/tests/gsfold_vert.inc $(VKB)/tests/gsfold_geom.inc $(VKB)/tests/gsfold_frag.inc $(VKB)/tests/gsfold_lut.inc

$(OUT)/bitwise_check: $(VKB)/tests/bitwise_comp.inc

$(OUT)/hangdisp_check: $(VKB)/tests/hangdisp_comp.inc

$(OUT)/fragdepth_check: $(VKB)/tests/fragdepth_vert.inc $(VKB)/tests/fragdepth_frag.inc

$(OUT)/pvnames_check: $(VKB)/tests/pvnames_vert.inc $(VKB)/tests/pvnames_frag.inc

$(OUT)/ycbcr_compute_check: $(VKB)/tests/ycbcr_comp.inc

$(OUT)/xfb_check: $(VKB)/tests/xfb_xfb_vert.inc $(VKB)/tests/xfb_plain_vert.inc $(VKB)/tests/xfb_so_geom.inc

$(OUT)/asyncpipe_check: $(VKB)/tests/cullmode_vert.inc $(VKB)/tests/dynstate_frag.inc
$(OUT)/compileburst_check: $(VKB)/tests/cullmode_vert.inc $(VKB)/tests/dynstate_frag.inc

$(OUT)/polygon_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/dynrend_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/gpuprof_report_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/imageless_check: $(VKB)/tests/polygon_vert.inc $(VKB)/tests/polygon_frag.inc

$(OUT)/vidyn_check: $(VKB)/tests/vidyn_vert.inc $(VKB)/tests/vidyn_frag.inc

$(OUT)/clampzero_check: $(VKB)/tests/clampzero_comp.inc

$(OUT)/pvbgra_check: $(VKB)/tests/pvbgra_write_comp.inc $(VKB)/tests/pvbgra_sample_comp.inc

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

# The MK-47 SPIR-V pass alone, on splitvary_check's shaders (host/splitvary_test.sh)
$(OUT)/splitvary_unit: $(VKB)/tests/splitvary_unit.c $(SRV)/spirv_splitvary.c $(SRV)/spirv_splitvary.h $(SRV)/spirv_edit.c $(SRV)/spirv_edit.h $(wildcard $(VKB)/tests/splitvary_*.inc)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I$(SRV) -I$(VKB)/tests -o $@ $(VKB)/tests/splitvary_unit.c $(SRV)/spirv_splitvary.c $(SRV)/spirv_edit.c
# The PV-01 SPIR-V pass alone, over a corpus (host/pvnames_test.sh)
$(OUT)/pvnames_corpus: $(VKB)/tests/pvnames_corpus.c $(SRV)/spirv_names.c $(SRV)/spirv_names.h $(SRV)/spirv_edit.c $(SRV)/spirv_edit.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -o $@ $(VKB)/tests/pvnames_corpus.c $(SRV)/spirv_names.c $(SRV)/spirv_edit.c

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

$(OUT)/lite_bench: $(VKB)/tests/lite_bench.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) -O2 $(SAN) -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lpthread

$(OUT)/present_path_check: $(VKB)/tests/present_path_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/pacer_check: $(VKB)/tests/pacer_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/asynctimer_check: $(VKB)/tests/asynctimer_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/querypoll_check: $(VKB)/tests/querypoll_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/asynclock_check: $(VKB)/tests/asynclock_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/cpuprof_unit: $(VKB)/tests/cpuprof_unit.c $(SRV)/server_cpuprof.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -o $@ $(VKB)/tests/cpuprof_unit.c

$(OUT)/fmtcache_check: $(VKB)/tests/fmtcache_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/fastcreate_check: $(VKB)/tests/fastcreate_check.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -Wall $(INC) -o $@ $< -ldl -lpthread

$(OUT)/querystuck_check: $(VKB)/tests/querystuck_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lpthread
