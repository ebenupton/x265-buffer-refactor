#!/bin/bash
# Rebuild PGO+BOLT trained on the CURRENT production config.
#
# The shipped profile was trained on async2 + single-slice; production now
# runs async0 + --slices 4 + --early-slice + ring depth 1, so the profile
# and the measured path disagree (the exact mistake ENTROPY-DECOUPLING
# warns about). Output is unchanged by construction -- PGO/BOLT alter code
# layout only -- so PSNR and bit-exactness are untouched; the win is encode
# time, which near 88% duty converts into queueing latency.
set -eu
XP=/home/pi/x265-buffer-refactor
SRC=$XP/source
BLD=$XP/build-prod
PGODIR=$XP/pgo-prod
W=/home/pi/yuv/work; mkdir -p $W
YUV30=/home/pi/yuv/bbb_30s_1080p30.yuv
COMMON="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76"
# the production encode, as tx.sh runs it
PROD="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes \
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut \
 --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 \
 --crf 26 --pools 4 --frame-threads 1 --async-lookahead 0 --slices 4 \
 --early-slice --repeat-headers --keyint 60 --vbv-bufsize 4000 \
 --vbv-maxrate 4000 --no-info"
IN="--input-res 1920x1080 --fps 30 --input-csp i420"

echo "STAGE generate $(date +%T)"
rm -rf $BLD $PGODIR; mkdir -p $BLD $PGODIR
cd $BLD
cmake $SRC -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$COMMON -fprofile-generate=$PGODIR -fprofile-update=atomic" \
  -DCMAKE_CXX_FLAGS="$COMMON -fprofile-generate=$PGODIR -fprofile-update=atomic" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate=$PGODIR" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-generate=$PGODIR" \
  -DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON >/dev/null
make -j4 >/dev/null 2>&1 || { echo GEN-BUILD-FAIL; exit 1; }

echo "STAGE train $(date +%T)"
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $PROD -o - > /dev/null 2>&1
echo "  gcda: $(find $PGODIR -name '*.gcda' | wc -l)"

echo "STAGE use $(date +%T)"
cd $BLD
cmake $SRC \
  -DCMAKE_C_FLAGS="$COMMON -fprofile-use=$PGODIR -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch" \
  -DCMAKE_CXX_FLAGS="$COMMON -fprofile-use=$PGODIR -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch" \
  -DCMAKE_EXE_LINKER_FLAGS="" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--emit-relocs" >/dev/null
make clean >/dev/null; make -j4 >/dev/null 2>&1 || { echo USE-BUILD-FAIL; exit 1; }

echo "STAGE perf $(date +%T)"
SO=$BLD/libx265.so.216
LD_LIBRARY_PATH=$BLD perf record -e cycles:u -F 4000 -o $W/pp.data -- \
  $BLD/x265 --input $YUV30 $IN $PROD -o - >/dev/null 2>&1
cd $W
llvm-bolt-19 $SO --aggregate-only --nl -p pp.data -o pp.fdata 2>&1 | tail -1

echo "STAGE bolt $(date +%T)"
llvm-bolt-19 $SO -o $W/libx265.so.216.prodbolt --data pp.fdata \
  --reorder-blocks=ext-tsp --reorder-functions=hfsort+ \
  --split-functions --split-all-cold --no-huge-pages 2>&1 | tail -2
cp $W/libx265.so.216.prodbolt $SO
echo "PROD-BUILD-DONE $(date +%T)  -> $BLD/x265 + $SO"
