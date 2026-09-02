#!/bin/bash
# PGO+BOLT pipeline adapted from docs/refactor/pgobolt-pipeline.sh for the
# /home/pi rig: soname .216, llvm-bolt-19 (21 unavailable here), training
# includes the recommended ft1 + --async-lookahead operating point so profile
# and measurement agree (per ENTROPY-DECOUPLING final section).
set -eu
XP=/home/pi/x265-buffer-refactor
SRC=$XP/source
BLD=$XP/build-pgo
INST=$XP/install-pgobolt
PGODIR=$XP/pgo-data
YUV30=/home/pi/yuv/bbb_30s_1080p30.yuv
YUV90=/home/pi/yuv/bbb_90s.yuv
W=/home/pi/yuv/work; mkdir -p $W
COMMON="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76"
DM1T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --max-merge 1 \
 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 \
 --no-scenecut --no-weightp --no-weightb --me dia --merange 16 --subme 0 \
 --no-info --bitrate 5000 --frame-threads 1 --pools none"
REC="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes \
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut \
 --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 \
 --no-info --bitrate 4000 --pools 4"
IN="--input-res 1920x1080 --fps 30 --input-csp i420"

stage() { echo "STAGE $1 $(date '+%H:%M:%S')"; }

stage pgo-generate-build
rm -rf $BLD $PGODIR $INST && mkdir -p $BLD $PGODIR
cd $BLD
cmake $SRC -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$COMMON -fprofile-generate=$PGODIR -fprofile-update=atomic" \
  -DCMAKE_CXX_FLAGS="$COMMON -fprofile-generate=$PGODIR -fprofile-update=atomic" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate=$PGODIR" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-generate=$PGODIR" \
  -DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON \
  -DCMAKE_INSTALL_PREFIX=$INST >/dev/null
make -j4 >/dev/null 2>&1 || { echo BUILD-FAIL-GEN; make 2>&1 | tail -20; exit 1; }

stage pgo-train
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $DM1T --output $W/t1.h265 >/dev/null 2>&1
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $REC --frame-threads 4 --output $W/t2.h265 >/dev/null 2>&1
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $REC --frame-threads 1 --async-lookahead 2 --output $W/t3.h265 >/dev/null 2>&1
echo "gcda files: $(find $PGODIR -name '*.gcda' | wc -l)"

stage pgo-use-build
cd $BLD
cmake $SRC \
  -DCMAKE_C_FLAGS="$COMMON -fprofile-use=$PGODIR -fprofile-correction -Wno-missing-profile" \
  -DCMAKE_CXX_FLAGS="$COMMON -fprofile-use=$PGODIR -fprofile-correction -Wno-missing-profile" \
  -DCMAKE_EXE_LINKER_FLAGS="" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--emit-relocs" >/dev/null
make clean >/dev/null; make -j4 >/dev/null 2>&1 || { echo BUILD-FAIL-USE; make 2>&1 | tail -20; exit 1; }
make install >/dev/null 2>&1
ls -la $INST/bin/x265 $INST/lib/libx265.so.216 || { echo INSTALL-INCOMPLETE; exit 1; }
echo PGO-PHASE-DONE
