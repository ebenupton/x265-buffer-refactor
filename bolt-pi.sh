#!/bin/bash
# BOLT stage per bolt-artifacts/README.md, llvm-bolt-19, soname .216.
set -eu
XP=/home/pi/x265-buffer-refactor
INST=$XP/install-pgobolt
YUV90=/home/pi/yuv/bbb_90s.yuv
W=/home/pi/yuv/work; mkdir -p $W
DM1T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --max-merge 1 \
 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 \
 --no-scenecut --no-weightp --no-weightb --me dia --merange 16 --subme 0 \
 --no-info --bitrate 5000 --frame-threads 1 --pools none"
REC="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes \
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut \
 --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 \
 --no-info --bitrate 4000 --pools 4 --frame-threads 1 --async-lookahead 2"
IN="--input-res 1920x1080 --fps 30 --input-csp i420"
SO=$INST/lib/libx265.so.216

stage() { echo "STAGE $1 $(date '+%H:%M:%S')"; }

stage perf-record
LD_LIBRARY_PATH=$INST/lib perf record -e cycles:u -F 4000 -o $W/p1t.data -- \
  $INST/bin/x265 --input $YUV90 $IN $DM1T --output $W/p1.h265 >/dev/null 2>&1
LD_LIBRARY_PATH=$INST/lib perf record -e cycles:u -F 4000 -o $W/p4t.data -- \
  $INST/bin/x265 --input $YUV90 $IN $REC --output $W/p4.h265 >/dev/null 2>&1

stage perf2bolt
cd $W
perf2bolt-19 -nl -p p1t.data -o p1t.fdata $SO >/dev/null 2>&1
perf2bolt-19 -nl -p p4t.data -o p4t.fdata $SO >/dev/null 2>&1
merge-fdata-19 p1t.fdata p4t.fdata > pgo-combined.fdata

stage llvm-bolt
llvm-bolt-19 $SO -o $W/libx265.so.216.pgobolt \
  --data $W/pgo-combined.fdata \
  --reorder-blocks=ext-tsp --reorder-functions=hfsort+ \
  --split-functions --split-all-cold --no-huge-pages 2>&1 | tail -3
cp $SO $INST/lib/libx265.so.216.prebolt
cp $W/libx265.so.216.pgobolt $SO

stage gate-pgobolt-local
REFHASH=$XP/tools/dm-gate-pi-local-ref.md5 \
  bash -c 'sed "s#REFHASH=.*#REFHASH=$REFHASH#" '$XP'/tools/dm-gate-pi.sh > /tmp/gate-local.sh; bash /tmp/gate-local.sh '$INST''
rm -f $W/p1.h265 $W/p4.h265
echo BOLT-PIPELINE-DONE
