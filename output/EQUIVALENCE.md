# 等价证明与性能数据

## 改造内容（纯增量，不改对外行为）

新增文件：
- `include/speex/speex_jitter.h` — 抖动缓冲公开 API（基于仓库 git 历史中的上游实现，新增 `JITTER_BUFFER_SET_MAX_DELAY`/`GET_MAX_DELAY`/`GET_DIAGNOSTICS`/`RESET_DIAGNOSTICS` 与 `JitterBufferDiagnostics`）
- `libspeex/jitter.c` — 通用抖动缓冲实现：按时间戳重排、丢包插值/静音由调用方选择、延迟可配且有硬上界、与解码器完全解耦（不 include `speex.h`/`speex_bits.h`，只操作 `JitterBufferPacket`）
- `libspeex/testjitter.c` — 验收测试（复用仓库自带编码器/解码器）

构建接线（无行为变化）：`libspeex/meson.build`、`libspeex/Makefile.am`、`include/speex/meson.build`、`include/speex/Makefile.am`、`libspeex/speex.def`、`win32/libspeex.def`。

既有源文件零改动（`git diff` 中无 `.c` 既有文件修改），因此对外行为不可能改变；以下用仓库自带测试入口 `testenc` 给出数据证明。

## 等价证明（仓库自带测试入口 testenc）

方法：同一输入 `output/input.sw`（确定性生成），分别用改造前（`git archive HEAD` 构建于 /tmp/speex-baseline）与改造后二进制运行 `testenc`，比较解码输出与编码码流。

结果（见 `output/equivalence_sha256.txt`）：
- 解码输出 `baseline_out.sw` == `after_out.sw`（SHA-256 一致，`cmp` 通过）
- 编码码流 `baseline_bits.dat` == `after_bits.dat`（SHA-256 一致，`cmp` 通过）
- 12.8MB 大输入（40000 帧）同样 BIT-EXACT
- 导出符号：`output/symbols_removed.txt` 为空（无删除/修改），`output/symbols_added.txt` 仅新增 11 个 `jitter_buffer_*` 符号；soname/版本不变

## 验收数据（`output/testjitter.log`）

场景 A（乱序深度 2 + 每 13 帧丢 1 包，margin=2 帧，bound=8 帧）：
- 乱序恢复：277/277 未丢包帧按原始顺序解码（A1 PASS）
- 丢包诊断：`lost_count=23` 与模拟丢包集合完全一致，`lost_ring` 按序给出丢失时间戳（A2 PASS）
- 乱序计数：`reordered_count=154` 与网络模拟精确一致（A5 PASS）
- 延迟上界：`max_buffered_observed=960 <= 1280`（A3 PASS），宽松上界无误杀（A4 PASS）

场景 B（2 倍速持续 overrun，bound=2 帧=320）：
- 上界严格成立：`max_buffered_observed=320 <= 320`（B1 PASS）
- 上界丢弃可诊断：`dropped_bound_count=148`，played=52，52+148=200 全帧可问责（B2/B3 PASS）

乱序与丢包同时出现时的明确行为：乱序包在 margin 内被吸收并计数（`reordered_count`），真丢包在播放点判定并记录时间戳（`lost_count`/`lost_ring`），二者独立计数、互不污染。

## 性能数据

testenc 12.8MB 输入（改造前 `output/perf_before.txt` / 改造后 `output/perf_after.txt`）：
- before: 5.98s / 3.92s / 5.75s，内存 2524-2584 KB
- after:  5.22s / 4.97s / 5.25s，内存 2412-2568 KB
- 结论：耗时与内存均在噪声范围内，无回归

抖动缓冲自身开销（`output/perf_jitter.txt`）：put+get+tick 约 672 ns/次，≈1.49 Mpackets/s，相对 20ms 帧实时富余约 3 个数量级。

## 录屏

- `output/recording.txt` — `script` 录制的完整验收过程（7 个分段标题）
- `output/recording.cast` — asciinema 版本（`asciinema play output/recording.cast`）
- 分段：1 基线构建 / 2 改造后构建 / 3 testenc 位精确等价 / 4 符号只增 / 5 抖动缓冲验收 / 6 性能数据 / 7 总结
