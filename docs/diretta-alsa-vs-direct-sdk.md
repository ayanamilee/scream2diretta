# Diretta 音频通路架构对比：ALSA vs Direct-SDK vs MemoryPlay

> 本文对比三种 Diretta 音频通路方案在架构、控制粒度和最终音质上的差异：
> 1. **DirettaAlsaHost** — 官方 ALSA 桥接方案
> 2. **Direct-SDK**（DRUP / slim2Diretta / scream2diretta）— 直接 SDK 方案
> 3. **MemoryPlay** — 官方 Direct-to-SDK 文件播放器方案
>
> 基于源码分析：DirettaAlsaHost `alsa_bridge.c` / `syncalsa_setting.inf`、DRUP `DirettaSync.cpp` / `DirettaRingBuffer.h`、s2d `diretta_ring.h` / `diretta_sync.cpp`、MemoryPlayControllerSDK `WAV.cpp` / `FlacDecode.cpp`。

---

## 一、架构总览

### DirettaAlsaHost（官方 ALSA 方案）

```
播放软件 (aplay / mpd / RoonBridge)
    │  snd_pcm_writei()
    ▼
┌─────────────────────────────────────┐
│  ALSA 用户空间库 (libasound)         │
└─────────────┬───────────────────────┘
              │ 系统调用
              ▼
┌─────────────────────────────────────┐
│  kernel: alsa_bridge.ko             │
│  copy_from_user()                   │
│  wait_event_interruptible_timeout() │  ← 100ms 粒度
└─────────────┬───────────────────────┘
              │ 共享内存 da_sync_mem
              ▼
┌─────────────────────────────────────┐
│  syncAlsa 守护进程                  │
│  轮询共享内存 rd/wd 指针            │
└─────────────┬───────────────────────┘
              │ setStream() push
              ▼
┌─────────────────────────────────────┐
│  Diretta SDK                        │
│  (DIRETTA::Sync / SyncBuffer)       │
└─────────────┬───────────────────────┘
              │ Diretta Protocol
              ▼
        Diretta Target → DAC
```

### DRUP / slim2Diretta / s2d（直接 SDK 方案）

```
网络音频源
    │  HTTP (DRUP) / SlimProto (slim2D) / Scream UDP (s2d)
    ▼
┌─────────────────────────────────────┐
│  解码 / 接收线程                     │
│  (FFmpeg / Scream receiver)         │
└─────────────┬───────────────────────┘
              │ 裸 PCM
              ▼
┌─────────────────────────────────────┐
│  Lock-free Ring Buffer              │  ← SPSC 无锁队列
│  (DirettaRingBuffer / PcmRing)      │
└─────────────┬───────────────────────┘
              │ getNewStream() pull
              ▼
┌─────────────────────────────────────┐
│  DIRETTA::Sync                      │
│  (SDK 内部 worker 线程)              │
└─────────────┬───────────────────────┘
              │ Diretta Protocol
              ▼
        Diretta Target → DAC
```

### MemoryPlay（官方 Direct-to-SDK 文件播放器）

```
本地音频文件 (WAV/AIFF/FLAC/DSD)
    │
    ▼
┌─────────────────────────────────────┐
│  MemoryPlayController (开源)         │
│  文件解析 / 解码 / DSD bit-pack      │
│  (WAV/AIFF 直通, FLAC 预解码,       │
│   DSD bit-reverse + pack)           │
└─────────────┬───────────────────────┘
              │ TCP + 自研分帧协议
              │ (FormatID + 裸 PCM/DSD)
              ▼
┌─────────────────────────────────────┐
│  MemoryPlayHost (闭源)               │
│  TCP server → 内部缓冲 → DIRETTA::Sync│
│  无 ALSA 层, 无 syncBufferCount      │
└─────────────┬───────────────────────┘
              │ Diretta Protocol
              ▼
        Diretta Target → DAC
```

---

## 二、核心差异：Push vs Pull

| 维度 | DirettaAlsaHost | DRUP / s2d | MemoryPlay |
|------|-----------------|------------|------------|
| **SDK 接口** | `setStream()` push | `getNewStream()` pull callback | `getNewStream()` pull callback（推断） |
| **数据流向** | syncAlsa 主动推给 SDK | SDK worker 主动来取 | Host 内部缓冲后 SDK 拉取 |
| **时钟主导** | syncAlsa 按 ALSA period 节奏推送 | SDK 按 target 需求节奏拉取 | Host 内部定时器 / SDK pull |
| **ALSA 层** | **有**（kernel driver + 共享内存） | **无**（完全用户态） | **无**（完全用户态） |
| **音频来源** | 任意 ALSA 应用 | 网络流（HTTP/UDP） | 本地文件（WAV/AIFF/FLAC/DSD） |
| **传输协议** | 共享内存 + 内部 IPC | UDP（s2d）/ HTTP（DRUP） | TCP + 自研分帧协议 |

DirettaAlsaHost 的 ALSA 层是不可绕过的：
- `alsa_bridge.c: snd_card_diretta_pcm_copy()` 使用 `copy_from_user` 把用户态音频数据拷贝进 kernel buffer
- 使用 `wait_event_interruptible_timeout(node->sync, ..., HZ/10)` 做 period 唤醒，粒度约 100ms
- `da_sync_mem` 共享内存结构中的 `rd`/`wd` 指针是 syncAlsa daemon 和 kernel driver 之间的同步原语

这一层引入了两个不可避免的抖动源：
1. **内核态拷贝** (`copy_from_user`) 和上下文切换开销
2. **period-based 唤醒** 的 scheduling granularity（ALSA period 通常 10-50ms）

DRUP/s2d 没有这层。数据从网络/解码器直进用户态 ring，SDK worker 通过 `getNewStream()` 直接取走。

---

## 三、为什么参数相同但效果不同

DirettaAlsaHost 和 DRUP 都有 `--thread-mode`、`--cycle-time`、`--buffer`、`--cpu-audio` 等参数，但**控制的对象不同**：

| 参数 | DirettaAlsaHost 控制对象 | DRUP/s2d 控制对象 |
|------|--------------------------|-------------------|
| `ThredMode` | syncAlsa 守护进程的 SDK 实例 | 直接控制 SDK worker 线程 |
| `CycleTime` | ALSA bridge 到 SDK 之间的发包周期 | SDK 到 target 的发包周期 |
| `CpuSend` | syncAlsa 的 CPU 亲和 | Diretta worker 的 CPU 亲和 |
| `syncBufferCount` | **ALSA Bridge 与包传输之间的 buffer** | **不存在这个参数** |

关键发现：`syncBufferCount` 只在 DirettaAlsaHost 的配置中出现。`readme.txt` 描述它为 *"Buffer count between ALSA Bridge and packet transmission"*。这说明即使把 SDK 参数调到最优，ALSA Bridge 这一层仍然是独立的、不可绕过的缓冲和同步点。

DRUP/s2d 的参数直接作用于「数据 → Diretta SDK」这一最短路径，中间没有 kernel driver、没有共享内存、没有 sync daemon。

---

## 四、控制粒度：DRUP 的精细调优

DRUP `DirettaSync.cpp` 中有大量 DirettaAlsaHost 不具备的精细控制：

### 1. SCHED_FIFO 实时调度
```cpp
bool setRealtimePriority(int priority = 50) {
    struct sched_param param;
    param.sched_priority = priority;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}
```
Diretta SDK worker 被提升到 SCHED_FIFO，减少被内核调度器抢占的 jitter。

### 2. CPU 亲和绑定
```cpp
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```
`--cpu-audio` 直接绑定到指定核心，配合 `THRED_MODE::OCCUPIED`。

### 3. 多门限启动策略
DRUP 的 `getNewStream()` 实现了 prefill → post-online stabilization → rebuffering 多级门限，避免 startup click。

### 4. SIMD 格式转换
AVX2/NEON 优化的 24-bit pack、16→32 upsample、DSD interleave，减少 CPU 负载和热噪声。

### 5. 显式 Lock-free Readers-Writer 协议
`RingAccessGuard` + `ReconfigureGuard` 实现了无锁的并发重配置保护（详见下一节）。

---

## 五、RingAccessGuard vs PcmRing：Lock-free 的两种实现

### DRUP：显式 Readers-Writer 协议

DRUP 的音频来源是**多线程**（UPnP HTTP 接收 + FFmpeg 解码可能并发写入），且格式切换/重配置可能随时从控制线程发起。因此需要显式协议保护 ring：

```cpp
class RingAccessGuard {
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) return;
        users_.fetch_add(1, std::memory_order_acq_rel);  // 宣布"我正在用"
        if (reconfiguring.load(std::memory_order_acquire)) {
            users_.fetch_sub(1, std::memory_order_relaxed);  // 双检：被抢占了，回退
            return;
        }
        active_ = true;
    }
    ~RingAccessGuard() {
        if (active_) users_.fetch_sub(1, std::memory_order_release);  // 宣布"我用完了"
    }
};
```

对应的 writer 侧：
```cpp
void beginReconfigure() {
    m_reconfiguring.store(true, std::memory_order_release);   // 关闭入口
    while (m_ringUsers.load(std::memory_order_acquire) > 0) { // 等所有读者退出
        std::this_thread::yield();
    }
}
```

这是一个完整的无锁 readers-writer 协议：
- `reconfiguring` = 写锁 gate
- `users_` = 读者引用计数
- `acq_rel` 保证 ring 操作 happen-before 计数归零

### s2d：简化 SPSC + 消费者侧 reconfig guard

s2d 的 `PcmRing` 是**严格单生产者单消费者**：

- Producer：唯一的 Scream UDP 接收线程
- Consumer：唯一的 Diretta SDK send 线程
- `PcmRing` 本身是纯 SPSC：只有 `m_writePos` / `m_readPos` 两个原子位置，无读者计数器。
- 但在 `ScreamDirettaSync::getNewStream()` 进入 ring 前加了 `RingUserGuard`（两阶段检查 + 读者计数），`deactivate()` 先置 `m_active=false` 再自旋等待计数归零。这样 reconfig/resize 时无需 stop/join consumer，receiver 线程可以持续 push 数据。

```cpp
alignas(64) std::atomic<size_t> m_writePos{0};   // 仅生产者写
alignas(64) std::atomic<size_t> m_readPos{0};    // 仅消费者写
```

没有 `reconfiguring` 标志或 writerside guard，因为**生产者只有一个且永远不会在 reconfig 时停止**。消费者侧用 `RingUserGuard` + `deactivate()` 关闭 in-flight cycle 窗口。

### 对比

| 维度 | DRUP (RingAccessGuard) | s2d (PcmRing) |
|------|------------------------|---------------|
| **并发模型** | 多生产者可能并发 + 控制线程重配置 | 严格 SPSC |
| **重配置安全** | 运行时无锁协议（producer + consumer 两侧 guard） | 消费者侧 `RingUserGuard` + `deactivate()`；producer 永不停 |
| **额外原子开销** | 每次热路径 2 次（add/sub） | consumer 侧 2 次（add/sub），producer 无额外开销 |
| **通用性** | 高，适配任意多线程音频源 | 低，绑定 Scream 单线程模型 |
| **代码复杂度** | 需 guard + 双检 + yield 等待 | producer 只需 read/write pos + mask；consumer 加一个 guard |

**结论**：DRUP 的 guard 是「多线程混乱世界里的防弹衣」；s2d 的 SPSC 是「把问题消灭在架构设计层面，不需要防弹衣」。两者都是各自场景下的正确选择。

---

## 六、MemoryPlay 详解

MemoryPlay 是 Diretta 官方推出的**绕过 ALSA** 的文件播放器方案，由两部分组成：
- **MemoryPlayControllerSDK**（开源）：负责文件解析、解码、格式转换，通过 TCP 发送裸 PCM/DSD
- **MemoryPlayHost**（闭源）：接收 TCP 数据流，直接送入 Diretta SDK，无 ALSA 层

### 6.1 架构定位

从 `memoryplayhost_setting.inf` 可见，Host 的配置中**没有 `syncBufferCount`**——这是 DirettaAlsaHost 特有的 ALSA Bridge buffer 参数。MemoryPlayHost 和 DRUP/s2d 一样，属于 **Direct-to-SDK** 架构。

Controller → Host 的协议是**自研分帧 TCP**：
- `Data` 帧：裸 PCM/DSD payload，前缀 `FormatID`
- `Command` 帧：控制指令（Connect/Seek/Play/Pause）
- `Tag` 帧：曲目标题和边界标记（`@@Diretta-TAG-QUIT@@` / `@@Diretta-TAG-LOOP@@`）

Controller 还实现了显式确认流控：发送数据后等待 Host 返回 `DataStack` / `DataTag` ACK，未收到则阻塞重试。

### 6.2 Controller 端解码策略

| 格式 | 解码方式 | 输出位深 | 备注 |
|------|----------|----------|------|
| **WAV (RIFF)** | **直通** — 直接读 `data` chunk | 原始位深（8/16/24/32） | 无需解码库 |
| **AIFF** | **直通** — 读 `SSND` chunk | 原始位深（8/16/24/32） | 做大端→小端 byte-swap |
| **FLAC** | **libFLAC++ 预解码**到内存 | 原始位深（8/16/24/32） | 一次性整文件解码 |
| **DSF** | **Bit-reverse + pack** | 32-bit DSD 打包字 | 1-bit → bit-reverse → 32-bit pack |
| **DFF** | **Bit-pack** | 32-bit DSD 打包字 | 1-bit → 直接 32-bit pack |

**关键澄清**：WAV/AIFF/FLAC 解码后**保持原始位深**，不是都变成 32-bit。只有当 Controller 以 `--null` 或 `ch2bit32` 模式运行时，才会把 8/16/24-bit 样本**左对齐提升到 32-bit**（例如 16-bit 值左移 16 位）。默认行为是透传原始位深。

### 6.3 DSD Bit-packing 机制

DSD 是 1-bit 采样，Diretta SDK 需要 32-bit 打包字。`ReadRese` 类负责把逐 byte 的 1-bit 样本 pack 进 `uint32_t`：

```cpp
// 每来 8 个 sample（1 byte per channel）
rest[c] <<= 8;
rest[c] |= in[c];   // 把 8 个 1-bit 样本 shift 进 64-bit 寄存器
count += 8;

// 当 count >= 32，输出一个 uint32_t
out[c] = rest[c] >> (count - 32);
```

- **DSF (Sony)**：样本是 LSB-first，需要先通过 `swapBitsTable[256]` 做 bit-reverse
- **DFF (DSDIFF)**：样本是 MSB-first，直接 pack

### 6.4 与 s2d / DRUP 的对比

| 维度 | MemoryPlay | DRUP | s2d |
|------|------------|------|-----|
| **音频来源** | 本地文件（WAV/AIFF/FLAC/DSD） | 网络 UPnP HTTP 流 | 网络 Scream UDP 流 |
| **解码位置** | Controller 端 | DRUP 内部（FFmpeg） | 上游（Windows ASIOScream） |
| **FLAC 解码** | libFLAC++ **预解码**（整文件到内存） | FFmpeg **流式解码** | 不涉及 |
| **DSD 处理** | Controller bit-pack | DRUP SIMD 优化 bit-pack | Scream 不传输 DSD |
| **传输层** | **TCP + 确认流控** | HTTP（TCP） | **UDP 无连接** |
| **反压机制** | 显式 ACK（`DataStack`/`DataTag`） | HTTP 层自然反压 | **无反压**，靠 ring 吸收/丢弃 |
| **位深提升** | Controller `ch2bit32` 可选 | DRUP ring SIMD 转换 | 直接透传 |
| **轨道边界** | 显式 `Tag` 消息标记 | UPnP track 切换 | 无（连续流） |

### 6.5 流控差异的本质

MemoryPlay 的 TCP 确认流控和 s2d 的无反压 UDP 是两种设计哲学的体现：

- **MemoryPlay（TCP）**：文件播放不能容忍任何丢帧，TCP 的可靠传输 + 应用层 ACK 保证数据完整性。代价是延迟和流控复杂度。
- **s2d（UDP）**：实时直播流，丢 1ms 和延迟 20ms 等重传都是瑕疵。选择 UDP fire-and-forget，靠 `SO_RCVBUF` + `PcmRing` 吸收 jitter，以极低延迟换取偶尔 drop  newest 的可接受性。

在正常局域网 + chrony 同步 + 合理 buffer 下，两者在稳态下**音质无架构性差异**——最终进入 Diretta SDK 的都是逐位相同的裸 PCM。

---

---

## 七、音质结论

### 正常工作条件下（无丢包、无 underrun、ring fill 稳定）

从音频架构师的角度，**DirettaAlsaHost 和 DRUP/s2d 在数据层面没有音质区别**。两者最终都输出**逐位相同的裸 PCM** 到 Diretta SDK。

### 差异存在于「架构开销」

| 开销源 | DirettaAlsaHost | DRUP / s2d / MemoryPlay |
|--------|-----------------|-------------------------|
| 内核态拷贝 | `copy_from_user` | 无 |
| 共享内存同步 | `rd`/`wd` 指针轮询 | 无 |
| ALSA period 唤醒 | ~10-50ms granularity | 无 |
| 额外进程 | syncAlsa daemon | 无 |
| 实时调度 | 通常无 SCHED_FIFO | 有（DRUP） |
| CPU 隔离 | 有限 | 精细（DRUP） |

这些开销在绝大多数系统中**不可闻**，但从工程纯粹性角度：

> **每多一层抽象，就多一个引入抖动的潜在来源。DRUP/s2d 的价值不在于「加了什么魔法」，而在于「减掉了什么冗余」。**

### 最终结论

| 方案 | 适用场景 | 优势 | 局限 |
|------|----------|------|------|
| **DirettaAlsaHost** | 通用 Linux 音频（任何 ALSA 应用） | 兼容性最好，即插即用 | 无法摆脱 kernel ALSA 层开销 |
| **MemoryPlay** | 本地高解析文件播放 | 官方 Direct-to-SDK，TCP 可靠传输 | 需预解码 FLAC（内存占用），闭源 Host |
| **DRUP** | 网络 UPnP/DLNA 播放 | 最精细的控制（SCHED_FIFO、SIMD、CPU 亲和） | 依赖 FFmpeg，架构复杂 |
| **s2d** | 实时 Scream UDP 接收 | 架构最简，零解码开销，最低延迟 | 依赖局域网质量，无显式反压 |

**工程纯粹性排序**：DirettaAlsaHost < MemoryPlay ≈ DRUP < s2d

- **DirettaAlsaHost** 层数最多（ALSA kernel + shared memory + sync daemon）
- **MemoryPlay** 和 **DRUP** 都直接到 SDK，但前者有 TCP + 预解码开销，后者有 FFmpeg + UPnP 开销
- **s2d** 数据通路最短：UDP → frame-align → ring → SDK，中间无解码、无 TCP、无文件系统

如果你的系统已经能通过 chrony 消除时钟漂移、通过 `SO_RCVBUF` 和 `PcmRing` 消除丢包和 underrun，那么 s2d 的数据通路在理论上已经达到该架构下的最优形态。
