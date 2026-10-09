# 独立 GBA 内核

本项目正在整体重写 GBA 模拟器。用户现指定的硬件研究目标为
`/home/tillsat/proto-core/soc-mlperf4`（`mlperf-tiny-4`）和
`/home/tillsat/proto-core/quad-issue-rvv-wide-peak`。下文已完成的 50 MHz 板测
原硬件基线使用 `soc` + `quad-issue-rvv`；现已完成最新核的私有 ZCU102 位流板测。
版本、内存路径与开关核对见 `build/gba-next/gameplay-board-20261009/HARDWARE.md`。
`src/gba-next/` 和 `include/gba-next/` 不使用 mGBA 的状态或设备回调。
旧 mGBA 只链接进差分测试作为参考。完整进度见 [process.md](../process.md)。

最新硬件的同一 50 MHz 实际关卡普通板测基线如下（缓存优化前，4500/120 完整帧，校验/采样关闭）：

| 关卡配置 | 原硬件 FPS | 最新硬件 FPS | 硬件加速比 |
| --- | ---: | ---: | ---: |
| 旧版，原有 HLE BIOS | 3.647 | 3.671 | 1.006 |
| 旧版，同一 ARM BIOS | 3.625 | 3.648 | 1.007 |
| 新版，标量 PPU | 4.908 | 4.968 | 1.012 |
| 新版，现有 RVV4 PPU | 5.276 | 5.360 | 1.016 |

随后针对最大 CPU 热点改进翻译缓存，默认入口由 8,192 增至 16,384，
关联度由四路改为八路，代码区保持 16 MiB。相同关卡的私有 Spike 诊断中，
120 帧编译次数由 8,322 降到 1,989（减少 76.10%），其中重复编译由
6,894 降到 561（减少 91.86%）。额外静态内存为 1.46875 MiB。
普通版实际上板 ELF 的 Spike 指令数为 722,720,869，原基线 774,400,793，
减少 6.67%；功能模拟的指令数不能换算实机 FPS。
最终候选通过 643,428 次差分状态比较，完整 4,620 帧的画面流、PCM、
CPU 和内存记录与 native 一致；默认 ELF/bin 与此候选逐字节一致。
最新同一 50 MHz 位流的普通关卡板测从 5.360 提高到 **6.333 FPS**，实测加速 1.1815 倍（18.15%）。周期 947,379,312，独立计时器 18,947,608 us，相互核对通过；最终 CPU/客体进度/音频生产计数匹配，AM exit 0。末状态 `BOOT UART`。

板卡重新连接后，先通过 JTAG 恢复与基线 hash 相同的最新位流，重新初始化 x16 PS DDR；64 MiB 地址位/端点读回以及核侧全空间地址模式、缓存写回、字节/半字和 DMA 检查均通过，再上传候选。恢复命令与日志 hash 见 board-restore.json，首次连接失败的证据仍保留在 board-attempt-1/。
证据见 `build/gba-next/cache-opt-20261009/REPORT.md`。

缓存优化前的最新 RVV4 关卡硬件 PC 采样中，最大自身分类为 **CPU 执行（生成代码/后端/解释器）
51.77%**；CPU 执行合计 51.77%，PPU
21.21%。共享总线/事件/设备工作另列；采样布局与 ISR 有
扰动，不能据此确认缓存/DDR 停顿原因。先诊断翻译缓存的描述符替换/组冲突和代码段回收，改善编译结果复用，再分析生成代码的执行开销；PPU 专用 RVV 的选择按其实际热点推进。
完整证据与限制见 `build/gba-next/latest-hardware-20261009/REPORT.md` 和 HOTSPOTS.md。
私有 RTL 小程序另复现新核向量/标量同时退休时 minstret 漏计；保留原计数，
该项 IPC/instret 不作精确工作量证据。独立计时器 FPS 和 PC 采样占比不依赖
minstret，证据与范围见 COUNTER-NOTE.md。
该硬件比较轮固定主频，仅比较硬件并诊断热点；随后的缓存优化见上文。

当前已具有独立 CPU、总线、设备、扫描线 PPU、音频、存档与可操作桌面前端。
龙珠验证到标题、剧情和关卡场景；整体重写仍在进行，未做完整通关或全 ROM
兼容验证。独立 RV32 原生后端已实现 ROM/RAM ARM/Thumb 块执行，具体范围和
开关见下节；支持跨访存驻留，以及同区域、同执行模式的直接分支、顺序后继
和已覆盖 Thumb 间接目标的块衔接。
纯解释器已完成 Spike / FPGA
开场基线：50 MHz、30 帧预热 / 120 帧测量为 2.059 FPS。此后加入了事件边界
批执行和 Thumb 跳转表分派；相同开场窗口的 Spike 宿主指令数减少约 30.3%，
关卡 4500/120 窗口减少约 16.5%。同一 50 MHz FPGA 上，优化后的开场实测
**3.289 FPS**，画面、音频和最终 CPU/内存校验一致；该阶段尚未进行关卡板测。
此后原生后端已有一份未及时写入旧记录的板测：同一 50 MHz、开场 30/120
为 **4.781 FPS**，见 `build/gba-next/fpga-current/gba-next-fpga.log`。仍低于
旧 mGBA/RV32 的龙珠 16.085 FPS。2026-10-09 恢复相同 50 MHz 配置后，
当前生产版开场默认标量实测 **7.243 FPS**，关卡显式 RVV4 实测
**4.644 FPS**。两项十条完整输出均与 native/Spike 一致；关卡完整回放
4,620 帧并保留声音与全帧校验。证据在 `build/gba-next/goal30/board-20261009/`；
开场比此前 4.781 FPS 提高约 51.5%，50 MHz / 30 FPS 目标尚未完成。
随后按用户要求新增 `GBN_VALIDATE=0`，关闭 CRC/PCM 检查开销，同一开场
实测 **8.775 FPS**（比完整校验口径提高 21.16%）；
完整渲染和音频模拟仍开启。随后已完成实际关卡四组配对板测，均为 50 MHz、
4500/120 完整帧、相同输入、新 RAM 存档、计时内校验关闭：旧版原有 HLE
**3.647 FPS**，旧版同一 ARM BIOS
**3.625 FPS**，新版标量
**4.908 FPS**，新版 RVV4
**5.276 FPS**。这是实际板端计时，完整原始证据在
`build/gba-next/gameplay-board-20261009/`。实机关卡也支持继续新版，保留旧版作参考。

2026-10-09 新旧架构审计支持继续独立内核：统一 BIOS、输入并排除旧前端
启动半帧后，关卡 4500/120 的 Spike 宿主退休指令旧版为 **1,474,556,990**，
新版关闭校验的标量版为 **864,391,339**，减少 **41.38%**。native 单独核对
计时 120 帧的画面流 CRC 均为 `7ec20646`，末帧、R0–R14/CPSR 与显示内存
CRC 相同；EWRAM/IWRAM 仍有差异，不能称为全机器逐周期等价或实机 FPS 胜出。
旧版开场可复用 89.17% 扫描线，并有更宽的跨块等待合并；对齐关卡扫描线
复用为零。因此开场 16.085 FPS 不能充当关卡基准。后续主线是关卡 CPU/
事件调度与 PPU；保留旧版作参考。证据与限制见
`build/gba-next/architecture-audit/REPORT.md`。该审计轮未修改核心或恢复暂停的目标；后续关卡配对板测结果见上文。

## 独立 RV32 原生执行（实验性，默认关闭）

`GBN_RV32=1` 为独立 benchmark 或 AM 窗口播放器启用新后端，与旧 mGBA 的 `RV32_RUNNER`
分开。第一次遇到 ROM/RAM ARM/Thumb 入口时生成 RV32 机器码，后续直接执行
缓存。低八个客户机寄存器、CPSR、客体时间和预取状态跨块保留在宿主
寄存器中，普通 RAM 访存不再结束块。

Thumb 支持常见算术/逻辑/立即数移位、ADC/SBC/MUL、高寄存器操作、直接分支、
ROM 常量池读取，以及对齐 EWRAM/IWRAM 单次读写。ARM 支持条件 ALU、MUL/MLA、
立即数及立即数控制的移位操作数、B/BL、单次字/半字/字节和有符号读写、
前索引写回和立即数后索引。普通 ROM 数据也可直接读取，保存端口回退。
同区域、同 ARM/Thumb 模式下，缓存命中的直接分支和顺序后继块可直接执行
下一块的机器码，共用一份寄存器和栈帧；全部指令与事件检查仍然执行，
普通路径不跳过客体指令。只读 Thumb 自循环可在所有被写寄存器、CPSR 和
预取状态达到不动点后合并完整重复，精确保留客体指令数和周期，且不越过
下一事件或指令预算。Thumb BL 的两半分别提交 LR，保留中间的事件和指令
预算边界；只有已在同一原生块执行的相邻前半条才能证明静态调用目标。
独立进入的后半条使用当前 LR；同区域、保持 Thumb 模式的 BX 使用运行时
目标。动态后继核对现有缓存的 PC、ROM 版本、映射和等待/模式键，再进入
目标块的 RAM 字节快照检查。缓存未命中、跨模式及跨区域仍经 C 或解释器处理。

Thumb 非空 PUSH/POP 可直接访问 EWRAM/IWRAM，普通栈操作继续保留低寄存器
驻留，PUSH 可包含 LR；同区域 POP PC 完成原生访问后亦可衔接缓存目标。按多字
传输的总线等待一次结算预取，保留 SP 低位和数据字对齐语义。空列表、
未对齐宿主 RAM、跨 RAM 镜像末端、跨区域或无效返回目标完整回退。

VCOUNT/DISPSTAT 等无读副作用的存储型 IO 支持原生读取；动态/有读副作用的
IO、未对齐/未映射访问、BIOS 及其他未覆盖编码走独立解释器。
DMA 计数读取返回零、计数/控制字读取实时组合零与控制半字；四个 DMA 通道
源/目标地址的对齐字写入直接更新带掩码的地址及 IO 镜像，保留正在进行的
传输游标、计数与事件。源/目标寄存器及只写显示寄存器的开放总线读取已原生化，按半字复制到
字的两个通道，保留 DMA 锁存选择、字节/有符号规则和活的 RAM 取指；缓存
入口若遇到旧预取的 IWRAM 尾部，会在状态提交前回退。计数/控制写入继续
使用设备路径；DMA 传输仍按原有总线拍和事件边界执行。
CPU 暂停期间，只有一个 DMA 通道待处理时，可连续执行普通内存传输，
省去中间不可观察的事件队列操作。首拍仍走完整路径；连续段在其他事件、
内存映射边界或未覆盖访问前结束。每拍先读后写，保留重叠、固定源、递减、
等待周期、总线锁存和两周期释放；IO、存档及未对齐宿主存储使用原路径。
没有新增缓存或设备状态。六项完整工作量输出一致，关卡指令减少约
12.80%–13.67%；同模型连续内存场景周期明显减少，但短 IO 搬运仍增加
16.33% 周期。这是已保留的局部代价，不代表所有 DMA 都更快，也不是游戏
FPGA FPS。原有核心差分与新增 1,328 组/5,182 个状态检查点通过，详见
`build/gba-next/goal30/dma-span/` 和 process.md。

RAM 代码在 C 入口及原生链入口均核对字节快照，写入本块及预取范围时回退，以保留
自修改及 DMA 更新后的旧预取语义。RAM 常量池动态读取，不作不可变折叠。每条指令前检查 cap
和事件，慢访问先提交完成的前缀，再从失败指令回退。

缓存由调用者提供：默认 16,384 个入口、八路组相联和一层快速索引，默认共享 16 MiB
可执行空间，按实际代码长度存放。可用 `GBN_RV32_SLOTS` / `GBN_RV32_WAYS`
分别调整入口数和关联度；均须为二的幂，关联度不大于入口数或 256。
C 查找和生成代码的间接边共用哈希宽度。`GBN_RV32_CODE_MIB=4/8/16/32`
单独控制代码区容量，较大的配置须满足目标 RAM 与链接器保留堆的要求。
空间分为 32 段，默认每段 512 KiB，回收时只失效被覆盖段中的入口，其他译码
保留。另有 RAM 快照和索引，默认 RV32 后端合计 19,863,808 字节，约 18.94 MiB。
需要旧内存配置时可显式设置 `GBN_RV32_SLOTS=8192 GBN_RV32_WAYS=4`。
每条跨块边检查目标 PC、ROM 版本、映射与等待状态；代码回收无需回写所有
来源块。ROM 在两次 `gbn_attach_rom` 之间必须只读；后端活跃时不能移动其
存储地址，重新初始化机器后也须重新初始化后端。
生成代码后执行 `fence.i`；宿主需完整 RV32 寄存器集、乘法扩展和 Zifencei，RV32E
及桌面 native 不执行生成代码。桌面诊断前端仍使用解释器。

```sh
make -j6 PLATFORM=spike BUILD_DIR=build/gba-next/rv32-test \
  GBN_RV32_STATS=1 test-gba-next-rv32
make -j6 PLATFORM=spike BUILD_DIR=build/gba-next/rv32-bench \
  GBN_RV32=1 GBN_WARMUP=30 GBN_FRAMES=120 \
  GBN_INPUT=am/test/data/gba-next-dragonball-play.input run-gba-next-bench
```

`GBN_RV32_STATS=1` 另开入口、回退和宿主工作分布诊断，性能对照关闭该项。
当前 RV32IM 与 Zve32x Spike 已实际执行生成代码，最新两者回归均通过 **610,660 次**状态
比较，覆盖多块驻留、逐边界事件/指令上限、未对齐宿主 RAM、跨块代码改写、
缓存冲突和分段回收，以及 IO 原生读取、不动点循环合并、BL 两半边界、
BX/POP 动态目标和描述符 PC/模式校验。最新 PPU 在 native、
RV32IM 和 Zve32x 通过 **28,358,640 次**像素比较，含窗口边界、恒等颜色方程、
透明行、纯色行、宿主视频存储对齐组合及动态调色板修改。以下表格为原生链阶段的历史结果；最新数值见 process.md。
各完整画面、PCM、CPU/内存及客户机时间仍与独立解释器参考一致：

| 场景（预热/测量帧） | 前版（已有批量 PPU） | 原生链 + 缓存改造 | 本轮减少 |
| --- | ---: | ---: | ---: |
| 龙珠开场 30/120 | 1,541,276,079 | 1,378,622,805 | 10.55% |
| 龙珠关卡 4500/120 | 3,389,518,092 | 2,431,241,468 | 28.27% |
| go 5/30 | 338,695,346 | 257,528,513 | 23.96% |

这些是关闭诊断后的 Spike 宿主退休指令数，不能换算实机 FPS 或当作超过
旧 mGBA/RV32 的证据。前版关卡分项显示译码编译本身占 CPU 工作量一半以上，
因此本轮同时改了块衔接与缓存保留；仅增加块衔接不足以保证关卡收益。
随后将入口/代码空间由 4,096/2 MiB 增至 8,192/4 MiB，关卡在前一候选
基础上又减少 15.82% 指令；代价是额外约 2.74 MiB 静态内存。
交互窗口、按键和 PCM 回归也已通过。最终归档在 `build/gba-next/cpu-dispatch/capacity/`，父目录保留 2 MiB 对照；
进度与后续方向见 [process.md](../process.md)。本阶段没有上板。

最新容量对照保持相同文本 PPU 内核、输入和测量窗口，关闭采样与生成代码
统计后，关卡 4500/120 从 4 MiB 的 1,621,435,370 条减少到 8 MiB 的
1,528,533,203 和默认 16 MiB 的 1,501,110,682（减少 7.42%）。代价是相对
4 MiB 版增加 12 MiB 静态内存；开场和 go 工作量基本不变。各完整输出一致，
见 `build/gba-next/goal30/arena-capacity/`，仍无此候选的 FPGA 帧率。

背景绘制现按扫描线预解码，文字背景以图块行为单位读取，仿射/位图背景
递增坐标，背景优先级每行排序后选择最近两个可见层。保留窗口、透明、
翻转、滚动、马赛克和混色语义；临时行缓冲增加约 2 KiB 栈，无跨行缓存。
此前这轮没有使用 RVV，详见 `build/gba-next/ppu-batch/report.md`。

CPU 本轮改造前的关卡诊断中，CPU 占 46.78%、PPU 34.37%、DMA 10.68%。PPU 内窗口/
合成/混色占整机 20.98%，文字背景解码 9.94%。跨块衔接本轮已实现，
这些旧份额不再代表最新 CPU 分布。比例来自指令量诊断，不能当成 FPGA 耗时份额
或向量加速倍率。改前数据保留在 `build/gba-next/device-profile/hotspots.md`。

最新 4 MiB 缓存版的关卡诊断中，CPU 占 26.76%，事件/设备占 68.22%，
其中编译占整机约 6.89%。CPU 路径的指令量比本轮改前减少 59.76%，编译
减少 80.29%；随后已继续优化 PPU、DMA 和事件选择。这些含插桩数据与
下方关闭诊断后的整机收益分开解释。

## 当前设备与合成路径优化

最新开放总线读取改动使关卡宿主指令减少 1.958%（标量）/
2.129%（显式 RVV4），分别为 **968,905,111** /
**889,436,188**。开场和 go 各约增加 0.15%，保留这一小幅代价。
六项完整输出一致，CPU 实际 RV32IM/Zve32x 各通过 610,660 次状态比较。
前两版在新增旧预取回归中失败，只有带保护的第三版保留。共享原生叶例程
不清空驻留状态、不增加缓存/栈帧；RAM 未覆盖尾部保持实时读取。同一 RTL
三个新 IO 场景周期变化为 -89.536% /
-94.174% / -87.549%，
这是微基准周期，不是游戏 FPS。证据在 `build/gba-next/goal30/slow-io-exits/`。
FPGA 镜像未上传，50 MHz / 30 FPS 尚未达到。

最新 CPU 调用/返回改动使同一龙珠关卡的宿主指令由 **1,021,635,984** 降至
**988,255,332**（标量），显式 RVV4 由 **942,167,061** 降至 **908,786,409**，
减少 3.27%/3.54%；开场减少约 0.37%，go 不变，六项完整工作量输出一致。
私有退出统计证实该关卡原生 POP PC 退出为零，优化依据是 BL/BX 与块分派
成本；POP 的改善来自共享动态后继实现，不将其作为游戏收益来源。
同模型十项 CPU RTL 的状态 hash 相同；调用和动态返回场景周期减少
85.40%–89.57%、POP PC 减少 62.80%，普通七项的最大退步为 0.026%。
这些是微基准周期，不是游戏 FPS。新采样中生成代码及管理约 34.64%、
PPU 约 27.97%，文字解码合计 9.75%。代码缓存和机器状态不增容；FPGA
默认标量 text 增加 2,652 字节，BSS 不变。证据在
`build/gba-next/goal30/indirect-return/`。当前软件尚无新的板测 FPS，
50 MHz / 30 FPS 仍未达到。

最新 OBJ 绘制在扫描线入口一次选择 OAM/调色板的对齐路径。两者均按
半字对齐时，属性、矩阵和颜色直接使用合法半字读取；其他宿主对齐走安全
字节路径。不增加缓存或设备状态，保留像素预算、优先级、翻转、仿射、
马赛克、OBJ 窗口和半透明规则。相同关卡宿主指令由 1,053,173,748 降至
**1,021,635,984**（标量），显式 RVV4 由 973,704,825 降至 **942,167,061**，
分别减少 2.99%/3.24%；开场和 go 不变，六项完整输出一致。
2,835 万次像素检查包含强化后的仿射 OBJ 宿主对齐组合。同 RTL 五个带
精灵场景周期减少 6.75%–19.96%，单 BG 增加 0.76%、空白减少 0.11%；
不代表所有场景更快。最新采样中生成代码及 rv32.c 约 34.83%、PPU 约
27.00%，文字解码合计 9.77%、OBJ 7.17%；这是指令份额。证据在
`build/gba-next/goal30/obj-halfwords/`。正式镜像与实测候选逐字节一致，
FPGA 默认标量构建 text 增加 1,800 字节、BSS 不变，尚未上传或取得新 FPS。

最新索引文字背景解码将首尾裁剪与完整图块分开，按地图连续行递增指针，
在 256 像素屏块边界重新计算映射；四字节对齐的图块行直接读一个字，其他
对齐仍合法。没有增加跨行缓存或设备状态。相对 DMA 连续处理版，普通
关卡宿主指令由 1,089,150,289 降至 **1,053,173,748**（标量），显式 RVV4
由 1,009,681,366 降至 **973,704,825**，分别减少 3.30%/3.56%。六项完整
工作量输出一致，native、RV32IM 和 RVV4 各通过 **28,358,640** 次像素比较，
包含新增的全部水平滚动值、地图回绕及四种 VRAM 对齐检查。
同模型五个非空白、无 alpha 场景周期减少 13.08%–15.02%，alpha 增加
1.24%、空白增加 0.30%；不能称所有模式都更快。新采样中 PPU 约 31.37%，
文字解码合计 10.16%，生成代码 24.02%、rv32.c 8.14%；这些是 Spike 指令
份额。正式标量/RVV4 关卡 ELF 和 FPGA 镜像与已测候选逐字节一致；证据在
`build/gba-next/goal30/text-map-walk/`。最新软件仍无板测 FPS，50 MHz /
30 FPS 未达到。完整 sanitizer 的旧参考移位告警及限定复查见 process.md，
不把带告警的原始运行称为干净通过。

`src/gba-next/ppu.c` 现在按扫描线预计算窗口掩码，混合寄存器只读取一次，
最多四个背景层的透明/优先级选择展开；没有窗口和混合目标时，直接选最前的
非透明背景或 OBJ。OBJ 扫描把 DISPCNT、mosaic 和一维布局寄存器提升到扫描线
级别。`src/gba-next/dma.c` 对齐 DMA 半字/字对 EWRAM、IWRAM、VRAM、Palette、
OAM 和有效 ROM 直接读写，IO、存档、开放总线和无效 ROM 仍走完整路径。
`src/gba-next/core.c` 保留缓存获胜者及独立的有序活动链，取消不再重扫 16 项表；
相等时间/优先级的历史获胜者规则保持不变。
这些路径对所有 ROM 通用，没有 ROM 地址或画面特判。

原生与 RV32IM/Spike PPU 回归通过 3,571,200 次像素比较；核心差分通过
1,036,279 次、DMA 对照通过 1,634 次。龙珠关卡固定工作量（预热 4,500、
测量 120 帧）从 4 MiB 缓存版的 2,431,241,468 条 Spike 退休指令降到
2,273,542,264，减少 6.49%；`go` 的 5/30 窗口从 257,528,513 降到
218,063,605，减少 15.32%。Zve32x Spike 同窗口为 2,283,631,643，输出
CRC、PCM、寄存器和内存与 native 一致。完整证据在
`build/gba-next/ppu-compose/verification-final.json` 和
`verification-zve32x-final.json`。这些是功能模拟指令量，不能换算 FPGA
FPS；本轮仍未使用 RVV，也没有新的板测结果。

最新候选进一步将代码空间改为默认 16 MiB，并加入上述 DMA 寄存器原生
访问。同一文本 PPU、关闭诊断的关卡 4500/120 从 1,621,435,370 条减少到
**1,456,387,098**（-10.18%）；开场 811,915,924、go 140,774,463，go 比原基线
增加 0.22%。三种工作量完整输出和客户机时间一致，证据在
`build/gba-next/goal30/dma-register-native/`。这些仍是 Spike 指令工作量，
50 MHz / 30 FPS 尚未达到，最新软件候选没有实机帧率。

随后标量 PPU 按连续相同窗口权限区间预筛背景，再分别处理 0–4 层及有/无
OBJ；先找最前非透明背景，再与 OBJ 比较一次优先级。区间处理独立为函数，
避免干扰通用混色循环的编译布局。默认关卡进一步降至 **1,419,074,195**
（-2.56%），开场 809,023,635、go 135,316,863，三项完整输出一致。native、
RV32IM、Zve32x 各通过 18,727,920 次像素比较。同 RTL 模型四个非空白场景
周期改善 1.13%–8.00%，空白 +0.41%；证据在
`build/gba-next/goal30/compose-spans-v3/`。512 KiB 图块行缓存及前两版区间
实验未合入；本轮仍无新的板测 FPS。

随后保留完整八像素 4bpp 行展开与透明后缀一次填充，关卡进一步降至
**1,397,655,934**（-1.51%），开场 807,150,190，go 135,316,863。三项完整
输出一致，三种像素回归各通过 18,727,920 次比较；同模型 RTL 非空白周期
改善 2.39%–13.37%。证据及未上传 FPGA 镜像在
`build/gba-next/goal30/row8-tail/`。调度器随后独立验收，再测量实际组合版本。

随后保留有序活动事件表 v2，并与新 PPU 组合完成三种完整工作量：关卡
**1,330,252,430**（相对上版 -4.82%），开场 770,917,516，go 134,381,672。
同模型 RTL 周期事件场景改善 34.22%–50.93%，但反复更新非头事件的场景
仍退步 259.80%；这是明确保留的取舍。原 RV32IM/Zve32x 各 346,288 次
状态回归通过，旧全扫描 oracle 的 6,284,878 次比较已加入核心回归。
该阶段证据及未上传镜像在 `build/gba-next/goal30/event-list-v2/`，仍无新实机
FPS，50 MHz / 30 FPS 尚未达到。

最新 Thumb 生成器直接使用驻留的低八个客户机寄存器，减少临时操作数搬运，
预算、事件、标志位及时间语义不变。关卡为 **1,327,992,768** 条（-0.170%），
开场 768,556,319（-0.306%），go 134,381,672（不变）；三种完整输出与 native
一致，RV32IM/Zve32x 各 346,288 次状态回归通过。同 RTL 的暖 CPU 算术/内存
微基准周期减少 7.63%/1.41%，不是游戏 FPS。证据、生产源码快照及最新未
上传镜像在 `build/gba-next/goal30/direct-operands/`。

操作数改动前的关卡重新采样显示生成代码约占 18.31%，已识别 PPU 约三分之一。
另一个私有用途标签探针将 1,223 个生成样本中的 1,208 个分类，15 个丢失：
预算/事件检查、时间/预取维护、进出块等管理工作共占全部生成样本 54.21%。
此诊断有额外编译与标签开销，不能当作生产时间或全部可消除的冗余。
采样证据分别在 `build/gba-next/goal30/list-profile/` 与 `jit-stage-probe/`。
当前尚无新板测，50 MHz / 30 FPS 仍未达到。

随后保留无有效混色、非马赛克 4bpp mode 0 的索引行合成：先保存单字节的
调色板组/texel，完成窗口和 BG/OBJ 优先级选择后才读取最终 RGB555。完整
未翻转图块行按字节通道展开，用两个内部对齐字存储写入；其他路径保留。
v1 关卡为 **1,312,601,272** 条（-1.159%），开场 765,290,199（-0.425%），
go 134,666,264（+0.212%）。三项完整输出一致；扩展后的 native、RV32IM、
Zve32x RVV=1/3 各通过 20,494,320 次像素比较，包含新路径的视频内存对齐组合。
七场景 RTL 输出一致，非混色场景周期改善约 15%–23%，混色增加 1.99%，
空白基本不变。证据及未上传镜像在 `build/gba-next/goal30/indexed-words/`；
直接绘制两版及单纯字节索引版关卡退步，未保留。该阶段没有新板测 FPS。

当前保留入口检查 v2，准入失败的行在建立辅助栈帧之前走通用绘图：关卡
**1,312,658,872** 条（相对操作数版 -1.155%），开场 764,660,559（-0.507%），
go 134,435,864（+0.040%）。四种像素回归各 20,494,320 次比较通过；同 RTL
非空白、非混色场景周期减少约 15%–23%，混色仍增加 1.56%。证据及最新
未上传镜像在 `build/gba-next/goal30/indexed-words-v2/`。该候选重新采样
6,474 点、无丢失，PPU 按源文件占 30.11%，生成代码占 18.61%；这些是功能
Spike 指令份额，不是硬件时间。50 MHz / 30 FPS 仍未达到，没有新板测 FPS。


最新 Thumb 栈操作原生化后，普通关卡为 **1,259,740,219** 条（相对索引入口
v2 减少 4.031%），开场 760,986,307（-0.481%），go 134,435,864（不变）。
完整输出一致，标量与 Zve32x 各通过扩展后的 370,844 次状态比较。八场景
同模型 RTL hash 一致，栈操作周期改善 77.53%–94.10%，旧四项 CPU 对照
基本不变；这不是游戏 FPS。最新普通生成代码回退探针中，ROM Thumb 的
PUSH/POP 回退均为零。新 6,215 点整体采样显示 PPU 31.76%、生成代码
19.42%、DMA 11.02%，不是硬件时间份额。证据、源码和最新未上传镜像在
`build/gba-next/goal30/thumb-stack/`，50 MHz / 30 FPS 仍未达到。

## 构建和验证

CPU 后端现在将 N/Z 分别保存在宿主寄存器中，避免每次设置标志时重复拼装
CPSR；在原生退出及固定点循环证明处合成完整状态，逐指令事件与预算语义不变。
同一关卡普通指令量，默认标量从 1,259,740,219 降到 **1,249,069,442**，
显式 RVV4 从 1,180,271,296 降到 **1,169,600,519**；三种工作量输出一致。
同模型八项暖 CPU RTL 对照七项周期改善、POP PC 短块退步 1.786%，hash
全部相同且正常退出。完整证据见 `build/gba-next/goal30/resident-nz/`。
仍没有当前镜像的 FPGA FPS，50 MHz / 30 FPS 尚未达到。

已保留 `GBN_PPU_RVV=4` 的索引区间合成选项：先批量选择背景索引和优先级，
再读取最终颜色并按连续字节写出。它使用 e8,m4 / e16,m8，按实际 VL 处理
尾部；在 VLEN=128 时每批最多 64 个像素。未对齐调色板仍使用标量实现。
普通 C 与 LTO 使用标量 ISA，仅 `ppu-rvv.c` 按运行目标的向量 ISA 编译：

```sh
make -j6 PLATFORM=spike_zve32x BUILD_DIR=build/gba-next/vector-index \
  RISCV_C_ISA=rv32im_zicsr_zifencei_zicbom \
  NEWLIB_ISA=rv32im_zicsr_zifencei_zicbom GBN_PPU_RVV=4 GBN_RV32=1 \
  GBN_WARMUP=4500 GBN_FRAMES=120 \
  GBN_INPUT=am/test/data/gba-next-dragonball-play.input run-gba-next-bench
```

Spike 的 `spike_zve32x` 默认 ISA 已显式包含 `_zvl128b`，匹配目标 VLEN=128。
仅声明 Zve32x 会使当前 Spike 使用 VLEN=32；本文此前未显式指定宽度的
历史向量指令数保留为原始 32 位结果，不能直接当作 128 位结果。
FPGA 构建另外指定 `RISCV_ISA=rv32im_zicsr_zifencei_zicbom_zve32x`。
`RISCV_C_ISA` 默认等于 `RISCV_ISA`；上述显式配置避免普通 C 循环被自动
向量化后增加本核周期。`GBN_PPU_RVV` 默认仍为 0，默认标量关卡 ELF 与
改动前逐字节一致，不能把可选路径的成绩记作默认配置的成绩。

新选项的关卡 4500/120 为 **1,180,271,296** 条宿主指令，比标量
1,259,740,219 减少 **6.31%**；开场减少 0.75%，go 不变，完整输出一致。
同模型七场景 RTL 输出均一致，适用的非混色场景周期减少 **3.37%–33.82%**。
正式 RVV=4、旧 RVV=1/3 各通过 20,494,320 次像素比较，新编译策略下 CPU
后端通过 370,844 次状态比较。正式关卡 ELF 和 FPGA 镜像与已测候选一致，
证据在 `build/gba-next/goal30/indexed-vector-explicit/`。
板端小程序尝试未获 Boot ROM 握手，尚未上传应用，没有新增硬件 FPS。
50 MHz / 30 FPS 仍未达到。

新关卡采样有 5,821 点、无丢失，完整输出通过对照。PPU（含向量内核）
占 27.14%，生成代码与翻译/分派 C 合计至少 28.65%；core.c 仍混有 CPU
和事件，未全归入 CPU。文字背景解码是最大独立 C 函数，约 10.22%。
这些是 Spike 指令份额，向量访存的多周期成本须另看 RTL/板测；下一轮
据此优先分析原生代码管理和剩余回退，继续比较文字解码候选。

```sh
make -j6 PLATFORM=native BUILD_DIR=build/gba-next/native test-gba-next
make -j6 PLATFORM=native BUILD_DIR=build/gba-next/native test-gba-next-ppu
make -j6 PLATFORM=native BUILD_DIR=build/gba-next/native-555 CPPFLAGS=-DCOLOR_16_BIT test-gba-next-ppu
make -j6 PLATFORM=native BUILD_DIR=build/gba-next/native build/gba-next/native/gba-next-run
make -j6 PLATFORM=spike BUILD_DIR=build/gba-next/spike SPIKE_PLUGIN=build/rv32/spike/protosoc.so test-gba-next
python3 am/tools/gba_next_bios.py --check
```

独立固件由 `src/gba-next/bios.s` 生成，修改后运行 `make regen-gba-next-bios`。
所有已实现的服务通过客户机 ARM 指令执行，允许正常 IRQ/DMA 介入；周期
不保证与原厂固件相同。未实现服务在 `PC=00000020` 明确停止。

## 诊断前端

```sh
build/gba-next/native/gba-next-run --steps 500000000 --cycles 1600000000 \
  --frames 5000 --input am/test/data/gba-next-dragonball-play.input \
  --frame build/gba-next/capture.ppm --wav build/gba-next/capture.wav roms/dragonball.gba
```

`--steps` 限制客户机指令数；`--cycles` 限制客户机总线时间，默认约 64 秒
客户机时间，也约束长时间 HALT。到达周期预算返回码为 2，诊断中显示
`stop=deadline`。`--bios FILE` 可装载用户自有的 16 KiB BIOS。

`--frames` 限制完整渲染帧数，`--frame FILE.ppm` 导出最后一个完整帧。
跳过 BIOS 后的首个局部帧不计为完整帧。日志分别报告 LCD VBlank 数和
`rendered_frames`，不将两者混用。

输入脚本每行是“十进制 VBlank 计数 十六进制按键掩码”，按计数递增，
同一计数可出现多行，最多 4096 条。支持空行和 # 注释。按下位：
A=001、B=002、Select=004、Start=008、右=010、左=020、上=040、下=080、
R=100、L=200。掩码一直保持到下一条记录；0 表示全部松开。
仓库中的龙珠脚本是功能测试输入，生产模拟器没有游戏地址或流程特判。

音频捕获为 32768 Hz、双声道、16 位 PCM WAV。内核音频采样率可由
SOUNDBIAS 改变，前端依据时间戳重采样。即使没有指定 WAV 也消费音频缓冲。

默认存档仅保存在内存中，不读取或写入 ROM 旁边已有的 `.sav`。
只有显式传入 `--save FILE` 才加载和保存该文件；写入使用独占临时文件
并重命名。验证应在 `build/` 下指定新的测试存档，不使用原游戏存档。

## Spike 窗口和声音

```sh
make -j6 PLATFORM=spike GBN_RV32=1 run-gba-next
```

默认加载 `roms/dragonball.gba`，其他游戏只需 `ROM=go`；也可以使用
`PLATFORM=spike_zve32x`。`src/platform/gba-next/player.c` 将独立 CPU/PPU/APU
接到已有 AM 媒体设备，240×160 画面默认放大为 720×480 的 SDL 窗口。
GBA 模拟、RV32 翻译块和像素转换都在 Spike 中执行；主机只显示并播放 PCM。

方向键移动，Z=A，X=B，A=L，S=R，Enter=Start，Backspace=Select，Esc
或关闭窗口退出。音频按客体时间戳重采样到设备实际协商的频率，显示按
主机时间限速；Spike 跑不满速时会慢放，运行观感不是 FPGA 帧率。
`GBN_PLAYER_AUDIO=0` 关闭声音输出（仍模拟并消费 APU）；
`GBN_PLAYER_FRAMES=N` 限制完整渲染帧数，默认 0 一直运行。
当前 AM 入口存档只保存在 RAM，退出不会写入 `.sav`。

`run-gba-next-bench` 继续提供无窗口的固定工作量对照；普通 `run` 仍是旧
mGBA 播放器。`PLATFORM=native run-gba-next-player` 可用相同 AM 前端运行解释器，
桌面平台不要设置 `GBN_RV32=1`。

交互验收复用已有 SDL 探针和自编 GBA 按键/音频 ROM，比较 native 与
Spike 的实际显示像素，检查按键按下/松开、非零 PCM、静音、帧数限制和
窗口关闭。`--video-driver x11` 可在桌面上实际打开测试窗口：

```sh
python3 am/test/gba-next-player.py --build-dir build/gba-next/player-test
```

## 桌面诊断窗口和声音

```sh
make -j6 PLATFORM=native BUILD_DIR=build/gba-next/desktop GBN_DISPLAY=1 build/gba-next/desktop/gba-next-run
build/gba-next/desktop/gba-next-run --display roms/dragonball.gba
```

方向键移动，Z=A，X=B，Enter=Start，Backspace=Select，A=L，S=R，Esc 退出。
显示按 GBA 时间节奏运行，PCM 经 SDL 队列播放。桌面运行速度不代表 RISC-V
硬件性能。`--display` 默认持续到窗口关闭，仍可指定指令、周期和帧预算。
存档仍需显式 `--save FILE`。

## 同一工作量的 Spike / FPGA 测量

`gba-next-bench` 是独立核心的固定工作量前端，仅链接 `libgba-next` 和 AM。
三个平台使用同一份前端源码，始终渲染每条可见扫描线并运行 APU、定时器和
音频 DMA。没有物理音视频输出，音频缓冲完整消费。按键脚本在构建时嵌入，
不改变客户机 CPU 执行。存档仍只在 RAM 中。

性能测速指定 **`GBN_VALIDATE=0`**，关闭逐帧画面 CRC、PCM 复制/逐样本统计与
CRC，以及最终画面和内存 CRC；完整 PPU/APU、音频采样和 DMA 仍执行。
无物理音频输出时只推进队列读指针并清空队列，避免积压或丢样，不检查 PCM。
日志明确打印 `GBN validation: off`，这类结果只核对执行进度、已有音频生产/
丢样计数和最终 CPU 寄存器，不能宣称画面/声音/内存校验通过。
`GBN_VALIDATE=1` 保留原有完整校验，默认仍为 1，便于复现历史数据。
后续性能比较统一显式指定 0，功能回归用 1 单独运行；两种口径分别记录。

```sh
make -j6 PLATFORM=fpga BUILD_DIR=build/gba-next/performance-startup \
  GBN_RV32=1 GBN_VALIDATE=0 GBN_WARMUP=30 GBN_FRAMES=120 \
  GBN_INPUT=am/test/data/gba-next-dragonball-play.input \
  PORT=/dev/ttyUSB2 BAUD=1041667 run-gba-next-bench
```

固定工作量前端通过 `gbn_run_batch()` 每批执行至多 256 条客户机指令，每条
指令前仍检查事件及 DMA/HALT/STOP 边界；事件到期后返回前端处理。指令原子
完成，允许与逐指令接口相同的周期超出。这样减少了前端逐指令轮询、音频
消费检查和统计工作，不跳过客户机指令。错误会立即终止并保留已完成条数。
诊断桌面前端仍使用 `gbn_step()`，保持逐指令追踪和精确的 `--steps` 上限。

```sh
make -j6 PLATFORM=spike BUILD_DIR=build/gba-next/bench-spike-startup \
  GBN_WARMUP=30 GBN_FRAMES=120 GBN_INPUT=am/test/data/gba-next-dragonball-play.input \
  SPIKE_PLUGIN=build/rv32/spike/protosoc.so run-gba-next-bench

# 先得到 Spike 结果，再用相同窗口和输入上板。
make -j6 PLATFORM=fpga BUILD_DIR=build/gba-next/bench-fpga-startup \
  GBN_WARMUP=30 GBN_FRAMES=120 GBN_INPUT=am/test/data/gba-next-dragonball-play.input \
  PORT=/dev/ttyUSB2 BAUD=1041667 WATCH=600 run-gba-next-bench

make -j6 PLATFORM=native BUILD_DIR=build/gba-next/bench-native-startup \
  GBN_WARMUP=30 GBN_FRAMES=120 GBN_INPUT=am/test/data/gba-next-dragonball-play.input \
  run-gba-next-bench
```

板卡应已加载匹配的 bitstream、完成 DDR 初始化并等待 `BOOT UART`。
`HEADLESS`、`AUDIO` 是旧前端的构建设置，不会关闭这个前端的 PPU/APU。
新前端用 `GBN_WARMUP` 和 `GBN_FRAMES` 单独控制完整渲染帧数，默认 30/120。
不指定 `GBN_INPUT` 则保持零输入。关卡移动测试使用同一脚本，改为
`GBN_WARMUP=4500 GBN_FRAMES=120`，从空白 RAM 存档执行至第 4620 帧。
前面的 4500 帧也全部模拟，只排除在性能计数区间之外。

测量开始/结束均在完整帧完成边界。区间包含 CPU/设备、PPU、APU 和音频缓冲
消费；`GBN_VALIDATE=1` 另外包含逐帧 CRC 和 PCM 检查，0 则关闭。
初始化、预热日志和最终报告不计入区间。完整校验模式记录所有完整帧的
BGR555 像素流 CRC、原始双声道 PCM CRC、音频数量、最终 CPU 寄存器及内存 CRC。
它们用于不同宿主架构上的确定性检查，不能
证明与真机逐周期相同，也不是所有设备内部状态的完整序列化比较。

```sh
python3 am/tools/gba_next_benchmark_report.py \
  --native build/gba-next/bench-native-startup.log \
  --spike build/gba-next/bench-spike-startup.log \
  --board build/gba-next/bench-fpga-startup.log \
  --output build/gba-next/bench-startup-measurement.json
```

三份日志均使用 `GBN_VALIDATE=0` 时，报告命令另外加 `--performance`。
报告器拒绝混用两种模式；性能报告中的 `validation_enabled=false` 表明
没有图像/PCM/内存内容校验，不能替代完整正确性报告。

Spike 的 `instret` 可用于统计 RV32 工作量；`mcycle` 是功能模拟计数，不能
当成 FPGA 周期数或换算硬件 FPS。只有板测报告实际经过时间、cycles/frame、
IPC 与 FPS。独立解释器是正确性基线，首版原生后端通过 `GBN_RV32=1`
单独启用；重写已能进入关卡，不代表性能优化已完成。

## 热点采样和 PPU 微基准

`PROFILE=1 CFLAGS='-O3 -DNDEBUG -g'` 启用带抖动的定时 PC 采样，测量窗口
仍由 `GBN_WARMUP/GBN_FRAMES` 控制。用 `am/tools/gba_next_profile_report.py`
读取该 ELF/日志，将静态 C 与生成 RV32 代码分别统计。报告是 Spike 指令
工作量分布；正式对照关闭 PROFILE 和 GBN_RV32_STATS。采样会改变布局并有
开销，不能把采样指令数混入正式计数。
生成代码额外按实际 RV32 指令种类统计；地址回收不会混成固定客户机 PC。
采样频率默认 500 Hz，可显式传入 `CPPFLAGS=-DAM_PROFILE_HZ=5000` 扩大样本，
该设置仍只用于诊断。最近的关卡采样及严格输出对照见 process.md。

`make PLATFORM=verilator gba-next-ppu-bench` 构建固定 PPU 工作的 ELF，
包含强制空白、单背景、多背景/OBJ/窗口、alpha、透明/纯色稀疏图块五类。
它没有客体 CPU/APU，用于测渲染周期，不能当成整机游戏 FPS。

`GBN_PPU_RVV` 默认 0；选项 1 为 Zve32x 4bpp 行解码，2 为无效果 RGB
向量合成，3 同时启用；4 为上文已验收的索引区间合成，应配合标量
`RISCV_C_ISA`。4 使用索引路径，2/3 使用 RGB 路径；这些配置分别验证。
当前核的索引和步长访存逐元素执行，已出现指令量减少而周期增加的场景。
因此向量方案仍须分别核对实际周期。标量/向量微基准
可统一设置 `NEWLIB_ISA=rv32im_zicsr_zifencei_zicbom`，共享标量 libc，避免
切换应用 ISA 时改变运行库。当前采样中断不保存向量状态，普通性能对照
关闭 PROFILE。向量诊断须保证 ISR、resolver 及调用的运行库均为标量，
核对实际反汇编并验证完整输出；不能据此声称已保存通用向量上下文。

## 验证边界

PPU 覆盖模式 0–5、文字/仿射背景、位图双页、4/8 位精灵、形状/翻转/仿射/
双尺寸、窗口、马赛克、优先级、混合/亮度及相邻像素绿色交换。以 BGR555
像素表示建立独立基线。
当前按扫描线在 HBlank 入口采样；尚未实现行内寄存器变化、显示总线争用和
完整的背景启用延迟，OAM 周期预算也仍是近似模型。

RGB555 参考构建验证 216 组组合场景、8,332,800 个像素；普通 RGB8 参考构建
验证 72 组组合场景、2,803,200 个像素。旧参考存在按颜色通道不一致的变暗
取整、模式 3/5 OBJ 窗口反向等问题；相关场景另用独立预期值验证，避免将
已知参考问题复制到新实现。参考逐像素通过不等于硬件逐周期准确。

CPU/总线/设备寄存器和若干事件边界与旧实现对照；音频包含 PSG 观察值、
FIFO 与定时器/DMA 联动测试，尚未证明真实游戏整段 PCM 一致。旧参考存在
一些历史行为差异，具体范围记录于阶段日志，不把差分通过解释为硬件逐周期
准确。Flash/EEPROM 忙等待时间属于当前兼容模型。

串口支持无对端的 GPIO、普通 8/32 位及多人模式。外部时钟等待真实触发，
不会凭空接收对端数据；UART 和 STOP 当前明确拒绝。其他 BIOS 服务和外设
继续依据实际程序阻塞点补齐。
