# BOLT layer on top of x265 4.1 refactor

Post-link optimization (llvm-bolt-21) applied to the refactor's `libx265.so.215`.
Bit-exact at 1t 5 Mbps, 1t 2 Mbps, and 4-thread real-time configs.

## Artifacts

| file | what |
|---|---|
| `libx265.so.215.pre-bolt-input` | refactor .so **rebuilt with `-Wl,--emit-relocs`** (BOLT input) |
| `libx265.so.215.bolt-refactor` | BOLT-optimized output |
| `perf-combined.fdata`(`.gz`) | merged 1t + 4t perf profile fed to BOLT |

## Measured impact (perf stat, cycles:u)

Baseline is refactor tree at commit `4289aab` on the local `x265-4.1-refactor` repo.

|  config              | refactor cycles | BOLT cycles | Δ cycles |
|----------------------|-----------------|-------------|----------|
| 1t 5 Mbps LL         | 270,596 M       | 266,819 M   | **−1.40%** |
| 4t 5 Mbps realtime   | 285,661 M       | 279,654 M   | **−2.10%** |

Wall-time deltas follow cycles (~−1 to −2%).
Branch-misses drop 7–9 %; IPC lifts 1.36 → 1.38 (1t) and 1.28 → 1.31 (4t).

## Reproduction recipe

1. **Build the refactor with emit-relocs** (from `x265-4.1-refactor/`):
   ```
   cmake ../source \
     -DCMAKE_C_FLAGS="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76" \
     -DCMAKE_CXX_FLAGS="-O3 -fno-omit-frame-pointer -mcpu=cortex-a76" \
     -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--emit-relocs" \
     -DENABLE_ASSEMBLY=ON -DENABLE_SHARED=ON \
     -DCMAKE_INSTALL_PREFIX=/path/to/install-refactor-bolt
   ```
   Override the version tag before build for bit-exact reproducibility with the
   MD5 baseline: force `-DX265_VERSION=4.1+1-1d117be` on the `version.cpp.o`
   compile line (edit `common/CMakeFiles/common.dir/{flags,build}.make`).

2. **Capture profile** (1t 5 Mbps + 4t realtime, 90-second clip = 3× bbb_30s):
   ```
   perf record -e cycles:u -F 4000 -o perf-1t.data -- \
     x265 ... --frames 2700 --frame-threads 1 --pools none ...
   perf record -e cycles:u -F 4000 -o perf-4t.data -- \
     x265 ... --frames 2700 --frame-threads 1 --pools 4 ...
   ```

3. **Convert & merge**:
   ```
   perf2bolt -nl -p perf-1t.data -o perf-1t.fdata libx265.so.215
   perf2bolt -nl -p perf-4t.data -o perf-4t.fdata libx265.so.215
   merge-fdata perf-1t.fdata perf-4t.fdata > perf-combined.fdata
   ```

4. **BOLT**:
   ```
   llvm-bolt libx265.so.215 -o libx265.so.215.bolt \
     --data perf-combined.fdata \
     --reorder-blocks=ext-tsp \
     --reorder-functions=hfsort+ \
     --split-functions \
     --split-all-cold \
     --no-huge-pages
   ```

## Notes

- aarch64 has no LBR/SPE, so we run `perf2bolt -nl` (no-LBR mode).
  Wins are modest (~2%) versus 5–10% typical on x86 with LBR.
- `--no-huge-pages` is mandatory on the Pi5 boot path per prior BOLT experience,
  and keeps the .so aligned page-friendly for shared use.
- The MD5 baseline (`4.1+1-1d117be`) is the pre-`git init` version tag. Later
  builds after `git tag v4.1-refactor` embed a different SEI, breaking
  bytewise MD5 gate while decoded YUV remains bit-identical.
