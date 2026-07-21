# DRUP vs s2d：Ring Buffer 数据进入方式与优化分析

**日期**：2026-06（基于技术讨论整理）  
**背景**：对比 scream2diretta（s2d）与 DirettaRendererUPnP（DRUP）中 PCM 数据如何进入各自的 ring buffer、DRUP 在 plain PCM 进入路径上的具体优化、内部实现细节，以及 s2d 可以合理借鉴的内容。

本文档将前期两个回答合并为一份自成体系的技术分析文档，供后续斟酌改进时参考。重点聚焦“PCM 进入 ring”这一路径，排除解码、bit-depth 转换、DSD 处理以及 SIMD/AVX 优化（除非它们直接影响进入机制）。

---

## 1. 数据进入路径：高层对比

### s2d（scream2diretta）
- **Ring 名称**：`PcmRing`（统一 PCM 队列，`src/diretta_ring.h`）
- **生产者**：Scream UDP receiver 线程（`src/scream.c` → `src/diretta.cpp` 中的 `diretta_output_send()`）
- **进入点**：`g_st.queue.pushFrames(data, bytes, bpf)`（经过 partial frame carry 处理 + 基于 Scream header bytes 0-3 的 per-packet 格式检测之后）
- **特点**：
  - 输入是“近似原始”的 Scream packet（网络驱动，理论上每包格式都可能变）。
  - 强调 frame alignment（`pushFrames` 保证是 `bytesPerFrame` 的整数倍，否则截断到能容纳的整帧）。
  - Ring 在格式切换期间仍然存活（unified queue 设计），避免 reconfigure 时丢失 track 开头的数据。
  - 大多数情况下直接 push 最终格式的 PCM（该层几乎没有转换）。

### DRUP（DirettaRendererUPnP）
- **Ring 名称**：`DirettaRingBuffer`（支持格式转换的 lock-free SPSC ring，`src/DirettaRingBuffer.h`；`src/sync/` 下也有一个变体）
- **生产者**：AudioEngine 解码线程（基于 FFmpeg，通过注册的 `AudioCallback`）
- **进入点**：`DirettaRenderer` 的 callback → `m_direttaSync->sendAudio(buffer.data(), samples)` → `DirettaSync::sendAudio()`（带 `RingAccessGuard`）→ `m_ringBuffer.push*()`（plain PCM 走 `push()`）
- **特点**：
  - 输入是 FFmpeg 解码后的音频（per-track 格式相对稳定）。
  - `sendAudio` 作为 dispatcher，根据缓存的格式标志（DSD vs PCM、24-bit packing、16→32 upsample 等）分发。
  - Plain PCM（无转换）时：直接调用 `m_ringBuffer.push(data, totalBytes)`。
  - Ring 与当前 `DirettaSync` 实例/格式绑定更紧密（track/format 切换时会重新配置）。
  - 各种 `pushXXX()` 方法内部实现了大量 on-the-fly 转换支持。

**核心架构差异**：
- s2d：Network packet → per-packet 检测 + partial carry → 直接 frame-aligned push。格式切换是“正常”现象，通过异步 reconfigure 处理。
- DRUP：Decoded stream（per-track 稳定）→ `sendAudio` dispatcher → ring push（必要时带转换）。格式切换是 track-oriented 的。

---

## 2. DRUP 在 Plain PCM 进入 ring 路径上的优化（排除转换）

严格聚焦“plain PCM 数据进入 ring”这一路径（即 dispatch 之后直接调用 `push()` 的那一支）：

1. **Direct Contiguous Write Fast-Path（价值最高的优化）**
   - 在 `DirettaRingBuffer::push(const uint8_t* data, size_t len)` 中：
     ```cpp
     // Fast path: try direct write (no wraparound)
     uint8_t* region; size_t available;
     if (getDirectWriteRegion(len, region, available)) {
         memcpy_audio(region, data, len);
         commitDirectWrite(len);
         return len;
     }
     // Slow path: handle wraparound with two-chunk memcpy
     ```
   - `getDirectWriteRegion()` 计算从写指针开始的连续空间（到 buffer 尾或到读指针），如果 `contiguous >= needed` 就返回直接指针。
   - `commitDirectWrite()` 只是简单推进 `writePos_`。
   - **收益**：在常见的不 wrap 情况下（buffer 较大、没快满），避免重复的 chunk 计算和潜在的第二次 memcpy，实现一次线性写入。
   - 该能力被暴露为公共 API，供上游做 zero-copy 写入（理论上）。

2. **Generation Counter 缓存配置状态（在 sendAudio 层）**
   - 在 `DirettaSync::sendAudio()` 中（调用 ring push 之前）：
     ```cpp
     uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
     if (gen != m_cachedFormatGen) {
         // 只在 generation 真正变化时才从原子变量 reload 所有格式标志
         // （dsdMode, pack24bit, upsample..., channels, bytesPerSample 等）
         m_cached... = ...
         m_cachedFormatGen = gen;
     }
     // 之后使用 *缓存值*（稳态下几乎零原子 load）
     if (...) ... else { written = m_ringBuffer.push(data, totalBytes); }
     ```
   - **收益**：对于 per-track 稳定的格式，`sendAudio`（进入 ring 的上一层）热路径可以避免 5-7 次原子 load，每次只做一次 generation 检查。
   - Plain PCM 路径间接受益（dispatch 阶段更便宜）。

3. **Plain 路径中的其他支持细节**
   - 所有位置运算都使用 power-of-2 + `& mask_`。
   - `writePos_` / `readPos_` 都做了 `alignas(64)`（cache-line 分离）。
   - 即使是 plain copy 也使用 `memcpy_audio()`（快速路径，可能带 SIMD）。
   - 部分路径有 `prefetch_audio_buffer()`。
   - 暴露了 direct-write API，未来可让调用者在 ring 内部 copy 之前就直接写入。

**说明**：这些优化是针对 DRUP “per-track 解码后输入稳定”的模型设计的。generation counter 在格式很少中途变化的场景下特别有效。

---

## 3. 内部实现细节

### 写指针 + Mask 实现（DirettaRingBuffer）
标准的 power-of-2 ring buffer 设计，追求速度：

- `resize()` 时：`size_ = roundUpPow2(newSize); mask_ = size_ - 1;`
- 写：`writePos_.store( (wp + len) & mask_ , std::memory_order_release );`
- 可用空间：`return (wp - rp) & mask_;`
- 空闲空间计算也使用类似的 masked 算术。
- **为什么用这个？** `& mask_` 比 `% size_` 快非常多（尤其在 size 不是编译期常量、或在较老/嵌入式 CPU 上）。wrap 处理代价很低。
- 位置变量都做了 cache-line 对齐，防止 producer（写）和 consumer（读）线程之间的 false sharing。
- s2d 的 `PcmRing` 中有非常类似（但不完全相同）的实现（`& m_mask`，producer/consumer 统计量分别 `alignas(64)`）。

### RingAccessGuard 并发保护
RAII guard，用于在允许 reconfiguration（格式切换、resize、clear 等）的同时，安全地访问 ring，避免 race 或 use-after-free。

**核心逻辑（两阶段检查，关闭 race window）**：
```cpp
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) return;  // 阶段1：提前退出
        users_.fetch_add(1, std::memory_order_acq_rel);             // 阶段2：递增
        if (reconfiguring.load(std::memory_order_acquire)) {        // 阶段3：再次检查（关键！）
            users_.fetch_sub(1, std::memory_order_relaxed);         // 退出路径：从未真正进入
            return;
        }
        active_ = true;
    }
    ~RingAccessGuard() {
        if (active_) users_.fetch_sub(1, std::memory_order_release);
    }
    bool active() const { return active_; }
};
```

**Reconfigure 一侧的典型模式**：
- 设置 `reconfiguring = true`
- 自旋等待：`while (m_ringUsers.load(acquire) > 0) yield();`
- 安全地修改 ring（resize、clear 等）
- 设置 `reconfiguring = false`

**为什么需要两阶段检查？** 经典的 check-then-act race：
- 线程 A 看到 `!reconfiguring`
- 线程 B 开始 reconfig 并置位标志
- 线程 A 这时才执行 `users.fetch_add(1)`
- B 以为“没人用了”，就去修改 ring → UB

递增后必须再检查一次 + relaxed bail-out 才能关闭这个窗口。递增用 `acq_rel` 保证可见性；递减用 `release` 保证之前的 ring 操作在计数下降前对其他线程可见。

**s2d 的对应物**：`RingUserGuard`（位于 `src/diretta_sync.cpp`）在精神和代码上几乎完全一致（两阶段检查、acq_rel/release 内存序、bail-out 用 relaxed）。注释里明确写了 “Mirrors DRUP's RingAccessGuard / beginReconfigure pattern”。s2d 把 guard 主要放在 consumer 侧（`getNewStream`），因为 producer（receiver）控制着 ring 的生命周期，必须在 cooldown 期间继续推数据。

---

## 4. 可被 s2d 借鉴的优化点（价值评估）

**价值较高、推荐优先考虑的点**：
- **Ring push 中的 Direct contiguous write fast-path**：在 `PcmRing::push` / `pushFrames` 中增加类似 `getDirectWriteRegion` 的早期检查，当空间连续时直接一次 memcpy + commit。receiver 热路径 wrap 不频繁，ROI 很高，代码改动小。目前 s2d 的 `pushFrames` 总是走 chunk 计算 + 最多两次 memcpy 的路径。
- **Generation counter 缓存稳定 per-format 状态**：在 push 路径或 DirettaState 中为“只在 reconfigure 时才变”的状态（例如当前 bpf、silence byte、某些阈值）引入 generation。只有 generation 变化时才 reload，快路径直接用缓存值。如果以后 push 路径状态增多，价值会更高。
- **暴露 direct-write API**：把 `getDirectWriteRegion` + `commitDirectWrite` 公开，作为未来 zero-copy 场景的扩展点。

**中等 / 视情况而定的点**：
- 在 plain push 中使用更激进的 prefetch 或 fast-memcpy wrapper（s2d 目前 ring 里用的是 `std::memcpy`，可以参考 DRUP 的 `memcpy_audio`）。

**价值较低或已经存在的点**：
- Power-of-2 + mask 以及 cache-line separation：s2d 的 `PcmRing` 早就实现了（`& m_mask`、producer/consumer 统计量的 `alignas(64)`）。
- RingAccessGuard / 两阶段 guard 模式：s2d 已经 1:1 采用为 `RingUserGuard`（并针对自身 async、producer 驱动 reconfig 的模型做了调整）。

---

## 5. 由于架构差异导致难以利用或价值不高的点

这些差异使得 DRUP 的某些做法在 s2d 中难以直接套用或价值有限：

- **输入模型不同**：
  - DRUP：来自 AudioEngine 的解码音频（per-track 格式稳定）。`sendAudio` 可以假设很多次调用内格式不变 → generation counter 缓存格式标志特别香。
  - s2d：Scream UDP packet（header bytes 0-3 理论上每包都可能变格式）。必须 per-packet 检测 + partial carry 处理。格式切换是设计中的“正常”情况，通过异步 reconfigure 处理。
  - **影响**：DRUP 在 entry dispatcher 里重度缓存格式标志的做法，在 s2d 中会引入 bug（缓存了旧格式）或过度复杂的状态机。s2d 已经通过 “unified queue + per-packet 检测 + async reconfig” 这条路解决了类似问题。

- **Reconfigure / Ring 生命周期模型不同**：
  - s2d：异步 reconfigure（单独线程）。Receiver（producer）在 cooldown 期间**必须继续推数据**（为新格式预填充）。Ring 是 “unified” 的，生命周期长于单个 `Sync` 实例。Guard 故意主要放在 consumer 侧（`getNewStream` 里的 `RingUserGuard`）；producer 通过 `deactivate()` 控制生命周期。
  - DRUP：更 track-oriented 的 reconfig。producer（`sendAudio`）和 consumer 两侧都要过 guard，在修改 ring 前 drain users。
  - **影响**：DRUP “在 producer 侧也做 guard 做 drain” 的对称模式，和 s2d “receiver 必须持续推” 的要求冲突。s2d 的 `RingUserGuard` + `deactivate` + bounded disconnect 已经是针对自身模型的对应实现。

- **转换路径 vs Plain 路径**：
  - DRUP：存在大量 `push*` 方法用于格式转换（24-bit packing、16→32、DSD planar 等）。Direct-write API 主要帮助在转换内部避免额外 staging copy。
  - s2d：plain PCM push（数据在上游 receiver + partial carry 阶段已经处理成 ring 的最终格式）。ring entry 层基本没有对应转换逻辑。
  - **影响**：对 s2d 的 plain PCM 直推路径，direct-write 的收益主要体现在 non-wrap 情况下，对转换场景帮助较小（s2d 在这个层面基本没有转换）。

- **热路径特性**：
  - s2d receiver push：极度延迟敏感、被 pin 到核心、UDP 驱动、必须处理 partial frame。
  - DRUP sendAudio：来自 decode 线程，更接近 “media player” 的节奏。
  - **影响**：假设“格式长期稳定”或“callback 驱动流程”的优化，无法直接映射。

**适应性总结**：
s2d 的核心优势（极简 per-packet 输入模型 + 异步 reconfig 不丢数据 + producer 控制 ring 生命周期）决定了它**不需要**（也无法安全地）采用 DRUP 那些“假设格式跨多次 push 稳定”或“在 reconfig 时阻塞 producer”的做法。最好的机会是 ring 内部的局部优化（contiguous fast path）和尊重 s2d per-packet 现实的状态缓存模式。

---

## 6. 推荐与下一步建议（基于 entry + consumer 两侧深入分析后的修订）

**修订后的 s2d 优先级建议（经过 producer entry 和 consumer exit 两侧的原子计数/分支预测/dispatch 位置/生命周期对比后）**：

最初把 ring entry 的“最高 ROI”项（Direct contiguous fast-path via getDirectWriteRegion + Generation counter）列为高优先，是基于表面相似性。经精确核算后（DRUP fast-path 的 push 实际执行 getFreeSpace + getDirectWriteRegion + commit load，acquire 数量比 s2d 单次 freeSpace +（很可能被 CSE 的）wp load 还多；“省掉”的那个分支在现代 CPU 上高度可预测、成本接近 0；s2d 的格式 dispatch 早在上游通过非 atomic 的 Scream header bytes + 按值传入 pushFrames 的 bpf 完成，push 热路径里没有等价的 7-atomic dispatch；s2d 在格式变化时会 recreate ScreamDirettaSync，因此 consumer 侧的标量是每个 Sync 配置一次，而不是 long-lived）——**不推荐把它们作为纯 ring 层的性能改动来做**。它们只会给 s2d 模型增加复杂度或原子流量，收益为零或负，违反 CLAUDE.md 的 hot path 规则和“避免 over-engineering”精神。

1. **不要**在 ring entry（producer）侧实现 Direct Contiguous Fast-Path 或 Generation Counter 作为优化。s2d 当前的 `pushFrames`（frame-aligned drop-newest + 可预测 wrap 分支的两段拷贝 + 按值参数）已经相当干净，适合自身模型。详见用户对原子负载的详细拆解和源码对照。
2. Consumer 侧的 generation counter（DRUP 的 `m_consumerStateGen` + getNewStream 里的 cached scalars）在 DRUP 的 long-lived Sync 模型里有价值，但对 s2d 优先级较低 / 价值较低（s2d 故意在格式变化时替换整个 ScreamDirettaSync 对象；prefill/rebuffer 等运行时 gate 状态每周期都必须 atomic 检查，无法“缓存掉”；unified queue + 异步 Sync 替换是核心优势，用于 reconfig 时不丢数据）。
3. Ring*Guard（s2d 只在 consumer 侧）、power-of-2 + mask、cache-line 分离、显式 memory order —— 保持现状，这些已经被正确借鉴/适配。
4. 可以参考（而非直接移植代码）DRUP 在 post-online gate 里基于 MTU + DSD multiplier 的 stabilization buffer 数量计算方法，用于改进 s2d 的 startup 调优、open-gate 逻辑或 --startup-*-ms 的默认/自动模式。这属于冷路径，对 pop 热路径无影响。
5. pop 之后通知 producer 的模式（DRUP getNewStream 里的 try_lock + cv）仅供参考——**不适用于** s2d。s2d 的 producer 是 live UDP，明确策略是 full 时 drop-newest、绝不 stall receiver 线程。
6. 继续保持诊断模型（严格用 `diretta_diag_armed()` + 双二进制 DCE，statusUpdate 用于 --info-cycle 可观测性，对 receiver 和 SDK audio worker 热路径零影响）。

**Consumer 侧 guard 适配说明**：两阶段 RingAccessGuard 模式已被镜像为 `RingUserGuard`（memory order 完全一致，bail-out 用 relaxed）。s2d 注释明确写着“Mirrors DRUP's...”。关键的正确决策是 s2d **只在 consumer 侧加保护**（getNewStream），因为 producer（receiver 线程）必须在 format-change cooldown 期间继续往 unified queue 里推。DRUP 两侧都 guard，是因为它们的模型允许 drain producer。

本文档（及对应的中文版）现已同时记录 producer-entry 和 consumer-exit 的对比，供后续“后续斟酌如何改善”使用。

---

## 7. Consumer 侧：PcmRing pop / getNewStream → Diretta SDK / Target

### High-Level Comparison

**s2d (ring → Target 通路)**：
- Producer 即使在 Sync 被 teardown / 新 Sync 异步打开期间，仍然继续往 *unified* PcmRing 推数据。
- 消费者是 `ScreamDirettaSync::getNewStream(diretta_stream&)`（SDK send 线程，通过 cpu-audio / OCCUPIED 绑定）。
- 第一件事：`RingUserGuard`（两阶段）保护对外部拥有 ring 的安全访问（reconfig 期间）。
- 预分配 `m_streamData`，直接设置 `s.Data.P` / `s.Size`（SDK 148 app-managed buffer 模型）。
- 在真正 pop 之前有复杂的 multi-gate 逻辑（全部用 acquire 原子）：
  - Gate 0：强制 silent warmup（`m_muteDone`，不从 ring pop —— 保护头部）。
  - Gate 1：prefill（等 queue fill 达到阈值，输出 silence，*不 pop*）。
  - Gate 1.5：`startup_real_delay`（prefill 之后可配置的 no-pop 延迟，用于 click mitigation）。
  - Gate 2/3：rebuffer 臂上 + underrun（如果 `available() < want`，设置 rebuffer target，调用 `popOrSilence` 由它 pad silence 并计数）。
- 真实路径：`m_ring->popOrSilence(dest, want)` —— 两段 memcpy（普通 `std::memcpy`），可能 pad silence + `underrunCount`，rp 的 release store。
- 更新 `poppedBytes`、`realCycles` / `silentCycles`（relaxed）。
- Egress 诊断（analyser/fader/dumper）严格受 `diretta_diag_armed()` 门控（在生产 binary `scream2diretta` 中 DCE 掉）。
- 独立的 `statusUpdate()` override（SDK 在 info-packet 周期调用），用于无锁捕获 live `getCycleTime`/mode/latency —— 完全不在数据热路径上；用于 stats 里的 `| live-cycle=...` 后缀（仅当与 open 时值不同、且主要在 TargetProfile 下有意义）。

**DRUP (ring → Target 通路)**：
- 更长寿的 `DirettaSync` + ring 跨格式存在。
- `DirettaSync::getNewStream(diretta_stream&)`（类似 SDK 148 的 app-managed buffer）。
- `RingAccessGuard`（与 producer 相同的两阶段模式）—— **producer 和 consumer 两侧都用**。
- 专用的 **consumer generation counter**（`m_consumerStateGen`，在 reconfigure 时与 producer 的 `m_formatGeneration` 一起递增）：
  - 热路径：只做一次 gen 的 acquire load；只有 gen 变化时才 reload 6+ 个原子（bytesPerBuffer、silenceByte、isDsd、sampleRate、bytesPerFrame、用于 44.1k 族 cycle drift fix 的 framesPerBufferRemainder）到缓存的 plain 值。
- 门控（类似但不完全相同）：silence remaining、stop、prefillComplete、精细的 post-online stabilization（基于 MTU + DSD multiplier 缩放需要的 silence buffer 数量，以获得一致的 wall-time warmup 时长）、rebuffering。
- Underrun 时臂上 rebuffer。
- `m_ringBuffer.pop(dest, ...)` —— 总是两段，用 `memcpy_audio`（即使在 pop 路径上也用这个 audio 优化 wrapper）。
- 成功 pop 之后：`try_lock` + `notify_one` 到 space-available cv（绝不阻塞 consumer 线程；用于给 producer 做 flow control / jitter reduction 信号）。
- 节流异步日志（`DIRETTA_LOG_ASYNC`）。

**Consumer 侧的核心生命周期 / 模型差异**：
- s2d 故意在格式变化时销毁旧 `ScreamDirettaSync` 并打开全新的（异步，通过 worker），同时 PcmRing（unified queue）和 receiver 继续不间断工作。这实现了“reconfig 时头部数据不丢” + 显式的多阶段 click 缓解（mute + prefill + realDelay）。RingUserGuard + deactivate() 关闭了 in-flight getNewStream 的安全窗口。
- DRUP 保留 Sync 对象；使用 reconfiguring flag + guard drain（beginReconfigure 自旋直到 ringUsers==0）+ gen bump，让*同一个* getNewStream 能 pick up 新的 cached scalars。更强调在 pop 后给 producer 发信号，以及精确的 stabilization 计时。

### Consumer / Exit 通路的可借鉴点（附评估）

**中等 / 视情况而定的价值（主要是思路，而非直接 port）**：
- Consumer generation counter，用于 getNewStream 里稳定标量的快照（DRUP 代码注释里叫“C1: single atomic load vs 5-6 loads”）。
  - **对 s2d 的评估**：价值低于 DRUP。因为 s2d 在格式变化时会替换整个 ScreamDirettaSync 对象（见 `diretta.cpp` 里的 `reconfigure()`、`open_sync_for_format()`、async open），per-format 的标量（`m_streamBytes`、silence）是在 `configureFormat()` 里每个 Sync 写一次。热路径里每周期都要检查的东西（prefillDone、realDelayDone、rebuffering、muteDone、`ring->available()`）是*动态 gate 状态*，是 unified queue 设计所必需的——无法“缓存掉”。加 gen 只会给每次 getNewStream 增加一次原子 + 分支，换来的只是省几个标量 load，性价比存疑。DRUP 需要这个模式是因为它们的 Sync 是 long-lived 的。
- 基于 MTU + DSD rate 的 post-online / stabilization silence buffer 数量缩放计算（DRUP 根据 effectiveMTU / bytesPerSec 算 cycleTimeUs，再 buffersNeeded = targetWarmupMs / cycleTimeUs，对 DSD 有 multiplier 特殊处理）。
  - **评估**：对 s2d 的 startup 逻辑有参考价值。s2d 已经有丰富的 CLI 控制（`--startup-mute-ms`、`--startup-real-delay-ms`、`--open-gate-*`、rebuffer percent 等）和基于字节的 open gate。借鉴这个*计算思路*可以用来改进自动计算的默认值或“自适应”模式，尤其对高 MTU 或高倍率 DSD 场景。仅冷路径，对 pop 热路径零影响。
- pop 之后用 try_lock 通知 producer（不阻塞 consumer）。
  - **评估**：不适用于 s2d 的 live UDP producer 模型。s2d 明确选择“full 时 drop-newest、绝不 stall receiver”（见 PcmRing 文档和 ring-full policy）。在 producer 侧加阻塞/等待会冒 UDP socket buffer 溢出或 pinned scream 线程延迟增加的风险。DRUP 的 decode 线程 producer 可以安全等待。这个模式是他们 jitter-reduction 工作的好工程实践，但和 s2d 的“PCM transport、live source”模型冲突。

**低价值（与 producer 侧结论相同）**：
- 即使在 pop 路径上也用专门的 `memcpy_audio`（以及 prefetch）。
  - 对于 consumer 侧由 SDK worker 执行的、小而规律的 cycle size 拷贝，收益在现代平台上很有限。s2d 在 `popOrSilence` 里用普通 `std::memcpy` 已经简单够用。（与 entry 侧分析结论一致。）

**已经正确实现 / 镜像的**：
- Consumer 侧（`getNewStream`）的两阶段 reconfig 保护：`RingUserGuard` 是 DRUP `RingAccessGuard` 的近 1:1 移植（acquire 读 flag、acq_rel 加、acquire 重检、relaxed 回滚减、release 减）。s2d 注释明确写了“Mirrors DRUP's...”。*区别*（只在 consumer 侧 guard，不在 push 路径上）是对 s2d 模型的正确适配。
- App-managed stream buffer：双方都预分配持久存储（`m_streamData`），在 hot callback 里直接设置 `diretta_stream.Data.P` + `Size`，而不是依赖 SDK resize。s2d 明确是为了 SDK 148 兼容做的。
- 在 prefill / startup 阶段“输出 silence 但不从 ring 消费”（这是 s2d unified queue 能保护 track 头部的核心）。
- real/silent cycles、underruns、popped bytes 的原子统计（通过 --stats 等暴露；DRUP 有类似计数器）。
- 稳态 real-PCM pop 路径里严格避免日志/工作（s2d 比 DRUP 的节流 async log 更严格；符合 CLAUDE.md “No logging in steady state”）。

**不值得借鉴（架构冲突）**：
- 假设 ring consumer 可以用 long-lived 对象 + gen snapshot 跨格式变化（DRUP 的模型）。s2d “tear down Sync、keep unified ring alive、open fresh Sync” 是深思熟虑的优势，用于格式/歌曲切换时零数据丢失 + click 缓解。移植整套 gen + drain 机制会与之冲突。
- Producer 侧在 sendAudio 等价物里做 RingAccessGuard draining（entry 侧已分析——receiver 必须继续 drain UDP）。
- Pop 侧的 direct read region / zero-copy 读到 SDK buffer（DRUP 自己都没实现对称的“getDirectReadRegion”；pop 始终是两段）。在 s2d 里会很危险（外部 ring 所有权、guard 下可能 resize、wrap 处理），而且读侧本来就没有 compelling 的 zero-copy 收益。
- 在 consumer 线程上做任何阻塞或等待（DRUP 自己都小心用 try_lock；s2d 数据路径里本来就没有）。

### Consumer 侧结论

与 producer（PCM entry）侧完全平行：在实际阅读了 DRUP 的 `getNewStream` + consumer gen + RingAccessGuard + beginReconfigure + post-pop notify，以及 s2d 完整的带 gate 的 `getNewStream` + `popOrSilence` + `RingUserGuard` + `statusUpdate` 之后，**没有非常高 ROI 的“把 DRUP 某个机制抄到 pop 路径里”的干净收益**。

DRUP 的 consumer generation 和 stabilization scaling 在*他们*的 long-lived Sync + decode-producer + 对 jitter 敏感的 media-player 语境下是有价值的。s2d 的 consumer 路径在 gating 上故意更复杂（为了让 unified queue + 异步 reconfig + 头部保护 + 多阶段 click 缓解工作），而 ring pop 原语本身保持了刻意的简单。

这次对比最持久的价值是：
- 确认 guard 已经被*正确*适配（只在 consumer 侧）。
- 理解为什么 DRUP 的某些模式（两侧 guard、producer 和 consumer 都用 gen、pop 后通知 producer）不映射。
- 验证 s2d 的 exit 路径虽然形状不同，但与 CLAUDE.md 里描述的整体架构是一致的。

基于本次对比，不推荐对 hot pop 路径做改动。

---

*文档结束。代码参考位置：*
- s2d: `src/diretta_ring.h`（PcmRing 的 push/pushFrames + popOrSilence）、`src/diretta_sync.cpp`（带完整 gates 的 getNewStream + RingUserGuard + popOrSilence 调用 + 用于 info-cycle 的 statusUpdate）、`src/diretta.cpp`（queue_push_frames + reconfigure + 异步 Sync 生命周期 + configureFormat）。
- DRUP: `Projects/DirettaRendererUPnP/src/DirettaRingBuffer.h`（push + getDirectWriteRegion + commitDirectWrite + pop + memcpy_audio）、`src/DirettaSync.cpp`（sendAudio + m_formatGeneration + getNewStream + m_consumerStateGen + 两侧 RingAccessGuard + beginReconfigure/endReconfigure + post-pop notify）、`src/DirettaRenderer.cpp`（到 sendAudio 的 callback）。

---

## 8. Scream Receiver 侧（原生 UDP 路径）——没有 DRUP 等价物，原生改进机会

这一侧 100% 是 s2d 原生实现（源自 scream 项目的 UDP receiver，经过适配）。没有 DRUP 的对应代码可以借鉴——DRUP 是从 AudioEngine / FFmpeg 解码回调拿到已解码音频，再调用 sendAudio，不涉及原始 Scream UDP 数据报。

### 当前架构（scream.c + network.c）
- 主线程 = Scream receiver 循环（可通过 --cpu-scream 绑定，可选 --rt-priority SCHED_FIFO）。
- 极薄的统一循环：
  ```c
  while (!g_shutdown_pending) {
      rcv_xxx(&receiver_data);          // network（unicast / multicast）...
      if (data) {
          if (receiver_tap_any_armed()) { ... tap ... }   // 生产 binary (NO_DIAGNOSTICS) 下编译期常量 0
          output_send_fn(&data);        // Diretta 时是 diretta_output_send（格式检测 + partial carry + 推 PcmRing）
      }
  }
  ```
- `rcv_network`（最常用 UDP 路径）：
  - 用 select(500ms 超时) 保证 shutdown 响应及时（历史上有纯阻塞 recvfrom + EINTR 时 Ctrl-C 不灵的问题）。
  - recvfrom 到固定的小上下文 buffer（MAX_SO_PACKETSIZE ≈ 1.1KiB）。
  - 可选 --allowed-source-ip 用户态过滤（recv 之后，静默丢弃）。
  - 解析 5 字节 Scream header（rate_byte、sample_size、channels、channel_map）填入 receiver_data_t。
  - **零拷贝指针交付**：`audio = &buf[5]`，`audio_size = n-5`。receiver 层不做数据拷贝。
- SO_RCVBUF：Diretta 输出时默认 4 MiB（吸收突发或调度延迟，让 userspace 有时间 drain 到 PcmRing）。用户可覆盖，-v 时会打印实际拿到的大小（内核可能会翻倍）。
- 每包都重新解析 header（Scream 设计上每包都可能带格式信息，歌曲切换 / screamalsa 行为都需要）。
- “推到 PCM Ring 的联动”：receiver 只负责交付数据报。真正干活的部分（每包格式变化检测、按 target bpf 做 partial-frame carry 保证整帧、DSD 变换或 bit-depth 降采样、queue_push_frames → PcmRing::pushFrames）都在 Diretta backend 里（diretta_output_send 及 helpers）。这一块在前面的 entry 侧分析中已经详细审视过。
- 所有重量级诊断（payload tap、raw-entry tap、startup analyzer、comparator、dump）都通过单一 `receiver_tap_any_armed()` 门控（init 时设一次的 plain int；生产 binary 里是常量 0 + DCE）。
- Shutdown：阻塞前和 EINTR/timeout 时都检查标志；故意不设 SA_RESTART。

这样 receiver 保持 backend 无关（`-o raw` 和 `-o diretta` 都走同一交付），热路径极简。

### 值得做的原生优化 / 改进点（没有 DRUP 可抄）
按价值 vs. 对热路径清洁度、shutdown 正确性、“避免 over-engineering”（CLAUDE.md）的风险排序。

**高 ROI、现实可做**：
- **recvmmsg 批量接收（Linux）** 在 network 路径上。
  - 当前：每个数据报都 select + recvfrom（即使数据已经积压）。
  - 建议：在支持时用 recvmmsg（带 MSG_DONTWAIT 或超时）一次拉 16~64 个数据报到 mmsghdr/iovec 数组，然后在内层循环里逐个处理（同样的指针交付、同样的 gated tap、同样的 output_send）。
  - 不支持时优雅回退到现有单包路径。
  - 为什么有价值：syscall 摊薄是高 pps UDP receiver 的经典优化。Scream 包率在高采样率/多声道或 screamalsa 频繁发送时会比较高。能给绑定的 --cpu-scream 线程省出明显 headroom。
  - 热路径影响：稳态下无（batch 循环干的活一样）；只在数据成批到达时受益。
  - Shutdown 处理：在 batch 内包与包之间检查 g_shutdown_pending；mmsg 调用本身可被中断。
  - 风险：中等（新增代码路径，需要妥善处理 batch 内出现格式切换的罕见情况）。边界清晰。
  - 当前状态：未实现；是原生改进的优质候选。

- **轻量、常开的 receiver 侧可观测性，融入 --stats**。
  - 增加廉价的 relaxed atomic（或单生产者场景下的普通计数）记录 packets_received、bytes_received（甚至 last receive 时间用于 gap 检测）。
  - 在 rcv_network 成功交付时递增（tap 检查前后均可）。
  - 在现有的周期性 stats（format_stats_line / maybe_print_periodic_stats）里或新增一小段 receiver 统计行，和 Diretta 的 producer stats（pushed/dropped/underruns/ring fill）一起输出（已有的限速机制）。
  - 同时在 stats 时刻尝试暴露实际 SO_RCVBUF 和可观测到的内核丢包（getsockopt 或简单 /proc 查询）。
  - 为什么有价值：用户（以及本次对话的历史日志）非常关心“UDP buffer 情况”、underrun 是源头 gap 还是 ring 行为导致。目前 stats 严重偏向 Diretta/backend 侧。
  - 代价：每包 1~2 个 relaxed 操作——和 ring 里已有的 pushedBytes 等同量级。生产和 debug binary 都安全。
  - 对热路径清洁度或日志零影响（stats 打印本来就是条件 + 节流的）。
  - 完美补充现有的 --udp-rcvbuf-bytes 开关和用户用 ss -m / watch ss 的习惯。

**中等 / 仅在测量后考虑**：
- 降低 select(500ms) 设计带来的每包 syscall 开销。
  - 当前 select 是为 idle 时 shutdown 延迟做的有意权衡（避免纯阻塞 recvfrom 依赖 EINTR）。
  - 替代：self-pipe / eventfd 在 shutdown 时唤醒（大部分时间纯阻塞 recvfrom，只在需要时同时 poll pipe+socket），或用 ppoll + sigmask，或干脆把超时改短（50~100ms）接受一点 idle 时的唤醒代价。
  - 流畅播放时 select 通常立刻返回（fd 可读），代价主要是多一次 syscall + fdset 准备。
  - 只有在 receiver 线程 profiling 显示这是瓶颈时才值得做。目前的实现是健壮且有详细注释的。

**低价值 / 不值得**：
- Receiver 里每包重新解析 header：5 字节 load + 填 struct。必须的（Scream 设计允许每包变格式），代价微不足道。下游 backend 还要做 change 检测。
- 把 partial carry / frame alignment 移到纯 receiver 层：不行——它依赖 active backend 协商后的 *target* bytes-per-frame（Diretta sink 配置之后）。raw backend 不需要同样保证。carry + push 联动放在 backend 里是正确的。
- 对 --allowed-source-ip 做内核级过滤（BPF/iptables）：对正常单发送者场景过度设计；当前 recv 后的用户态检查简单够用。
- 在 receive 循环里引入任何每包分配、日志或重量级计算：设计上和 CLAUDE.md 热路径规则都禁止。目前已经干净。
- 从内核 UDP buffer 零拷贝一直到 PcmRing：极其复杂（循环 buffer、frame 对齐、partial carry、reconfig 时 resize 安全、wrap 处理），对音频数据率来说收益很小。不符合“保持 receiver 干净”的目标。

### Scream Receiver 侧结论
原生 receiver 是有意做得很极简和正确的：薄循环、零拷贝指针交付原始数据报+header、可靠的 shutdown 语义、门控诊断、针对 Diretta 用例的良好默认（大 SO_RCVBUF 喂 userspace ring），以及 backend 无关。

“把 PCM push 到后面 PCM ring 联动”的实际工作（partial、转换、真正 push）主要在 Diretta backend 里，前面 entry 侧已经详细分析过，对 s2d 的约束来说是干净的。

最有吸引力的原生改进是：
- recvmmsg 批量接收（真正降低高包率场景下的每包 syscall 代价）。
- 把接收侧计数和 UDP 健康度作为一等公民放入 --stats 输出（直接解决用户可观测性需求，不增加默认路径的噪音或复杂度）。

这些都尊重项目所有规则：热路径保持精简、无新增稳态日志/分配、诊断默认关闭、receiver 线程在 ring 满时绝不阻塞。

这里完全没有 DRUP 模式可套用——这一层在 DRUP 架构里根本不存在。

（Receiver 侧分析结束。前面的 6–7 节已深入覆盖 ring entry 和 exit。）