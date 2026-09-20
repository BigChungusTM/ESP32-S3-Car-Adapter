#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/idf-env.sh"
cd "$PROJECT_ROOT/firmware/controller-probe"
for probe_case in 0 1 2 3 4; do
    idf.py -B "build-$probe_case" -DIDF_TARGET=esp32s3 \
        -DSDKCONFIG="build-$probe_case/sdkconfig" -DPROBE_CASE="$probe_case" build
done
