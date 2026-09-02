#!/bin/bash
# dm-gate-tot.sh with paths adapted to the /home/pi rig; logic unchanged.
set -u
INST=${1:?usage: dm-gate-pi.sh <install-prefix> [capture]}
MODE=${2:-check}
YUV=/home/pi/yuv/bbb_30s_1080p30.yuv
W=/tmp/claude-1000/-home-pi-video-pipe/74bb767e-361f-4503-a3f9-a6cccdf94e65/scratchpad/dm-gate; rm -rf $W; mkdir -p $W
if   [ -x "$INST/bin/x265" ]; then X265=$INST/bin/x265; LIBD=$INST/lib
elif [ -x "$INST/x265" ];     then X265=$INST/x265;     LIBD=$INST
else echo "GATE-ERROR: no x265 binary under '$INST'"; exit 2
fi
echo "gate: using $X265" >&2
REFHASH=/home/pi/x265-buffer-refactor/tools/dm-gate-pi-local-ref.md5

BASE="--input $YUV --input-res 1920x1080 --fps 30 --input-csp i420 \
 --preset ultrafast --tune zerolatency --bframes 0 --rd 1 --max-merge 1 \
 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao \
 --ctu 16 --no-scenecut --no-weightp --no-weightb --me dia --merange 16 \
 --subme 0 --no-info"

run() {
    LD_LIBRARY_PATH=$LIBD $X265 $BASE $2 --output "$W/$1" >/dev/null 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then echo "GATE-ERROR: config $1 exited rc=$rc"; exit 2; fi
    if [ ! -s "$W/$1" ]; then echo "GATE-ERROR: config $1 produced no output"; exit 2; fi
}

run c1 "--bitrate 5000 --frame-threads 1 --pools none"
run c2 "--bitrate 2000 --frame-threads 1 --pools none"
run c3 "--bitrate 5000 --frame-threads 4 --pools 4"

STOCK="--input $YUV --input-res 1920x1080 --fps 30 --input-csp i420 \
 --preset ultrafast --tune zerolatency --frames 120 --no-info"
LD_LIBRARY_PATH=$LIBD $X265 $STOCK --bitrate 4000 --frame-threads 1 --pools 4 \
  --output "$W/c4" >/dev/null 2>&1
rc=$?
[ $rc -ne 0 ] && { echo "GATE-ERROR: config c4 exited rc=$rc"; exit 2; }
[ -s "$W/c4" ] || { echo "GATE-ERROR: config c4 produced no output"; exit 2; }

( cd $W && md5sum c1 c2 c3 c4 ) > $W/got.md5
if [ "$MODE" = capture ]; then
    cp $W/got.md5 $REFHASH; echo "captured:"; cat $REFHASH
else
    if diff -q $REFHASH $W/got.md5 >/dev/null; then echo GATE-PASS
    else echo GATE-FAIL; diff $REFHASH $W/got.md5; exit 1; fi
fi
