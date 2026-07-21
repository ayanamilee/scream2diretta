# scream2diretta

**v0.7** · 原生 **Scream UDP → Diretta SDK** 桥接器，适用于 Linux (aarch64, x86-64)。

通过网络 UDP 接收来自远程机器（运行 ASIOScream / ScreamDriver / scream-alsa）的连续无压缩 PCM 流，并直接通过 Diretta Host SDK 转发到支持 Diretta 的 DAC —— **无需 ALSA、FFmpeg、UPnP 或任何中间软件层**。

> **scream2diretta 是 PCM 传输器，不是音乐播放器。**
> 没有曲目边界、没有播放列表、没有 HTTP 客户端、没有解码阶段。所有解码都在上游的 Windows 或 Linux 机器上完成。

---

## 架构

```text
Scream 发送端 (Windows / Linux / macOS)
    │  UDP 单播/组播  端口 4010
    ▼
┌──────────────────────────────────────────────────────┐
│  scream2diretta (Linux 主机)                         │
│                                                      │
│  ┌─────────────────────────────┐                    │
│  │  Scream UDP 接收器          │  ← --cpu-scream    │
│  │  SO_RCVBUF = 4 MiB          │                    │
│  └──────────────┬──────────────┘                    │
│                 │  帧对齐 PCM                       │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  PcmRing (无锁 SPSC)         │  ← 统一队列       │
│  │  抖动/预填充缓冲区             │                    │
│  └──────────────┬───────────────┘                   │
│                 │  getNewStream() 回调              │
│                 ▼                                    │
│  ┌──────────────────────────────┐                   │
│  │  ScreamDirettaSync           │  ← 继承           │
│  │  Diretta SDK 内部工作线程     │    DIRETTA::Sync  │
│  └──────────────────────────────┘                   │
└──────────────────────────────────────────────────────┘
    │  Diretta 协议 (UDP/Ethernet)
    ▼
Diretta Target → DAC
```

### 线程模型

| 线程 | 职责 | 亲和性 |
|------|------|--------|
| Scream UDP 接收器 | 接收数据包，将 PCM 推入 PcmRing | `--cpu-scream` |
| Diretta SDK 工作线程 | 通过 `getNewStream()` 拉取 PCM | `--cpu-audio` (SDK OCCUPIED) |
| 异步打开/清理 | 阻塞式 SDK 生命周期操作 | `--cpu-other` |

稳态热路径是**双线程 SPSC 架构**：接收器将 PCM 推入 `PcmRing`，SDK 工作线程从同一个 `PcmRing` 拉取 PCM。

---

## 为什么需要 scream2diretta？

[**DirettaRendererUPnP (DRUP)**](https://github.com/cometdom/DirettaRendererUPnP) 已经为基于 UPnP 的播放（LMS、Roon、Audirvana、BubbleUPnP 等）提供了出色的音质。**scream2diretta 旨在补充 DRUP**，覆盖 DRUP 原生无法处理的场景：

### 场景 1：非 UPnP 音乐服务（桌面端、移动端、便携播放器）

Apple Music、Spotify 和 Tidal 不支持 UPnP 串流。你无法从这些应用向 DRUP 发送 HTTP 流。

**桌面端（Windows / Linux / macOS）**：对于 Foobar2000、Spotify 桌面版等支持 ASIO / ScreamAlsa 的播放器，直接输出到 **ScreamAlsa**（或 Windows 上的 ASIOScream）。PCM 通过网络 UDP 发送到运行 **scream2diretta** 的设备。

**移动端 / 便携播放器（iPhone、Fiio M21 等）**：将 Apple Music / Spotify 输出到 USB gadget 设备（通常是配置为 USB Audio Class 设备的树莓派）。音频可经过 **CamillaDSP** 进行 DSP 处理，再送入 **ScreamAlsa**，最后传输给 **scream2diretta**。

可以运行在**单机模式**（USB gadget + ScreamAlsa + scream2diretta 在同一台树莓派上），也可以采用**双机模式**（USB gadget/ScreamAlsa 发送端 → 网络 → 独立的 scream2diretta 接收端）。推荐双机模式以获得最佳音质。

```text
桌面端：  Foobar2000 / Spotify → ASIOScream / ScreamAlsa → network → scream2diretta → Diretta Target → DAC

移动端：  iPhone / Fiio M21 → USB gadget (Raspberry Pi)
                                      ↓
                                CamillaDSP (optional)
                                      ↓
                                ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

### 场景 2：HQPlayer / NAA 无需 DirettaAlsaHost

HQPlayer / NAA（Network Audio Adapter）原生不支持 Diretta。传统上，你需要 **DirettaAlsaHost** 作为 ALSA 插件中间层，将 NAA 输出桥接到 Diretta 目标设备。

**解决方案**：将 NAA 配置为输出到 **ScreamAlsa**。PCM 流通过网络发送到 **scream2diretta**，完全绕过 ALSA 层，直接送入 Diretta 目标设备。

```text
HQPlayer → NAA → ScreamAlsa → network → scream2diretta → Diretta Target → DAC
```

这消除了 DirettaAlsaHost 引入的 ALSA 周期/缓冲区/唤醒抖动。

> **注意：** HQPlayer 和 NAA 的设计初衷是与音频硬件直接交互。此链路并非 HQPlayer 的推荐使用方式，而是在没有原生 NAA→Diretta 通路时，除 DirettaAlsaHost 之外的另一种选择，用于将 HQPlayer 升频后的 PCM 传输至 Diretta Target。

### 场景 3：UPnP 流的 DSP 处理

DRUP 直接接收 HTTP 流并传递给 Diretta SDK。然而，在这条路径中**没有方便的 DSP 处理钩子**。

**解决方案**：使用 **upmpdcli + MPD** 作为 UPnP 渲染器。配置 MPD 输出到 **ScreamAlsa**，**CamillaDSP** 作为 DSP（EQ、分频、房间校正等）的 PCM 管道。处理后的 PCM 再发送到 **scream2diretta**。

```text
LMS/Roon → upmpdcli → MPD → CamillaDSP → ScreamAlsa
                                               ↓
                                          network
                                               ↓
                                        scream2diretta → Diretta Target → DAC
```

这让你获得完整的 UPnP 兼容性**并支持** DSP 处理，这是仅用 DRUP 难以实现的。

你也可以使用 **aprenderer / aplayer**（Windows / Linux）作为 Scream 发送端，直接播放给 **scream2diretta**。

---

## 编译

### 依赖

- C++17 编译器 (gcc 11+, clang 13+)
- CMake 3.7+
- Diretta Host SDK (如 `DirettaHostSDK_149`) —— 获取方式参见 [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) 或 [slim2Diretta](https://github.com/cometdom/slim2Diretta) 项目主页

如需自行编译 **ScreamAlsa**，请前往 [ScreamAlsa 项目](https://github.com/Scream-Projects/scream-alsa)。

### 方法一：安装脚本（推荐）

交互式安装脚本会自动处理依赖、检测架构、编译二进制文件，并可选安装 systemd 服务。

```bash
bash scripts/install.sh
```

**功能：**
- 安装编译依赖 (gcc, cmake, pkg-config)
- 根据 CPU 标志 / 页大小自动检测 `DIRETTA_ARCH_SUFFIX`
- 在项目目录、上级目录、`$HOME`、`/opt`、`/usr/local` 中搜索 Diretta SDK
- 编译 `scream2diretta`
- 可选安装 systemd 服务并选择目标设备

**非交互式命令行快捷方式：**
```bash
bash scripts/install.sh --full      # 完整安装（含服务）
bash scripts/install.sh --build     # 仅编译
bash scripts/install.sh --update    # 重新编译并更新已安装的二进制文件
bash scripts/install.sh --test      # 验证安装并列出目标设备
bash scripts/install.sh --uninstall # 移除二进制文件和服务
```

### 方法二：手动编译

```bash
mkdir build && cd build

# 基础编译 (自动检测架构)
cmake -DDIRETTA_ENABLE=ON -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 ..
make -j$(nproc)

# 完整编译 (指定架构、关闭 SDK 日志)
cmake -DDIRETTA_ENABLE=ON \
      -DDIRETTA_SDK_ROOT=../DirettaHostSDK_149 \
      -DDIRETTA_ARCH_SUFFIX=aarch64-linux-15k16 \
      -DDIRETTA_NOLOG=ON \
      ..
make -j$(nproc)
```

### 架构变体

| `DIRETTA_ARCH_SUFFIX` | 平台 |
|-----------------------|------|
| `x64-linux-15v2` | x86-64 基线 (无 AVX2) |
| `x64-linux-15v3` | x86-64 AVX2 (默认) |
| `x64-linux-15v4` | x86-64 AVX-512 |
| `x64-linux-15zen4` | AMD Zen 4 |
| `aarch64-linux-15` | ARM64, 4KB 页 |
| `aarch64-linux-15k16` | ARM64, 16KB 页 (树莓派 5) |
| `riscv64-linux-15` | RISC-V 64-bit |
| `*-musl*` | musl libc 变体 (如 `aarch64-linux-15-musl`) |
| `*-nolog` | 禁用 SDK 内部日志 (附加到任意变体) |

#### 如何确定你的 `DIRETTA_ARCH_SUFFIX`

**x86-64:**
```bash
# 查看 CPU 指令集标志
grep -m1 '^flags' /proc/cpuinfo
```
- 包含 `avx512f` → 使用 `x64-linux-15v4`
- 包含 `avx2` 但 **不含** `avx512f` → 使用 `x64-linux-15v3` (最常见)
- 两者皆无 → 使用 `x64-linux-15v2`

**ARM64:**
```bash
getconf PAGE_SIZE
```
- `16384` → `aarch64-linux-15k16` (树莓派 5)
- `4096` → `aarch64-linux-15` (大多数其他 ARM64 开发板)

**musl libc:** 如果你的系统使用 musl 而非 glibc (如 Alpine Linux)，在基础变体后附加 `-musl`。

**生产构建:** 附加 `-nolog` 以禁用 SDK 内部日志 (如 `aarch64-linux-15-nolog`)，或传入 `-DDIRETTA_NOLOG=ON`。

**静态链接:** CMake 默认生成动态链接可执行文件。如需完全静态链接的二进制，请设置 `LDFLAGS` 或使用 `scripts/install.sh --full`，该脚本会处理静态链接并剥离二进制。

输出：`scream2diretta-<DIRETTA_ARCH_SUFFIX>` (例如 `scream2diretta-aarch64-linux-15k16`)

---

## 运行

### 基本用法

```bash
# 列出可用的 Diretta 目标设备
sudo ./scream2diretta --list-targets

# 使用第一个目标设备运行（默认组播 239.255.77.77:4010）
sudo ./scream2diretta

# 绑定目标 #1，单播端口 4010，每 5 秒显示详细统计
sudo ./scream2diretta -t 1 -p 4010 -vv --stats --stats-interval 5

# 使用 CPU 亲和性（推荐用于最佳音质）
sudo ./scream2diretta -t 1 -p 4010 \
  --cpu-scream 2 --cpu-audio 3 --cpu-other 1 \
  --thread-mode 1 --transfer-mode auto
```

### 参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--target, -t <index>` | 1 | 按 1-based 索引选择 Diretta 目标设备 |
| `-L` | 关闭 | 使用原始 5 字节 Scream 头（legacy 模式）。ap2renderer、ASIOScream 以及旧版 scream-alsa 会发送字节交织的 DSD 和旧的 rate 编码，需要加此开关。 |
| `--list-targets` | — | 发现并列出目标设备 |
| `-p <port>` | 4010 | Scream UDP 端口 |
| `-i <iface>` | — | 绑定到指定网络接口 |
| `-g <group>` | 239.255.77.77 | 组播组地址（仅组播模式） |
| `--thread-mode <mask>` | 1 (CRITICAL) | SDK 线程模式位掩码 |
| `--transfer-mode <mode>` | auto | auto / varmax / varauto / fixauto / autofix / random（见[传输模式](#传输模式)） |
| `--mtu <bytes>` | auto | 网络 MTU（默认自动检测，巨型帧通常为 9000） |
| `--pcm-buffer-ms <ms>` | 1000 | PcmRing 总大小 |
| `--pcm-prefill-ms <ms>` | 500 | 首次拉取前的填充阈值 |
| `--rebuffer-percent <pct>` | 50 | 欠载恢复阈值 |
| `--udp-rcvbuf-bytes <bytes>` | 4194304 | 内核 SO_RCVBUF |
| `--cpu-scream <core>` | — | 将接收线程绑定到指定核心 |
| `--cpu-audio <core>` | — | 将 SDK 工作线程绑定到指定核心 |
| `--cpu-other <core>` | — | 将辅助线程绑定到指定核心 |
| `--stats --stats-interval <sec>` | off | 周期性统计 |
| `-v` / `-vv` | off | 详细 / 非常详细 |

### 传输模式

`--transfer-mode` 决定 s2d 如何配置 Diretta SDK 的发送 profile。
所有模式都保持**每个 cycle 只发一个 packet**的不变量：1-packet
物理上界是 `varmax_cycle`（在当前格式下恰好填满一个 `effMtu`
的周期）；显式 `--cycle-time` 还会额外与 `safe_max =
varmax_cycle x 0.97`（3% 裕度，用于吸收 overhead 推断抖动）比较，
以防 `cycle_packets` 静默涨到 2。

| 模式 | 无 `--cycle-time` | 有 `--cycle-time` |
|------|-------------------|--------------------|
| `auto` | 低码率（≤16bit 且 ≤48k）或 DSD → VarAuto；否则 VarMax | `cycle ≤ safe_max` → **VarAuto**(cycle)（`auto-varauto-cycle`）；否则 VarMax-override + `[warn]`（`auto-varmax-override`） |
| `autofix` | 同 `auto`（低码率/DSD → VarAuto，否则 VarMax） | `cycle ≤ safe_max` → **FixAuto**(cycle)（`autofix-fixauto`）；否则 VarMax-override + `[warn]`（`autofix-varmax-override`） |
| `varmax` | VarMax(cycle) | VarMax(cycle) |
| `varauto` | VarAuto(cycle) | VarAuto(cycle) |
| `fixauto` | FixAuto(cycle) | FixAuto(cycle) |
| `random` | Random(cycle, cycle-min) | Random(cycle, cycle-min) |

**`auto` 与 `autofix` 在带 `--cycle-time` 时的唯一区别**：两者都在
1-packet 上界内承载同一个目标周期，但锚定方式不同：

- **`autofix` 用 FixAuto**（周期-锚定）：周期被逐字 honor。实测
  `sdk_cycle` 与 `target_cycle` 精确相等（例如 800us → 800us）。
- **`auto` 用 VarAuto**（size-锚定）：SDK 锚定负载 size，周期退化
  为按帧量化的副产物。实测中，当单 packet 能装下时偏差可
  忽略（例如 800us → 803us，约 0.4%，零 underrun）。

两者都落在 `cycle_packets=1`。如果你希望 SDK 报告的周期与你请求值
精确一致，用 `autofix`；其余情况用 `auto`（默认）。

在 Target Profile 激活时（`--target-profile-limit > 0`），`autofix`
等同 `fixauto`（`pm.configTransferFixAuto(cycle)`）。

#### 读懂 `transfer:` 日志行（`-vv`）

```
transfer: mtu=1518 mode=auto-varauto-cycle mode_sdk=variable target_cycle=800us sdk_cycle=803us cycle_size=616B cycle_packets=1
```

- `mode` — s2d 的决策字符串（走了哪个分支，如 `auto-varauto-cycle`、
  `autofix-fixauto`、`auto-varmax-override`）。
- `mode_sdk` — `Sync::getMode()` 的读回：SDK 把我方 config 量化后实际
  采用的发送 profile 模式（`variable` / `fix` / `random` / `triangolo`）。
  可用于核对，例如 `autofix-fixauto` → `mode_sdk=fix`。
- `target_cycle` — s2d 下给 SDK 的周期（**指令**）。
- `sdk_cycle` / `cycle_size` / `cycle_packets` — SDK 协商好的发送
  profile **快照**读回（在 open / 重协商时确定）。
- `min_cycle` — `Sync::getMinCycleTime()` 的读回，**仅当 > 0 时才打印**。
  SelfProfile（`--target-profile-limit 0`）下该值为 0，字段被抑制以
  避免噪声。

注意：所有读回 getter 暴露的是**本机侧协商好的发送 profile**，而非
 target 实时遥测；它们只在格式重协商时变化。

### 音质调优

1. **在发送端和接收端都启用 chrony**，对齐系统时钟，消除 PCM 环漂移：
   ```bash
   sudo apt install chrony
   sudo systemctl enable chrony --now
   ```

2. **使用 CPU 亲和性**隔离音频热路径：
   ```bash
   --cpu-scream 2 --cpu-audio 3 --cpu-other 1
   ```

3. **如果网络支持，使用巨型帧**（MTU 9000）以降低数据包开销。

---

## Scream 协议

Scream 通过 UDP 发送原始无压缩 PCM 或 DSD。s2d 同时支持**扩展版 6 字节 ScreamALSA 头**（默认）和**原始 5 字节 Scream 头**（`-L` legacy 模式）。

### 扩展头（默认）

当前 ScreamALSA 驱动使用。支持 PCM 16/24/32-bit 以及最高 DSD512 的 DSD。

```
Byte 0:       采样率倍频（低 8 位）
Byte 1:       采样位深（16 / 24 / 32，或 1 表示 DSD）
Byte 2:       声道数
Byte 3:       声道映射
Byte 4:       位 3:0 = 采样率倍频高位
              位 4    = 0 -> 48k 族，1 -> 44.1k 族
              位 7    = 曲目结束标记
Byte 5:       wire_layout（0 = 打包 S24_3LE，1 = S24_LE 4 字节容器）
Byte 6+:      交错采样
```

24-bit PCM 的 `wire_layout` 字节区分两种容器：
- `0` — 打包 3 字节小端（`S24_3LE`）。这是 Diretta SDK 以
  `FMT_PCM_SIGNED_24` 原生消费的格式。
- `1` — 32-bit 小端容器中的 24-bit 样本（`S24_LE`）。s2d 在入队前
  剥离填充字节，使队列中仍是打包的 S24_3LE。该转换无损，并在
  接收线程中完成。

### Legacy 头（`-L`）

原始 Scream 驱动、ap2renderer 和 ASIOScream 使用。

```
Byte 0: 采样率代码（>=128 -> 44100*(code-128)，否则 48000*code）
Byte 1: 采样位深（16 / 24 / 32，或 1 表示 DSD）
Byte 2: 声道数
Byte 3: 声道映射低字节
Byte 4: 声道映射高字节
Byte 5+: 交错采样
```

Legacy DSD 以字节交织方式发送；s2d 在交给 Diretta SDK 之前会将其
反交织为标准 ALSA `DSD_U32_BE` 的字顺序。

- 无序列号，无重传 —— 纯 UDP。
- 格式变化通过头字节变化检测。
- 连续流，无曲目边界。

---

## 致谢

- **[DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP)** by cometdom —— 缓冲区参数命名（`--pcm-buffer-ms`、`--pcm-prefill-ms`）、`RingAccessGuard` 模式和 CPU 亲和性设计的主要参考。DRUP 为基于 UPnP 的 Diretta 播放设立了高标准，scream2diretta 的存在是为了补充 DRUP 在非 UPnP 场景下的使用。
- **[slim2Diretta](https://github.com/cometdom/slim2Diretta)** by cometdom —— `DirettaSync` / `DirettaRingBuffer` / `getNewStream()` 拉取模型的主要架构参考。
- **[duncanthrax/scream](https://github.com/duncanthrax/scream)** —— Scream 接收器代码衍生自该项目的 Unix 接收器。
- **Yu Harada** —— Diretta SDK。

---

## 许可证

见 [LICENSE](LICENSE)。
