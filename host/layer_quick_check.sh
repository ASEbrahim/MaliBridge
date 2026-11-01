#!/bin/bash
# The layer's checks, the fast part only: for iterating on the layer.
#
#   host/layer_quick_check.sh [layer-build-dir]
#
# The same as host/layer_check.sh without the BCn emulation (the repo's bcn_check, which is the
# slow part): the layer loads, vkcube renders with it, the push descriptor and the clip distance
# emulations work, and the app is shown the layer's capability model.
cd "$(dirname "$0")"
exec ./layer_check.sh quick "$@"
