# Target (GentooPlayerRpi4) CPU0 system time 翻转诊断

**状态**: 诊断中 / 等待最终验证
**日期**: 2026-06-24
**相关**: `docs/s2d_静音流与Target释放问题分析及改进方案.md` 中的 #2 idle-release 路径

---

## 1. 问题现象

Diretta Target 端( GentooPlayer RPi4 RAM 系统)在运行一段时间后,**CPU0 的 system time 会从一个很低的值(约 0.1%)逐渐升高**,中间经历约 12%,最终爬到 40%+:

- 不伴随卡顿、爆音或明显的 underrun。
- 触发条件不稳定:连续播放 3 小时不出现;切采样率、反复 release/重连、或长时间停止后再恢复播放时可能出现。
- **release Target / 重启 s2d / 重连 DAC 都无法让 CPU0 回落**;只有**整机重启 Target**才能恢复。

---

## 2. 已确证的数据事实

所有数据均来自 `/proc/stat`、`/proc/interrupts`、`/proc/softirqs`、`/proc/timer_list`、`/proc/meminfo` 等自带接口,在精简 RAM 系统上无需额外工具。

| 指标 | 健康态(开机播放) | 恶化态 | 结论 |
|---|---|---|---|
| `cpu0` 7 列中 system 增量(/10s) | ~1 tick (≈0.1%) | ~120 tick → 470 tick (≈12% → 47%) | **烧的是 system time** |
| `cpu0` irq 增量(/10s) | 稳定 | 稳定(~8 tick) | 不是硬中断 |
| `cpu0` softirq 增量(/10s) | 稳定 | 稳定(~17 tick) | 不是 softirq-NET |
| `28:` xhci-hcd:usb1 中断率 | 2407 irq/s | 2407 irq/s | USB 中断节奏没变 |
| `11:` arch_timer CPU0 累计次数 | 与其他核接近 | **1.8 亿次,为其他核约 1000 倍** | CPU0 定时器/协调工作暴高 |
| `IPI0:` Rescheduling interrupts | 分散 | **CPU0 + CPU2 各 6000 万+** | 大量跨核重新调度 |
| HI softirq CPU0 | 低 | ~2407/s | 与 USB 节奏同频 |
| active timers 总数 | 32 | ~30 | 不是定时器链表膨胀 |
| Slab / SReclaimable | 22 MB / 7 MB | 同量级 | 不是内存回收积压 |
| diretta_app_target 线程状态 | — | 全部为 `S`(sleep),无 `R` | 不是 diretta busy-poll |
| CPU2 idle | — | 占绝大多数 | CPU2 没自旋 |
| governor / freq | performance / 1.5 GHz | 同 | 不是降频 |
| temperature | 38–41 °C | 同 | 不是热节流 |

---

## 3. 已排除的根因

按数据逐一排除:

1. **USB xhci 中断本身**:irq28 稳定在 2407/s,没有随时间增加;挪核也证明 top-half 在 CPU1,不在 CPU0。
2. **网络 softirq / RPS**:NET_RX 在 CPU1 稳定,CPU0 的 NET_RX 几乎为 0。
3. **定时器膨胀**:active timers 约 30 个,两态无变化。
4. **内存 / Slab 回收**:Slab 22 MB,SReclaimable 7 MB,稳定。
5. **cpuidle 退化**:该精简内核没有 `/sys/devices/system/cpu/cpu0/cpuidle/`,cpuidle 框架未启用。
6. **diretta busy-poll**:diretta 线程全在睡,CPU2 大量 idle。
7. **温度 / 频率**:governor=performance,1.5 GHz 锁定,温度 40 °C 左右。
8. **s2d 发送数据量变化**:连播 3 小时不犯,说明不是单纯"长时间播放"累积。

---

## 4. 当前最佳解释(假设,待验证)

### 4.1 机制概述

Target 内核 cmdline 包含:

```
isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3
```

- `isolcpus=1-3`:CPU1/2/3 从普通调度器隔离,专门给音频线程用。
- `nohz_full=1-3`:当这些核上**只有 1 个 runnable 任务**时,关掉它们的周期 tick,让任务不被打断。
- `rcu_nocbs=1-3`:CPU1/2/3 的 RCU 回调由 housekeeping 核(CPU0)代劳。

**`nohz_full` 有一个硬前提:一个核上必须只有 1 个真正在跑的任务。** 如果同时有 ≥2 个任务要跑,内核必须重新给这个核发 tick,或者由 housekeeping 核(CPU0)远程发 `Rescheduling IPI` 来驱动抢占。

### 4.2 与 diretta 的交互

`diretta_app_target` 在 CPU2/3 上挂了 10+ 个线程。当某些线程同时 runnable 时,CPU2/3 不满足 `nohz_full` 的 tickless 前提。结果是:

- **CPU0 作为唯一的 housekeeping 核,被迫高频地:**
  - 维持 CPU1/2/3 的 timekeeping(`arch_timer` 暴增);
  - 发 Rescheduling IPI 驱动 1/2/3 上的线程切换;
  - 执行 HI softirq / RCU 等回调。

所有这些都记账在 **CPU0 的 system time**,而不是任何可见线程上。

### 4.3 为什么触发不稳定

是否触发取决于 diretta 线程的** runnable 分布**:

- 当只有 1 个线程在 CPU2/3 上真正 runnable → nohz_full 生效 → CPU0 闲。
- 当采样率切换 / 重连 / 长时间停止后恢复等操作改变了线程活跃分布 → 多个线程同时 runnable → nohz_full 失效 → CPU0 开始高频协调。

这种分布一旦进入"多线程 runnable"状态,就**自锁**了;只有整机重启才能重新初始化到干净状态。

---

## 5. 对音质的影响(未证实,勿过度解读)

### 5.1 信号层大概率无害

- s2d 传给 Target 的 PCM 比特没有变化(bit-perfect)。
- USB 异步 DAC 用本地时钟恢复采样时钟,主机 CPU 抖动不直接进入 DAC 时钟域。
- 未观察到 underrun、爆音、卡顿。

### 5.2 "数码声"的主观报告

用户报告在 CPU0 高时听感"数码声变多",但:

- 这与 CPU0 高之间的**因果关系未被证实**;
- 因为 htop 上 CPU0 数字可见,**期望偏误(心理作用)**是最合理的解释之一;
- 建议通过**盲听测试**验证:在看不到监控数字的情况下,随机切换高/低态,判断能否稳定区分。

**结论**:在确认盲听可区分之前,CPU0 高更可能是一个"数字不好看"的问题,而不是音质问题。

---

## 6. 待完成的验证实验

### 实验:去掉 `nohz_full=1-3`,保留 `isolcpus=1-3`

修改 GentooPlayer 启动 cmdline,例如:

```
isolcpus=1-3 rcu_nocbs=1-3
```

(去掉 `nohz_full=1-3`)

**预期结果**:

- CPU1/2/3 仍然是隔离的音频核(无普通任务干扰);
- 但不再要求 tickless,因此 CPU0 不必替它们高频协调;
- CPU0 system time 应稳定保持低位,不再"翻转"。

**实验方法**:

1. 修改 cmdline 后重启 Target;
2. 正常播放,隔一段时间用下面命令观察 CPU0:
   ```sh
   awk '/^cpu0 /{print $4}' /proc/stat; sleep 10; awk '/^cpu0 /{print $4}' /proc/stat
   ```
3. 尝试之前会触发翻转的操作(切采样率、停止后恢复等),看 CPU0 是否还涨。

**结果记录区**:

- [ ] 实验日期:
- [ ] 修改后 cmdline:
- [ ] 是否复现 CPU0 上涨:
- [ ] 备注:

---

## 7. 在精简 RAM 系统上可用的诊断命令

该系统缺少 `perf`、`paste`、`dmesg`、debugfs 等常见工具,但以下命令足够完成本次诊断:

```sh
# CPU0 各类时间增量
awk '/^cpu0 /{print $4,$7,$8}' /proc/stat; sleep 10; awk '/^cpu0 /{print $4,$7,$8}' /proc/stat

# 所有硬中断的 CPU0 列 5 秒增量
awk '{print $1, $2}' /proc/interrupts > /dev/shm/i1; sleep 5; awk '{print $1, $2}' /proc/interrupts > /dev/shm/i2
awk 'NR==FNR{a[$1]=$2;next}{d=$2-a[$1]; if(d>0) print $1, d/5"/s (CPU0)"}' /dev/shm/i1 /dev/shm/i2

# 所有 softirq 的 CPU0 列 5 秒增量
awk '{print $1, $2}' /proc/softirqs > /dev/shm/q1; sleep 5; awk '{print $1, $2}' /proc/softirqs > /dev/shm/q2
awk 'NR==FNR{a[$1]=$2;next}{d=$2-a[$1]; if(d>=0) print $1, d/5"/s (CPU0)"}' /dev/shm/q1 /dev/shm/q2

# 定时器总数
grep -c '#' /proc/timer_list

# 看当前正在 CPU0 上 running 的任务
ps -eLo stat,psr,comm | awk '$1~/^R/&&$2==0'

# IRQ affinity
cat /proc/irq/28/smp_affinity_list
cat /proc/irq/28/effective_affinity_list

# 内存 / Slab
grep -E 'Dirty|Writeback|SReclaimable|^Slab' /proc/meminfo

# 线程 CPU affinity
pgrep -f diretta | while read p; do
  echo "pid=$p affinity=$(grep Cpus_allowed_list /proc/$p/status)"
done
```

---

## 8. 如果验证失败(备选方向)

若去掉 `nohz_full` 后 CPU0 仍然翻转,则当前假设不成立,需回到以下方向:

1. 检查 `rcu_nocbs=1-3` 单独造成的 RCU 回调压力(CPU0 仍被甩 RCU 活);
2. 检查是否有某个内核线程(非 diretta)在 CPU0 上周期性自旋;
3. 检查 Target 端 diretta 的二进制是否在某些事件后启用了内部 busy-loop 模式。

---

## 9. 与 s2d 代码的关系

此问题**大概率不是 s2d 的 bug**,而是 Target 内核 cmdline 与 diretta 多线程运行时的交互。

s2d 端可以做的缓解(如果假设成立):

1. **尽量减小触发概率**:
   - sender + s2d 主机使用 chrony 对齐时钟,降低 PcmRing 漂移;
   - 保持稳定的 prefill,避免 rebuffer 后节奏抖动。
2. **重连路径一致性**:
   - 确保 `try_reconnect_same_format()` 重连时使用的 FormatConfigure、THRED_MODE、cpuMain/cpuOther 参数与首次 open 完全一致,避免重连后改变 diretta 线程行为。

但这些只能降低触发概率;**若要在 Target 侧根除,需调整 cmdline 或 diretta 配置。**

---

**文档版本**: 0.1
**下一步**: 完成第 6 节的 cmdline 验证,回填结果记录区。
