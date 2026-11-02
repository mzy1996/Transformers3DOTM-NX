# T3DOTM Switch 移植 · fix41 派发说明

> 主题：给「**既没有真映射、也没有影子**」的那一类页补上**最后一级阶梯** —— 按**单页（4 KB）散页**粒度真映射一页（只花 2 条目），让 883 MB 延迟提交块里被引擎真正写到的页落地；而不是在一次写里撞上「谁都没法服务」直接 `svcBreak`。
>
> 本批（fix40 现场）**不再是上批那种空转**：fix40 的闸门确实在跑，574 条诊断把 **2,228,224 次**「无真映射」访问逐次服务掉（575/575 全部 `via=sombra`），引擎在**缓慢推进**；但影子只让指令「看起来做完」，**没有把页落地**，最终一次**写**撞上「真映射、影子都没有」⇒ 三级阶梯全失效 ⇒ 转 SEH ⇒ `[fix39] fatal sin servir` ⇒ `diagAbortWithResult(102)` ⇒ Atmosphere **`User Break 0xCD59 (2345-0102)`**。

---

## 一、本批（fix40 现场）实机日志的定性

### 1.1 现场文件（`J:\XBOX360 port\Transformers Dark of the Moon\.dumate\inbox`）

| 文件 | 规模 | 内容 |
| --- | --- | --- |
| `01793542297_0540c4720b100000.log` | 245,905 B / 3,554 行 | Atmosphere 崩溃报告（**运行 A**） |
| `01793542577_05e2b6d3eb447000.log` | 240,157 B / 3,471 行 | Atmosphere 崩溃报告（**运行 B**） |
| `rex_crash(25).log` | 15,871 B / 215 行 | 移植层崩溃快照 —— **一个文件里两组**（A 组 + B 组），尾部是栈帧 |
| `rex_stderr(25).log` | 225,891 B / 1,219 行 | 取证明细（`[fixNN]`）；**本批唯一**含 `[fix40]` 的文件（属运行 B） |
| `rex_latido(14).log` | 206,104 B / 484 行 | 264 条 `[fix27]` 心跳（`t=1.0 s` → `263.7 s`） |
| `rex_perfil(24).log` | 169,249 B / 3,081 行 | 剖析快照 + 停顿采样（属运行 B：`imagen 0x00000012ae20f000`） |
| `T3DOTM_001(24).log` | 58,274 B / 425 行 | 游戏侧日志（22:07:57 起） |
| `T3DOTM_002(22).log` | 57,865 B / 422 行 | 游戏侧日志 |

八份的落盘时间都是 `2026-11-01 22:30:24`（同一次导出）。

### 1.2 版本判定：本批**实测就是 fix40**（不再是推定）

- `[fix40] reintento sin mapeo real` = **574 行**，且**只出现在 `rex_stderr(25).log`**（同目录其余 150 份日志全为 0）⇒ 运行 B 跑的就是 **fix40** 产物。
- `[fix41]` = **0 条** ⇒ 本批设备上**没有** fix41（本次交付的正是它）。
- 运行 A 的 stderr 未随附，A 的版本只能按「同一次导出」与 B 同期看待（两份报告的 PC/LR 偏移一字不差，见 1.3）。

### 1.3 崩溃报告：同一份 NRO 的**两次**运行

| 字段 | 运行 A | 运行 B |
| --- | --- | --- |
| Result | `0xCD59 (2345-0102)` | 同 |
| Type | `User Break` | 同 |
| Break Size | `0x4` | `0x4` |
| Program ID | `0540c4720b100000` | `05e2b6d3eb447000` |
| Process ID | `00000000000000ad` | `00000000000000af` |
| Break Address | `00000044bb39e99c` | `00000012b4a5f99c` |
| PC / LR（**镜像内偏移**） | `+0x551fee8`（`svcBreak` 自身） / `+0x551b8cc`（`diagAbortWithResult+0x1c`） | **完全一致** |

- `Break Address` 落在**各自运行的宿主地址空间**（模块/其堆邻近区间，**不是 guest 地址**），是移植层 fatal 时携带的自有参数；两次运行的 ASLR 基址不同（A 的模块地址在 `0x44b4…`、B 在 `0x12ae…`），而 **PC/LR 的镜像内偏移一字不差** ⇒ **同一份 NRO 的两次独立崩溃**。`rex_crash(25).log` 里也正好有两组快照（A 组 `pc=0x00000044b739d748`、B 组 `pc=0x00000012b0a6e748`），与两份报告一一对应。
- guest 端**真正没被服务掉的那一页**由移植层日志给出（两次各一条，逐字）：

```
[fix39] fatal sin servir en_ventana=1 offset=1:0x6D400000 escritura=1 permiso=1 acceso=3 respaldo=0 |
  vista=si pag=6D400000 off=6D400000 trozo=6D400000+200000 prot=no cerca=6D400000+200000 mapeado=si confirmado=no
[fix39] fatal sin servir en_ventana=1 offset=1:0x6DA00000 escritura=1 permiso=1 acceso=3 respaldo=0 |
  vista=si pag=6DA00000 off=6DA00000 trozo=6DA00000+200000 prot=no cerca=6DA00000+200000 mapeado=si confirmado=no
```

读法：这一次是**写**（`escritura=1`）、落在窗口里（`en_ventana=1`）、块被标成已映射（`mapeado=si`），却**没有保护条目**（`prot=no`）、**没有确认落地的后备**（`confirmado=no`）、**没有影子**（`respaldo=0`）。

### 1.4 本批现场（读数 + 出处）

| 读数 | 值 | 出处 |
| --- | --- | --- |
| 时长 / 帧率 | `263.7 s` 全程 **0.0 fps**（末行 `t=263.7s fps=0.0 pres=79 lento=248s rep=26`） | `rex_latido(14).log` |
| fix40 闸门 | 574 行诊断 = **575 处 `via=sombra servido=1`**；`via=ninguna` / `servido=0` **0 条**；`n` 从 `0` 涨到 **2,228,224** | `rex_stderr(25).log` |
| 闸门首/末行 | `invitado=0xBF9A0260 … escritura=1 … via=sombra servido=1 (n=0)` → `invitado=0xFF4E1E30 escritura=0 … (n=2228224)` | 同上 |
| 页级路径撞顶 | **16×** `[fix33] por paginas: techo de entradas (5658 + 512 > 6144) off=0x36E00000` | 同上 |
| **页级路径实际落地了哪些块** | **8 条 `[fix29]`**：块 1–8 的 `off=` 依次是 `0x36600000 / 0x6D200000 / 0x36800000 / 0x6D400000 / 0x36A00000 / 0x6D600000 / 0x36C00000 / 0x6D800000`，**每条都是 `len=0x200000`、`n=512`、`runs=512`**，耗时 7–10 ms，`arena libre` 从 78 MB 逐块掉到 64 MB | 同上 |
| 8 块与条目账对得上 | **8 × 512 = 4096**，与心跳 `ent_pag=4096`、`blq=8` 完全一致（`map=4840/818` = `4096 + 744`） | `rex_latido(14).log` |
| 池子压缩 | 1× `[fix33] piscina: compactada, 48 lonchas recuperadas (96 MB), en uso 1172 MB de 1268 MB tomados` | 同上 |
| 预算让位 | **64×** `[fix39] presupuesto para el fault: liberadas 1 vistas crudas` | 同上 |
| 条目账（关键） | 末行心跳 `pags=4096 blq=8 ent_pag=4096`、`map=4840/818 ent=4840/818`、`mg=2534/164112` | `rex_latido(14).log` |
| 内存账 | `pisc=1268/1268 MB inactiva=0 MB`、`arena u=3006 f=64 t=44K n=14656`、`hilos=38`、`corte=191`、`neg2m=28 sin_back=20 al4k=1` | 同上 |
| CPU | 末行 `XThread551F900 96.3%`；剖析里两线程约 96% / 32%，其余线程 ≤ 6% | 同上 / `rex_perfil(24).log` |
| 缺页速率 | 9.1 s 起 `manejador SDK 22971/s`，同一行的 `lectura emulada / reintento emulado / SEH / fisica confirmada` **全 0**，`vistas 6` | `rex_perfil(24).log` |
| 镜像 | `imagen 0x00000012ae20f000, codigo hasta 0x00000012b37dd000`（运行 B） | 同上 |
| 初始化 | `[DIAG] RexGmInit entered size=0x120001000`；`svcGetInfo(InfoType_ResourceLimit) failed rc=0x0000E401`；`Direction F map FAILED (rc=0x0000D401)` → `code-memory path unavailable`；`prealloc disabled on Switch (on-demand fault-in only)`；`[fix23] modo=2`；`[fix37] hito … arena u=32 MB … cortes=1` | `rex_stderr(25).log` |
| 监视窗 | `[fix20] vigilancia instalada caller=0x12ae3715c8 ventana=0x7FEA0000 largo=0x10000 access=0`、`caller=0x12ae372204 ventana=0x7E010000`；`solo-lectura RECHAZADA rc=0x0000D401` | 同上 |
| 崩溃快照 | `[fix21] lectura nativa #1..#8`（A 组 guest `0x67359500…0x67360300`、B 组 `0xE3359500…0xE3360300`）；`[fix15] far=0x6e78401880 invitado=0x10401880 escritura=0 codigo=1 reparado=0 bits=4`；`[fix16] cubre=0 nunca se asigno ese rango / fisica coincidencias=0 / cola seq=7503 op=2 addr=46F90000 size=37080000`；`[fix18] zero-load #1…#28673 invitado=0x36E00000…0x36E70000 pc=…f01138 instr=0xB9400002` | `rex_crash(25).log` |
| 883 MB 块 | `fix13: Using pre-allocated block at 46F90000 for size=0x37080000 (committed=true)`、`fix14 alloc: addr=46F90000 size=0x37080000 type=0x20801000 protect=0x3 commit=true page_size=0x10000`（22:11:32） | `T3DOTM_001(24).log` |
| 游戏日志写入 | `[fix28] log del juego: ruta='…/T3DOTM_002.log' largo=45 rc 0xFFFFFFFF errno=5`、心跳 `log=0KB(rc 0xFFFFFFFF)`（但两份游戏日志本身有 58 KB 内容） | `rex_stderr(25).log` / 心跳 |
| 停顿标记 | `[fix22]` 4 条（强制采样）、`[fix24]` 1 条（5 s 立即转储） | `rex_stderr(25).log` |
| 时钟 | 本批 `rex_stderr(25).log` 里**没采到** `relojes:` 行（上批为 CPU 1020.0 / GPU 460.8 / 1600.0 MHz） | — |

### 1.5 机制判定

1. **空转已经被掐断。** 上批是「同址 5.7 万次/秒空转 15 分钟」，`reintento emulado / SEH / lectura emulada / fisica confirmada` 全 0；本批 `[fix40]` 闸门**确实在跑**，2,228,224 次「无真映射」访问被逐次服务掉，guest 地址持续前进（开头两条 `0xBF9A0260` → `0xBF9A0264`，**每次 +4**，是连续的写）⇒ fix40 的「不认领就不空转」生效了。
2. **但影子不是答案。** 575/575 全部走 `via=sombra`：影子让指令「看起来做完」，**没有把页落地**。账本同时给出三条旁证：`pags=4096 blq=8 ent_pag=4096`（页级路径把 **4096 条条目全花在 8 个块**上）、`[fix33] techo de entradas (5658 + 512 > 6144)` ×16、`pisc=1268/1268 inactiva=0`（池子用满、无闲置）⇒ 883 MB 块（guest `0x46F90000`，`fix13`/`fix14` 已 `committed=true`）**永远排不上队**：2 MB 块走页级路径要 **512 条目/块**，而内核 per-process 上限是 **6144**（`svcGetInfo(InfoType_ResourceLimit)` 提限被拒 `rc=0x0000E401`）。 **`[fix29]` 把这件事钉死了**：页级路径**确实**落地了 8 个块（见 §1.4 表内新增两行），`0x36600000 / 0x6D200000 / 0x36800000 / 0x6D400000 / 0x36A00000 / 0x6D600000 / 0x36C00000 / 0x6D800000`，每块 512 条目、合计正好 4096 —— 第 **9** 个块就再也进不来了（`[fix33] … 5658 + 512 > 6144 off=0x36E00000`）。而两次的致命页恰好贴在那条边界上：**B 的 `0x6DA00000` 正是第 8 块 `0x6D800000 + 0x200000` 之后紧邻的那一块**（整条链的第 9 个），A 的 `0x6D400000` 是块 4 的块首。
3. **致命一击**：一次**写**落到该块的某页（A：`0x6D400000`；B：`0x6DA00000`），该页 `prot=no`、`confirmado=no`、`respaldo=0` —— 真映射没有、影子也没有 ⇒ fix40 三级阶梯**全失效**（借页只对读、`fault-in` 撞自己的保护条目、影子无后备）⇒ 按设计 `break` 转 SEH ⇒ SEH 也没能救 ⇒ fatal 记账打印 `[fix39] fatal sin servir` ⇒ `diagAbortWithResult(102)` ⇒ Atmosphere `User Break 2345-0102`。**这就是「停在第一个 LOGO 画面后崩溃」的现场。**
4. **为什么日志里看不到那条 `via=ninguna servido=0`（诚实说明）：** `[fix40]` 串是**限流**的（前 32 次 + 每 4096 次一条），致命那次的 `n≈2,228,2xx` 既不在前 32 内、也不是 4096 的倍数 ⇒ **它那一行根本没有被打出来**（另一种可能是 stderr 缓冲未刷出；两种解释都指向同一个结论：不是「没走闸门」，而是「没被打印」）。这条推论对 fix41 的有效性判断很关键，因此 §四 给了一条明确的判读规则。
5. **本批的 0 fps 不是算力问题**：`arena` 还有 `f=64 MB`，`[fix33]` 刚回收 96 MB 长片，而两条线程约 96% 的 CPU 全花在缺页通路上（`manejador SDK 2.3 万/秒`，其余四类计数为 0）。

### 1.6 与上批（fix39 现场）逐项对比

| 项目 | fix39 现场 | **本批（fix40 现场）** |
| --- | --- | --- |
| 帧率 | 928.4 s 全程 0.0 | **263.7 s 全程 0.0** |
| 缺页计数 | `manejador 5.7 万/秒`，其余四类全 0 | `manejador ~2.3 万/秒`，**`[fix40]` 574 条全被影子服务** |
| 结局 | **无崩溃**（crash 日志未刷新），纯空转 | **两次崩溃**（`User Break 2345-0102`），fatal 有记账 |
| 池子 | `384 MB` 里只用 6 MB，`inactiva=378 MB` | **`1268/1268 MB`、`inactiva=0 MB`** |
| arena | `u=1672 f=62 n=4115` | **`u=3006 f=64 n=14656`** |
| `corte` | 47 恒定 | **186 → 191（仍在增长）** |
| `pags / blq / ent_pag` | `0 / 0 / 0`（页级路径从未打开） | **`4096 / 8 / 4096`（页级路径打开但撞顶）** |
| 主线程 CPU | 99.7% / 99.9%（两线程满转） | 约 96% / 32% |

> 结论：fix40 把「静默空转」换成了「有据可查的上报」，本批这份上报**也一起把下一步的抓手交出来了** —— 一个已 committed、已标映射、却**没有后备**的大块，以及唯一还能在预算耗尽时落地一页的路：**散页**。

---

## 二、fix41 改了什么（2 个文件 / 9 处 hunk / `+295 −4` 行）

### 2.1 `src/core/guest_memory_switch.cpp`（3,534 → 3,805 行，`+275 −4`）

| hunk | 位置（fix41 后行号） | 内容 |
| --- | --- | --- |
| `@@ -569,6 +569,43 @@` | 597–610 | `struct PaginaSuelta{ raw, alias, res }` + `kSueltasMax = 2048`（8 MB，最坏 4096 条目）+ 计数器 `g_sueltas_n / g_sueltas_vistas / g_sueltas_neg` |
| `@@ -612,6 +649,14 @@` | 657 / 659 | `State` 新增 `std::map<size_t,PaginaSuelta> sueltas;` 与 `std::set<std::pair<size_t,size_t>> sueltas_vistas;` |
| `@@ -1923,6 +1968,69 @@` | 1979–2035 | `SoltarSueltasDeRango(s, inicio, largo)`（整段归还散页）+ `AbsorberSueltas(s, c)`（块落地后把散页并进它的后备） |
| `@@ -1930,6 +2038,8 @@` | 2042 | `MapChunkIntoView`：块真的落地后 `AbsorberSueltas(s, c)` |
| `@@ -1962,6 +2072,8 @@` | 2076 | `ReleaseChunk` 顶部（**在 prealloc 早退之前**）`SoltarSueltasDeRango(S(), c.offset, c.length)` |
| `@@ -2721,6 +2833,154 @@` | 2846–2981 | `ServirPaginaSuelta()` + `extern "C" RexGmServirPaginaSuelta()`（本补丁主体，见 2.3） |
| `@@ -3387,15 +3647,26 @@` | 3647–3672 | `RexGmShadowFor`：查影子之前先查散页，命中即返回 `alias + (off − página)` —— 「窗口调用没进也能被仿真服务掉」的关键 |

### 2.2 `src/core/exception_handler_switch.cpp`（1,522 → 1,542 行，`+20`）

| hunk | 位置（fix41 后行号） | 内容 |
| --- | --- | --- |
| `@@ -113,6 +113,7 @@` | 116 | `extern "C" int RexGmServirPaginaSuelta(void* address);`（该行是 **LF**，与文件混合 EOL 风格一致） |
| `@@ -1409,6 +1410,25 @@` | 1414–1431 | fix40 三级阶梯之后加**第 4 级（散页）**与**影子兜底**，`via40` 新增 `suelta` / `sombra41` 两个取值 |

```cpp
          /* fix41: el ultimo escalon, y el unico que sirve un bloque comprometido que no cabe en
           * ningun sitio. … Una pagina suelta cuesta dos entradas en vez de las 512 de la via por paginas. */
          if (!servido40 && RexGmServirPaginaSuelta(reinterpret_cast<void*>(far)) != 0) {
            servido40 = true;
            via40 = "suelta";
          }
          /* fix41: la pagina puede existir aunque su mapeo en la ventana no entrara. */
          if (!servido40 && EmulateViaShadow(ctx)) {
            servido40 = true;
            via40 = "sombra41";
          }
```

### 2.3 为什么是「单页散页」

`ServirPaginaSuelta(window_address)`（`guest_memory_switch.cpp:2846`）只做一件事：**把这一页真正落地**，成本 1 页 + 2 条目。

1. 只服务「已 committed、未落地、且上面三级都没能服务」的块页（`ChunkAt` 命中、`!c->shadow`）；`fix18` 的 zero-load 页与已落地块**不抢**这条道。
2. 名额上限 `kSueltasMax = 2048` 页（8 MB，最坏 4096 条目）；超了只计 `g_sueltas_neg` 并打印 `el tope de 2048 esta alcanzado`。
3. 预算账 `EntradasTotales() + 2 > TopeTotal()` 时**只警告 + 调 `LiberarPresupuestoParaCarga` 腾余量，仍然继续尝试**（与 fix40 里「返回 true = 放弃下次重试」的语义**相反**：这是致命前的最后一道闸门，放弃了就等于 fatal）；真正的裁决交给内核返回的 `rc`。
4. 落地四步：`PaginaTomar()` + `memset` → `virtmemFindCodeMemory(kPageSize*2, kPageSize)` + 对齐 + `virtmemAddReservation` → `svcMapProcessCodeMemory` → `svcSetProcessMemoryPermission(Perm_Rw)`（失败则回滚 reservation 并 `PaginaCeder`，同时 `AnotarRechazoKernel(rc)` 让 `fix33` 的账跟着下调）。
5. 记账：`AnotarMapeoCodigo(alias, 4 KB)` = **1 条目**；窗口那一步走 `Intersect + MapWindowRange` = **1 条目**（每页合计 **2 条目**，对比页级路径的 **512 条目 / 2 MB 块**）。
6. 若该页已在保护账上（`s.protection`）或窗口调用已记入 `sueltas_vistas`，直接回「可续跑」；窗口调用失败也**不算拒绝** —— 因为 `alias` 已在，`RexGmShadowFor` 能靠它把指令仿真掉（这正是 2.1 最后一行 hunk 的作用）。

> 诚实边界：散页是**成本最小的一步**，不是「把 883 MB 一次落地」的方案（见 §八）。它保证的是：**引擎真正碰到的那些页有真后备**，致命不再来自「谁都服务不了」。

---

## 三、构建与产物校验（实测）

| 步骤 | 做法 | 结果 |
| --- | --- | --- |
| 预检 | `ninja -n`；`.ninja_log`(83,116 B) / `.ninja_deps`(2,387,392 B) **均未删除** | 恰好 **4 步** 待办 |
| 全量构建 | `nohup setsid ninja -j 4 > fix41_build.log 2>&1 & disown`，`-DT3DOTM_LTO=ON` | **4/4 完成**，`error:` **0 行**，仅 2 warning（`serial compilation of 128 LTRANS jobs`、`creating DT_TEXTREL in a PIE`），05:28:19 写出 `T3DOTM.nro` |
| NRO 结构校验 | `python tools/verify_nro.py recomp/out/sw8/T3DOTM.nro` | **Verdict: PASS / rc=0** |
| 补丁进产物（串） | `strings -a` | `[fix41]` **4 条** + 裸字面量 `suelta`、`sombra41`；`[fix40]` 串仍在 |
| 补丁进产物（符号） | `nm`（未剥的 `out/sw8/T3DOTM`，共 150,277 个符号） | `T RexGmServirPaginaSuelta`（0x15e470）、`T ServirPaginaSuelta`（0x15d940）、`t SoltarSueltasDeRango(State&,size_t,size_t)`（0x15b150）+ 5 个 `ServirPaginaSuelta` 局部静态 |

产物指纹：

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| **`T3DOTM.nro`（交付件）** | **109,455,374** | **`f4cdab82c9639324bfb113d3266f242eb55632bf6c0d91c200d63fa62e2b57b5`** |
| `T3DOTM.stripped.elf` | 109,356,168 | `450e987c14584fd6ab16013ea6993ccab56bfc8903d3b3269bd52bd5cb2c6bf9` |
| `T3DOTM`（未剥离 ELF） | 128,154,728 | `3f2d9afe90a3a8b3a12d57efcc635c6965542b0c611740aac167d883eb71ee8b` |
| `T3DOTM.nacp` | 16,384 | `d0767bba6fde2f9f502a3a967f969935f96bfb07738ae8000bd185c7e3bbdfa4` |

> ⚠️ 本次 NRO 比 fix40 大 **4,096 B**（109,455,374 vs 109,451,278）—— 新增代码没有完全落进 4096 对齐余量里。**认版本仍请按 SHA-256**：fix41 = `f4cdab82…`，fix40 = `3ba8f408…`。

---

## 四、在你的设备上怎么复测

1. 备份旧档后放置新档（细节见 §七）：`T3DOTM.nro` → `sd:/switch/T3DOTM.nro`。
2. 先删掉上一批的日志，避免混淆（或用 `[fix41]` 串区分）。
3. **不要开超频**（本批 0 fps 是通路问题，不是算力问题，见 §六）。
4. 冷启动后**至少让现场持续 5–10 分钟**；崩溃则照旧让它自己记录下来。
5. 退出后把 `sd:/switch/Transformers3DOTM/logs/` 下的 `rex_latido*.log`、`rex_perfil*.log`、`rex_stderr*.log`、`rex_crash*.log`（以及 `T3DOTM_00X.log`）打包回 `.dumate\inbox`。

**看三件事：`[fix41]` 串、`[fix40]` 串里的 `via=`、以及 `[fix39] fatal sin servir` 还出不出现。**

`[fix41]` 判读表：

| 串 | 含义 | 下一步 |
| --- | --- | --- |
| `primera pagina suelta`（Hito） | 第一页散页落地 | 通路活着，看 `n` 是否继续涨 |
| `[fix41] pagina suelta off=0x… vista=… n=… entradas=…/… sueltas=…` | 成功落地一页（前 32 页 + 每 4096 页一条） | `sueltas` 涨、崩溃消失 ⇒ 这一级就是答案 |
| `[fix41] pagina suelta sobre el techo (… + 2 > …) off=0x…: se intenta` | 预算账已满，但**仍然尝试** | 只要紧接着出现落地串就说明赌对了 |
| `[fix41] pagina suelta RECHAZADA rc=0x… off=0x… negadas=…` | 内核拒绝真映射（`0xCE01` 会同时把上限下调） | 这条道在本机走不通，下一批转块级/大页路径 |
| `[fix41] pagina suelta: el tope de 2048 esta alcanzado off=0x…` | 8 MB 名额用满 | 下一批按 `TopeTotal()` 余量自适应上限 |
| `[fix40] … via=suelta` | **第 4 级阶梯**服务成功（在 `[fix40]` 串里） | 与上表配合看比例：`suelta` 占比越高，说明影子只是陪跑 |

崩溃重现时的判读规则：

1. 有 `via=suelta` 或 `[fix41] pagina suelta …` 但仍崩 ⇒ 散页落地了，**还有别的页/别的形态**没被服务：核对新的 `[fix39] fatal sin servir` 里的 `off=` / `pag=` / `trozo=` 是否已不是 `0x6D400000`（A）/ `0x6DA00000`（B）。
2. **一条 `[fix41]` 都没有**且仍崩 ⇒ 致命那次连闸门都没走或没被打出来（§1.5④）。下一批（fix42）会把「服务不掉」改成**一次性 `RexSwitchDiagLine` 上报**（写进 `rex_crash.log`，能扛过 abort），这样即使打印被限流或缓冲丢掉也能看到。
3. 有 `[fix41] pagina suelta RECHAZADA rc=0x…` ⇒ 内核拒绝，改走块级/大页路径。

配套判断（照旧）：`fallos/s` 里 `manejador SDK` 是否**降下来**；`[fix33] techo de entradas` 是否还出现；`pags/blq/ent_pag` 是否不再停在 `4096/8/4096`；`[fix22]/[fix24]` 停顿是否还在；心跳 `corte` 是否停止增长；crash 报告的**时间戳/哈希是否有变化**（有变化才说明发生了新的异常）。

---

## 五、12 项快查（判读任何一批日志都用这张表，附本批读数）

| # | 看什么 | 本批读数 | 医好长什么样 |
| --- | --- | --- | --- |
| 1 | `[fix26]` 宿主初始化 | 本批 stderr 里 **0 条**（初始化序列改由第 2–5 行的串承载） ℹ️ | 无 fatal、无早退（上批口径） |
| 2 | `[DIAG] RexGmInit` | `size=0x120001000`，`ResourceLimit rc=0x0000E401` | `base/size` 与上批一致 |
| 3 | `[fix23] RexGmConfigure` | `modo=2` ✅ | `modo=2` |
| 4 | `[fix37] hito fin de RexGmInit` | 一行，`arena u=32 MB … cortes=1` ✅ | 一行、含 pool/arena/cortes |
| 5 | `[fix35]`/`[fix36]` 预算 | **`[fix35]` 16 条 / `[fix36]` 8 条**：`sin losa nueva (piso) arena libre 0 MB en 214 trozos, en uso 0 MB`、`cortes 1`；`no reservo mas, inactiva 384 MB de 384 MB tomados y 0 MB en uso` → `inactiva 336 MB de 480 MB, 144 MB en uso`；心跳末行 `pisc=1268/1268 MB inactiva=0 MB` ⚠️ | 池有闲置、`inactiva` 不为 0 |
| 6 | `[fix20]` 监视安装 | 两窗 + 只读被拒 `rc=0x0000D401` ✅ | 与设计一致（只读写拒绝是预期） |
| 7 | `[fix27]` 心跳 | **`fps=0.0` 全程** ❌ | `fps` 稳定非 0 |
| 8 | `[fix22]`/`[fix24]` 停顿 | `[fix22]` 4 条 / `[fix24]` 1 条 ❌ | 不再出现 |
| 9 | `[fix28]`/`[fix29]` 存储 | `[fix28]` **1 条**：`ruta='/switch/Transformers3DOTM/logs/T3DOTM_002.log' largo=45 rc 0xFFFFFFFF errno=5` ❌；`[fix29]` **8 条**（块 1–8 落地的正常流水，见 §1.4）ℹ️ | `rc=0`、游戏日志有实际内容 |
| 10 | 剖析热点 | 落在异常/桥接桩区（`manejador 2.3 万/秒`） ❌ | 热点回到游戏镜像内部 |
| 11 | crash 报告 | 两份、`PC=+0x551fee8` ❌ | 不出现（或哈希/时间戳变化即新异常） |
| 12 | 时钟 | 本批未采到 `relojes:` ⚠️ | `CPU 1020 / GPU 460.8 / 1600` = 未超频 |

---

## 六、性能目标与超频策略

- 目标：「**不需要超频**，流畅 60 帧」。
- 本批 0 fps 与算力无关：两线程约 96% 的 CPU 全花在缺页通路上，`manejador 2.3 万/秒`，而 `arena` 还有 64 MB、`[fix33]` 刚回收 96 MB 长片 ⇒ 是**通路卡死**，不是算力不够。
- fix41 之后**仍不要超频**：先让通路打通、看到「有帧、且帧率随内容变化」，再谈帧率优化。
- 后续若确实需要余量，已核实的上限口径是：CPU **1020 MHz**、GPU **460.8 MHz**、内存 **1600 MHz**（上批实测读数即此，说明当前 APM 配置已经是该口径，并未越界）。

---

## 七、放置、回退与指纹表

- 交付件：**`T3DOTM.nro`**（109,455,374 B / `f4cdab82c9639324bfb113d3266f242eb55632bf6c0d91c200d63fa62e2b57b5`）+ 本说明文档。
- 放置：解压后把 `T3DOTM.nro` 放到 `sd:/switch/T3DOTM.nro`（覆盖旧档即可）。
- **先核对哈希**（Windows 上）：`certutil -hashfile T3DOTM.nro SHA256`。
- 回退：`T3DOTM_dispatch_fix40.zip` 内的 NRO 就是**本批实机运行的那一版**（`3ba8f408…`）。

| 版本 | NRO SHA-256 前缀 | 说明 |
| --- | --- | --- |
| **fix41** | **`f4cdab82c9639324…`** | 本次交付 |
| fix40 | `3ba8f408667b19e8…` | **本批实机运行的那一版（实测：`[fix40]` 574 条）** |
| fix39 | `29c14a1bef0…` | 上批运行（推定） |
| fix38 | `3492f026…` | |
| fix37 | `29f21278…` | |
| fix36 | `a271aa08…` | |
| fix35 | `e9af9252…` | |
| fix34 | `d01a2630…` | |
| fix33 | `b5d1e058…` | |

源码指纹表：

| 文件 | fix40 | **fix41** |
| --- | --- | --- |
| `exception_handler_switch.cpp` | 1,522 行 / 69,971 B / CRLF 1018 + LF 504 / 无 BOM / `e3cfab89…` | **1,542 行 / 71,028 B / CRLF 1038 + LF 504 / 无 BOM / `028ad459885b15ac6099f6a8928613659ab09303e413c57abdf1b46eeedd42c2`** |
| `guest_memory_switch.cpp` | 3,534 行 / 170,511 B / 纯 CRLF / 无 BOM / `773b1939…` | **3,805 行 / 183,825 B / 纯 CRLF（3805/3805）/ 无 BOM / `06613b6e911d25a33b0c381fa60219e428823a996f551d24101275b604d0e6e0`** |
| `switch_perf.cpp` | 1,754 行 / 79,062 B / `6e180117…` | 未改动 |
| `cmake/rexglue_switch.cmake` | 161 行 / 8,505 B / `81baa7e6…` | 未改动 |

备份（改前基线，供逐行回退核对）：

| 文件 | 说明 |
| --- | --- |
| `exception_handler_switch.cpp.fix41.bak` | pre-fix41 = fix40 版：1,522 行 / 69,971 B / `e3cfab89…` |
| `guest_memory_switch.cpp.fix41.bak` | pre-fix41：3,534 行 / 170,511 B / 纯 CRLF / `773b1939…` |
| `guest_memory_switch.cpp.fix41mid.bak` | 中途快照（173,022 B），仅供追溯，**未参与交付** |

---

## 八、仍未完成 / 待确认（诚实清单）

1. **60 帧目标仍未验证**：fix41 解决的是「一次写落到没有后备的页 ⇒ 直接 abort」；要看到 60 帧，先要让引擎跑过初始化并产生首帧。
2. **fix41 的有效性尚未实机验证**：本批日志只能证明它**打在正确的分支上**（致命那次确实走的是 `!real40` 闸门，见 §1.5③④），不能证明它一定服务成功。
3. **883 MB 块的整体落地仍未解决**：散页上限 2048 页 = 8 MB，只覆盖「被引擎反复碰到的页」。真正的解法有两条，都还没做：① 让该块走**块级/大页**路径（需要条目预算或减少同时驻留的块）；② 回到 Xbox 侧看**为什么一次性 commit 883 MB**（`fix13`/`fix14` 的调用者）能否拆小。
4. **`[fix40]` 限流导致的取证盲区**：致命那次没留下 `via=` 行（§1.5④）。fix42 候选：把「服务不掉」改成一次性 `RexSwitchDiagLine`（写进 `rex_crash.log`，能扛过 abort）。
5. **两份报告的 Program ID 不同**（同一 NRO 两次运行，Process ID `…ad` / `…af`）：两次启动的路径/安装位置不同所致，**原因未确证**。
6. **两个地址族仍只定位到「哪个地址」，没定位到调用者**：`[fix15] far=0x6e78401880 invitado=0x10401880`（读、`reparado=0`、`bits=4`）与 `[fix18] zero-load #1…#28673 invitado=0x36E00000…0x36E70000`（`instr=0xB9400002` = `ldr w2,[x0]`）。
7. **`[fix28] errno=5` 与心跳 `log=0KB` 是否同因未确证**；两份游戏日志本身有 58 KB 内容，说明「写不进去」是阶段性的，不是全程失败。
8. **上批提到的「约 880 MB 那道墙」本批换了形态**：上批是 pool 闲置、卡通路；本批是 `pisc=1268/1268 inactiva=0` 用满、且 `[fix33]` 已在压缩池子 ⇒ 与 §1.5② 的结论一致，但「池子为什么会涨到满」仍未逐条核到。

---

## 九、交付物

| 文件 | 说明 |
| --- | --- |
| `T3DOTM.nro` | 109,455,374 B / `f4cdab82c9639324bfb113d3266f242eb55632bf6c0d91c200d63fa62e2b57b5` |
| `README_dispatch_fix41.md` | 本文档 |
| `T3DOTM_dispatch_fix41.zip` | 内层单一运行文件 `T3DOTM.nro`（已流式核对内层 SHA-256 = 上表值） |

同步位置：工程根目录 `J:\XBOX360 port\Transformers Dark of the Moon\` 与 `.dumate\inbox`。