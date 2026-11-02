# T3DOTM Switch 移植 – fix37：把缺页风暴限流到「每块每 2 秒一次」，并把宿主那 ≈1.6 GB 定名

本轮依据：你回传的 fix36 实机复测 6 份日志 —— `.dumate/inbox/rex_crash(21).log`、`rex_latido(10).log`、`rex_perfil(20).log`、`rex_stderr(21).log`、`T3DOTM_001(20).log`、`T3DOTM_002(18).log`。

**一句话结论**：fix36 三项**全部生效**（前端 NULL 崩溃消失、池闲置被压在 ≤370 MB、`[fix36]` 配额如期），前端动画段还首次跑到 **20.0 fps（峰值 23.9）**。但它把新的主导机制暴露干净了：`MaterializarBloque` 里那条「试满 `kIntentosBackingMax=4` 就永久放弃」的闩锁，在风暴路径上**根本走不到**（三条拒绝出口没有一处 `++c.fallos`），于是**每一次缺页都要完整付一遍昂贵扫描、而且全程持着映射锁**——定格期 `corte` 以 **~780 次/秒**增长（56.1 s→58.1 s：23,411→24,976，单次 ≈1.3 ms），`Main XThread` 占 **97.5–97.6%**，其它线程被一起拖死，画面定格。fix37 把这条闩锁改成**时间窗限流**（同一块最多每 2 s 重试一次，其余直接拒绝、不付代价），给 `mallinfo()` 加 250 ms 缓存，并装上**宿主堆尺寸类直方图**与**四个一次性 hito 快照**，用来回答本批唯一没能定名的问题：`proceso 3185 MB` 里、扣掉 guest 自报的 1,372 MB 之后，那 **≈1,643 MB 到底是谁持有的**。

---

## 一、上一批（fix36）实机日志的定性

### 1. 确证本批跑的是 fix36

| 判据 | 本批命中 | 出处 |
| --- | --- | --- |
| `[fix36]` 独占串 | `piscina: no reservo mas` **3 条**（`[fix36]` 就这一种） | `rex_stderr(21).log` |
| `[fix35]` 独占串 | `piscina: sin losa nueva` **16 条**（fix36 有意保留历史串，命中 ≠ 跑的是那一版） | `rex_stderr(21).log` |
| `[fix33]` 独占串 | `piscina: compactada` **1 条**（同上，历史串） | `rex_stderr(21).log` |
| 崩溃种类 | `Failed to get a Vulkan upload buffer` **0 条** | `rex_stderr(21).log` |
| fix36 新仪表 | 心跳行含 `inactiva=… MB` 字段 | `rex_latido(10).log` |

⚠️ **版本只能用 NRO SHA-256 定**（见 §七）；字符串命中 `[fix27]/[fix31]/[fix33]/[fix35]` 都说明不了版本——本批心跳行的前缀就是历史遗留的 `[fix27]`。

### 2. 本批现场（全部实测自本批 6 份日志）

| 观测项 | 本批读数 | 出处 |
| --- | --- | --- |
| 前端空指针崩溃（fix35 批的 `hilo `/`esr ` 行） | **消失**，`rex_crash(21)` 只剩两条 `[fix15]` 缺页诊断 | `rex_crash(21)` |
| 池闲置（fix36 的目标） | `inactiva` 峰值 **370 MB**（登记值一路 342→370 MB，≤384 ✓） | `rex_latido(10)` |
| 池两本账 | 早期 `pisc=384/42…88 MB`；定格现场 **`pisc=1270/1270 MB inactiva=0`** ⇒ 池里没有闲置可让 | `rex_latido(10)` |
| `[fix36] no reservo mas` | 3 条（配额 8，如期、未刷屏） | `rex_stderr(21)` |
| 帧率轨迹 | 跑图段曾到 **22.9 / 23.9 fps**；定格后 `fps=0.0` | `rex_latido(10)` + `rex_perfil(20)` |
| 定格时长 | `lento` 从 0 涨到 **32 s**（即 ≈t=25 s 起定格），看门狗记 `parones 1` | `rex_latido(10)` + `rex_perfil(20)` |
| 主线程占用 | `Main XThread` **97.5–97.6%**（同刻 `XThread… 16.6% / 2.8%`） | `rex_latido(10)` |
| 缺页拒绝计数 | `corte` **24,976**；定格期增速 **~780 次/s**（56.1→58.1 s：23,411→24,976） | `rex_latido(10)` + `rex_perfil(20)` |
| 映射锁占用 | `lock 1–4 ms`（峰值 `max 7ms/551`） | `rex_latido(10)` |
| 2 MB 拒绝/回退 | `neg2m=4,984`、`sin_back=4,967`（定格期仍在涨） | `rex_latido(10)` |
| 未落位块 | `pendientes 288 MB (144 bloques)`（比 fix35 的 160 MB / 80 块更多，全程未回落） | `rex_perfil(20)` |
| 页级窗口 | `pags=4608 blq=9 ent_pag=4608`（顶格）、`map/ent=5285/755`、`lib=126,8` | `rex_latido(10)` |
| 宿主账目 | `arena u=3015 f=56 MB tope 68 KB n=15,102`；`proceso 3185/3189 MB`；`invitado 1372/1122 MB` | `rex_perfil(20)` |
| guest 映射 | `mapeos: codigo 5285 (~5285 entradas, 1372 MB), ventana 755 (~755 entradas, 1507 MB)`；`liberadas 126 vistas crudas en 8 eventos, presion 1` | `rex_perfil(20)` |
| 时钟 | `CPU 1785.0 / GPU 921.6 / memoria 1600.0 MHz`（三档上限，未启用超频兜底） | `rex_perfil(20)` |
| 游戏日志停止点 | 两份逐行一致，仍停在 `fix14 alloc: addr=46F90000 size=0x37080000` | `T3DOTM_001(20)` / `T3DOTM_002(18)` |

本批两段启动都能看到：第一段（前端）10.0 s 时 **`juego 20.0 fps`**、`arena usado 1702 MB`、`piscina 384 MB`、`materializados 81`；第二段（进第二画面）10.0 s 时 `juego 9.4 fps`，随后进入 `modo: tiron`（`muestras 988`）、`fps` 归零并定格。

### 3. 机制判定

**(a) fix36 三项全部生效，但两条预置判读键都要改道。** fix36 README §五 给了两条分支：①「若前端仍崩，把闲置上限从 384 MB 降到 192 MB」——**不采用**（崩溃已消失）；②「若 `pendientes` 仍不降」——确如所料没降（160 MB→288 MB），但**原因不是池囤积**：定格现场 `inactiva=0`，池里根本无闲置可让 ⇒「继续缩闲置上限」这条线被数据否决。

**(b) 缺页风暴是新的主导机制。** 证据链：
- 定格期 `corte` 以 ~780 次/s 增长（56.1→58.1 s：23,411→24,976），即每次拒绝 ≈1.3 ms；
- 同刻 `Main XThread` 97.5–97.6%，`lock` 单次持锁 1–4 ms（峰值 7 ms，551 次）；
- `arena u=3015 f=56 MB n=15,102` ⇒ 空闲链已被切成 **15,102 段**，每次 `mallinfo()` 都要走完整条链；
- `pendientes` 288 MB（144 块）在本批**从未回落**。

**(c) 代码层证据：放弃闩锁不可达。** `MaterializarBloque` 的三条拒绝出口没有一处推进 `c.fallos`：
- 2 MB 拒绝分支：只 `++g_denegaciones_2m`（`neg2m` 已在涨，证明该路径确被走到）；
- 页级硬闸门分支：`return false`，注释就写着「sin tocar c.fallos」；
- 终局失败：走 `LiberarPresupuestoParaCarga` 之后也不 `++c.fallos`。

因此 `if (c.fallos >= kIntentosBackingMax) { /* 放弃 */ }` 这条闩锁**永不命中**，每一条缺页都要完整跑：`memalign(2 MB)` 扫 15,102 段空闲链 → `PiscinaReservar → PiscinaTomarLosa → mallinfo()` 堆走查 → 阶梯 memalign，且**全程持映射锁**。定格不是「内存不够」的新形态，而是**每次都付这份代价 + 占着锁**造成的自锁。

**(d) 账目里那 ≈1,643 MB 仍未定名。** `arena usado 3015 MB`，而 guest 自报只有 `invitado 1372 MB`（≈1,372 MB），差额 **3,015 − 1,372 ≈ 1,643 MB** 既不是 guest 堆、也不等于 `piscina`（1,270 MB 另有其账）。这正是 (C)+(B) 要回答的问题。

### 4. 与 fix35 批的差别

| 项 | fix35 批 | fix36 批（本批） |
| --- | --- | --- |
| 前端 | 启动即 NULL 崩溃（`hilo `/`esr `） | **不再崩**，前端动画段跑到 20.0 fps |
| 池两本账 | `pisc` 恒 1,536/1,536（顶格） | `pisc=384/…` → `1270/1270`，闲置被压到 ≤370 MB |
| 定格现场 | 前端崩 | 进第二画面后定格，`Main XThread` 97.6% |
| `corte` | 十万量级 | 24,976，但定格期增速 **~780 次/s** 且不降 |

---

## 二、fix37 的改动（2 个源码文件 + 1 个构建配置）

### 1. `guest_memory_switch.cpp`（3,120 → 3,377 行；148,457 → 161,781 B）

**(D) 缺页风暴限流（核心）**
- `Chunk` 增加 `u64 ultimo_intento = 0;`（@439）；
- 新增常量 `kRetiradaEsperaMs = 2000`、`kTicksPorMs = 19200`（@279-280）⇒ 2 秒 = 38,400 tick；
- `MaterializarBloque` 把「永久放弃」改成**时间窗限流**（@1673-1678）：同一块距上次尝试不足 2 s 就直接拒绝、**不付扫描代价**，满 2 s 才放行一次；
- 终局失败补 `if (c.fallos < kIntentosBackingMax) ++c.fallos;`（@1769），让既有的「试满 4 次放弃」闩锁**可达**，作为限流之外的第二道闸。

**(A) 把 `mallinfo()` 摘出热路径**
- `kArenaCacheMs = 250` + `ArenaLibreCacheado()`（@1054-1059）：250 ms 内复用上次的空闲量；
- `PiscinaTomarLosa` 用它取代现场 `mallinfo()`（@1127）。

**(B) 一次性 hito 快照**
- `Hito()`（@1073）在四个关键点调用（@1475 / @1723 / @1771 / @2294），把「进入 / 失败 / 终局 / 退出」四态的 arena、池、拒绝计数各打印一次，避免只能看心跳末值。

**(C) 宿主堆尺寸类直方图（用来认领 ≈1.6 GB）**
- `kCajaLimites[7] = {32, 128, 512, 2048, 8192, 65536, 524288}`（@57）；
- `g_caja_*` 原子计数（@60-64），在 `__wrap_malloc/calloc/realloc/free/memalign`（@118/124/130/144/149）里用 `malloc_usable_size` 记账；
- `__real_*` 声明（@112-116），与 §二.3 的 `--wrap` 配套；
- 导出 `RexGmMallocVivo` / `RexGmMallocBloques`（@3364 / @3367），供 perf 线程读取。

### 2. `switch_perf.cpp`（1,748 → 1,754 行；78,751 → 79,062 B）

- 心跳行追加 `mg=%zu/%zu`（宿主堆「活块 / 总块」），与既有 `pisc= / corte= / inactiva=` 并列；
- 新增 `[fix37] hito %s: arena u=%zu MB f=%zu MB t=%zu KB n=%zu | piscina %zu/%zu MB cortes=%zu …`（对应 (B) 的四次调用）；
- 新增 `[fix37] heap clases (KB vivos/bloques): <=32B %llu/%llu | …`（把 (C) 的七个尺寸类打成一行）。

### 3. `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake`（8,132 → 8,505 B，+373 B）

- 新增 wrap 项：在 `"LINKER:--wrap=svcSleepThread"` 之后加一行
  `"LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=memalign")`
  —— 这是 (C) 直方图能生效的前提（devkitA64 newlib 没有 `malloc_info` / `posix_memalign`，只能用 `--wrap` + `malloc_usable_size`）；
- 在 `target_link_options(rexglue_switch_startup INTERFACE` 之前插入 3 行注释，说明直方图是为「定名 ~1.6 GB」而加。

⚠️ `repos/masseffect-nx/sdk/cmake/rexglue_switch.cmake`（8,464 B）是同名**参考副本**，**不要动它、也不要用它的指纹比对**。

---

## 三、构建与产物校验（本轮实测）

- 构建：`bash scripts/build_and_verify.sh -j 4`（`export DEVKITPRO=/opt/devkitpro`；全量日志 `.dumate/tmp/build_fix37.log`）。
- 编译：`error:` = **0**、`warning: format` = **0**；`lto-wrapper.exe: warning: using serial compilation of 128 LTRANS jobs` 与 `ld.exe: warning: creating DT_TEXTREL in a PIE` 属常态。
- 链接耗时：`recomp/out/sw8/.ninja_log` 末行 `8058 8699694 … T3DOTM 5e25abb899b2d73e` ⇒ 最终链接 **8,700 s ≈ 2 h 25 min**；`[5/5] Linking CXX executable T3DOTM; Packaging T3DOTM.nro`。
  ⚠️ **判进度看 `recomp/out/sw8/.ninja_log` 末行，不要看 build log 尾部**——stdio 缓冲会让它长时间停在 `[4/5]` 不动。
- `verify_nro.py`：**Verdict: PASS**（text `0x055cd000` / rodata `0x0056a000` / data `0x00d01000` / MOD0 bss `0x634d58` = 6,507,864 B，比 fix36 的 `0x634b98` 多 448 B）。
- 产物：
  - `recomp/out/sw8/T3DOTM.nro` = **109,447,182 B (0x686080E)**，SHA-256 **`29f212782c9e1c21088e37eb1276dc4b25465009a6af6e37ace7f2efb0d3ef70`**；
  - `T3DOTM`（未 strip ELF）= 128,144,184 B；`T3DOTM.stripped.elf` = 109,347,976 B。
  - ⚠️ **体积自 fix33 起固定为 109,447,182 B，不再随改动变化——绝不能用体积判版本，只能看 SHA-256。**
- 字符串 / 符号核对（`.dumate/tmp/check_fix37_nro.py`，从源码抽字面量后在 NRO 里逐条计数）：9 个模式**全部在位、0 缺失**：
  `[fix37] hito %s: …`、`[fix37] heap clases (KB vivos/bloques): …`、心跳尾 `mg=%zu/%zu`，以及既有 `[fix36]`、`[fix31] piscina: losa`、`[fix33] piscina: compactada`、`inactiva=`、`arena u=`（×3）、`pisc=`（×2）；
  ELF 符号 `RexGmMallocVivo` / `RexGmMallocBloques` / `__wrap_malloc` / `__wrap_memalign` / `__wrap_calloc` / `malloc_usable_size` 均在。
  ⚠️ `__real_malloc` / `__real_memalign` **不出现是正常的**：`--wrap` 生效后 `__real_*` 由链接器消解；**链接成功本身就是 `--wrap` 生效的证据**（否则它们会是未定义符号）。

---

## 四、复测步骤

1. 把 `T3DOTM.nro` 放到 `/switch/T3DOTM/T3DOTM.nro`（覆盖旧文件），保留 `romfs` 与 TU 覆盖层不变；
2. 冷启动进游戏，**完整跑一次前端动画 + 进第一个可玩画面**，等画面静止 30 s 以上再取日志；
3. 采集 6 份日志（`rex_crash`、`rex_latido`、`rex_perfil`、`rex_stderr`、`T3DOTM_001`、`T3DOTM_002`）回传；
4. 想确认跑的是 fix37，先算 NRO 的 SHA-256 对 `29f21278…d3ef70`（§七）——**不要**用体积、也不要用字符串命中 `[fix35]/[fix27]`。

---

## 五、判读方法（本轮唯一重点：缺页风暴有没有停下来）

| # | 判读键 | 期望（fix37 生效） | 若不符 |
| --- | --- | --- | --- |
| 1 | **`corte` 增速** | 定格期从 ~780 次/s 明显回落（限流后每次缺页不再付扫描代价，同一条链应只剩零星重试） | 仍 ~780 次/s ⇒ (D) 未走到或 tick 口径不对（查 `kTicksPorMs`） |
| 2 | **`Main XThread` 占比** | 应从 97.6% 明显回落（不再 100% 陷在 `mallinfo ← PiscinaReservar ← RexGmFaultIn`） | 仍 ≈97% ⇒ 热点另有其处，看 `[fix37] hito` 落在哪一段 |
| 3 | **`[fix37] hito` 四条快照** | 「进入 → 失败 → 终局 → 退出」四态各打一次，给出行号级现场 | 缺某条 ⇒ 对应分支本批未被走到 |
| 4 | **`[fix37] heap clases`** | 给出七个尺寸类的「活块 / 总块」，用来认领那 ≈1.6 GB | 全 0 ⇒ `--wrap` 没链上（回到 §二.3） |
| 5 | **心跳 `mg=`** | 「活块 / 总块」随时间变化，可当宿主堆压力的连续曲线 | 恒定不变 ⇒ 直方图未刷新 |
| 6 | **`pendientes`** | 期望开始回落；若仍顶在 288 MB（144 块）⇒ 真墙是**进程容量**，转页级部分物化 | |
| 7 | **游戏日志** | 是否越过 `fix14 alloc: addr=46F90000 size=0x37080000`（本批仍停在这里） | 未越过 ⇒ 装载处仍未过去 |
| 8 | **`fps`** | 是否在第一个可玩画面段首次脱离 0.0 并稳住一段 | 若已稳一段 ⇒ 转 T18 帧率测定 |
| 9 | 旧仪表 | `pisc= / corte= / inactiva=`、`arena u/f/t/n`、`proceso`、`pags/ent_pag/blq`、`neg2m/sin_back/al4k`、`map/ent/lib` 一并对读 | |

### 本批新增观察项（(D) 的语义边界，必看）

把「永久放弃」改成「每一块每 2 秒放行一次」后，被放行的块会**重新入册**并**再试一次**。这是期望行为，但有一个副作用要看：若游戏接着去读那些仍未落位的页，可能出现**零数据症状**（贴图错色 / 几何缺块 / 贴地异常）——这和「有没有画面」是两件事，请一并记录（出现在哪一屏、是否可复现）。

---

## 六、性能目标与超频策略

- 本批时钟：`CPU 1785.0 / GPU 921.6 / memoria 1600.0 MHz` ⇒ **三档都已在当前上限**，而定格**不是算力问题**（§一.3(b) 已证是自锁）。
- 因此本轮**不启用** `[apm]` 超频兜底，也不动任何频率；等画面不再定格、进入稳定帧段后，再按 T18 规则逐档评估（CPU ≤1785 / GPU ≤921 / 内存 ≤1600 MHz），目标仍是**不超频稳 60 帧**。
- 现有性能基线（fix36 批实测）：前端动画段 **20.0 fps（峰值 23.9）**，第二段 10 s 处 **9.4 fps** —— 这两个数就是「进入稳定帧段后要往上顶」的起点。

---

## 七、放置方法与回退

### 1. 本批交付物

| 文件 | 内容 |
| --- | --- |
| `T3DOTM.nro` | 109,447,182 B，SHA-256 `29f212782c9e1c21088e37eb1276dc4b25465009a6af6e37ace7f2efb0d3ef70` |
| `README_dispatch_fix37.md` | 本文件 |

放置：`/switch/T3DOTM/T3DOTM.nro`。

### 2. NRO 回退指纹表

| 版本 | NRO SHA-256 | 说明 |
| --- | --- | --- |
| **fix37** | `29f212782c9e1c21088e37eb1276dc4b25465009a6af6e37ace7f2efb0d3ef70` | 本批（(D)+(A)+(C)+(B)） |
| fix36 | `a271aa085cd2d42edfe105894670639c17918e1eb773cf5bed81e3433361e94a` | 池闲置上限 384 MB |
| fix35 | `e9af9252…` | 阶梯 `break → continue` |
| fix34 | `d01a2630…` | 8192 次等待 |
| fix33 | `b5d1e058…` | 池重组 |

⚠️ fix33 起体积固定 109,447,182 B ⇒ **回退只能按 SHA-256 认**。

### 3. 源码指纹表（本轮改动的两个源码文件 + 构建配置）

| 文件 | fix37 | fix36 |
| --- | --- | --- |
| `guest_memory_switch.cpp` | 3,377 行 / 161,781 B / SHA-256 `d55a25c2e5ce6209f99896216ad9262253eca3148a46afd0bf9b5813b55a064e` / CRLF=3,377 / 无 BOM | 3,120 行 / 148,457 B / `56090226…` |
| `switch_perf.cpp` | 1,754 行 / 79,062 B / SHA-256 `6e180117d51b63ccead1c5d3db583d040215318036cbf76b7039102b2b0181af` / CRLF=1,754 / 无 BOM | 1,748 行 / 78,751 B / `6f291c40…` |
| `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` | 8,505 B / SHA-256 `81baa7e6d4030e3ef5693db3e1d7719fe44b26e5347798b3bc0f80db9338e7ce` / CRLF=151 + 10 处孤立 LF / 无 BOM，wrap 行 @74 | 8,132 B |

---

## 八、仍未完成 / 待确认

1. **(D) 的语义边界**：每 2 s 放行一次会让被放弃的块重新入册，可能带来零数据症状（见 §五「新增观察项」），本批无法预判，留给复测。
2. **`pendientes` 是否回落**：本批 288 MB（144 块）且 `proceso 3185/3189 MB` 顶格。若 fix37 后仍不降，真墙就是**进程容量**，下一步要转**页级部分物化**或**削减原始窗口收费**（`ventana 755 entradas / 1507 MB` 是最贵的一本账）。
3. **非 guest 那 ≈1.6 GB 的归属**：只有 (C) 直方图 + (B) hito 能定名；devkitA64 下 `malloc_info` / `posix_memalign` 不可用，只能靠 `--wrap` + `malloc_usable_size`。
4. **T18 仍阻塞**：装载处（`46F90000 + 0x37080000`）尚未越过，还没有稳定帧段可测。
5. **`limite de mapeo 0/0 MB`**（`svcGetInfo(InfoType_ResourceLimit)` rc `0x0000E401`）根因已定、尚未解决；`liberadas` 通路已在 fix30 榨干（本批 126 个 vistas crudas / 8 事件）。
6. 本批未逐行读尽的区段：`rex_stderr(21).log` 第 280 行之后、`T3DOTM_002(18).log` 中段（关键判读键均由完整检索得到，结论不受影响）。

---

*本文档与 `T3DOTM_dispatch_fix37.zip`（内层仅 `T3DOTM.nro`）同步放入 `.dumate/inbox/` 与工程根目录。*