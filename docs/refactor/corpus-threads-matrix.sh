#!/bin/bash
# Corpus x frame-threads matrix on the final PGO+BOLT build.
# Preferred config; --pools 4 fixed; --frame-threads 1..5 (5 = wildcard).
set -u
C=/home/eben/bolt-boot/ffmpeg/corpus
XP=/home/eben/bolt-boot/ffmpeg/x265-patches
INST=$XP/install-rebase-pgobolt
OUT=$C/results-threads-matrix.csv
PREF="--preset ultrafast --tune zerolatency --bframes 0 --rd 1 --limit-modes \
 --limit-refs 3 --no-rect --no-amp --aq-mode 0 --no-sao --ctu 16 --no-scenecut \
 --no-weightp --no-weightb --me dia --subme 1 --max-merge 2 --merange 8 \
 --no-info --bitrate 4000 --pools 4"
echo "video,ft,fps" > $OUT
SEQS="blue_sky_1080p25 crowd_run_1080p50 ducks_take_off_1080p50 in_to_tree_1080p50 old_town_cross_1080p50 park_joy_1080p50 pedestrian_area_1080p25 riverbed_1080p25 rush_hour_1080p25 station2_1080p25 sunflower_1080p25 tractor_1080p25"
for seq in $SEQS; do
  for ft in 1 2 3 4 5; do
    line=$(LD_LIBRARY_PATH=$INST/lib $INST/bin/x265 --input $C/$seq.y4m $PREF \
      --frame-threads $ft --output /tmp/tm.h265 2>&1 | grep -oE 'encoded [0-9]+ frames in [0-9.]+s \([0-9.]+ fps\)')
    fps=$(echo "$line" | grep -oE '\([0-9.]+ fps' | tr -d '( fps')
    echo "$seq,$ft,$fps" >> $OUT
    echo "TM $seq ft$ft fps=$fps"
  done
done
rm -f /tmp/tm.h265
echo "temp=$(vcgencmd measure_temp) throttled=$(vcgencmd get_throttled)"
echo THREADS-MATRIX-DONE
