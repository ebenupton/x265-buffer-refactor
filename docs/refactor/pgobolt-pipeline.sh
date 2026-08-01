#!/bin/bash
# PGO+BOLT pipeline for x265-4.1-refactor on the banklow boot (2026-08-01).
# Single build dir (gcc .gcda paths are keyed to object paths, so generate
# and use phases must share it). Recipe follows bolt-artifacts/README.md,
# with compiler PGO added underneath.
set -eu
XP=/home/eben/bolt-boot/ffmpeg/x265-patches
SRC=$XP/x265-4.1-refactor/source
BLD=$XP/x265-4.1-refactor/build-pgo
INST=$XP/install-refactor-pgobolt
PGODIR=$XP/x265-4.1-refactor/pgo-data
YUV30=/home/eben/bolt-boot/ffmpeg/yuv/bbb_30s_1080p30.yuv
YUV90=/home/eben/bolt-boot/ffmpeg/yuv/bbb_90s.yuv
W=/tmp/claude-1000/-home-eben-bolt-boot/f0e6eaf2-ae48-4390-a056-90d4b962b782/scratchpad
COMMON="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76"
DM1T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --max-merge 1 \
 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 \
 --no-scenecut --no-weightp --no-weightb --me dia --merange 16 --subme 0 \
 --no-info --bitrate 5000 --frame-threads 1 --pools none"
REC4T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes \
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut \
 --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 \
 --no-info --bitrate 4000 --frame-threads 4 --pools 4"
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
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $DM1T  --output $W/t1.h265 >/dev/null 2>&1
LD_LIBRARY_PATH=$BLD $BLD/x265 --input $YUV30 $IN $REC4T --output $W/t2.h265 >/dev/null 2>&1
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

stage gate-pgo
bash $XP/dm-gate.sh $INST

stage perf-record
SO=$(readlink -f $INST/lib/libx265.so.215 2>/dev/null || echo $INST/lib/libx265.so.215)
LD_LIBRARY_PATH=$INST/lib perf record -e cycles:u -F 4000 -o $W/p1t.data -- \
  $INST/bin/x265 --input $YUV90 $IN $DM1T --output $W/p1.h265 >/dev/null 2>&1
LD_LIBRARY_PATH=$INST/lib perf record -e cycles:u -F 4000 -o $W/p4t.data -- \
  $INST/bin/x265 --input $YUV90 $IN $REC4T --output $W/p4.h265 >/dev/null 2>&1

stage perf2bolt
cd $W
perf2bolt-21 -nl -p p1t.data -o p1t.fdata $SO >/dev/null 2>&1
perf2bolt-21 -nl -p p4t.data -o p4t.fdata $SO >/dev/null 2>&1
merge-fdata-21 p1t.fdata p4t.fdata > pgo-combined.fdata

stage llvm-bolt
llvm-bolt-21 $SO -o $W/libx265.so.215.pgobolt \
  --data $W/pgo-combined.fdata \
  --reorder-blocks=ext-tsp --reorder-functions=hfsort+ \
  --split-functions --split-all-cold --no-huge-pages 2>&1 | tail -3
cp $SO $INST/lib/libx265.so.215.prebolt
cp $W/libx265.so.215.pgobolt $INST/lib/libx265.so.215

stage gate-pgobolt
bash $XP/dm-gate.sh $INST

rm -f $W/t1.h265 $W/t2.h265 $W/p1.h265 $W/p4.h265
echo PGOBOLT-PIPELINE-DONE
