#!/bin/bash
# Built-in preset table checks (server/presets_builtin.inc), data only; the matching itself is
# covered by shmbarrier_test/wave32_test (VKBRIDGE_PRESET_FAKE_BUILTIN).
#   1  Resident Evil 3 (952060) on Mali-G925: shared-memory barriers after the lockstep rewrite on
#      the two RE Engine draw-list compaction shaders only (the RE2 pair). Shown on the S11 Ultra
#      (G925, DX11 branch, 11-09 session 2026-11-09-45-steam): with VKBRIDGE_SHARED_BARRIER=2 and
#      LOCKSTEP=all the lockstep rewrite took exactly modules 2a0debecc286be40 and e28ac7ddfa639dc4
#      and the hallway boxes stopped flickering (played through).
#      11-14: named by shape (ONLY=lockstep + LOCKSTEP=all), not by hash: on Proton Experimental's DXVK the same two
#      shaders have other hashes and the hash entry rewrote nothing (Poco X8 Pro Max G925 MC11, flicker back).
#   2  no GPU-tested RE3 entry carries WAVE32 or barriers on every module (=all without ONLY cost gpu_busy 70-80 -> 87-93).
#   3  RE2's G925 entry unchanged.
#   4  Devil May Cry 5 (601150) on Mali-G925: the same entry as RE3. Shown on the S11 Ultra (G925, test60, 11-14 session
#      2026-11-14-48-steam): a building flickered in session 47 (preset not applied: 'not tested on
#      Mali-G925'); with this entry from a preset file the lockstep rewrite took exactly 2a0debecc286be40 and
#      e28ac7ddfa639dc4 (the RE2/RE3 pair) and the flicker was gone (the same building); fps 44 vs 46.
set -u
cd "$(dirname "$0")/.."
inc=server/presets_builtin.inc
pair='e28ac7ddfa639dc4,2a0debecc286be40'
fail=0
step() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }
grep -q "^{\"952060\", \"Mali-G925\", \"VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=lockstep VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all\"," $inc
step $? "1 RE3 Mali-G925 entry: barriers only on the modules the lockstep rewrite takes"
! grep -E '^\{"952060", "[^"]+",' $inc | grep -qE 'WAVE32' && grep -E '^\{"952060", "[^"]+",' $inc | grep -q 'ONLY='
step $? "2 no GPU-tested RE3 entry with WAVE32 or without ONLY"
grep -q "^{\"883710\", \"Mali-G925\", \"VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=lockstep VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all\"," $inc
step $? "3 RE2 Mali-G925 entry the same way"
grep -q "^{\"601150\", \"Mali-G925\", \"VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=lockstep VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all\"," $inc
step $? "4 DMC5 Mali-G925 entry the same as RE3 (building flicker)"
[ $fail = 0 ] && echo "presets_table: all pass" || echo "presets_table: FAIL"
exit $fail
