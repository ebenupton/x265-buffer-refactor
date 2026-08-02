#!/usr/bin/env python3
"""Perceptually-motivated quality metrics beyond frame-average PSNR.

Usage: qmetrics.py <src.yuv> <rec.yuv> <width> <height>

Reports, on the luma plane:
  PSNR    frame-average PSNR (the usual scoreboard, for reference)
  TC      TEMPORAL CONSISTENCY: mean |(rec_t - rec_t-1) - (src_t - src_t-1)|.
          Flicker/shimmer that per-frame metrics are structurally blind to:
          every frame can score well individually while the sequence pumps.
  P5      5th-percentile 64x64-block PSNR. Humans notice the worst block,
          not the mean; frame averages hide localised failures.
  BAD     % of 64x64 blocks below 25 dB - a count of visible-failure blocks.
Streams frame by frame; never holds the sequence in memory.
"""
import sys, numpy as np

def main():
    src_p, rec_p, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    ysz, fsz = W * H, W * H * 3 // 2
    B = 64
    nby, nbx = H // B, W // B

    se_tot = 0.0          # global squared error (for PSNR)
    npix = 0
    tc_tot, tc_n = 0.0, 0
    blk_psnr = []

    with open(src_p, 'rb') as fs, open(rec_p, 'rb') as fr:
        prev_s = prev_r = None
        while True:
            bs, br = fs.read(fsz), fr.read(fsz)
            if len(bs) < fsz or len(br) < fsz:
                break
            s = np.frombuffer(bs[:ysz], np.uint8).reshape(H, W).astype(np.int16)
            r = np.frombuffer(br[:ysz], np.uint8).reshape(H, W).astype(np.int16)

            d = s - r
            se_tot += float(np.sum(d.astype(np.int64) ** 2))
            npix += ysz

            # temporal consistency: does the reconstruction move like the source?
            if prev_s is not None:
                tc = np.abs((r - prev_r) - (s - prev_s))
                tc_tot += float(tc.mean()); tc_n += 1

            # per-block PSNR over 64x64 blocks
            dd = d[:nby * B, :nbx * B].astype(np.float32)
            blocks = dd.reshape(nby, B, nbx, B).transpose(0, 2, 1, 3).reshape(-1, B * B)
            mse_b = np.maximum((blocks ** 2).mean(axis=1), 1e-6)
            blk_psnr.append(10.0 * np.log10(255.0 * 255.0 / mse_b))

            prev_s, prev_r = s, r

    if not npix:
        print("no frames"); return
    psnr = 10.0 * np.log10(255.0 * 255.0 / (se_tot / npix))
    allb = np.concatenate(blk_psnr)
    print(f"PSNR={psnr:.3f} TC={tc_tot / max(tc_n,1):.4f} "
          f"P5={np.percentile(allb, 5):.3f} BAD={100.0 * np.mean(allb < 25.0):.2f}")

main()
