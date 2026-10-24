#!/bin/bash
# Builds the Khronos Profiles layer for devprofile_check.sh into $VKB_DEVPROFILE_CACHE/vp
# (default ~/.cache/malibridge-devprofile; about 300 MB with its dependencies), with two changes
# to its generated code:
#   - the GPU's identity is taken from the profile too (deviceName, vendorID, deviceID, driverVersion,
#     deviceType, driverID, driverName, driverInfo; upstream keeps the real driver's), so the
#     bridge's per-GPU paths (errata by driver release, the G715's robustness2, MK-20) see the phone
#   - VkFormatProperties3 for a format the profile does not list keeps its sType (upstream zeroes the
#     whole struct, sType too, and the bridge then sees a chain it cannot encode)
set -e
CACHE=${VKB_DEVPROFILE_CACHE:-$HOME/.cache/malibridge-devprofile}
REV=${VKB_PROFILES_REV:-d1ff934a5b9ea4a23880ba993436343a7bcc729c}
mkdir -p "$CACHE"; cd "$CACHE"
[ -d vp/.git ] || git clone -q https://github.com/KhronosGroup/Vulkan-Profiles.git vp
git -C vp fetch -q origin "$REV" 2>/dev/null || true
git -C vp checkout -q "$REV"
[ -x venv/bin/python3 ] || { python3 -m venv venv && venv/bin/pip -q install -r vp/requirements.txt; }
export PATH=$CACHE/venv/bin:$PATH
cd vp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON -DBUILD_TESTS=OFF -DBUILD_PROFILES_LIBRARY=OFF \
  -DSKIP_PYTHON_AUTO_INSTALL=ON -DPython3_EXECUTABLE="$CACHE/venv/bin/python3" >"$CACHE/cmake.log"
make -C build -j"$(nproc)" VpLayer_generate >"$CACHE/make.log"
g=layer/profiles_generated.cpp
sed -i -E \
  -e 's/GET_VALUE\(prop, (driverVersion|vendorID|deviceID), true,/GET_VALUE(prop, \1, false,/' \
  -e 's/GET_VALUE_ENUM_WARN\(prop, deviceType, true,/GET_VALUE_ENUM_WARN(prop, deviceType, false,/' \
  -e 's/"deviceName", dest->deviceName, true\)/"deviceName", dest->deviceName, false)/' \
  -e 's/GET_VALUE_ENUM_WARN\(member, driverID, true,/GET_VALUE_ENUM_WARN(member, driverID, false,/' \
  -e 's/"(driverName|driverInfo)", dest->(driverName|driverInfo), true\)/"\1", dest->\2, false)/' "$g"
grep -q 'sp->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;' "$g" ||
  sed -i 's/^\(\s*\)\*sp = physicalDeviceData->map_of_format_properties_3_\[format\];/&\n\1sp->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;/' "$g"
n=$(grep -c 'deviceName, false)\|driverInfo, false)\|sp->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3' "$g")
[ "$n" -ge 4 ] || { echo "the generated code changed: the identity/format patches did not apply ($n)"; exit 1; }
# /fast: without regenerating (that would undo the patches)
make -C build -j"$(nproc)" ProfilesLayer/fast >>"$CACHE/make.log"
echo "layer: $CACHE/vp/build/layer"
