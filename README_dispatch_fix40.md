# T3DOTM Switch 移植 · fix40 派发说明

> 主题：给「看着像已处理、其实什么也没做」的缺页加一道**真映射闸门** —— 没有真映射时先借页 / 先清陈旧保护再补缺 / 再影子仿真，三条都不成就 `break` 转 SEH 据实上报，从根上掐死同址空转。
>
> 上一批（本批实测）的无限黑屏**不是崩溃**：崩溃日志一个字节都没被刷新，`RexSwitchPerfCount(2)` 以 **56.1k–57.2k 次/秒**在同一地址空转 15 分钟以上，`reintento / SEH / lectura emulada / fisica confirmada` 四类计数**全为 0**。

---

## 一、本批（fix39 现场）实机日志的定性

### 1.1 现场文件（`J:\XBOX360 port\Transformers Dark of the Moon\.dumate\inbox`）

| 文件 | 规模 | 内容 |
| --- | --- | --- |
| `rex_latido(13).log` | 2,786 条心跳，到 `t=928.4 s` | 每秒一行 `[fix27]` 心跳（fps / `corte` / `pisc` / `arena` / `map` / `rep`） |
| `rex_perfil(23).log` | 93 条快照 / 469,838 B | 剖析快照 + 全部线程的停顿采样（热点 pc / 栈 / 每线程 CPU） |
| `rex_stderr(24).log` | — | 取证明细（`[fixNN]` 计数、`[fix20]` 监视安装、`[DIAG]`） |
| `T3DOTM_001(23).log` / `T3DOTM_002(21).log` | 各 45 B | **游戏自身**的日志（内容不完整，见 1.5） |
| `rex_crash(23).log` / `rex_crash(24).log` | — | 崩溃报告 —— **两份逐字节相同** |

### 1.2 ⚠️ 先说版本：本批只能按「最近一次派发」推定，请核对设备上的哈希

本批日志里 **`[fix39]` 的三条取证串一条都没有**（`[fix39] fatal sin servir` / `fisica a la primera` / `presupuesto para el fault` 全 0），`[fix38] fisico liberado` 也是 0 条。因此**不要**用日志去反推版本。请把设备上 `sd:/switch/T3DOTM.nro` 拖出来核对 SHA-256：

- 若 = `29c14a1bef0…`（fix39 版）→ 本批现场就是本节结论的适用对象；
- 若 ≠ 该值 → 请把该 NRO 的哈希告诉我，我按它的实际内容重跑一遍判读。

### 1.3 本批现场（读数 + 出处）

| 读数 | 值 | 出处 |
| --- | --- | --- |
| 运行时长 / 帧率 | 928.4 s 全程 **0 fps**（2,786 条心跳里 2,784 条 `0.0`，2 条 `1.0`） | `rex_latido(13).log` |
| 故障计数（关键） | `manejador SDK` **56,980–57,218 /s**，而 `reintento emulado = 0`、`SEH = 0`、`lectura emulada = 0`、`fisica confirmada = 0`、`vistas = 0` | `rex_perfil(23).log` 91 条 `normal` 快照 |
| CPU | **201%–203%**（4 核 = 400%）；`XThreadB37E0A80` **99.6–100.4%** + `Main XThread (F` **99.7–99.9%**，其余线程全部 ≤ 0.7% | 同上 |
| 停顿采样热点 | 两个满转线程的采样 100% 落在镜像尾部同一段：`pc imagen+0x551fcf8` / `0x551fd20`，`lr imagen+0x551fe4c` | 同上 |
| 触发机制 | `pisc = 384/6 MB inactiva = 378 MB`（池几乎全空）、`corte = 47` 恒定、`map = 49/55 ent = 49/55`、`pags = 0 blq = 0 ent_pag = 0`；arena **u = 1672 MB / f = 62 MB / t = 100K / n = 4115**（末行 4116） | 心跳与快照 |
| 死机判据 | `[fix22] paron: 4 s sin fotogramas, muestreo forzado` + `[fix24] paron: volcado inmediato a los 5 s`；`=== durante los tirones: 1 fotogramas de mas de 45 ms (5001 ms en total) ===` | `rex_stderr(24).log` / `rex_perfil(23).log` |
| 崩溃 | **没有**：`rex_crash(23).log` 与 `rex_crash(24).log` 字节完全相同（SHA-256 均 `e70b6e09b5c850b804337de8e1609eb1e3e1a0ef43f1d00a1152f3ad87e31217`） | 两份 crash 文件 |
| 时钟 | `relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1600.0 MHz`（未超频） | `rex_stderr(24).log` |
| 监视安装 | `[fix20] vigilancia instalada caller=0x64f1b9e458 ventana=0x7FEA0000 largo=0x10000 access=0`、`[fix20] vigilancia instalada caller=0x64f1b9f094 ventana=0x7E010000 largo=0x10000 access=0`、`[fix20] solo-lectura RECHAZADA rc=0x0000D401: vigilancia por desmapeo` | `rex_stderr(24).log` |
| 监视实现方式 | 剖析原文：**`vigilancia: desmapeo (cada lectura falla)`**（每一次读都落进缺页处理器） | `rex_perfil(23).log` |

上批（fix38）与本批的逐项对比：

| 项目 | fix38 批 | 本批（fix39 现场） |
| --- | --- | --- |
| 池两本账 | 1256 MB 中 1152 用过 / `inactiva=104 MB` | 384 MB 中 6 MB 用过 / **`inactiva=378 MB`** |
| arena | `u=2991 f=… n=10202` | **`u=1672 f=62 t=100K n=4115`** |
| `corte` | 47 | 47（不再增长） |
| 崩溃 | 同址持续崩溃（`imagen+0x4441e4` / `far=…+0xBA842730`） | **无新崩溃**（crash 日志未刷新） |
| fps | `9.9 → 2 → 6 → 0`（尾段归零） | **全程 0.0** |
| 主线程占用 | 94.9% / 97.7% | **99.7% / 99.9%**（两线程） |
| 故障计数 | 未测 | **`manejador 56.1–57.2k/s`，其余四类全 0** |
| `neg2m/sin_back/al4k` | 0 / 0 / 0 | 0 / 0 / 0 |

### 1.4 机制判定

1. **不是崩溃路径**。crash 日志未被刷新 ⇒ 本批没有任何新的访问异常报告。里面那份仍是上一批的现场（`imagen+0x4441e4` / `far=…+0xBA842730`），**不得当作本批证据引用**。
2. **是缺页处理器在同址空转**。唯一在涨的计数是 `manejador SDK`（5.1 s 时 24,200/s，10.0 s 起 91 条快照稳定在 56.98k–57.22k/s），而同一个剖析行里的 `reintento emulado / SEH / lectura emulada / fisica confirmada` 全是 0 ⇒ 处理器**声称**已经处理，PC 没有前进，重试判定恒为「可重试」。
3. **空转的对象是两处 64 KB 监视窗**（`0x7FEA0000`、`0x7E010000`）。它们的只读视图被内核拒绝（`svcMapProcessMemory … rc=0x0000D401`），于是退到**解映射**来实现监视 —— 剖析原文 `vigilancia: desmapeo (cada lectura falla)`，全树 `vigiladas 1592 pags`。两个 100% 线程的所有采样点都落在镜像尾部同一段异常/桥接桩区，`lr imagen+0x551fe4c`，说明它们就卡在这条通路上。
4. **为什么空转不结束**（源码阅读结论，日志给出的是强旁证）：`MMIOHandler::Install` 是 `handlers_` 里的**唯一**处理器；`ExceptionCallback` 在 `mapped_ranges_` 未命中时**不早退**，而是取 `memory::QueryProtect`，只要「该页仍可访问」就 `return true`；`AccessViolationCallback` 命中 `LookupHeap + TriggerCallbacks` 时同样 `return true`。调用方读到 `true` ⇒ 认定已处理、`ctx->pc.x` 不前进；而 `reintentable = ok && (写 ? a == REX_GM_WRITE : a != REX_GM_NONE)` 对这类页**恒真** ⇒ 以 56.5k 次/秒的速度在同一条指令上重试，**永不**转入 SEH、永不落到 fatal 上报（这正是本批 `[fix39] fatal sin servir` 0 条、`SEH 0` 条、crash 日志未刷新的原因）。
   > 诚实边界：这条链路里「谁返回了 true、PC 为何不动」的逐行组合来自源码（`system/mmio_handler.cpp:53/73`、`system/xmemory.cpp:796-897`、`exception_handler_switch.cpp`），日志侧最强证据是「只有 `manejador` 在涨而其余四类拒绝计数全 0」。fix40 正是要**把这道判定变成可观测的**：命中一次就留下一行 `[fix40]`，下一次日志会直接告诉我到底是哪一页、哪一次权限、走了哪条分支。

5. **本批的 0 fps 与内存完全无关**。池 384 MB 里 378 MB 闲置（`[fix36] no reservo mas, inactiva 384 MB de 384 MB tomados y 0 MB en uso`），arena 1672/1734 MB、`corte=47` 不再增长 ⇒ 预算这本账比上批宽得多，**卡住的是通路本身，不是容量**。

### 1.5 顺带核到的两处小账（不改变主结论）

- `[fix28] log del juego: ruta='/switch/Transformers3DOTM/logs/T3DOTM_002.log' largo=45 rc 0xFFFFFFFF errno=5` ⇒ 游戏自身的日志写入是失败的，两份 `T3DOTM_00X.log` 都停在 `Creating graphics pipeline state with VS 1E6883FCCDE1F688`（06:32:55 / 07:04:29）。因此「游戏卡在哪句日志」只能当**弱证据**（写不进去，停下可能只是写失败的位置）。
- `map = 49/55`、`allocs = 49`、`entradas = 49` 是同一量级；`pags = 0 blq = 0 ent_pag = 0` 说明**跨页**（> 4 KB 的读写）这一支根本没被打开过 —— 这一支与 fix40 的诊断串互补，若下次日志里出现非 0，说明问题在跨页这一侧。

---

## 二、fix40 改了什么（1 个文件 / 1 处 / +69 行）

改动位置：**`repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp`**（其余文件一律未动）。

在 MMIO 处理器**接受一次访问**之前，先加一道真映射闸门：

```cpp
        if (ok) {
          // … 原有逻辑保护条目查询 …
          bool real40 = !!(info40.perm & Perm_R) || !!(info40.perm & (is_write ? Perm_W : Perm_R));
          real40 = real40 && far >= info40.addr && far < info40.addr + info40.size;
          if (!real40) {                 // 真映射不在 ⇒ 不认领
            bool servido40 = false; const char* via40 = "ninguna";
            if (!is_write && RexGmBorrowWatchForRead(far)) { servido40 = true; via40 = "prestamo"; }
            if (!servido40) {
              const size_t pago40 = static_cast<size_t>(far - RexGmBase()) & ~size_t(0xFFF);
              RexGmClearProtect(pago40, 0x1000);
              if (RexGmFaultIn(far)) { servido40 = true; via40 = "fault-in"; }
            }
            if (!servido40 && EmulateViaShadow(ctx)) { servido40 = true; via40 = "sombra"; }
            … 有界诊断：前 32 次 + 每 4096 次一条 [fix40] …
            if (servido40) { RexSwitchPerfCount(2); RexResumeFromException(ctx); }
            break;   // no hay quien lo sirva: mejor el informe que un bucle de reintentos
          }
        }
```

三条补救的语义：

1. **借页给读** —— 只读访问命中「无真映射」时，借一页监视窗（`RexGmBorrowWatchForRead`），让这次读**真正**被服务掉，而不是靠声称。
2. **清陈旧保护条目 + 补缺** —— 按页清掉陈旧的逻辑保护条目（`RexGmClearProtect(pago40, 0x1000)`），再 `RexGmFaultIn(far)` 真正把页补进来；这一步直接针对「归还 raw alias 时把逻辑保护留在账上」这一类脱钩。
3. **影子仿真** —— 都不行时退回 `EmulateViaShadow(ctx)` 真正把这一次读写仿真掉（返回值放回 `ctx->cpu.IT`）。
4. **仍不成 → `break`** —— 不再 `return true` 认领，交回上层转 SEH/上报。**这一条才是根治**：把「以 56.5k/s 空转 15 分钟」降级为「一行诊断 + 一次上报」。

诊断串（新增，用了 `%zu/%d/%s` 规避 ARM 上 `%lu` 的变参错配）：

```
[fix40] reintento sin mapeo real: invitado=0x%llX manejador=%zu escritura=%d permiso=%d acceso=%d real=%d via=%s servido=%d (n=%u)
```

- `invitado` = guest 地址（已减去 arena base）；`manejador` = 处理器序号；`escritura` = 这次是不是写；`permiso` = `ok`；`acceso` = 访问类型 `a`；`real` = 真映射判定；`via` = `prestamo` / `fault-in` / `sombra` / `ninguna`；`servido` = 最终有没有服务掉；`n` = 累计次数（**前 32 次全打，之后每 4096 次一条**，所以本批这个串在日志里出现 1 次是正常的）。

源码指纹（请一并核对）：

| | 行数 | 字节 | CRLF/LF | BOM | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| 改动前（fix39） | 1,450 | 65,971 | 946/504 | 无 | `390a79d008ef5a9a…` |
| **改动后（fix40）** | **1,522** | **69,971** | **1018/504** | 无 | **`e3cfab891d247450917947a5a19c0914fe1aa75b19413739975d91f73ef2d296`** |

- 备份：`exception_handler_switch.cpp.fix40.bak`（= 改动前的 fix39 版，65,971 B / `390a79d0…`）；
- 物理行 1,522 = 逻辑 1,450 + 插入 69 行 + 因换行落单而多出 3 个物理行，逐行 EOL 全部保留（新增行一律用 CRLF，与文件主流风格一致）；
- **未改动**：`src/core/guest_memory_switch.cpp`（3,534 行 / 170,511 B / 纯 CRLF 无 BOM / `773b1939…`）、`src/ui/switch_perf.cpp`（1,754 行 / 79,062 B / `6e180117…`）、`cmake/rexglue_switch.cmake`（161 行 / 8,505 B / `81baa7e6…`）。

---

## 三、构建与产物校验（实测）

| 步骤 | 做法 | 结果 |
| --- | --- | --- |
| 单 TU 前置编译 | `aarch64-none-elf-g++` 单独编译改动后的 `exception_handler_switch.cpp` | rc = 0（无告警输出） |
| 全量构建 | `ninja -j 4`，`-DT3DOTM_LTO=ON`（图内 `-flto` 计数 86），日志 `recomp/out/sw8/fix40_build.log` | **624/624**，`error:` 0 行，`[624/624] Linking CXX executable T3DOTM; Packaging T3DOTM.nro`，NRO 于 **20:16** 写出 |
| NRO 结构校验 | `tools/verify_nro.py` | **Verdict: PASS**（text `0x055ce000` / rodata `0x0056a000` / data `0x00d01000` 三段 `[OK]`；Total mapped `0x6839000` = 104.2 MiB；MOD0 bss `0x634d58`） |
| 补丁确实进了产物 | `strings` 计数 | `[fix40] reintento sin mapeo real:` **1** 条；分片 `manejador=` / `escritura=` / `permiso=` / `acceso=` / `real=` / `via=` / `servido=` 均在；`[fix39]` 三条**保留** |
| 符号 | `nm` | `RexResumeFromException` / `RexGmClearProtect` / `RexGmFaultIn` / `RexSwitchPerfCount` 各 1 处 |
| 补丁 TU 重编译确认 | 构建日志 | `Building CXX object …/exception_handler_switch.cpp.obj`（33,264 B / 19:10）✅ |

产物指纹：

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| **`T3DOTM.nro`（交付件）** | **109,451,278** | **`3ba8f408667b19e81800c3253c4d3eb9a7b8862de2fb728df2873ea722de376f`** |
| `T3DOTM.stripped.elf` | 109,352,072 | `d0b6ef78225348575e9889fb5cbd65053681c44e0e87bb04a80f23b1058ad6f2` |
| `T3DOTM`（未剥离 ELF） | 128,149,016 | — |

> ⚠️ **`T3DOTM.nro` 与 fix39 的字节数完全相同（都是 109,451,278）** —— 新增代码落在了 4096 对齐的余量里，体积没变。**认版本只能看 SHA-256**：fix40 = `3ba8f408…`，fix39 = `29c14a1b…`。

> 构建方式备注（下批复用）：本轮因清理了陈旧的 `.ninja_log` 触发 624 项重编（含游戏分片与 FFmpeg），耗时很长；**`.ninja_deps` 绝不能删**（会触发全量重建）。另外，派发构建的那个工具调用**必须正常返回**——本轮 18:41 那次被中断后进程树连根被杀（日志停在 `[2/3]`），19:08 那次正常返回后一路跑到 `[624/624]`。

---

## 四、在你的设备上怎么复测

1. 备份旧档后放置新档（详细见 §七）：把 `T3DOTM.nro` 放到 `sd:/switch/T3DOTM.nro`。
2. 先删掉上一批的日志，避免混淆；或用 `[fix40]` 串区分也行。
3. **不要开超频**（本批 0 fps 是通路问题，不是算力问题；见 §六）。
4. 冷启动后**至少让黑屏现场持续 5–10 分钟**（本批的首次快照是在 5.1 s 触发的，之后 10.0 s 起每 1 s 一条）。
5. 退出后把 `sd:/switch/Transformers3DOTM/logs/` 下的 `rex_latido*.log`、`rex_perfil*.log`、`rex_stderr*.log`、`rex_crash*.log`（以及 `T3DOTM_00X.log`）打包回 `.dumate\inbox`。

**只看一件事：`[fix40]` 串里 `via=` 与 `servido=` 的取值。**

| `via` / `servido` | 含义 | 下一步 |
| --- | --- | --- |
| `prestamo` / `1` | 借页把这次读服务掉了 | 若帧率起来 ⇒ 修对了；若仍 0 fps ⇒ 还有第二处在空转，看 `n` 的增长速度 |
| `fault-in` / `1` | 清陈旧保护 + 补缺成功 | 说明就是「保护条目与真映射脱钩」，后续应把这类条目在归还时清掉 |
| `sombra` / `1` | 影子仿真是对的通路 | 说明该页该由仿真提供，后续可把仿真提前、避开借页/补缺 |
| `ninguna` / `0` | 三条都救不了，已转 SEH | 这一批一定还会出一条 SEH/fatal 报告（本次 `break` 的目的），**那条报告就是下一批的抓手** |
| 完全不出现 | 没有「无真映射」的认领 | 说明空转另有原因（例如 `a == REX_GM_WRITE` 但页确实是写保护的），下一批改查「真映射在、却不该认领」这一侧 |

配套判断（照旧）：`fallos/s` 里 `manejador SDK` 是否**掉下来**（本批稳定在 5.7 万/秒）；`reintento emulado / SEH / lectura emulada` 是否开始出现非 0（出现非 0 是**好事**：说明它终于不再假装已处理）；心跳里 `rep` 是否停止增长；`XThreadB37E0A80` / `Main XThread (F` 的 CPU 是否从 99.7/99.9% 掉下来。

---

## 五、12 项快查（判读任何一批日志都用这张表）

| # | 看什么 | 医好长什么样 |
| --- | --- | --- |
| 1 | `[fix26]` 宿主初始化 | 无 fatal、无 `return 早退` |
| 2 | `[DIAG] RexGmInit` | `base/size` 与上一批一致（`0x35e000000` / `0x120001000`） |
| 3 | `[fix23] RexGmConfigure` | `modo=2`（0/1 都是没配好） |
| 4 | `[fix37] hito fin de RexGmInit` | 一行、含 `pool/arena/cortes` 三个账目 |
| 5 | `[fix35]` / `[fix36]` 预算 | 池不再增长且有 `inactiva`；`[fix36]` 只在真的没再预留时出现 |
| 6 | `[fix20]` 监视安装 | `ventana` / `largo` / `access` 与设计一致；只读窗被拒（`rc=0x0000D401`）是**预期**行为 |
| 7 | `[fix27]` 心跳 | `fps` 稳定非 0 且 `t` 与 `fps` 一致；`corte` 不增；`rep` 不涨 |
| 8 | `[fix22]` / `[fix24]` 停顿 | 不再出现（出现即仍在停顿） |
| 9 | `[fix28]` / `[fix29]` 存储 | 有 `rc=0` / 游戏日志有实际内容（本批仍是 `errno=5` 写入失败） |
| 10 | 剖析热点 | 热点回到**游戏镜像内部**（而不是镜像尾部的异常/桥接桩区） |
| 11 | crash 报告 | **时间戳/哈希有变化**才说明发生了新异常；本批两份完全相同 ⇒ 未刷新 |
| 12 | 时钟 | `CPU 1020 / GPU 460.8 / 1600`：不变即未超频 |

---

## 六、性能目标与超频策略

- 目标：「**不需要超频**，流畅 60 帧」。
- 本批的 0 fps 与算力无关：两线程各 100%，但 `manejador` 5.7 万/秒空转、池 378 MB 闲置、`arena` 还有 62 MB 裕量 ⇒ 是**通路卡死**。
- 因此 fix40 之后**仍不要超频**：先把通路打通、看到「有帧、且帧率随内容变化」，再谈帧率优化。
- 后续若确实需要余量，已核实的上限口径是：CPU **1020 MHz**、GPU **460.8 MHz**、内存 **1600 MHz**（本批实测读数即此，说明当前 APM 配置已经是该口径，并未越界）。

---

## 七、放置、回退与指纹表

- 交付件：**`T3DOTM.nro`**（109,451,278 B / `3ba8f408667b19e81800c3253c4d3eb9a7b8862de2fb728df2873ea722de376f`）+ 本说明文档。
- 放置：解压后把 `T3DOTM.nro` 放到 `sd:/switch/T3DOTM.nro`（覆盖旧档即可）。
- **先核对哈希**（Windows 上）：`certutil -hashfile T3DOTM.nro SHA256`。
- 回退：`T3DOTM_dispatch_fix39.zip` 内的 NRO 就是本次运行的那个版本。

| 版本 | NRO SHA-256 前缀 | 说明 |
| --- | --- | --- |
| **fix40** | **`3ba8f408667b19e8…`** | 本次交付 |
| fix39 | `29c14a1bef0…` | 本批实机运行的版本（推定，请核对） |
| fix38 | `3492f026…` | |
| fix37 | `29f21278…` | |
| fix36 | `a271aa08…` | |
| fix35 | `e9af9252…` | |
| fix34 | `d01a2630…` | |
| fix33 | `b5d1e058…` | |

源码指纹表：

| 文件 | fix39 | **fix40** |
| --- | --- | --- |
| `exception_handler_switch.cpp` | 1,450 行 / 65,971 B / `390a79d0…` | **1,522 行 / 69,971 B / `e3cfab89…`** |
| `guest_memory_switch.cpp` | 3,534 行 / 170,511 B / `773b1939…` | 未改动 |
| `switch_perf.cpp` | 1,754 行 / 79,062 B / `6e180117…` | 未改动 |
| `cmake/rexglue_switch.cmake` | 161 行 / 8,505 B / `81baa7e6…` | 未改动 |

---

## 八、仍未完成 / 待确认（诚实清单）

1. **60 帧目标尚未达成、也尚未验证**：fix40 解决的是「无穷空转 ⇒ 无界拖延」，把黑屏从**静默死锁**降级为**有据可查的上报**；要看到流畅 60 帧，必须先让引擎跑过初始化并产生首帧。
2. `a == REX_GM_WRITE` 具体落在哪些页仍未确证（本批只有 `0x7FEA0000` / `0x7E010000` 两处 64 KB 监视，`access=0`）。fix40 的 `[fix40]` 串就是为了把这件事从「推断」变成「读数」。
3. **fix39 版本归属待你核对**：本批日志没有 `[fix39]` 阳性串，我只能按「最近一次派发」推定（§1.2）。
4. `[fix20] caller=0x64f1b9e458` 归属哪个 SDK 组件未确证。
5. `[fix28] rc 0xFFFFFFFF errno=5`（日志写入失败）与心跳里 `log=0KB` 是否同因未确证。
6. 本批剖析供应商**没有**采到栈样本（`Muestreo de pilas: desactivado`），所以「热点在 `SoltarFisicoSinUso` / `_Rb_tree_increment`」**不能**写成实测结论；本批能实测的是「两个线程的采样点全在镜像尾部异常桩区」。
7. 上一批提到过的「约 880 MB 那道墙」在本批**没有出现**（arena 只到 1672 MB），因此它是不是真正的硬上限，仍待后续批次确认。

---

## 九、交付物

| 文件 | 说明 |
| --- | --- |
| `T3DOTM.nro` | 109,451,278 B / `3ba8f408667b19e81800c3253c4d3eb9a7b8862de2fb728df2873ea722de376f` |
| `README_dispatch_fix40.md` | 本文档 |
| `T3DOTM_dispatch_fix40.zip` | 内层单一运行文件 `T3DOTM.nro`（已流式核对内层 SHA-256 = 上表值） |

同步位置：工程根目录 `J:\XBOX360 port\Transformers Dark of the Moon\` 与 `.dumate\inbox`。