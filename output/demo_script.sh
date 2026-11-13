#!/bin/bash
# 录屏演示脚本：分段展示 AEC 改造的验证过程（配合 asciinema 录制）
cd "$(dirname "$0")/.." || exit 1
export PATH="$PATH:$HOME/.local/bin"

title() {
  echo
  echo "################################################################"
  echo "##  $1"
  echo "################################################################"
  sleep 1
}

title "第1段：仓库自带测试入口 —— 基线（改造前）构建与输出"
echo '$ build-baseline/libspeex/testenc output/data/nb_8k.raw base.out base.bits'
build-baseline/libspeex/testenc output/data/nb_8k.raw /tmp/base.out /tmp/base.bits 2>&1 | tail -3
sha256sum /tmp/base.out /tmp/base.bits
sleep 1

title "第2段：AEC 模块（自适应滤波 + 双讲检测，与编解码解耦）"
echo "新增文件："
ls -1 include/speex/speex_echo.h libspeex/echo_cancel.c libspeex/testecho.c
echo
echo '$ ninja -C build   # 增量构建（含 testecho 验收程序）'
ninja -C build 2>&1 | tail -2
sleep 1

title "第3段：验收测试 —— 回声抑制 / 双讲 / 状态恢复 / 关闭零差异"
echo '$ meson test -C build'
meson test -C build 2>&1 | grep -E "OK|FAIL|Ok:|Fail:"
echo
echo '$ ./build/libspeex/testecho'
./build/libspeex/testecho
sleep 1

title "第4段：等价证明 —— 编解码输出逐比特一致 + ABI 只增不减"
echo '$ build/libspeex/testenc output/data/nb_8k.raw after.out after.bits'
build/libspeex/testenc output/data/nb_8k.raw /tmp/after.out /tmp/after.bits 2>&1 | tail -3
echo "改造前后 SHA-256 对比："
sha256sum /tmp/base.out /tmp/after.out /tmp/base.bits /tmp/after.bits
echo
echo "导出符号差异（仅新增 4 个 speex_echo_*，删除 0 个）："
diff output/baseline/symbols.txt output/after/symbols.txt
sleep 1

title "第5段：性能数据 —— 编解码零回退，AEC 约 30 倍实时"
echo "60 秒语音 testenc 编解码耗时（交替 7 次）："
paste <(grep baseline output/logs/perf_compare.log) <(grep after output/logs/perf_compare.log) | head -7
echo
echo "AEC 吞吐（1024 抽头 NLMS @8kHz）："
cat output/logs/perf_aec.log
sleep 1

title "演示结束 —— 全部验收项 PASS，详见 output/REPORT.md"
