#!/bin/bash
# Source this (run_tests.sh and run_ci_tests.sh do): the host suite imitates a real Mali driver's
# extensions and features by default, so a fix that leans on something lavapipe/RADV has and Mali
# lacks fails here instead of passing on the PC (VK_EXT_memory_budget: every Mali server log in the
# players' uploads says -EXT_memory_budget).
#   VKB_MALI_PROFILE=common   (default) what EVERY Mali driver in the players' logs has: Vulkan 1.1,
#                             34 extensions (host/profiles/mali-common.txt)
#   VKB_MALI_PROFILE=r49      one driver family (host/profiles/mali-<family>.txt: r18-r19, r21-r26, r32-r34,
#                             r38-r40, r44, r49, r54, r56)
#   VKB_MALI_PROFILE=off      the desktop driver as it is (the suite before 2026-11-05)
# The profiles come from players' vkbridge-server.log files (profiles/gen_mali_profiles.py); the switches
# from profiles/mali_profile_env.py; every server and client the suite starts gets them through
# out/libvkb_mali_profile.so (LD_PRELOAD; profiles/mali_profile_shim.c), added to the test's own
# VKBRIDGE_FAKE_MISSING / VKBRIDGE_SERVER_HIDE_EXTS (also when a test starts its server with env -u).
# Not imitated (no switch exists): the core features listed in VKB_PROFILE_NOT_IMITATED (shaderFloat64,
# pipelineStatisticsQuery, ...), memory sharing, formats and limits, the server's own use of the
# driver's extensions (only what clients are shown changes, and the emulations the tokens select).
#   mp_apply <profile|off>   switch this shell (and what it starts next) to another profile
_mp_host=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mp_apply() {
  local p=${1:-common} line
  unset VKB_PROFILE_FAKE_MISSING VKB_PROFILE_HIDE_EXTS VKB_PROFILE_FAKE_API VKB_PROFILE_NOT_IMITATED
  export VKB_MALI_PROFILE_CUR=$p
  if [ "$p" = off ]; then
    return 0
  fi
  local f=$_mp_host/profiles/mali-$p.txt
  [ -f "$f" ] || { echo "mali_profile.sh: no profile $f" >&2; return 1; }
  [ -f "$_mp_host/out/libvkb_mali_profile.so" ] || { echo "mali_profile.sh: out/libvkb_mali_profile.so missing (make -C host)" >&2; return 1; }
  while IFS= read -r line; do
    export "$line"
  done < <(python3 -I "$_mp_host/profiles/mali_profile_env.py" "$f" "$_mp_host/../third_party/Vulkan-Headers-1.4.341/registry/vk.xml")
  case ":${LD_PRELOAD:-}:" in
    *":$_mp_host/out/libvkb_mali_profile.so:"*) ;;
    *) export LD_PRELOAD=$_mp_host/out/libvkb_mali_profile.so${LD_PRELOAD:+:$LD_PRELOAD} ;;
  esac
}
mp_apply "${VKB_MALI_PROFILE:-common}" || exit 1
echo "Mali profile: ${VKB_MALI_PROFILE_CUR}${VKB_PROFILE_FAKE_MISSING:+ (Vulkan $VKB_PROFILE_FAKE_API; tokens $VKB_PROFILE_FAKE_MISSING; $(echo "$VKB_PROFILE_HIDE_EXTS" | tr ',' '\n' | wc -l) extensions hidden; not imitated: $VKB_PROFILE_NOT_IMITATED)}"
