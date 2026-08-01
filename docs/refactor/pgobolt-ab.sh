#!/bin/bash
set -u
XP=/home/eben/bolt-boot/ffmpeg/x265-patches
A=$XP/install-refactor
B=$XP/install-refactor-pgobolt
YUV30=/home/eben/bolt-boot/ffmpeg/yuv/bbb_30s_1080p30.yuv
YUV90=/home/eben/bolt-boot/ffmpeg/yuv/bbb_90s.yuv
IN="--input-res 1920x1080 --fps 30 --input-csp i420"
REC4T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 --no-info --bitrate 4000 --frame-threads 4 --pools 4"
DM1T="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --max-merge 1 --limit-modes --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut --no-weightp --no-weightb --me dia --merange 16 --subme 0 --no-info --bitrate 5000 --frame-threads 1 --pools none"
W=/tmp/claude-1000/-home-eben-bolt-boot/f0e6eaf2-ae48-4390-a056-90d4b962b782/scratchpad
run() { # $1 inst-path $2 yuv $3 cfg-var $4 tag
  local cfg; eval cfg=\$$3
  LD_LIBRARY_PATH=$1/lib perf stat -e cycles:u -o $W/ab.stat -- \
    $1/bin/x265 --input $2 $IN $cfg --output $W/ab.h265 >/dev/null 2>&1
  wall=$(grep 'seconds time elapsed' $W/ab.stat | awk '{print $1}')
  cyc=$(grep 'cycles:u' $W/ab.stat | awk '{gsub(",","",$1); printf "%.1f", $1/1e9}')
  echo "AB $4 wall=${wall} cycles=${cyc}G"
}
run $A $YUV30 REC4T warmup-discard
for i in 1 2 3; do run $A $YUV30 REC4T "30s4t-ref-$i"; run $B $YUV30 REC4T "30s4t-pgb-$i"; done
for i in 1 2;   do run $A $YUV90 REC4T "90s4t-ref-$i"; run $B $YUV90 REC4T "90s4t-pgb-$i"; done
for i in 1 2;   do run $A $YUV30 DM1T  "30s1t-ref-$i"; run $B $YUV30 DM1T  "30s1t-pgb-$i"; done
echo "temp=$(vcgencmd measure_temp) throttled=$(vcgencmd get_throttled)"
echo AB-DONE
