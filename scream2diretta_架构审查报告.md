# scream2diretta 架构审查报告（v2）

> 审查对象：[ayanamilee/scream2diretta](https://github.com/ayanamilee/scream2diretta)（HEAD `25efa0c`）
> 横向对比基准：cometdom/slim2Diretta、cometdom/DirettaRendererUPnP（下称 **DRUP**）
> 视角：音频架构师
> 说明：**架构固有风险**只做提示；**可解决的错误/风险/优化**为重点，已标注修复方向。
>
> **v2 修订**：根据「Scream 默认恒为 32-bit 声明」这一已确认前提，对下变换相关问题重新评级，并把作用于 **32-bit 主路径** 的问题提到最高优先级；下变换字节序问题从 P0 下调为 P3 潜伏缺陷。

---

## 0. 关键前提：源默认恒为 32-bit（已确认）

本次审查的一个决定性前提：**asioscream（Windows）与 screamalsa（Linux）默认都只声明 32-bit**，而绝大多数 Diretta DAC 都支持 32-bit 容器。该前提已从 Scream 官方接收端源码与 scream2diretta 自身代码两侧交叉确认：

- **字节序为小端（LE）**：Scream 官方 [PulseAudio 接收端](https://github.com/duncanthrax/scream/blob/master/Receivers/unix/pulseaudio.c) 把 16/24/32 映射为 `PA_SAMPLE_S16LE / S24LE / S32LE`；[ALSA 接收端](https://github.com/duncanthrax/scream/blob/master/Receivers/unix/alsa.c) 映射为 `SND_PCM_FORMAT_S16_LE / S24_3LE / S32_LE`，全部 LE 且不做任何字节序转换。源头是 Windows WASAPI/MSVAD 的 LE PCM，驱动原样封包。
- **scream2diretta 自身的设计前提**：`negotiate_sink_format`（diretta.cpp 738 行）先 `checkSinkSupport(source_fc)`，成功即直接用源位深返回，**完全不进 convert 路径**；只有当 DAC 拒绝 32-bit 时才按 `{32→24→16}` 逐级降级。convert dispatch（1461–1465 行）也只覆盖 `4→3 / 4→2 / 3→2` 三条**纯降级**路径，没有任何升频或同位深路径。日志文案（756 行）更直接写明「若用 Scream Windows 驱动或 Album Player，建议把发送端设为 N-bit」。

**结论**：在默认 32-bit + Diretta DAC 普遍支持 32-bit 的现实下，`checkSinkSupport(source_fc)` 一上来就成功，**整条下变换 fallback 路径在常规使用中几乎不会被触发**。因此本版把真正每包必走的 **32-bit 主路径** 上的问题（帧对齐、漂移）提为最高优先级，把下变换字节序问题降为潜伏缺陷。

---

## 0.1 总体评价

scream2diretta 的整体架构是健康且有针对性的。它选择直接子类化 `DIRETTA::Sync` 并以 **PULL（getNewStream 回调拉取）** 模型驱动 SDK，而不是像 slim2Diretta / DRUP 那样走 **PUSH（worker 线程 + sendAudio）** 模型。这个选择对 Scream 这种「已解码 PCM、单一恒定码流、由网络节奏驱动」的源是合理的：

- Scream 交付的是**已解码的 PCM**，不需要 slim 那套 staging buffer / SIMD / S24 自动探测 / FLAC·MP3·AAC·Ogg 解码链路；
- 单生产者（接收线程，`scream.c` 主循环同步调用 `diretta_output_send`）→ SPSC 无锁环（`PcmRing`）→ 单消费者（SDK 在 `getNewStream` 回调里拉取）的拓扑，是最干净的实时音频拓扑；
- gate 级联（mute / prefill / startup_real_delay / rebuffer / underrun-arm）与 `RingUserGuard` + 两阶段 `deactivate()` 的设计，明显是从 DRUP 的 `RingAccessGuard` / `beginReconfigure` 谱系继承并**做了细化**（例如 `m_steadyState` 稳态快路是 DRUP 所没有的优化）。

---

## 1. 明显错误 / Bug

### 🔴 Bug #1（P0，作用于 32-bit 主路径，必走）：`getCycleSize()` 在正常路径未做帧对齐

**位置**：`src/diretta_sync.cpp` 198–207 行
```cpp
size_t cycle = getCycleSize();
if (cycle == 0) {
    // fallback：这里做了帧对齐
    cycle = bytesPerSecond / 1000;
    if (bytesPerFrame > 0) cycle = (cycle / bytesPerFrame) * bytesPerFrame;
    if (cycle == 0) cycle = bytesPerFrame;
}
m_streamBytes.store(cycle, ...);   // ← 正常路径未向 bytesPerFrame 取整
```

**问题**：帧对齐取整**只在 `cycle == 0` 的 fallback 分支里做**。正常分支直接把 SDK 返回的 `cycle` 当作每次 `popOrSilence(dest, want)`（233/315/439/474 行）的字节数。

注意：同一函数里其它所有阈值——prefill（141 行）、underrun（157 行）、mute（177 行）、realDelay（189 行）——**全部都做了 `(x / bytesPerFrame) * bytesPerFrame` 取整**，唯独主路径的 `cycle` 漏了。这是一处一致性缺口。

生产者侧已经被严格保证写入环的永远是**整数个 `dst_bpf` 帧**（见 `diretta_output_send` 约 3960–4020 行的 partial-frame carry：非整帧尾部缓存到 `partial_frame`，只有 `whole = (len/src_bpf)*src_bpf` 的整帧才进队列）。因此环里内容总是帧对齐的。但**消费者**若按非帧整数倍的 `cycle` 去 pop，每个 cycle 就会切到一帧中间，**L/R 交织随时间逐步错位**——听感是声道串扰/相位漂移，且缓慢累积。这条路径在 32-bit 常规播放中**每个 cycle 必走**，是真正该优先修的。

**修复**（把 fallback 已有的取整提到正常路径，零风险）：
```cpp
size_t cycle = getCycleSize();
if (bytesPerFrame > 0 && cycle > 0) {
    cycle = (cycle / bytesPerFrame) * bytesPerFrame;
}
if (cycle == 0) {
    cycle = bytesPerSecond / 1000;
    if (bytesPerFrame > 0) cycle = (cycle / bytesPerFrame) * bytesPerFrame;
    if (cycle == 0) cycle = bytesPerFrame;
}
m_streamBytes.store(cycle, std::memory_order_release);
m_streamData.assign(cycle, silence);
```

---

### 🟡 Bug #2（P3，潜伏缺陷，常规不触发）：下变换路径丢的是 MSB，不是 LSB

**位置**：`src/diretta.cpp` 的下变换函数
- `convert_32_to_16`（约 1330 行）：保留字节 `[0,1]`
- `convert_32_to_24_packed`（约 1320 行）：保留字节 `[0,1,2]`
- `convert_24_to_16`（约 1339 行）：保留字节 `[0,1]`

**问题**：Scream PCM 是**小端**（已在第 0 节确认）。小端下字节 0 是 LSB、最高字节才是 MSB。当前这些函数保留的是**低位字节**，等于**丢掉 MSB、把 LSB 当高位**——结果不是「安静的降质音频」，而是接近**噪声地板的垃圾数据**。

正确的小端截断应保留**高位字节**：

| 转换 | 当前（错误） | 正确（小端截断） |
|---|---|---|
| 32 → 16 | 保留 `[0,1]` | 保留 `[2,3]` |
| 32 → 24 | 保留 `[0,1,2]` | 保留 `[1,2,3]` |
| 24 → 16 | 保留 `[0,1]` | 保留 `[1,2]` |

**对照**：slim2Diretta 的 `convert16To32`（`DirettaRingBuffer.h`）把 16-bit 值放进**高位字节** `[2,3]`，方向正确，可作反向参照确认字节序判断无误。

**触发条件与实际影响（重要）**：仅当 DAC 协商出**比源更低的位深**（fallback 路径，`negotiate_sink_format` 738 行先 `checkSinkSupport(source_fc)` 失败后才进）时才走到。结合第 0 节前提——源默认恒为 32-bit、Diretta DAC 普遍支持 32-bit——**这条路径在常规使用中几乎永不触发**。因此评级从 P0 下调为 **P3**。但代码客观仍是错的：一旦触发就输出噪声垃圾，属于「埋雷」。

**修复（二选一）**：
1. **修正字节序**（几行）：按上表改字节索引，可选加 **TPDF dither + 简单 noise-shaping** 避免截断相关性失真。
2. **更诚实的做法**：既然这条路径几乎不该被走到，可**直接删掉 convert 路径，协商失败就报错拒播并提示用户把发送端调低位深**——避免在极端情况下给出错误音频，也少维护三个易错的转换函数。

> 附注（24-bit 封装核对）：Scream 24-bit 在线路上是 **3 字节 packed（`S24_3LE`）**，不是 4 字节容器。请顺手确认 `source_bytes_per_frame` 对 24-bit 按 **3 字节/样本** 计算（与 ALSA 端 `bytes_per_sample=3` 一致），否则 partial-frame carry 与帧对齐会全错。32-bit 按 4 字节计算，与 ALSA 端 `S32_LE` 一致，主路径无此问题。

---

### 🟡 Bug #3（P2，无害但应改）：`calculateCycleTime` 里的死三元

**位置**：`src/diretta.cpp` 约 925 行
```cpp
overhead = (mtu > 2000u) ? 6 : 6;   // 两个分支都是 6
```
注释（913–918 行）写明：标准帧（MTU ≤ 2000）overhead 应≈22，巨型帧（>2000）≈6。现在两分支都是 6，**标准帧被低估了 overhead**。

**缓解事实**：紧随其后的 `inferredOverhead` 路径（约 922–923 行）通常会在 SDK 反馈真实 overhead 后**覆盖**此值，因此该 bug **只在首次 open、推断缓存尚未填充时**短暂生效。

**对照**：DRUP 用常量 `OVERHEAD = 3`，并注明「SDK 的 `m_effectiveMTU` 已计入 IP/UDP 头」——说明 overhead 语义在不同 SDK 版本上确有歧义，更应显式处理而非留死分支。

**修复**：恢复 `22 : 6` 真实分支；或确认 inference 总会覆盖后，改成显式注释「初值无意义，等待 inference」并删掉误导性三元。

---

### 🟡 Bug #4（P2，类型/可读性）：`open_sync_worker_blocking` 用 `return false` 返回 `uint32_t`

声明返回 `uint32_t`（语义是 accepted_bits），连接失败时写 `return false;`。`false → 0` 数值上恰好等价「0 bit 被接受」，**当前不出错**，但语义混乱，应改为 `return 0;`。

---

## 2. 可解决的风险与优化（重点）

### ⭐ 优化 #1（P1，作用于 32-bit 主路径）：补上 DRUP 已有、scream2diretta 缺失的「44.1k 家族小数帧漂移修正」

**状态**：✅ **已在 scream2diretta 中实现**（commit 待提交）。实现方式与 DRUP 不同但目标一致：保持 MTU-based cycle 不变，在 `getNewStream()` 中累计 `getCycleSize()` 被截断到整帧时丢掉的字节数，每凑够一帧就补发一帧。

**事实**：DRUP 在 `getNewStream` 里（`src/DirettaSync.cpp` 约 1620–1630 行）有一个**小数帧累加器**：当每个 cycle 的理想字节数不是整帧时，用 fractional-frame accumulator 把累积小数攒够一帧后补发，专门解决 44.1k / 88.2k / 176.4k 这类「采样率 × cycle 时间不落在整帧」的长期漂移。DRUP 采用该机制**不是因为涉及 FFmpeg 解码**，而是因为 Diretta pull 模型里 `getCycleSize()` 返回的字节数本身就不保证是整帧。

**scream2diretta 原状态**：Bug #1 修复后已经把 cycle 截断到整帧，避免了 L/R 声道错位，但截断导致每个 cycle 都少消费 0..(bytesPerFrame-1) 字节。以 44.1k/2ch/32bit/MTU=1500 为例，每个 cycle 约少 0.75 帧，消费速度约为源速度的 99.6%，长期会把 PcmRing 逼到周期性 `drop-newest`。

**实现概要**（`src/diretta_sync.cpp`）：
- `configureFormat()` 计算 `raw_cycle = getCycleSize()`、`cycle = 截断到整帧`、`remainder = raw_cycle - cycle`；
- 保存 `m_cycleRemainder` 并把累加器 `m_cycleRemainderAccumulator` 归零；
- `m_streamData` 预分配 `cycle + bytesPerFrame`，确保补帧时无需堆分配；
- `getNewStream()` 每周期把 `remainder` 累加，达到 `bytesPerFrame` 时把本周期 `want` 加一帧。

**结论**：该优化与是否解码无关，是 pull 模型下 cycle/帧对齐的固有问题。当前实现不改变 cycle time，只修正长期平均消费字节数，对 44.1k/48k 等标准 MTU 场景有实际音质收益。

---

### ⭐ 优化 #2（P1）：把 `m_steadyState` 稳态快路的「退出条件」审一遍

`m_steadyState` 锁存快路（`getNewStream` 里一旦进入稳态就跳过整个 gate 级联）是相对 DRUP（generation-counter cache）的一个**好优化**。但风险点在于：**什么情况下应该退出稳态、重新进入 gate 级联**。

**建议**：确认以下事件**都会**清掉 `m_steadyState`（建议集中成单个 `exitSteadyState()` 调用点，避免遗漏）：
- underrun 触发（应回到 Gate2/Gate3 重新缓冲）；
- format change / reconfigure；
- ring resize / `deactivate()`；
- SDK 报告 offline / 重新 online。

任何一个事件**没有**清稳态，就会出现「underrun 后仍走快路、不重新 prefill」的吞字/爆音风险。可解决——把退出条件收敛到一处并加断言。

---

### 优化 #3（P3）：下变换路径加 dither（仅当选择保留 convert 路径）

若 Bug #2 选择「修正字节序保留 convert」，则**直接截断**仍会引入与信号相关的量化失真，fallback 路径加 TPDF dither 成本极低、收益明确。若选择「删掉 convert 路径」，本项作废。

---

### 优化 #4（P2）：环满 drop 策略与 underrun 计数的可观测性

`PcmRing` 当前是 **drop-newest**（环满丢最新），对实时音频是合理选择，但**建议确认**：
- drop 事件有独立计数器且能在周期性 stats 里看到（便于判断「源太快」还是「消费太慢」）；
- drop-newest 在格式切换瞬间不会把新格式首帧丢掉（reconfigure 时 `partial_frame_len` 已清零，逻辑看起来正确，但建议补一条日志确认首帧入队）。

均为**可观测性**改进，不改变行为，但能大幅降低现场排障成本。

---

### 优化 #5（P2）：`source_gap_ms` / underrun 归因时间戳已做得很好，建议固化为契约

`diretta_output_send` 入口处**先**盖 `last_pcm_packet_at` 时间戳、再做任何可能阻塞的 SDK open（约 3318 行注释），以及 `SO_TIMESTAMPNS` 的 NIC 到达时间戳（0 表示不可用的语义保留），这套**归因时间戳设计非常专业**。建议把「时间戳必须在任何阻塞工作之前采集」写成代码注释契约/单测，防止后续重构打乱顺序。

---

## 3. 架构固有风险（仅提示，了解即可）

这些是 PULL 模型 + Scream 源本身带来的，**不建议为此改架构**：

1. **PULL 模型把节奏完全交给 SDK 回调**。好处是简单、零额外线程；代价是**无法主动 pace**——SDK 回调节奏抖动只能靠环缓冲吸收。slim/DRUP 的 PUSH + worker 能主动 pace 但复杂度高。对 Scream 恒定码流，PULL 是对的取舍。

2. **Scream 无重传、无序号保证（UDP 多播）**。丢包就是丢包，环缓冲只能吸收抖动不能补包。这是协议层决定的，已用 `udp_rcvbuf_bytes` / busy-poll / NIC 时间戳把能做的都做了。

3. **单生产者线程同时负责收包 + 帧对齐 + 转换 + 入队**（`scream.c` 主循环同步调用）。鉴于第 0 节确认源恒为 32-bit、转换路径几乎不触发，这条线程在常规 48k/96k 下预算充裕。转换是「intentionally simple（no SIMD）」的判断在当前前提下成立；仅当将来要支持极高倍率 DSD 或多路才需移线程或上 SIMD。**当前不是问题**。

4. **format change 期间的 gap 不可避免**：cooldown（`reconfigure_ready_at`）+ open-gate 填充阈值意味着切歌/切格式必然有一小段静默。scream2diretta 已用「先填队列、晚点 open，让 SDK 第一个 cycle 就拉到真实 PCM」把它最小化，做法优于「open 后再 mute」。这是 SDK 重协商的固有代价。

---

## 4. 与 slim2Diretta / DRUP 的横向对比小结

| 维度 | scream2diretta | slim2Diretta | DRUP |
|---|---|---|---|
| 驱动模型 | **PULL**（子类 `Sync`，override `getNewStream`） | PUSH（worker + sendAudio） | PUSH（worker + sendAudio） |
| 源类型 | 已解码 PCM（Scream/UDP，默认恒 32-bit LE） | FLAC/MP3/AAC/Ogg/DSD（SlimProto/LMS） | UPnP/DLNA PCM |
| Ring | `PcmRing` SPSC、pow2、cache-line 对齐、drop-newest | 带 staging、S24 自动探测、DSD 位反转、AVX2/NEON SIMD | 独立 RingBuffer + generation cache |
| 转换 | scalar、按需下变换（fallback，常规不触发） | SIMD 上/下变换、对齐分配器 | — |
| reconfigure 安全 | `RingUserGuard` + 两阶段 `deactivate()`（自旋 `m_ringUsers`） | — | `RingAccessGuard` / `beginReconfigure`（**谱系来源**） |
| 稳态优化 | **`m_steadyState` 快路**（独有） | — | generation-counter cache |
| gate 级联 | mute/prefill/realdelay/rebuffer/underrun-arm（**最细**） | 较简单 | silenceRemaining/stopRequested/prefillComplete/postOnlineDelay（**谱系来源**） |
| 小数帧漂移修正 | ❌ **缺失** | — | ✅ **有**（44.1k 家族累加器） |
| overhead 处理 | 死三元 + inference 覆盖（Bug #3） | — | 常量 `OVERHEAD=3` + 注释 |

**结论**：scream2diretta「针对 Scream 源做减法」的方向是对的——没为不需要的解码/SIMD/staging 付复杂度，又继承了 DRUP 的并发安全谱系并用 `m_steadyState` 做了细化。在「源恒 32-bit」前提下，**最值得做的两块是作用于 32-bit 主路径的：(1) 修 `getCycleSize` 帧对齐（Bug #1）；(2) 移植 DRUP 的小数帧漂移累加器（优化 #1）。** 下变换字节序（Bug #2）虽客观错误但几乎不触发，建议「修字节序」或「删路径」择一处理以拆雷。

---

## 5. 专题：info-cycle 可观测性（为何 `-vv` 的 stat 里始终看不到 SDK 反馈周期）

> 背景：用户用 `-vv` 跑了很久，5 秒一个 stat，但 stat 里**始终看不到** `info-cycle update #N` 这一行，`journalctl ... | grep "info-cycle update"` 与 `/var/log/scream2diretta.log` 全为空，无法比对「当前 cycle 与 SDK 协商 cycle 是否一致」。本章经过一轮 tcpdump 实证 + SDK 头/反汇编交叉验证后**已推翻报告早期版本的「SelfProfile 正常行为」假说**，把真实根因、DRUP 对照、本次三项代码改动、以及验证方法固化下来。

> ⚠️ **更正声明**：本章 v1 曾断言「看不到 info-cycle 是 SelfProfile 的正常行为，稳态不存在 info 通道」。该结论**错误**，已被 5.1 的 tcpdump 实证否定。错误来源是把「上层回调没被触发」误当成「底层没有 info 交换」。下面是更正后的版本。

### 5.1 结论先行（tcpdump 实证）

**info-cycle 在物理链路上确实在持续发生，且非常稳定。** 在 end0（Diretta target NIC）上对 target `fe80::7736:6632:6235:91f3`（MAC `e4:5f:01:ea:72:d8`）抓包，5 秒抓到约 80 个回包，规律为：

- target 每 **100ms（10Hz）** 回送一组：**48 字节 info 包（源端口 19642）+ 16 字节 feedback 包（源端口 19645）**；
- 周期严丝合缝、零抖动 —— 正好对应配置里的 `info_cycle_us=100000`。

所以「stat 里看不到 info-cycle」**根本不是因为没有 info 交换**，而是因为 s2d 上层那个负责打印的回调 `statusUpdate()` **从来没有被 SDK 调用过**。这与音质/稳定性无关——SDK 在内部已经把 info/feedback 消化掉了（用于周期自适应、FEEDBACK 平均），只是没有把消化结果回吐给上层钩子。

### 5.2 真实根因：`statusUpdate()` 是 SDK 149 从不调用的死回调

1. **回调契约**：`statusUpdate()` 在 `Sync.hpp:255` 是一个**非纯虚**函数（带默认空实现），SDK 把它定义成「可选的上层通知钩子」。s2d 作者押注了 **push-callback 模型**——以为 SDK 每收到一次 info/feedback 就会回调一次 `statusUpdate()`，于是在里面累加 `info_update_count`、刷新 `current_sdk_cycle_us`，再由 stat 的「live-cycle 后缀」打印出来。

2. **实际行为**：SDK 149 **从不调用** `statusUpdate()`。info/feedback 完全在 SDK 内部闭环消费，上层钩子始终是死的。于是：`info_update_count` 永远是 0 → 显示门的「数据门」永远关闭 → `info-cycle update #N` 这一行一辈子不会出现。**这既不是 config 问题，也不是 async-worker 问题，更与 SelfProfile/TargetProfile 无关。**

3. **DRUP 怎么做的（对照）**：DRUP 同样把 `statusUpdate() override {}` 写成**空实现**（`src/DirettaSync.h:506`、`src/sync/DirettaSync.h:244`），**从不**依赖它，也从不调用 `getCycleTime()/getLatency()/getMode()`（grep 全空）。DRUP 的 cycle 由自己的 `DirettaCycleCalculator` 算；稳定性靠**本机侧**指标监控——本地 ring 占用 + underrun 计数（`DirettaSync.cpp:1770` 起），这些是 host-side、与 target 无关的量。它的 `dumpStats()`（`DirettaSync.cpp:1552`）只打印 State/Format/Buffer/MTU/Streams/Pushes/Underruns，**没有任何 target 遥测**。这恰好印证：DRUP 早就认定「上层 statusUpdate 不可靠」，转而用本机稳态指标做健康判据。

### 5.3 本次改动 ①：清理 `statusUpdate` 死代码簇（按「DRUP 对齐」方案）

既然 `statusUpdate()` 永不被调用，围绕它的整套上层遥测就是死代码。本次按与 DRUP 一致的策略清理：

- **保留** `void statusUpdate() override {}` 空实现（`diretta_sync.h:202`）+ 一段注释，说明「SDK 149 从不调用此钩子，与 DRUP 一致，cycle 信息改走 pull 路径」——保留空 override 是为了满足虚函数契约、并给后续维护者留下明确说明；
- **删除**所有围绕它的死遥测：`statusUpdate()` 的完整实现体、私有字段 `m_lastInfoCycleUs/m_lastInfoMode/m_lastInfoLatencyUs/m_infoUpdateCount` 及对应访问器、stats 结构里的 `current_sdk_cycle_us/current_profile_mode/current_latency_us/info_update_count` 四个字段、stat 的 live-cycle 后缀拼接块、以及 `info-cycle telemetry active` 那条 DLOG。
- 净效果：`diretta_sync.cpp` -57 行、`diretta_sync.h` 大幅瘦身、`diretta.h` stats 结构去掉 4 个死字段。

> 说明：报告 v1 在 5.2 引用的 `diretta.cpp:2520` 显示门、`info_update_count`、`current_sdk_cycle_us`、live-cycle 后缀等代码，**本次已全部删除**，相关描述就此作废。

### 5.4 本次改动 ②（已撤销）：`--target-info` 原型 → 确认无价值 → 删除

> **结论先行：SDK 149 在应用层无法读取 target 的实时运行状态。** 唯一的实时出口 `statusUpdate()` 是死钩子（5.2）；其余所有接口给的都是 open 时的配置/能力/握手快照，不随运行时间更新。因此本次先原型了 `--target-info <秒>`、经实测验证后**确认它达不到「实时观测 target」的目的，已整段删除**，相关源码回退（`scream.c` 回到 baseline，`diretta.cpp/h` 移除 `maybe_print_target_info`/`target_info_arm`/字段/默认值）。

**为什么原型、又为什么删——逐条记录,避免日后重蹈：**

原型的初衷是：既然 push 回调死了,想用 SDK 的 const getter「主动 pull」出 target 协商参数。原型确实能跑、零 underrun、`[target-info]` 每 5 秒一行,7 次格式切换时 cycle_us 跟随 266→800→244 变化、与 open 协商值完全吻合。**但实测正是在这里暴露了一个语义陷阱**:

这五个 getter 读的**不是 target 的实时状态,而是本机发送侧在 open/重协商时定稿的传输 profile 快照**。逐字看 `Sync.hpp` 注释,主语都是本机:

| getter | `Sync.hpp` 原文注释 | 实际语义 |
|---|---|---|
| `getCycleTime()` | `get **Generated** transmission interval` (134) | 本机生成的发送周期 |
| `getMinCycleTime()` | `get Minimum transmission interval` (136) | 最小发送周期 |
| `getCycleSize()` | `get Stream data size per transmission` (138) | 每次发送的数据量 |
| `getCyclePackets()` | `get Stream packet count per transmission` (140) | 每次发送的包数 |
| `getMode()` | `get **Send** Profile Mode` (142) | 本机的发送 Profile 模式 |

关键词是 `Generated` / `Send`——主语都是 host。这些值由 `configTransferVar/VarMax/FixAuto`（`Sync.hpp:120-130`）在协商时写进 Sync 对象,getter 只是读回。它们**只在格式重协商时变**,且 **sink open 时已经打印过一次**——稳态周期性打印它们 = 重复打印 open 时已知的常量,零增量信息。

**它「会变」≠「实时 target 遥测」**:实测 7 次格式切换时 `cycle_us` 确实跟着 266→800→244 变,但那是「随协商事件刷新本机快照」,不是「向 target 轮询当前真实状态」。两次切换之间 target 端任何漂移,这个值都不动,因为它根本不查 target。

**全 SDK 接口审查(`Sync.hpp` 328 行通读)**——按「能否提供 target 实时运行状态」分类,结论是**没有任何一个能**:

- **配置/快照类**(open 时写入、之后只读回):上述 5 个 getter、`currentWorkMode()`(91)、`getSinkConfigure()`(180)、`getNoneBaseMin/getFrameMax`(192-194);
- **连接状态布尔**:`is_connect/is_online/is_disconnect/is_active/isPlay`(163-173)只返回 `connectState` 4 态枚举(DISCONNECT/CONNECT_REQ/CONNECT/DISCONNECT_REQ),只能判「连没连上」,不含运行遥测;
- **握手能力快照**:`getSinkInfo()`→`struct Info`(199-245) 是 sink 在握手时上报的**能力/规格**(buffer 容量、`latencyHw/latencyMax`、MTU、支持格式),不是运行时占用;`getLatency()`(175,非 const)同为握手定的规格值;
- **连接阶段主动查询**:`inquirySupportFormat`(182)/`inquiryParameter`(184)/`mtuTest/mtuCheck`(187-189) 是 open 前/协商期的能力探测,会发包但拿的是规格,不是运行态;
- **唯一实时出口**:`statusUpdate()`(255,非纯虚)——SDK 149 从不调用(5.2,tcpdump+代码双证)。

即:**push 通道(statusUpdate)是死的,pull 通道(getter)读的是本机配置快照,两条路都拿不到「实时 target 遥测」。这是 SDK 149 本身的接口缺失,不是配置/线程/代码问题。**

**澄清——删 `statusUpdate` 死代码(①)并未删掉「本来能工作的实时通道」**:`statusUpdate()` 即使保留也永远空转(SDK 从不调它),它从来拿不到任何实时 target 数据。①删的是从未被触发过的代码,与本节(②)的结论一致、互不矛盾。

### 5.5 本次改动 ③（Q3）：消除临时第三线程继承的 FF80

排查中发现：切换采样率时短暂出现的第三个线程（async open worker）也是 **SCHED_FIFO 80**。根因是 `pthread`/`std::thread` 会**继承父线程（接收线程，FF80）的调度策略+优先级**，而该 worker 入口原本只调了 `::nice(-10)`——**在 SCHED_FIFO 下 `nice` 是 no-op**，所以它实际跑在 FF80，和音频线程抢同档实时优先级。

修复：在 async open worker（`diretta.cpp:2009`）和 cleanup 线程（`diretta.cpp:2116`）入口处，**先**降到 `SCHED_OTHER`（`pthread_setschedparam(pthread_self(), SCHED_OTHER, {prio=0})`）**再** `nice(-10)`，让 nice 真正生效。这两个 worker 都是一次性的（仅用于采样率切换时的格式重协商），降到普通调度不影响功能，反而避免了对真正音频热路径的实时抢占。

> 三处 RT 应用点的全貌：接收线程（`scream.c:1110`，显式 FIFO 80）、SDK 发送线程（`diretta_sync.cpp:269-287`，首次 `getNewStream()` 显式 FIFO 80）保持不变；只有这两个 housekeeping worker 从「继承来的 FF80」被纠正为 SCHED_OTHER。

### 5.6 `thread_mode = 16433` 解码（本次 FEEDBACK32 测试配置）

用户本次测试配置 `THREAD_MODE=16433`，解码为：

| 位值 | 标志 | 含义 |
|---|---|---|
| 1 | `CRITICAL` | 最高优先级线程类别 |
| 16 | `OCCUPIED` | 把线程钉在指定 CPU（非独占） |
| 32 | `FEEDBACKOFFSET`（×1） | FEEDBACK 偏移档位（FEEDBACKMASK=0x7×32=224，此处取 1 档=32） |
| 16384 | `NOFIREWALL` | 不发用于关闭防火墙的探测包 |

（完整位定义见 `Sync.hpp:22-51`：CRITICAL=1, NOSHORTSLEEP=2, NOSLEEP4CORE=4, OCCUPIED=16, FEEDBACKOFFSET=32, NOFASTFEEDBACK=256, IDLEONE=512, IDLEALL=1024, NOSLEEPFORCE=2048, LIMITRESEND=4096, NOJUMBOFRAME=8192, NOFIREWALL=16384, NORAWSOCKET=32768。）注意本次配置**未**含 `NOFASTFEEDBACK`，正是为了保留 fast feedback 做对照测试。

### 5.7 线程行为与 `--rt-priority` / `--cpu-other` 的真实作用域

这次排查同时澄清了三个被误解的参数语义，都已从源码 + SDK 反汇编 + 实测线程交叉确认：

**(a) `--rt-priority 80` 作用于两个线程，而非一个：**
- **接收线程**（scream.c:1110）：`diretta_apply_rt_priority()` → `pthread_setschedparam(pthread_self(), SCHED_FIFO, 80)`。
- **SDK 发送线程**（diretta_sync.cpp:269-287）：s2d 的 `ScreamDirettaSync`（SDK `Sync` 子类）在首次 `getNewStream()` 回调里**惰性**应用 FIFO 80（该回调跑在发送线程上）。
- **关键事实**：`SCHED_FIFO/RR` 的优先级是**按线程**生效的，**不会**自动传播到进程内其它线程——这也正是 Q3（5.5）里 async worker「继承」FF80 必须显式纠正的原因。

**(b) `--cpu-other` 的语义被命名误导：** s2d 的 `--cpu-other`（`cfg.cpu_other`）**只**被透传给 SDK `open()` 的 `cpuOther` 参数，它**不覆盖** s2d 自己的 worker（async_open/cleanup）。

**(c) `getCycleTime()/getMinCycleTime()/getCycleSize()/getCyclePackets()/getMode()` 全是 const getter**：非阻塞、不建线程、不碰 socket，从 Sync 里读缓存的**本机发送 profile 快照**（见 5.4：它们不是 target 实时状态，仅在格式重协商时更新）。

### 5.8 验证方法清单（便于复现）

- **证 info-cycle 物理存在**：在 target NIC 上 `tcpdump -i end0 -e 'src host <target-ll-addr>'`，可见每 100ms 一组 48B（port 19642）+ 16B（port 19645）回包。
- **证 statusUpdate 从不被调用**：清理前曾在回调里加 DLOG，跑 5 分钟日志全空 → 钩子是死的（本次已随死代码一并删除）。
- **数线程 / 验 Q3 修复**：`ps -L`、`chrt -p <tid>` 观察稳态 2 线程（接收/发送均 FIFO 80）；切采样率时第三线程短暂出现，修复后应为 SCHED_OTHER（`chrt` 显示 `SCHED_OTHER` 而非 `FIFO 80`）。
- **健康信号建议**：不要把「info-cycle 出现」当健康判据（上层钩子本就死的），也不要试图「实时读 target 状态」（SDK 149 无此接口，见 5.4）。用 `PcmRing` 稳态指标（占用水位、underrun 计数、drop 计数、`source_gap_ms`，见优化 #4/#5）做持续健康信号，与 DRUP 的本机侧做法一致。

### 5.9 本次改动 ④：transfer-mode 重构（`auto+cycletime` 改用 VarAuto 承载 + 新增 `autofix` 模式 + transfer 日志补 `mode_sdk`/`min_cycle`）

这一节记录第 4 组改动，全部落在 `apply_transfer_mode()`（`diretta.cpp:929–1058` 区域）与 `transfer:` 日志行（`diretta.cpp` 约 1724–1755）以及 `scream.c` 的 CLI 解析。**总原则不变：单 packet/cycle（`cycle_packet=1`）的大前提始终成立**——所有分支仍以 `varmax_cycle`（恰好填满一个 effMtu 的周期）为 1-packet 物理上界，`safe_max = varmax_cycle × 0.97` 的 3% 安全裕度判定原样保留。

#### 背景：先厘清当前 `transfer-mode` 的真实行为

| 调用方式 | 行为（改动前） |
|---|---|
| `--transfer-mode=auto`（无 cycletime） | 低码率（≤16bit 且 ≤48k）或 DSD → VarAuto；其余高码率 PCM → VarMax |
| `--transfer-mode=auto` + cycletime | `cycle ≤ safe_max` → **FixAuto**(cycle)（周期被锚定逐字honor）；超限 → VarMax-override + warn |

用户确认了上面两条语义后，提出第 4 组改动：把 `auto+cycletime` 满足分支的**承载方式**从 FixAuto 换成 VarAuto；同时**新增**一个 `autofix` 模式来保留原 FixAuto 行为，二者并存、各取所需。

#### 改动 4-A：`auto + cycletime` 满足分支 FixAuto → VarAuto

`diretta.cpp` AUTO case 满足分支（原 `configTransferFixAuto(cycle)` 返回 `auto-fixauto`）改为 `configTransferVarAuto(cycle)`，返回 `auto-varauto-cycle`。超限分支（`> safe_max` → `configTransferVarMax(varmax_cycle)` + warn，返回 `auto-varmax-override`）**完全不动**；`auto` 无 cycletime 的 B 分支（低码率→VarAuto / 否则→VarMax）**完全不动**。

- **FixAuto vs VarAuto 的语义差**：FixAuto 锚周期（cycle 被逐字 honor，是主约束），VarAuto 锚 size（cycle 退化为副产物、按帧边界量化）。理论上 VarAuto 偏差更大；**但实测在单 packet 能装下时偏差可忽略**——用户生产日志（`TRANSFER_MODE=varauto TARGET_PROFILE_LIMIT=0 CYCLETIME=800`）显示 `target_cycle=800us → sdk_cycle=803us`，偏差仅 0.4%，零 underrun，印证 `cycle_packet=1` 前提下周期偏移极小。

#### 改动 4-B：新增 `autofix` 模式（保留原 FixAuto 周期-锚定行为）

- `diretta.h` enum 末尾新增 `DIRETTA_TM_AUTOFIX`（命名与 SDK 已有的 `fixauto`/`configTransferFixAuto` 区分：`autofix` 是「auto 的 cycle-锚定变体」）。
- direct-config switch 新增 `DIRETTA_TM_AUTOFIX` case，**完整复刻原 `auto+cycletime`**：`cycle ≤ safe_max` → `configTransferFixAuto(cycle)` 返回 `autofix-fixauto`；超限 → `configTransferVarMax(varmax_cycle)` + warn 返回 `autofix-varmax-override`。
- `autofix` 无 cycletime（`cycle_us==0`）时与 AUTO 的 B 分支一致（低码率/DSD→VarAuto 返回 `autofix-varauto`/`autofix-varauto-dsd`；否则→VarMax 返回 `autofix-varmax`）。
- ProfileMaker 路径（`target_profile_limit_us>0`）下 `autofix` 等同 `FIXAUTO`：`pm.configTransferFixAuto(cycle)`，name `profile-autofix`。
- CLI 同步：`scream.c` 的 `parse_transfer_mode`、`--usage` 字符串、错误提示字符串都加 `autofix`；启动配置打印的 `tmode_name` switch 加 `autofix`。

**等价关系（改动后）**：现在的 `autofix + cycletime` ≡ 改动前的 `auto + cycletime`（都走 FixAuto 周期锚定）；现在的 `auto + cycletime` 改用 VarAuto size 锚定承载同一个 cycletime 目标。

#### 改动 4-C：`transfer:` 日志补 `mode_sdk` 与条件 `min_cycle`

5 个 const getter（`Sync.hpp:134–143`）里，`transfer:` 日志原本只用了 3 个（`getCycleTime`/`getCycleSize`/`getCyclePackets`）。本次补齐剩下两个：

- **`mode_sdk=%s`**：`sync->getMode()`（返回 `Profile::ModeType`）映射 `VARIABLE(0)/FIX(1)/RANDOM(2)/TRIANGOLO(3)` → `variable/fix/random/triangolo`。这是 SDK 把我方 config 量化后实际采用的发送 profile 模式的**读回**，用于核对「指令」与「SDK 实际采用模式」是否一致（例如 `auto-varauto-cycle` 指令应读回 `mode_sdk=variable`）。
- **`min_cycle=%lldus`（条件打印）**：`sync->getMinCycleTime()`，**仅当 >0 时**才追加（`snprintf` 拼到临时 buffer，否则置空串）。原因：SelfProfile（`TARGET_PROFILE_LIMIT=0`）下该值在此前 `--target-info` 测试期间始终返回 0，无条件打印只会制造噪声。
- **getter 全景**：`getCycleTime`✅ `getCycleSize`✅ `getCyclePackets`✅（原有）+ `getMode`✅ `getMinCycleTime`✅（本次补）= 5 个 const getter 全覆盖。`getLatency()`（`Sync.hpp:175`）为**非 const** 排除；`getNoneBaseMin`/`getFrameMax`/`getSinkInfo` 语义不同（sink 能力上限，已在别处打印）。
- **重要定性（沿用 5.x 结论）**：这些 getter 读的仍是**本机侧协商好的发送 profile 快照**（open / 重协商时确定），**不是 target 实时遥测**。`transfer:` 日志因此混合「指令」（`mode`/`target_cycle`）与「读回快照」（`mode_sdk`/`sdk_cycle`/`cycle_size`/`cycle_packets`/`min_cycle`）。这与 5.4 总结论不冲突：SDK 149 仍无任何接口读 target 实时状态。

#### 验证状态

- 沙箱静态校验通过：5 处 `AUTOFIX` 落地齐全（enum / ProfileMaker switch / direct-config switch / `tmode_name` / `scream.c` parse）；`diretta.cpp` 大括号 412/412 平衡，圆括号差值与 baseline 一致（均为注释中孤立 `)`，非代码不平衡）。
- 沙箱缺完整 Diretta SDK 树（仅 `Sync.hpp`/`Profile_utf8.hpp` 两个头），无法完整链接编译；**最终编译验证在 Pi5 由用户执行**。
- 三个修改文件已 push 回 Mac（`diretta.h` 16060B / `diretta.cpp` 192462B / `scream.c` 49862B，与沙箱字节一致）。

---

## 修复优先级建议（v2，已按「源恒 32-bit」重排）

| 优先级 | 项 | 作用路径 | 工作量 |
|---|---|---|---|
| **P0** | Bug #1 `getCycleSize` 帧对齐 | **32-bit 主路径，每 cycle 必走** | 小（提一行到正常路径） |
| **P1** | 优化 #1 移植 DRUP 小数帧漂移累加器 | 32-bit 主路径，44.1k 家族 | 中 |
| **P1** | 优化 #2 收敛 `m_steadyState` 退出条件 | 主路径鲁棒性 | 小–中 |
| P2 | 优化 #4 drop 可观测性 / #5 时间戳契约 | 可观测性/可维护 | 小 |
| ✅ 已做 | §5 ①清理 statusUpdate 死代码（DRUP 对齐）③Q3 修复临时线程继承的 FF80 | 可观测性/排障/调度 | 已完成（待 Pi5 编译验证） |
| ✅ 已做 | §5.9 ④transfer-mode 重构：`auto+cycletime` 改 VarAuto 承载 + 新增 `autofix` 模式（保留原 FixAuto 行为）+ `transfer:` 日志补 `mode_sdk`/条件 `min_cycle` | 传输模式/可观测性 | 已完成（待 Pi5 编译验证） |
| ⛔ 已撤销 | §5.4 ②`--target-info` PULL 观测：实测确认读的是本机发送 profile 快照而非 target 实时状态，无增量价值，已删除回退；**结论：SDK 149 无任何实时读取 target 状态的接口** | —— | 已删除 |
| P2 | Bug #3 死三元 / Bug #4 return 类型 | 清洁度 | 极小 |
| **P3** | Bug #2 下变换字节序（修字节序或删路径） | fallback 路径，常规不触发 | 小 |
| P3 | 优化 #3 下变换 dither（仅当保留 convert） | fallback 路径 | 小 |
