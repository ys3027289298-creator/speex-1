# 免提通话回声消除（AEC）改造报告

日期：2026-11-13 ｜ 仓库：xiph/speex @ 09b569d ｜ 环境：Ubuntu 24.04, gcc, meson 1.12.1

## 1. 需求与方案

| 需求 | 实现 |
|---|---|
| 自适应滤波估计回声路径 | 逐样本 NLMS 自适应 FIR（默认 1024 抽头 = 128 ms @ 8 kHz），步长 mu=0.5，功率归一化 |
| 双讲检测避免吃掉近端语音 | Geigel 检测器（max\|mic\| >= 0.5 * max\|far\| 判双讲）+ 12 帧 hangover，双讲期间冻结滤波器更新 |
| 与编解码链路解耦、可单独开关 | 独立模块 `speex_echo_*` API，不依赖编解码器任何内部状态；`SPEEX_ECHO_SET_ENABLED` 运行时开关 |
| 算法状态可保存与恢复 | `SPEEX_ECHO_SAVE_STATE` / `SPEEX_ECHO_RESTORE_STATE` 序列化全部算法状态（含魔数/版本校验） |
| 关闭时输出逐样本一致 | 关闭分支直接 `memcpy(rec -> out)`，不触碰任何算法状态 |

## 2. 结构改造内容（只增不改，对外行为不变）

新增文件：

- `include/speex/speex_echo.h` — 公共 API（4 个函数 + ctl 请求码）
- `libspeex/echo_cancel.c` — AEC 实现（NLMS + Geigel DTD + 状态序列化）
- `libspeex/testecho.c` — 验收测试（无外部输入文件，信号内置确定性生成）

修改文件（全部为追加式，不改动任何既有行）：

- `libspeex/meson.build` — 源码列表追加 `echo_cancel.c`；test-binaries 下追加 `testecho` 并注册 `meson test`
- `libspeex/Makefile.am` — 同上（autotools 侧，`make check` 跑 `testecho`）
- `include/speex/meson.build` / `include/speex/Makefile.am` — 安装新头文件
- `libspeex/speex.def` / `win32/libspeex.def` — 导出 4 个新符号

既有编解码源码 **零改动**（`git diff` 中无任何 `.c` 编解码文件变更）。

## 3. 改造前后等价证明

### 3.1 编解码输出逐比特一致

用仓库自带测试入口 `testenc` / `testenc_wb` / `testenc_uwb`，同一确定性输入
（`output/data/nb_8k.raw` / `wb_16k.raw` / `uwb_32k.raw`），分别跑基线构建
（`build-baseline/`，改造前源码）与新构建（`build/`），比较全部输出（解码 PCM + 码流）SHA-256：

```
testenc.out     ff1701... == ff1701...   (一致)
testenc.bits    b3161e... == b3161e...   (一致)
testenc_wb.*    一致
testenc_uwb.*   一致
```

完整哈希见 `output/baseline/SHA256SUMS` 与 `output/after/`。结论：**CODEC OUTPUT BIT-EXACT: PASS**。
另外用 autotools 构建（`./configure && make`）的 `testenc` 复跑，输出哈希与基线同样一致（跨构建系统验证）。

### 3.2 ABI 只增不减

`nm -D` 导出符号对比（`output/baseline/symbols.txt` vs `output/after/symbols.txt`）：

- 删除符号：0 个
- 新增符号：4 个（`speex_echo_state_init` / `speex_echo_state_destroy` / `speex_echo_cancellation` / `speex_echo_ctl`）

既有 57 个导出符号全部保留，存量 ABI 完全兼容。

### 3.3 性能无回退

60 秒 8 kHz 语音经 `testenc` 编码+解码，交替各跑 7 次取平均（`output/logs/perf_compare.log`）：

| 构建 | 平均耗时 | 峰值内存 |
|---|---|---|
| 改造前 | 0.326 s | ~2.6 MB |
| 改造后 | 0.324 s | ~2.5 MB |

差异在测量噪声内，编解码链路性能零回退（AEC 不在编解码路径上，符合解耦设计）。

## 4. 验收结果（仓库测试入口：`meson test` / `make check`，均为 PASS）

测试程序 `libspeex/testecho.c`：合成房间冲激响应（64 ms 指数衰减尾，能量归一化），
前 200 帧仅远端（回声场景），后 200 帧双讲（近端语音 + 远端回声）。原始输出见 `output/logs/testecho.log`。

| 验收项 | 指标 | 门限 | 实测 | 结果 |
|---|---|---|---|---|
| 回声抑制度 | ERLE = 10log10(P_mic/P_out) | >= 15 dB | **24.6 dB** | PASS |
| 双讲不失真 | 近端 SNR = 10log10(P_near/P_err) | >= 10 dB | **31.2 dB** | PASS |
| 状态可恢复 | 保存 8272 字节，换新实例恢复后续跑 30 帧 | 逐比特一致 | **bit-exact** | PASS |
| 关闭零差异 | 关闭后 out 与 rec 逐样本比较（含中途开关切换） | 完全一致 | **sample-exact** | PASS |

健壮性：损坏的状态块（魔数翻转）被 `RESTORE` 拒绝（返回 -1），不会崩溃。

## 5. AEC 自身性能

1024 抽头、160 样本/帧、8 kHz，单线程 -O2（`output/logs/perf_aec.log`）：

- **约 30x 实时**（600 秒音频 19.8 秒处理完），即单路免提通话约占 3% CPU。

## 6. 使用示例

```c
#include <speex/speex_echo.h>

SpeexEchoState *st = speex_echo_state_init(160, 1024);   /* 帧长, 滤波器长 */
int on = 1;
speex_echo_ctl(st, SPEEX_ECHO_SET_ENABLED, &on);          /* 可单独开关 */
speex_echo_cancellation(st, mic, far_end, out);           /* 每帧调用 */

spx_int32_t size;                                          /* 状态保存 */
speex_echo_ctl(st, SPEEX_ECHO_GET_STATE_SIZE, &size);
speex_echo_ctl(st, SPEEX_ECHO_SAVE_STATE, buf);           /* buf >= size */
speex_echo_ctl(st, SPEEX_ECHO_RESTORE_STATE, buf);        /* 恢复 */

speex_echo_state_destroy(st);
```

## 7. 复现步骤

```sh
meson setup build -Dtest-binaries=enabled -Dtools=disabled
ninja -C build
meson test -C build          # 或 ./build/libspeex/testecho
# autotools 交叉验证（需 libogg）：
./autogen.sh && ./configure && make -C libspeex check
```

## 8. 产物清单（output/）

- `REPORT.md` — 本报告
- `demo.cast` — 分段录屏（asciinema，含各段标题）
- `baseline/` `after/` — 改造前后测试输出与 SHA-256、导出符号表
- `logs/` — testecho / meson test / 性能对比原始日志
- `data/` — 确定性测试输入（nb/wb/uwb + 60 s 性能样本）
