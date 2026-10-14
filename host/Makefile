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
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare
INC := -I$(VKB)/third_party/Vulkan-Headers-1.4.341/include -I$(VKB)/common -I$(VKB)/common/gen
COMMON_SRC := $(VKB)/common/vkb_wire.c $(VKB)/common/gen/vkb_gen_structs.c
SERVER_SRC := $(wildcard $(SRV)/*.c) $(VKB)/common/gen/vkb_gen_server.c $(COMMON_SRC)
CLIENT_SRC := $(wildcard $(VKB)/client/*.c) $(VKB)/common/gen/vkb_gen_client.c $(COMMON_SRC)

TESTS := vkbridge-smoke bcn_check opaque_check dmabuf_image_check recycle_check desc_traffic_check query_check placed_check crash_contain_check desc_limits_check flicker_check nulldesc_check pcache_check

all: $(OUT)/vkbridge-server $(OUT)/libvulkan_droidbridge.so $(OUT)/vkbridge_icd.json $(addprefix $(OUT)/,$(TESTS))

$(OUT)/vkbridge-server: $(SERVER_SRC) $(wildcard $(SRV)/*.h $(VKB)/common/*.h $(VKB)/common/gen/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) $(WARN) $(INC) -I$(SRV) -o $@ $(SERVER_SRC) -ldl -lpthread -lm -rdynamic

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

$(OUT)/flicker_check: $(VKB)/tests/flicker_check.c $(VKB)/tests/smoke_spv.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SAN) -funsigned-char -Wall $(INC) -I$(VKB)/tests -o $@ $< -ldl -lm -lpthread

test: all
	$(VKB)/host/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all test clean
