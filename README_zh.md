# mGBA AM 验证播放器

这个仓库在 `am/` 接口之上构建了一个精简的 mGBA 播放器，用来在三种环境中
验证同一套模拟器和软件设备：

| 场景 | 执行 mGBA 的 CPU | 设备路径 | 用途 |
| --- | --- | --- | --- |
| Native | Linux 主机 CPU | SDL2 | 快速参考和交互调试 |
| Spike | Spike 中的 RV32IM | 模拟 AM 设备 -> SDL2 | 验证 RV32 软件和设备协议 |
| 自己的核 + ysyxSoC | ysyxSoC Verilator wrapper 中的目标核 | ysyxSoC GPIO + 模拟媒体设备 -> SDL2 | 验证 CPU/SoC 集成 |

`0x40000000` 是仿真媒体设备地址：它把像素提交到主机窗口，并接收 PCM
音频数据。这不表示 SoC 已经实现了物理显示或音频硬件。mGBA 指令、游戏
模拟、软件渲染和音频生成都在目标 CPU 上执行。

下面的三条路径验证的是功能交互，ysyxSoC Verilator 仿真耗时不能代表
游戏实时性能。

英文版本见 [README.md](README.md)。

## 最小运行命令

每条命令都会在需要时构建播放器，然后启动对应的交互路径。Native 和 Spike
默认使用 `roms/dragonball.gba`；ysyxsoc 默认使用仓库自带的 4 KiB 测试 ROM
`cinema/gba/obj/2d-wrap/test.gba`，画面为白色和绿色方块，以缩短 Flash 启动
加载时间。默认值定义在根目录的 `Makefile` 中。使用其他 GBA 游戏时只需传入
游戏名，例如 `ROM=go` 会加载 `roms/go.gba`；GB/GBC 游戏可以保留扩展名，例
如 `ROM=game.gb`。临时测试仍可传入完整路径。

### Native + SDL

```sh
make PLATFORM=native run
```

### Spike + 模拟设备 + SDL

```sh
make PLATFORM=spike run
```

Spike 需要支持 RV32IM/ILP32 的 `riscv64-unknown-elf-` 工具链、Spike 头文件
和库、C++20 编译器、Python 3、pkg-config 以及 SDL2 开发包。第一次交叉编译
会在 `build/newlib/` 下准备固定版本的 Newlib。默认的 `SPIKE_PREFIX` 定义在
`am/platform/spike.mk` 中。如果 Spike 安装在其他位置，可以在执行 Make 时
传入变量覆盖：

```sh
make PLATFORM=spike SPIKE_PREFIX=/path/to/spike run
```

### 自己的核 + ysyxSoC

```sh
make PLATFORM=ysyxsoc \
  YSYX_NPC=/path/to/your-core/soc/ysyxsoc \
  YSYX_SOC=/path/to/ysyx/ysyxSoC \
  run
```

`YSYX_NPC` 和 `YSYX_SOC` 没有默认值，需要显式指定。

## 按平台划分的变量

### 通用变量

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `PLATFORM` | 构建目标：`native`、`spike` 或 `ysyxsoc` | `native` |
| `ROM` | `roms/` 下的游戏名，裸机平台会将 ROM 嵌入程序；省略扩展名时默认补 `.gba` | Native / Spike：`dragonball`；ysyxsoc：`cinema/gba/obj/2d-wrap/test.gba` |
| `BUILD_DIR` | 构建输出目录 | `build` 或平台专用目录 |

### Native

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `ARGS` | 传给 Native 播放器的选项，例如 `--headless --frames 120` | 空 |

Native 通过 `ARGS` 接收运行参数。例如，不开窗口和音频，运行 120 帧后退出：

```sh
make PLATFORM=native run ARGS="--headless --frames 120"
```

| 写在 `ARGS` 中的参数 | 作用 |
| --- | --- |
| `--frames N` | 按最快速度运行 N 帧后退出，输出最后一帧的 CRC32 |
| `--headless` | 关闭窗口和音频，配合 `--frames N` 使用 |
| `--benchmark N` | 在无窗口、无音频的情况下测量 N 帧的运行性能 |
| `--warmup N` | 配合 `--benchmark` 使用，先运行 N 帧预热再开始计时；默认 30 帧 |

`HEADLESS`、`AUDIO`、`FRAMES` 这三个 Make 变量不会控制 Native。
Native 交互运行默认启用音频，目前没有单独关闭音频的选项。

### Spike

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `HEADLESS` | 禁用窗口、音频和主机节拍 | `0` |
| `AUDIO` | 是否启用模拟音频输出 | `1` |
| `FRAMES` | 帧数限制，`0` 表示不限帧 | 交互运行时为 `0`；无画面时必须为正数 |
| `CROSS` | RV32 GCC 前缀 | `riscv64-unknown-elf-` |
| `NEWLIB_ROOT` | Newlib 构建和安装缓存目录 | `build/newlib` |
| `SPIKE_PREFIX` | Spike 安装目录，可在 Make 命令行覆盖 | 定义在 `am/platform/spike.mk` |
| `GPIO` | Spike 的固定输入掩码 | `0` |

Spike 的游戏存档只在本次运行中有效。

### 自己的核 + ysyxSoC

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `HEADLESS` | 禁用窗口、音频和主机节拍 | `0` |
| `AUDIO` | 是否启用模拟音频输出 | `1` |
| `FRAMES` | 帧数限制，`0` 表示不限帧 | 交互运行时为 `0`；无画面时必须为正数 |
| `CROSS` | RV32 GCC 前缀 | `riscv64-unknown-elf-` |
| `NEWLIB_ROOT` | Newlib 构建和安装缓存目录 | 平台管理 |
| `YSYX_NPC` | 自己的核的 ysyxSoC wrapper 目录 | 必填 |
| `YSYX_SOC` | 包含 `build/ysyxSoCFull.v` 的 ysyxSoC 源码目录 | 必填 |
| `AM_MEDIA_ROOT` | 传给 wrapper 的内部 media bridge 源码路径 | 自动设置 |
| `AM_MEDIA_NATIVE` | 传给 wrapper 的内部主机 SDL 对象 | 自动设置 |
| `MAX_CYCLES` | ysyxSoC Verilator 周期上限，`0` 表示不限 | 根据 `FRAMES` 决定 |

ysyxSoC 的游戏存档只在本次运行中有效。

## 按键和 GPIO 映射

SDL 主机按键先映射为 AM 逻辑按键。Spike 和 ysyxSoC 路径读取同一个
十位 GPIO 值。某一位为 1 表示对应按键处于按下状态。

| AM 按键 | 主机按键 | GPIO 位 |
| --- | --- | ---: |
| Up | 上方向键 | 0 |
| Down | 下方向键 | 1 |
| Left | 左方向键 | 2 |
| Right | 右方向键 | 3 |
| A | Z | 4 |
| B | X | 5 |
| L | A | 6 |
| R | S | 7 |
| Start | Enter | 8 |
| Select | Backspace | 9 |

Q、Escape 和关闭窗口都会请求退出。窗口失去焦点时，十个游戏按键全部释放。
逻辑 AM 按键在 `src/platform/am/player.c` 中转换为 GBA 按键，CPU 不会直接
读取 SDL 事件。两个 guest driver 都从 `0x10002004` 读取 GPIO 输入。Spike
可以用 `GPIO=0x10` 预先拉高 A 键，同时仍然接受 SDL 输入。
