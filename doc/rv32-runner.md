# 实验性 ARM/Thumb → RV32 基本块翻译器

`RV32_RUNNER=1` 把重复执行的 ROM ARM/Thumb 基本块翻译成 RV32 机器码，后续
直接执行缓存代码。首次缓存未命中先解释执行，再次遇到该入口才翻译，
减少一次性初始化路径的编译成本。默认值仍为 `0`。

这是 CPU 执行路径的实验性替换。常见 RAM 读写和普通 IO 读取也生成原生代码；其他访存、
DMA、音视频、BIOS 和事件调度继续复用现有模拟器。建议同时启用
`RUNNER_THREADED=1`，使未翻译的 Thumb 路径使用现有较快的解释器。

2026-10-07 用户明确整体重写模拟器为主线。本文描述的 runner 是已有的
局部替换及后续对照基线；新内核需要独立的 CPU、总线与事件架构，并逐步
替换外设和音视频。具体边界和首阶段验收见 [process.md](../process.md)。
主频由用户另行处理；PMU 是辅助诊断，不是开始重写的前置条件。

独立新内核已在 `src/gba-next/` 开始实现，接口为
`include/gba-next/core.h`，用 `make test-gba-next` 做 native 差分，也支持
`PLATFORM=spike`。当前覆盖 ARM/Thumb 执行、寄存器分组、ROM/BIOS 总线与
异常入口、显存及部分 IO、扫描线事件、IRQ 控制器和四通道 DMA。内置的
独立 BIOS 固件已有 CpuSet/CpuFastSet、HALT、IntrWait/VBlankIntrWait、
LZ77 WRAM/VRAM 和 IRQ 分发。定时器、PSG/Direct Sound 音频、断开连接的
串口与 EEPROM/SRAM/Flash 存档已接入；未实现的设备和 BIOS 服务仍明确停止。
独立 PPU 已覆盖模式 0–5、背景/精灵、窗口及混合；`make run-gba-next`
支持帧导出、WAV、可重放输入和显式测试存档。SDL 桌面前端可操作，龙珠已
验证从启动到关卡的 5,000 帧输入片段，尚未验证完整通关。构建及操作见
[独立内核说明](gba-next.md)。PPU 精确时序、其余 BIOS/外设兼容及完整原生
后端仍待完成。独立解释器开场已板测 3.289 FPS；首版独立 Thumb RV32 后端
已通过 Spike 验证但尚无板测 FPS，通过 `GBN_RV32=1` 启用。它与本文旧
runner 分开构建；实际范围和验证结果持续记录在 `process.md`。

## 最终目标与验收

最终目标是整体重写通用的 RV32 GBA 模拟器，在 FPGA/真实芯片上减少每帧
周期数，向接近 GBA 原速（约 60 FPS）推进；本任务不负责提高主频。不能把某一个
ROM 的局部优化、翻译器能运行、CRC 通过或 Spike 指令数下降当作最终完成。

- 优化依据指令和硬件语义，不使用 Dragonball 的固定地址、流程或画面特征。
- 保留 CPU、访存、外设及事件语义；以差分测试和多 ROM 验证正确性。
- 性能参考包含已有的 threaded 解释器；不仅与较慢的普通解释器比较。
- Spike 用于功能及退休指令数验证。最终性能必须在真实硬件上计时，记录
  频率、配置、帧数、输入和运行片段，并覆盖多种工作负载。
- 达不到总体目标时继续推进执行、访存及后续音视频路径；阶段性成果仍为
  未完成的整体重写工作。当前 ARM 翻译仅覆盖部分指令，尚未完成真实硬件性能验收。

## 实现范围

- 每块最多 32 条 ARM 或 Thumb 指令；原生算术消除逐指令分派，Thumb 算术段
  还消除后续会覆盖的标志位计算。R0–R7、SP、CPSR 和周期计数保留在 RISC-V
  被调用者保存寄存器中，算术直接使用这些寄存器；跨块连接不重新装载。
  调用外部 C 回调或退出生成代码前同步 CPU 状态，回调后重新读取可能修改的状态。
  未被固定映射的高寄存器仍保存在 CPU 结构中。
- Thumb 支持 MOV、ADD、SUB、NEG、CMP、TST、AND、EOR、ORR、BIC、MVN 和立即数移位，
  包括高寄存器、SP 相对算术；涉及 PC 的算术仍调用原处理函数。
- 普通 LDR/LDRB/LDRH、LDRSB/LDRSH 的地址计算和结果写回直接生成代码。
  EWRAM/IWRAM 的 8/16/32 位读取直接使用 RV32 访存指令，保留 RAM 镜像、
  未对齐旋转、符号扩展、等待周期和 ROM 预取队列状态。生成代码检查访存
  回调身份，不满足原生访问条件时调用原函数。
- 可直接返回寄存器存储值的 IO 读取也生成 RV32 代码，覆盖 8/16/32 位读取、
  LDRSB/LDRSH、不同宽度的地址镜像、未对齐旋转、等待周期及预取效果。
  依照 `GBAIORead` 的语义维护 `haltPending`；32 位读取必须两个半字均可直接
  读取。计时器计数值、按键回调、波形 RAM、SIOCNT/RCNT、条件音频寄存器、只写
  寄存器和未定义地址等继续调用原函数，保留读取的副作用。
- 普通 STR/STRB/STRH 的 EWRAM/IWRAM 写入也直接生成代码，保留对齐、镜像、
  等待周期和预取效果；其他地址及被替换的回调执行原 Thumb 处理函数。
  成功的 RAM 写入继续执行块内后续指令，不再强制拆块。写入回调返回后通过
  缓存重新检查入口，保留 DMA、WAITCNT、代码改写及显式失效的影响。
- 访存生成器把常见 IWRAM 快路径和完整的 RAM/IO 检查、等待周期逻辑一起
  放入共享 RV32 例程，避免每条访存重复生成快速路径。
  IWRAM 快路径检查回调、区域、读取对齐和预取等待
  条件，全部检查通过后才修改状态；复杂情况继续走完整路径。
  两种路径均保留驻留的客户机寄存器，不往返 C，也不新建栈帧。
  对齐读取省去旋转；首次顺序取指已覆盖访存等待时，直接计算周期和原有
  预取位置，其他情况仍执行完整的有限预取队列逻辑。
  共享例程在缓存两端各放一份，使默认缓存内可用一条 `JAL` 调用；超出范围
  时用 `AUIPC/JALR`。这样缩小访存指令在块内的体积，减少长块被迫截断。
- 共享访存例程从 A7 接收当前指令的客户机流水线 PC。Thumb 原生访存成功时，
  不再逐条写回 PC 和两个预取值；事件退出、块结束和调用外部函数前重建完整
  流水线。未翻译指令的回退入口也先重建状态，仍保留每条访存的事件检查。
- PC 相对的 ROM 常量读取直接从当前 ROM 缓冲区加载，不把内容折叠成常量。
  保留六个 ROM 区域的等待周期及镜像，越界或回调被替换时使用原路径。
- 条件分支和无条件直接分支生成条件判断及流水线刷新。同一 ROM 区域内、
  不涉及空转检测/休眠的跳转直接维护映射相关状态；检查原回调身份、区域、
  ROM 大小及空转状态。跨区、越界、空转检测/休眠及特殊回调仍调用
  `GBASetActiveRegion` 或当前回调，完整保留副作用。
- 已缓存的块之间通过原生分派代码直接连接，共用 64 字节栈帧和寄存器约定，
  无需逐块返回 C。同一区域内的直接分支在编译时确定目标缓存槽，命中时
  合并区域维护、周期更新和进入目标块，省去公共分派器的哈希及重复流水线读写。
  入口检查回调身份、模式、ROM 的可变性、空转状态、块标签、映射、
  等待周期和实时读取的两个预取字；未命中不改变 CPU/GBA 状态，继续执行
  原分支路径。缓存替换不会留下指向旧块内容的可执行链接。
- 直接分支首次通过完整校验后，在目标块记录当前验证代次。同一代次内的
  后续连接只检查代次和目标标签，直接完成分支的周期、映射掩码及流水线
  维护。纯原生路径不会修改模式、映射、等待周期、空转策略或 ROM 代码；
  回调/事件、显式失效、缓存槽重建和代次回绕会使旧证明失效。
  外部 C 回调之前用生成代码推进代次；纯轮询的探测不走直接连接，保持代次惰性，
  避免每个事件都为未使用的块间连接支付维护成本。
- PC 和预取字在算术段间及直接分支连接时延后写回，在辅助函数、通用分派
  及事件出口前恢复；目标块在第一条指令前遇到事件也能恢复正确流水线。
  所有外部 C 回调仍能观察到原解释器相同的 CPU 状态。
- ARM 支持带条件的 AND、EOR、SUB、RSB、ADD、TST、TEQ、CMP、CMN、ORR、MOV、
  BIC、MVN，以及立即数、立即数移位/旋转、RRX 和 PC 源操作数；保留 S 位、
  全部 NZCV 条件和解释器的 shifter 状态。B/BL 复用原生区域维护及直接块连接。
  立即数 LDR/LDRB 支持前/后索引和基址写回，直接访问 RAM/普通 IO；
  USER/SYSTEM 模式的 LDRT/LDRBT 也可直接读取。拒绝路径保留原处理函数的
  写回顺序、临时权限切换及 banked registers 副作用；目的寄存器与基址
  相同、SP、条件不成立及 PC 基址写回均有差分覆盖。
  ARM 的 PC 相对 ROM 字读取也直接加载当前内容，保留未对齐旋转、镜像、
  越界回退和独立的数据等待周期，不把可变数据折叠为常量。
  这些读取保留 ARM 的 32 位取指等待周期和 GBA 16 位 ROM 预取总线语义。
  ARM 每条指令前检查事件。其他指令调用原处理函数并结束当前块。
- ARM 读取遇到特殊 IO、回调替换或未支持的权限状态时，恢复当前指令前的
  流水线并解释执行剩余事件片段。随后 32 个 ARM 事件片段采用解释器，再
  尝试翻译；Thumb 不受这段退避影响。该有界退避避免特殊读取循环不断支付
  寄存器同步、入口检查和代码验证成本，也允许读取地址变化后重新进入原生
  路径。它不跳过读取、不改变输入采样或事件语义。
- ARM/Thumb 共用缓存与寄存器约定，但入口、映射、预取宽度、等待周期及模式
  分别检查，不会把同地址的 ARM 块当作 Thumb 块执行。
- 对一个块内跳回入口的只读轮询循环，先做寄存器依赖分析，排除计数器、
  移动指针、写入及未支持的指令；运行时最多执行两次 Thumb 或三次 ARM 完整
  迭代，检查所有可变 CPU 字段及预取队列是否不变。ARM 多一次探测用于让
  shifter carry 与新 CPSR 状态收敛。只有未调用外部或有副作用的回调、每条
  指令周期均为正且状态达到固定点时，才把下个事件前的重复整轮合并为周期
  推进。事件前不足一轮的部分继续执行原指令，事件后重新验证。
  定时器、输入、EEPROM 数据读取、特殊 IO 和被替换的回调等不参与合并；普通 IO 与
  ROM 分支对空转状态的更新保持原有语义。该优化不依赖特定 ROM 地址。
  轮询入口只负责有界探测；快照、固定点比较和周期推进均由生成的 RV32 代码
  完成。快照只保存依赖分析确认可能写入的 GPR，并保存 CPSR、流水线、取指
  掩码、移位器及预取队列状态；这些受限路径不能修改 banked registers、事件
  截止时间或回调表，因此不再逐轮复制整个 CPU 结构，也不再为每次合并回到 C。
  差分测试仍比较完整 CPU/GBA 状态。
- `RV32_TRACE_POLL=1`（在 `RV32_RUNNER=1` 下默认开启）进一步合并跨多个
  Thumb 块的固定点循环。在成功的后向直连处比较 R0–R14、CPSR 和
  `haltPending`；分支校验保证映射和逻辑流水线一致，预取位置已归零。
  RAM 写入仍实际执行，只有逐字节不改变内容的写入才保留快照；任何改变
  RAM 的写入都清除快照，任何外部 C 回调都禁用本次原生链的证明。
  验证周期单调后，只合并下个事件前的完整迭代，剩余部分照常执行。
  每次从 C 进入原生链重新证明，不跨事件复用；不依赖特定游戏地址。
  `RV32_TRACE_POLL=0` 可关闭此项，保留原有的单块只读循环优化。
- Thumb 的标准 EEPROM 半字读取在非 READ 命令下只返回忙状态：通过只读
  辅助函数查看 EEPROM 的完成事件是否仍在队列中，不同步整份 CPU、不破坏
  直接分支证明。这种状态在下个事件前保持不变，因此符合条件的忙等待循环
  可按完整迭代推进周期。事件后重新读取状态，不能跨过事件。
  READ 命令仍执行原回调并逐位消耗数据；其他存档类型、访问宽度及被替换
  的回调也保留原行为。保留奇地址旋转、LDRSH 符号扩展、ROM 等待周期和预取
  队列语义；该路径没有游戏专用地址或存档内容假设。
- 其他 Thumb 指令调用已有指令处理函数。特殊写入、未支持指令及外部访存回调返回后
  重新进入缓存，使 DMA、WAITCNT 和代码修改的影响在下个入口检查。
- 可写 ROM（`isPristine=false`）也能翻译，包括装载时补齐的非 2 次幂 ROM。
  每个 context 维护代码验证代次：设备回调或事件/主机边界更新代次，缓存
  块在下一次使用时逐字比较未来指令；同一代次内只验证一次。检查覆盖直接
  原生块连接；不依赖所有卡带、补丁和调试器写入路径都调用失效钩子。
  已进入两级预取队列的指令保留旧值；只验证尚未预取的指令和块尾预取字。
  代码变化会使块失效并重新查找/翻译，代次计数回绕也会清除旧验证记录。
  外部回调在执行途中显式清空当前 context 时，剩余片段退回解释器；下一次
  公开入口重新取得缓存，避免继续使用已失去 CPU 所属关系的 context。
- RAM/BIOS 中的代码仍回到原解释器；映射或预取不匹配时重新查找或回退。
  ARM 的寄存器控制移位、ADC/SBC/RSC、乘法、复杂访存、PC 目的操作数和模式
  切换等当前仍使用原处理函数，尚未全部原生化。

事件不能穿过被合并的算术段而丢失中间状态。如果下一个事件落在段内，
该次执行回到解释器。单条指令允许跨过事件时刻，和原解释器一致。
CPU 重置/销毁或 ROM 指针变化会使缓存失效。生成代码以 `fence.i` 同步。

缓存按 CPU 延迟分配：每个 context 默认 512 块，每块最多 768 个 RV32
指令字，含分派代码、32 KiB 共享访存例程空间及元数据约 1.8 MB。扩大单块容量是为了容纳可写代码验证
与完整的短 ARM 读取/比较/分支循环，避免过早拆块破坏轮询合并。最多保留
4 个 context，重置/销毁后复用已分配的空间；分配失败退回解释器。索引混合地址高位，减少不同 ROM
页中对齐函数的冲突。代码缓冲区不足时只保留完整段，重新分块；不截断
已有段的标志位语义。

实现针对当前单线程 AM 播放器，需要可写且可执行的内存及正确实现的
`fence.i`。RAM/IO 路径、ROM 常量读取和块连接都基于指令/内存语义，没有游戏专用地址。
已完成 50 MHz FPGA 首轮生成代码、CRC、性能和采样验证，见下文；取指缓存失效
和流水线等待的具体成本仍需进一步测量。

## 验证

```sh
make -j8 PLATFORM=spike RV32_RUNNER=1 RUNNER_THREADED=1 test-rv32
```

测试在 Spike 上实际执行生成的 RV32 代码，再与原 ARM/Thumb 处理函数比较完整
`ARMCore` 状态，不仅比较图像 CRC：

- 19,875 种算术编码，636,000 组边界值、随机值、NZCV 和事件检查；另有
  636,000 组固定寄存器约定下的算术状态对照，覆盖新算术生成器及调用约定。
- 12,800 种读取编码，204,800 组寄存器别名、未对齐地址、符号扩展及周期检查。
- 5,632 种直接分支编码，90,112 组全部 NZCV 组合及跳转后等待周期检查。
- 1,024 个随机算术块，以及集成测试中的事件截断、ROM 镜像、陈旧预取、
  ROM 写入、WAITCNT、BX 和大 SP 偏移。
- 19,712 组真实 RAM/IO/ROM 读取对照，比较完整 CPU 和 GBA 结构，覆盖地址
  镜像、未对齐、预取队列占用、等待周期、寄存器别名及运行时替换访存回调。
- 58,184 组 IO 读取对照，扫描 1 KiB IO 窗口的每个字节，另测不同访问宽度
  的窗口外镜像。覆盖寄存器别名、未对齐、符号扩展、空转状态、32 位访问
  横跨特殊寄存器、运行中的计时器及波形通道、按键回调和回调身份替换。
- 17,280 组真实 RAM 写入对照，覆盖字节/半字/字、别名、未对齐、镜像、
  预取队列、WAITCNT 写入，以及会修改寄存器/标志位的替换回调。
- 全部 2,048 种 PC 相对 LDR 编码的 32,768 组对照，覆盖目标寄存器、偏移、
  六个 ROM 区域、数据替换、越界和特殊回调。
- 4,320 组真实映射回调的分支对照，覆盖条件执行、空转检测/休眠、BIOS/ROM
  区域变化和越界，比较完整 CPU 和 GBA 结构。
- 256 个混合长块，覆盖生成缓冲区边界、负周期起点、回调可见的 CPU 状态，
  以及回调修改寄存器、标志位和周期计数后原生执行的恢复。
- 256 组 A/B 块循环和自循环，包含无需经过 C 的普通 ROM 跳转及特殊回调
  路径，验证单次原生调用可保留寄存器并连接直到事件。普通 ROM 用例替换
  公共分派器为失败出口，确认实际走直接连接；另有 2,048 组缓存失效、标签
  冲突、映射、等待周期和预取字变化的入口检查。
- 1,344 组 Thumb 直接连接的成功/拒绝状态，覆盖六个 ROM 区域、全部可变入口条件，
  以及目标事件出口的延迟流水线恢复；拒绝时完整 CPU/GBA 状态保持不变。
  同代次复用校验结果的路径另验证完整状态相同，且实际执行的 RV32 指令更少。
- 192 组连续块执行状态，覆盖访存回调替换跳转函数、开启空转检测，以及
  后续分支进入空转地址，比较完整 CPU/GBA 状态。
- 4,848 组轮询和事件切片对照，覆盖 RAM/IO/ROM 读取、符号扩展、地址镜像、
  等待周期、预取、负周期起点、循环中的事件截断和事件修改读取值后的退出。
  返回固定值但修改设备状态的读取回调仍必须逐次执行；另有计数器、移动
  指针、写入和未支持指令的拒绝用例。其中 48 个长循环检查确实发生了合并，
  48 个 Thumb 探测直接在生成代码中完成；这些合成用例的局部指令数收益不代表
  游戏整体性能。
- 多 CPU context 的复用/淘汰、保留旧预取字时替换 ROM，以及显式缓存失效。
- 12,672 种 ARM 算术编码的 202,752 组完整状态对照，覆盖全部条件码/NZCV、
  移位/旋转边界、立即数、PC 源操作数、寄存器别名及负周期起点。
- 62,720 组 ARM RAM/IO/ROM 读取状态，覆盖镜像、未对齐、地址偏移、预取、
  独立变化的 16/32 位等待周期、前/后索引写回、寄存器别名、全部权限模式、
  banked registers、PC 相对常量及越界；替换回调会观察完整寄存器和权限状态。
- 2,048 组 ARM B/BL 状态及 1,344 组直接连接成功/拒绝状态，覆盖条件执行、
  LR、六个 ROM 区域、流水线、回调和 ARM/Thumb 缓存模式隔离。
- 1,280 组 ARM 事件切片，覆盖长块/代码缓冲区边界、直接连接、回调修改
  寄存器/标志/周期/等待周期、ROM 写入、陈旧预取和双向 ARM/Thumb 切换；
  这些测试经过公开的 `ARMRunLoop` 入口。
- 6,178 组 ARM 轮询/事件状态，包含 37 个确认发生合并的长循环，其中 36 个
  探测直接在生成代码中完成；覆盖可写
  ROM、LDRT/LDRBT、推进基址的拒绝，以及设备读取退避后恢复原生执行。
  计数循环和具有设备副作用的读取仍逐次执行，事件修改 IO 后重新判断条件。
- 3,456 个可写 ARM/Thumb 事件切片，覆盖 ROM 别名、主机/回调/事件代码改写、
  已预取旧值、只读转可写、验证代次回绕和直接块连接的中部指令改写；比较
  完整 CPU/GBA 状态和最终 ROM 内容，并确认 93 次在原生直接边上检测到失效、
  45 次从原生回调路径正确处理代次回绕，并验证执行中显式清空缓存后的回退。
- 3,072 组 EEPROM 读取/事件状态：两种 EEPROM、所有命令、数据位计数边界、
  对齐/奇地址/有符号读取、别名、等待周期、事件链表 root/reroot 及替换回调。
  比较完整 CPU/GBA 状态，确认 READ 命令逐位推进；64 个长忙等待循环确认
  合并生效，并验证事件结束忙状态后重新读取和退出。
- 另以共享访存形式执行 157,896 组 RAM/IO 读写和 ARM 读取对照，覆盖上述
  对齐、符号、别名、预取、等待周期及替换回调；ARM 拒绝路径先确认完整状态
  未被修改，再解释执行该指令。
- 6,912 组连续写入/读取/运算事件切片，检查原生块确实覆盖多次访存，比较
  完整 CPU/GBA 状态、目标 RAM 和 ROM 内容。覆盖寄存器别名、未对齐镜像、
  事件截止、真实 WAITCNT 写入，以及回调观察/修改寄存器、改变等待周期、
  修改未来代码和执行中清空缓存。

共 2,153,218 组状态/事件对照，另加上述多指令及入口检查，均通过。`RUNNER_THREADED=0` 和 `1` 两种解释器回退配置均验证。
Dragonball 120 帧还通过 `CPPFLAGS=-DMGBA_RV32_VERIFY` 的运行时 ARM/Thumb 算术状态对照；
该选项重复执行算术段用于验证，不用于测性能。访存回调不会重复执行。验证
辅助函数会阻止所在轮询循环合并；包括 EEPROM 忙等待在内的合并路径由上述事件差分测试及正常游戏
构建的 CRC 对照覆盖。
默认 Native 构建通过。

## 当前结果

相同配置：`PLATFORM=spike GBA_IDLE_SKIP=1 RUNNER_THREADED=1 HEADLESS=1 AUDIO=0
BENCHMARK=1 WARMUP=5`，无按键输入。仅切换 `RV32_RUNNER`。
下表是 Spike `-g` PC histogram 的**全程退休指令总数**，包含启动、ROM 装载、
预热、翻译和运行；变化为 `(翻译器 / threaded 解释器 - 1)`。

| ROM | 测量帧数（另有 5 帧预热） | threaded 解释器 | 翻译器 | 变化 | 双方最终 CRC32 |
| --- | ---: | ---: | ---: | ---: | --- |
| Dragonball | 120 | 1,061,514,433 | 770,344,083 | -27.4297% | `3642819F` |
| go | 30 | 238,238,812 | 239,507,704 | +0.5326% | `EB2AB463` |
| 2d-wrap | 30 | 87,435,133 | 45,773,911 | -47.6481% | `C0EA6C09` |

当前 Dragonball 开场及 2d-wrap 两个片段的指令数进一步下降，go 基本持平；不能
据此推断所有游戏或完整流程都更快。**这些不是 FPGA FPS 或实测硬件加速
比例。** ROM 装载等固定开销也包含在内，不能把总数当作稳态每帧 CPU 成本；
最终仍需真实硬件计时。

相对上一轮已支持分支校验复用和 EEPROM 忙等待的版本，本轮变化如下：

| ROM | 上一轮 | 本轮 | 增量变化 |
| --- | ---: | ---: | ---: |
| Dragonball | 779,779,521 | 770,344,083 | -1.2100% |
| go | 239,503,553 | 239,507,704 | +0.0017% |
| 2d-wrap | 57,940,400 | 45,773,911 | -20.9983% |

本轮把只读轮询的状态快照、固定点比较和周期推进下沉到生成的 RV32 代码，
减少每次探测回到 C 的开销；同时保留上一轮的访存路径重组和普通 RAM 写入后
继续执行。共享完整访存逻辑以缩小块内代码，同时将常用 IWRAM 访问内联，
减少调用和通用检查。优化基于访问区域、对齐、等待周期、寄存器依赖和回调身份，
不识别游戏地址或场景。

龙珠片段减少约 2,140 万条指令；另两个片段略有增加，因此不能声称该重组对
所有 ROM 都更快。`go` 相对原解释器仍有约 0.53% 的额外开销，尚未获得整体
收益。共享例程的布局、分支和指令缓存效果仍需在 FPGA/真实芯片上测量。

go 的非 2 次幂文件在装载时被复制到可写缓冲区，现在能够进入翻译器。
它的热点包含特殊 IO 读取；初版开放翻译后，重复辅助函数调用及缓存验证
造成明显退化，因此保留原读取行为并采用有界退避。没有使用特定 ROM 地址
或绕过按键回调，也没有把特殊 IO 轮询当作可消除的纯循环。

生成代码的入口检查、辅助函数和状态同步仍占明显开销。当前轮询合并仅支持
一个缓存块内的自循环，不能合并跨块循环或带有外部副作用的循环。RAM/BIOS
代码和多类 ARM 指令也尚未原生化。**整体重写及硬件性能目标尚未完成。**

## 2026-10-07：面向 quad-issue-rvv 的调度和测量

这一轮保留已有语义和差分框架，先让生成代码适应目标核，再建立稳定的上板
测量口径。没有改动 RTL，也没有放宽 IO、事件和可写代码的正确性约束。

### 标志位指令调度

`_emitFlagsAddSub` 将独立的 C、V、Z 计算交错排列，减少相邻 RAW 依赖。
ADD 仍为 17 条 RV32 指令，SUB 的指令数也保持不变。目标核按程序顺序发射
连续前缀，无法跳过中间被依赖阻塞的指令，因此指令数相同也可能消耗不同周期。

`build/rv32/core-comparison/` 中的独立 RTL 实验使用
`/home/tillsat/proto-core/quad-issue-rvv` 的当前源码重新构建 Verilator，
并做逐周期参考模型对照。5 组 ADD 边界输入、每组 3,200 次标志位计算中，
旧顺序约 35,212 周期，新顺序约 25,612 周期，局部周期降低约 27.3%。
实验使用理想 TCM，不包含游戏、DDR、cache miss 和真实取指布局，不能换算成
游戏 FPS。完整 ARM/Thumb 差分测试在 `RUNNER_THREADED=0/1` 两种配置下均通过。

### 只统计测量帧的计数器

proto-soc 的 benchmark 默认输出：

```text
Counters: cycles=...; instret=...; sampling=0
```

使用 RV32 的 `mcycle[h]`、`minstret[h]`，在高位变化时重读以避免跨越 32 位
回绕。计数从预热结束开始，到测量帧结束停止；排除 ROM 装载、预热、CRC 和
报告打印，包含主循环、软件渲染、RGB 转换和模拟音频逻辑。
`sampling=1` 表示计数还包含采样开销。实际硬件 IPC 为 `instret / cycles`。
Spike 的 `minstret` 可比较指令量，但其 cycles、IPC、时间和 FPS 不代表硬件性能。

新口径的 Spike 对照如下，均使用 5 帧预热、固定零输入、全新存档、
`RUNNER_THREADED=1 GBA_IDLE_SKIP=1 HEADLESS=1 AUDIO=0 BENCHMARK=1 PROFILE=0`：

| ROM / 测量帧 | threaded 解释器指令数 | 当前 runner 指令数 | 相对变化 | CRC32 |
| --- | ---: | ---: | ---: | --- |
| Dragonball / 120 | 837,898,879 | 564,283,278 | -32.6550% | `3642819F` |
| go / 30 | 108,226,607 | 109,212,830 | +0.9113% | `EB2AB463` |
| 2d-wrap / 30 | 73,116,330 | 36,337,516 | -50.3018% | `C0EA6C09` |

这是整个 runner 相对 threaded 解释器的对照，**不是本轮调度改动单独带来的收益**。
与上文包含启动开销的历史数字口径不同。`go` 仍稍有退化，特殊 IO 读取和回退
仍需在板上采样后处理。结果、对应 ELF、SHA256 和日志保存在
`build/rv32/scheduled-results/`；三组 CRC 都与 Native 的全新存档运行一致。
Native 默认加载 ROM 旁的 `.sav`，复现时应将 ROM 复制到临时目录，避免旧存档
改变开场流程，也避免测试写入日常存档。

### 动态代码 PC 采样

`PROFILE=1` 现在适用于 `fpga`、`spike`、`spike_zve32x`、`verilator`。
静态 C 代码仍按 ELF 符号统计；生成代码通过 `RV32ResolveCode` 在采样中断内
立即解析为 `(kind, address, offset)`，不会等到结束后再查已经被复用的缓存槽。
kind 1/2 为 Thumb/ARM 块，address 为客户机块起始地址；kind 3–6 为两种模式
的分派和分支例程；kind 7 为共享访存例程，address 表示例程编号。offset 是
该生成块或例程内的 RV32 字节偏移，不是最终 ELF 中的偏移。

收集器使用 8,192 个表项，每次最多探测 8 个位置；表满或碰撞超过上限时报告
`dropped`。`outside` 仍表示所有静态 text 外的样本，其中已识别的生成代码由
`JIT profile` 单列。报告同时列出未知样本、丢失样本和按客户机块聚合的排名。
同一客户机地址被重新翻译时，偏移可能合并不同代码版本，不能据此反汇编最后
一个缓存版本。中断内不分配内存，也不打印。

Spike 的龙珠 120 帧功能检查取得 284 个样本，其中 149 个生成代码样本全部
识别，丢失和未知样本均为 0；CRC 保持 `3642819F`。这只验证采样链路，不能
将该分布视为 FPGA 耗时比例。额外测试覆盖缓存槽/上下文复用、失效地址、
共享例程镜像、哈希表饱和、CLINT 中断与 CSR 恢复，以及报告的守恒检查。

```sh
make -j6 PLATFORM=spike RV32_RUNNER=1 RUNNER_THREADED=1 \
  BUILD_DIR=build/rv32/scheduled-full \
  SPIKE_PLUGIN=build/rv32/spike/protosoc.so test-profile test-runtime

python3 am/tools/profile_report.py \
  --elf build/rv32/scheduled-results/dragon-profile.elf \
  --log build/rv32/scheduled-results/dragon-profile.log \
  --out build/rv32/scheduled-results/dragon-profile.json --platform spike
```

### 上板构建与后续实测

已准备同一龙珠片段、30 帧预热、120 帧测量的三个 FPGA 构建目录：
`build/rv32/fpga-reference`（threaded 解释器）、`build/rv32/fpga-release`
（无采样 runner）、`build/rv32/fpga-profile`（采样 runner）。每个目录包含
`mgba.elf`、`mgba.bin`、`ram-loader.bin`。无采样版本用于 FPS/IPC，采样版本
用于找热点，二者不要混用为同一条性能记录。

连接 ZCU102 电源和 USB UART 后，需要确认 FPGA 已加载匹配的
`quad-issue-rvv`、DDR、缓存和时钟配置。FPGA 若在断电后丢失配置，需要通过
JTAG 等方式重新加载对应 bitstream；USB UART 连接本身不会配置 FPGA。
先检查串口设备和启动信息，再上传到易失 RAM。例如此前的 50 MHz DDR 设计
使用 `/dev/ttyUSB2` 和 1041667 baud：

```sh
make -j6 PLATFORM=fpga RV32_RUNNER=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 BUILD_DIR=build/rv32/fpga-release \
  PORT=/dev/ttyUSB2 BAUD=1041667 run

# 重新进入 Boot ROM UART recovery 后采样
make -j6 PLATFORM=fpga RV32_RUNNER=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 PROFILE=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 BUILD_DIR=build/rv32/fpga-profile \
  PORT=/dev/ttyUSB2 BAUD=1041667 run

python3 am/tools/profile_report.py --elf build/rv32/fpga-profile/mgba.elf \
  --log build/rv32/fpga-profile/fpga.log --out build/rv32/fpga-profile/hotspots.json
```

随后已完成物理板卡实测，结果见下一节；尚未达到 GBA 原速。

## 2026-10-07：真实 FPGA 对照和代码膨胀证据

ZCU102 重新加载此前验证过的 `quad-issue-rvv` bitstream，50 MHz、32 KiB
I/D cache、64 MiB PS DDR4，经 32-bit AXI 访问。复用的是 2026-10-03 的精确
硬件镜像，并非重新综合当前 RTL。镜像、历史输入清单和当前源码差异记录在
`build/rv32/board-20261007/inputs.json`；bitstream SHA256 为
`c81384e40c03be2dfa96b5aef0a2579679b961564cb7d5d5122f1dee82f1e856`。

DDR 几何匹配、全 64 MiB 地址模式、cache writeback、字节/半字更新和 DMA
检查均通过。A53/R5 保持复位，无 ARM 工作负载。三组测试使用同一龙珠开场，
固定零输入、全新存档、30 帧预热后测量 120 帧；均开启
`RUNNER_THREADED=1 GBA_IDLE_SKIP=1`，包含软件渲染、RGB 转换和音频模拟。

| 版本 | FPS | 周期 | 退休指令 | IPC | CRC32 |
| --- | ---: | ---: | ---: | ---: | --- |
| threaded 基线，无采样 | 9.184 | 653,276,556 | 755,063,581 | 1.1558 | `3642819F` |
| 当前 runner，无采样 | 10.514 | 570,666,410 | 509,571,079 | 0.8929 | `3642819F` |
| 当前 runner，采样版 | 10.942 | 548,328,501 | 510,358,725 | 0.9308 | `3642819F` |

三组均正常退出且匹配同条件 Native CRC。无采样 runner 相对基线指令减少
32.51%、周期减少 12.65%、FPS 提升 14.48%；每条指令的平均周期增加 29.44%。
这是整个 runner 的软件对照，不能归为本轮标志位指令重排的单独收益。
当前 10.514 FPS 约为 GBA 原速的 17.6%。各镜像本轮各运行一次，不能代表
其他 ROM 或完整游戏流程。采样版布局不同且运行中有中断，不能用它与普通版
的差值估算纯采样开销，也不选用它的较高 FPS 作为无采样结果。

硬件采样共 5,464 个点，其中生成代码 1,994 个，未知和丢失均为 0。
按全体样本计，生成 Thumb 块体为 26.26%，生成的分派/分支例程为 7.41%，
共享访存例程为 2.82%，C 层 `RV32RunLoop` 为 3.61%，翻译/机器码生成为
6.04%。`main` 内的 RGB 转换循环经反汇编确认占 12.28%；名字以 `GBAudio`
或 `GBAAudio` 开头的函数占 13.25%（不含其他名字的音频辅助函数）。
这些是 self samples，不能直接量化缓存失效、分支预测失败或 LSU 等待。

程序退出并刷新缓存后，通过只读 PS JTAG 读取运行器 arena，得到保留的
427 个翻译块、622,000 字节机器码。在用于诊断的一处热点范围内，13 个
重叠块覆盖 70 个不同的客户机字节，生成 22,572 字节 RV32 代码。完整 arena
预留约 1.85 MB。快照中块有效位已被正常退出过程清除；这是保留翻译的布局
证据，不是全部时序工作集，也不能反向替代采样时的身份解析。

下一轮优先处理生成代码体积、事件退出后的重叠入口、跨块检查，以及预热后
仍在发生的编译。依据通用 ARM/Thumb、访存和事件语义优化，再用差分和相同
硬件片段复测。代码膨胀已观察到；它对 IPC 降低的具体贡献尚未得到直接测量。

归档目录包含三套精确 ELF/bin/loader（`images/`）、原始串口日志、
`measurement.json`、`hotspots.json`、`hotspot-groups.json`、`context.bin` 和
`cache-layout.json`。`analyze.py` 会检查镜像 SHA256、帧数、CRC、时钟、计数器
与计时的一致性；`analyze-cache.py` 解析退出后的缓存快照。

```sh
python3 build/rv32/board-20261007/analyze.py
python3 build/rv32/board-20261007/analyze-cache.py
```

最后板卡正常回到 `BOOT UART`，错误状态为零，保留供电和已加载的设计。

## 2026-10-07：共享 IWRAM 路径与推迟流水线写回

本轮保留原有 50 MHz bitstream，在同一块持续上电的板卡上对照两个候选。
第一步将原本每条访存都内联的 IWRAM 快路径放到共享访存例程的入口，
仍保留回调身份、地址区域、对齐、等待周期和预取检查。第二步通过 A7 传递
客户机 PC，使 Thumb 原生访存不必先写回完整流水线；只有回调、事件退出
和块结束才重建状态。两步均依据通用指令语义，未加入 ROM 地址特判。

测试条件与上一节相同：龙珠开场、全新 RAM 存档、零输入、30 帧预热、120 帧
测量，保留软件渲染、RGB 转换和音频模拟，无显示/音频输出、无采样。

| 版本 | FPS（串口值） | 周期 | 退休指令 | IPC |
| --- | ---: | ---: | ---: | ---: |
| 上一轮 runner 原镜像，本轮复测 | 10.514 | 570,664,224 | 509,571,079 | 0.8929 |
| 仅共享 IWRAM 快路径 | 11.329 | 529,574,802 | 510,404,013 | 0.9638 |
| 共享快路径 + 推迟流水线写回，当前保留版 | 11.734 | 511,304,372 | 498,848,129 | 0.9756 |

三组均为 `CRC32: 3642819F`、`AM exit: 0`，与同条件 Native 相符。
最终版相对本轮原镜像复测，FPS 提升 **11.61%**、周期减少 **10.40%**、
退休指令减少 **2.10%**；相对上一轮 threaded 解释器的 9.184 FPS 提升
**27.77%**。按计时计算约 11.7347 FPS，为 GBA 原速的 **19.65%**。
原镜像两次运行退休指令完全相同，周期相差 2,186，约 0.00038%；两个候选
本轮各测一次，结果仅代表此开场片段。尚未达到接近原速的总体目标。

第一步在退休指令略增 0.16% 时仍降低周期，支持继续减少热代码重复；本轮
没有新增硬件失效/停顿计数，不能把收益全部定量归因于 I-cache。
第二步相对第一步再提升约 3.57%，因此保留两步合并版本。

### 代码体积与验证范围

用旧物理快照中的 427 个块重新生成相同输入，第一步将块体从 622,000 字节
降到 532,516 字节（-14.39%）；算上两份共享例程的有效代码，总量下降
13.25%。最终版增加事件退出所需的流水线重建代码：426 个保持相同指令数
的块从 619,024 降到 569,432 字节（-8.01%）；另一个块因容量限制由 14 条
缩短至 13 条，不计入这个同长度比较。缓存槽数和 arena 预留大小不变。
这是固定客体输入的静态重生成实验，不是新一轮物理缓存快照或运行期工作集。

最终版通过 `RUNNER_THREADED=0/1` 两套完整 Spike 差分，每套 34 组，包含
ARM/Thumb 算术、RAM/IO/ROM 访问、未对齐和镜像、等待周期、访存后继续执行、
事件退出、回调可见状态、EEPROM、直接连接、可写代码和显式失效。
龙珠 30/120、go 5/30、2d-wrap 5/30（预热/测量帧）CRC 分别为
`3642819F`、`EB2AB463`、`C0EA6C09`。Spike FPS 不用于物理性能结论。

归档位于 `build/rv32/compact-memory/` 和 `build/rv32/deferred-memory/`。
前者包含原始源码、第一候选、复测日志、代码体积重生成程序，以及全部板测的
`measurement.json`；后者包含最终源码快照、精确 FPGA ELF/bin/loader、
差分测试 ELF、`verification.json`、三个 ROM 日志和最终板测日志。
两个目录的 `inputs.json` 保存 SHA256、构建参数和固定硬件配置。

```sh
make -j6 PLATFORM=fpga RV32_RUNNER=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 BUILD_DIR=build/rv32/deferred-memory/fpga all
python3 -u am/tools/run_fpga.py --protosoc /home/tillsat/proto-core/soc \
  --port /dev/ttyUSB2 --loader build/rv32/deferred-memory/fpga/ram-loader.bin \
  --app build/rv32/deferred-memory/fpga/mgba.bin --baud 1041667 --watch 300 \
  --log build/rv32/deferred-memory/board-release-2.log
python3 build/rv32/compact-memory/analyze.py
```

最终板测日志确认回到 `BOOT UART`。事件打断后的重叠入口仍会独立翻译，
下一步应研究已有原生段入口的安全复用，并统计翻译次数和缓存替换；恢复到
算术融合段中间时必须保留中间标志位、预取和事件语义。

## 2026-10-07：同核 GB 与存储容量对照

新增独立 `am/bench/peanut/`，固定 Pico-GB 提交携带的 Peanut-GB 头文件，
不替换 mGBA。Native/Spike/FPGA 使用同 ROM、输入、帧数及编译开关。
其 CPU、PPU 和 RGB565 转换计时，APU 和物理输出关闭；这不同于当前 mGBA
保留音频模拟的基准。没有新的 Pico 硬件测量。

在同一块 50 MHz FPGA 上，dmg-acid2 约 73.710 FPS；作者公开的 2048 ROM
标题画面约 74.053 FPS，固定按键进入游戏后约 46.432 FPS。游戏段两次板测
各测量 600 帧，退休指令均为 734,722,395，周期差约 0.00018%；Native、
Spike、FPGA 的帧缓冲、WRAM、VRAM、PC、扫描线数一致。详细开关和编译
方式见 [am/README.md](../am/README.md#pico-gb-comparison-benchmark)。

另用可放入现有 64 字节 ITCM/DTCM 的 44 字节内核，测得热缓存 DDR 与
TCM 的依赖读取均约 2 周期/次。固定内核、扩大数据工作集到 64 KiB 以上，
平均达到约 25–26 周期/次；固定四指令跳转块、扩大指令工作集到 64 KiB
以上，平均从约 2 增到约 24–25 周期/块。这些数字包含循环开销，不能
当作龙珠的实际 miss 率或 TCM 的整机加速预测。

重新采样原 11.734 FPS 版本，生成代码/共享访存/分支/分派合计 35.11%，
C 层 `RV32RunLoop` 另占 3.63%，动态样本未知和丢失均为零。优先处理该
执行路径的重复工作和布局；扩大 TCM 不再被视为已证实的首选修复。

已单独排除：块长 16（11.097 FPS）、缓存槽 1024（11.241 FPS）、周期数
和标志位直接驻留（11.273 FPS）。第三项指令量下降 2.304%，仍然更慢。
原精确镜像复测维持 11.734 FPS，周期相差 0.00047%，退休指令完全相同。
最后单独保留“事件检查直接读取周期寄存器”的候选，得到 11.665 FPS，
也未采用。两套各 34 组差分、三个 ROM 的 CRC 全部通过。本轮没有提高
龙珠帧率，生产源码已逐字节恢复到原 11.734 FPS 版本（SHA256
`d3c537f54d5847b146be0e0b4530a9487e9febc52e26c766028115aeae1dd03f`），
仍为每块最多 32 指令、512 个缓存槽。实验参数入口仅保存在候选源码快照中。
每项候选各板测一次，原精确基线跨轮共测两次。

本轮归档为 `build/rv32/gb-comparison/`：`inputs.json` 保存精确镜像、
源码快照及构建配置哈希，`measurement.json` 汇总龙珠与微基准，
`2048-play-results.json` 保存 GB 三平台状态核对，`dragon-profile.json`
保存采样归因。候选源码与差异保留，负收益项不并入当前运行器。
下一步优先量化事件打断后的重复翻译、重叠入口和跨块执行成本；当前采样
还不能将 IPC 损失定量分摊给 cache miss、分支与 LSU 等待。

```sh
python3 build/rv32/gb-comparison/analyze.py
python3 am/tools/gb_benchmark_report.py \
  --native build/rv32/gb-comparison/2048-play-native.log \
  --spike build/rv32/gb-comparison/2048-play-spike.log \
  --board build/rv32/gb-comparison/2048-play-board-1.log \
          build/rv32/gb-comparison/2048-play-board-2.log \
  --out build/rv32/gb-comparison/2048-play-results.json
```

## 2026-10-07：重编译和内部入口复用实验

新增默认关闭的 `RV32_STATS=1`，统计编译、重叠入口、原生分派/直连和回到
C 的原因。预热后清零计数，按 PC 保留是否曾编译的记录；编译时间包含解码、
生成和指令缓存同步，不含诊断用重叠扫描。统计表溢出会报告。诊断会增加
指令并改变布局，不能用其 FPS 代替普通版本的物理性能。

龙珠 30/120 帧的 Spike 与 FPGA 逻辑计数完全一致：完整编译 1,280 次，
重复地址 706 次，450 次入口落在有效块内部；原生分派/直连命中率已为
99.53% / 99.92%。编译边界内 FPGA 周期 33,623,754、退休指令 15,853,044，
只能表示该诊断镜像的成本。

内部入口复用依次试过 C 层恢复、原生入口校验、保留等待循环合并、替换时
清理依赖入口。最后一种将完整编译减至 1,119 次、编译指令减约 19.9%，但
无诊断板测为 11.705 FPS，低于原版 11.734 FPS。独立共享返回候选使同一批
427 个块的静态机器码减少 6.883%，板测为 11.618 FPS。两者均撤回；各候选
源码、两种回退模式的差分与三个 ROM 的 CRC 结果保存在实验目录。

最终只保留诊断工具。最终诊断构建完整 34 组差分通过，龙珠 CRC 为
`3642819F`；Spike ELF 与最初诊断镜像逐字节一致。默认构建的 FPGA 镜像
与已测的 11.734 FPS 原版逐字节一致，SHA256 为
`e51a1d617041c40d83335a3562ef41a4aeb11dc6ccaaa2fda7db0526cd83704a`。
本轮没有帧率提升；后续优先量化热点循环中反复执行的访存、状态同步和
事件检查，当前证据仍不能将损失定量分摊给 cache miss、分支或 LSU。

```sh
make -j8 PLATFORM=spike BUILD_DIR=build/rv32/entry-reuse/final-stats-spike \
  RV32_RUNNER=1 RV32_STATS=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 HEADLESS=1 AUDIO=0 BENCHMARK=1 \
  all test-rv32 SPIKE_PLUGIN=build/rv32/spike/protosoc.so
/home/tillsat/tools/spike-20feb9c2/bin/spike \
  --isa=rv32im_zicsr_zifencei_zicbom --priv=m -m0xa0000000:0x04000000 \
  --extlib=build/rv32/spike/protosoc.so --device=am_protosoc,0 --device=am_media \
  build/rv32/entry-reuse/final-stats-spike/mgba.elf
python3 build/rv32/entry-reuse/analyze.py
```

详细过程见 [process.md](../process.md)。`build/rv32/entry-reuse/` 中的
`inputs.json` 保存输入哈希，`measurement.json` 汇总板测及诊断。

## 音频常量表与跨块循环合并（2026-10-07）

`GBAudioRun` 的两张噪声常量表改为静态存储，省去每次更新复制 256 字节
到栈的工作。新的跨块 Thumb 固定点证明也允许反复写入相同 RAM 内容，
覆盖原有单块只读轮询之外的等待路径。两项均不依赖特定游戏地址。

同一 50 MHz ZCU102、相同 bitstream，龙珠开场 30 帧预热、120 帧测量，
新建 RAM 存档、零按键、软件渲染/RGB/APU，无物理音视频输出；采样与
诊断关闭。CRC 均为 `3642819F`。

| 实现 | FPS | 周期 | 退休指令 |
| --- | ---: | ---: | ---: |
| 原版精确镜像复测 | 11.734 | 511,306,775 | 498,848,129 |
| 仅静态音频表 | 12.180 | 492,599,679 | 474,611,114 |
| 加跨块循环合并 | **13.592** | **441,405,595** | **391,272,978** |

合计提升 **15.8%**，循环合并相对仅音频改动再提升 **11.6%**。
两种解释器回退和开启 `RV32_STATS` 的构建各通过完整 35 组差分，其中
新增 6,912 个跨块事件切片与 192 个长循环加速检查。三个 ROM 的画面
CRC、测量期 PCM 音频 CRC 与最终完整保存状态 CRC 均匹配原版。

跨块检查存在成本。`go` 的 5/30 帧对照从 15.845 降至 15.658 FPS；
`2d-wrap` 从 38.298 降至 36.728 FPS。两者画面 CRC 仍匹配，不能将
龙珠收益推广到所有负载。反向顺序复测 `2d-wrap` 的两版结果一致，
回退约 4.1%；`go` 单次对照回退约 1.2%。此时总体目标仍未完成。

优化后重新上板采样得到 4,666 个样本，生成代码占 28.3%，无丢失或
未知地址。RGB 转换及行循环占 14.1%，噪声音频函数占 5.5%，新增跨块
证明例程占 1.9%。该采样含额外开销，单独归档用于下一步定位，不代替
表中关闭采样的物理计时。

```sh
make -j8 PLATFORM=fpga BUILD_DIR=build/rv32/hot-work/final-fpga \
  RV32_RUNNER=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 all
# 关闭跨块合并的独立对照：另设 BUILD_DIR 并添加 RV32_TRACE_POLL=0
python3 build/rv32/hot-work/analyze.py
```

默认构建与已实测候选逐字节相同，SHA256 为
`11637710c4c07eb2fa97b534ec8e2f9de71c0a9309da6cfd9f5920edfc1bebac`。
归档目录 `build/rv32/hot-work/` 保存源文件快照、日志和哈希；上传失败或
被交互中断的日志不纳入性能结果。串口 `/dev/ttyUSB2` 已通过完整传输、
SDRAM CRC 和应用正常退出验证恢复。

## 扫描线 RGB 输出（2026-10-07）

前端全屏 RGB 转换在上一轮约占 14.1%。`AM_SCANLINE_RGB=1`（默认）把
转换并入 GBA 软件渲染器的脏扫描线输出，并维护现有 RGB 缓冲区；未变化
的行保留已有结果。原生 framebuffer、像素读写、初始化、强制空白和绿色
交换语义保留，每帧继续完整模拟、渲染和输出。GB/GBC 与其他渲染器继续
原来的转换路径；`AM_SCANLINE_RGB=0` 可关闭 RGB 缓冲区关联作对照。

相同 50 MHz FPGA、相同 bitstream、零按键和新建 RAM 存档，软件渲染、
RGB 与 APU 全部保留，无物理输出，无采样或计数诊断：

| ROM（预热/测量帧） | 前一版 FPS | 新版 FPS | 提升 |
| --- | ---: | ---: | ---: |
| Dragonball（30/120） | 13.592 | **16.041** | **18.0%** |
| go（5/30） | 15.658 | **19.837** | **26.7%** |
| 2d-wrap（5/30） | 36.728 | **64.601** | **75.9%** |

三个 CRC 保持 `3642819F`、`EB2AB463`、`C0EA6C09`。新增 renderer 专项
覆盖所有显示模式、混色、完整 RGB 像素、stride、恢复及缓冲区生命周期；
32 位、16 位、RGB565 与 RV32 均通过。三个 ROM 的 150 帧 native 对照
逐帧匹配原生像素、RGB、PCM 和完整状态；额外 RV32 诊断也匹配前轮 PCM
及最终状态。运行器 35 组差分、前端 18 组 smoke 检查通过。

龙珠镜像 SHA256 为
`5efd4f5887c040e401b5b2db7cca2538e55652e2bee105aef590632143c783e0`。
归档及复核脚本位于 `build/rv32/scanline-rgb/`。总体原速目标仍未达到。

新版独立采样共 4,240 个样本，无丢失或未知地址；生成代码约占 35.0%，
`RV32RunLoop` 占 6.4%，两项主要音频函数分别占 6.2% 和 5.5%。软件
扫描线输出占 2.6%，`main` 本次没有样本。下一步优先验证运行时缓存块
元数据布局及原生执行开销，保留现有语义校验。

```sh
make -j8 PLATFORM=fpga BUILD_DIR=build/rv32/scanline-rgb/fpga \
  RV32_RUNNER=1 RUNNER_THREADED=1 GBA_IDLE_SKIP=1 \
  ROM=dragonball WARMUP=30 FRAMES=120 all
make -j8 PLATFORM=native BUILD_DIR=build/rv32/scanline-rgb/native \
  RUNNER_THREADED=1 GBA_IDLE_SKIP=1 test-gba-rgb \
  RGB_TEST_ROMS='roms/dragonball.gba roms/go.gba cinema/gba/obj/2d-wrap/test.gba'
python3 build/rv32/scanline-rgb/analyze.py
```

## 元数据布局对照（2026-10-07）

当前 `RV32Block` 把运行时检查字段集中在前部，块大小 3,552 字节及
`nativeCode` 偏移 480 不变。同一 50 MHz FPGA 的 Dragonball 由 16.041
到 16.085 FPS（单次约 +0.275%）；go 为 19.838、2d-wrap 为 64.600 FPS，
相对上一版基本持平。三款退休指令数在各自 FPGA/Spike 环境内完全不变。
两种回退各 35 组差分通过，完整 RGB、PCM 和状态诊断一致。

这是一次小幅布局实验，不足以说明主要瓶颈已经解决；未测到具体缓存缺失
或分支等待的比例。归档与校验入口：`build/rv32/block-layout/analyze.py`。
板测已完成，最终回到 `/dev/ttyUSB2` 的 `BOOT UART`，连接恢复。

## 复现游戏对照

原解释器与翻译器分别构建，避免覆盖结果：

```sh
make -j8 PLATFORM=spike BUILD_DIR=build/rv32/reference RV32_RUNNER=0 \
  GBA_IDLE_SKIP=1 RUNNER_THREADED=1 ROM=dragonball HEADLESS=1 AUDIO=0 \
  BENCHMARK=1 FRAMES=120 WARMUP=5 all build/rv32/reference/protosoc.so
make -j8 PLATFORM=spike BUILD_DIR=build/rv32/translated RV32_RUNNER=1 \
  GBA_IDLE_SKIP=1 RUNNER_THREADED=1 ROM=dragonball HEADLESS=1 AUDIO=0 \
  BENCHMARK=1 FRAMES=120 WARMUP=5 all build/rv32/translated/protosoc.so
```

使用本机默认安装的 Spike 采集（另一组替换 `translated` 为 `reference`）：

```sh
/home/tillsat/tools/spike-20feb9c2/bin/spike -g \
  --isa=rv32im_zicsr_zifencei_zicbom --priv=m -m0xa0000000:0x04000000 \
  --extlib="$PWD/build/rv32/translated/protosoc.so" \
  --device=am_protosoc,0 --device=am_media build/rv32/translated/mgba.elf \
  >build/rv32/translated.log 2>build/rv32/translated.hist
python3 - <<'PY'
from pathlib import Path
total = 0
for line in Path('build/rv32/translated.hist').read_text().splitlines():
    columns = line.split()
    if len(columns) == 2 and columns[1].isdigit():
        int(columns[0], 16)  # PC
        total += int(columns[1])
print('retired RV32 instructions:', total)
PY
```

先确认运行正常结束且 CRC 一致，再比较指令数。当前工作区的原始对照日志
和 histogram：

- 参考组：Dragonball 为 `build/rv32/resident-reference.{log,hist}`（重新构建
  后与上一轮一致）；其余为 `build/rv32/current-reference-{go,wrap}.{log,hist}`。
- 翻译组：`build/rv32/compact-release-{dragon,go,wrap}.{log,hist}`；测量所用 ELF
  保存在同前缀的 `.elf` 文件中。`build/rv32/compact-release-counts.json` 记录
  指令总数、对照变化、CRC 和 ELF SHA-256。
- 当前轮询下沉后的测量组：`build/rv32/polling-release-{dragon,go,wrap}.{log,hist}`，
  ELF 位于对应目录，`build/rv32/polling-release-counts.json` 记录本轮总数、CRC
  和 ELF SHA-256。
- 上一轮：`build/rv32/edge-release-{dragon,go,wrap}.{log,hist}`。
- 差分测试：`build/rv32/compact-release-0-test.log`（普通解释器回退）、
  `build/rv32/compact-release-1-test.log`（threaded 回退）。
- 运行时算术段验证：`build/rv32/compact-release-verify.log`。
- Native 构建：`build/rv32/compact-release-native-build.log`。
- 当前构建的符号表：`build/rv32/compact-release-{dragon,go,wrap}-symbols.txt`。
  动态生成代码通常不在 ELF 符号表内，归因时应单独统计，不能把未匹配地址
  直接解读为某一种指令的开销。
- 回调分布诊断：`build/rv32/data-load-profile.log`（含临时计数及生成代码区域地址）。
- 块分派诊断：`build/rv32/edge-profile.{log,hist}`（仅增加生成代码地址输出，
  含少量诊断开销，不用于上表性能比较）；生产代码中已移除该临时输出。
- 本轮块布局诊断：`build/rv32/block-profile.{log,hist}`。缓存槽可能重复编译，
  不能把某槽的累计样本直接归给最后一次记录的 GBA 地址；临时输出已移除。
- go 取指区域诊断：`build/rv32/arm-region-go.log`；临时输出已从生产代码移除，
  该诊断构建不用于上表性能比较。

这些产物由 `build/` 的忽略规则管理。
