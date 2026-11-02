# T3DOTM Switch 移植 · fix42 派发说明

> 主题：给「**块已经materializado、页没进窗口、步(2)又带不进来**」的那一类页补上**单页真映射**阶梯 —— 用**一个窗口条目**（不是 512 条目的整页通道，也不是整块）把 guest 真正写到的 4 KB 页映射进视图，把「每次访问都走异常 + 影子模拟」的洪流换掉。
>
> 本批现场（fix40 产物，实机第 N 次）**不是崩溃，是定格**：`[fix40] reintento sin mapeo real` 实测 **569 条**（`n` 从 0 累计到 **2,211,840**），**569/569 全部 `via=sombra`、`real=0`、`manejador=0`**；心跳全程 `fps=0.0`（末段 1.0），233.8 s 里 **218 s 被判为卡顿**，只推出 **79 帧**；剖析显示 **CPU 236–245% / 400%**、单线程 **98.3–99.3%**、`manejador SDK` **每秒 6.0k–8.1k 次**。换算下来：**2,211,840 ÷ 233.8 s ≈ 9.5k 次/s**，平均每次「无真映射访问」摊到约 **106 µs** 的整机时间 —— 引擎在动，但慢到看不见，**不崩、不死锁**。快照 `rex_crash(26).log` 的 `fatal` 串为 **0**（上一批 `rex_crash(25).log` 是 2），本批**没有** Atmosphere 崩溃报告。

---

## 一、本批实机日志的定性

### 1.1 现场文件（`J:\XBOX360 port\Transformers Dark of the Moon\.dumate\inbox`）

| 文件 | 规模 | 行数 | 内容 |
| --- | --- | --- | --- |
| `rex_stderr(26).log` | 198,679 B | 1,043 | 取证明细（`[fixNN]`）；**本批唯一**含 `[fix40]` 的文件，属运行 B（`[fix37] hito fin de RexGmInit` 只出现一次） |
| `rex_latido(15).log` | 226,238 B | 530 | `[fix27]` 心跳 **264 条**；文件里是**两段**（运行 A `t=1.0 s → 241.0 s`，运行 B `t=1.0 s → 233.8 s`） |
| `rex_perfil(25).log` | 149,896 B | 2,712 | 剖析快照 + 停顿采样（`imagen 0x0000003f2afd4000` ⇒ 属**运行 B**） |
| `rex_crash(26).log` | 7,750 B | 34 | 快照，**两组**（A 组 `pc=0x0000007159…`、B 组 `pc=0x0000003f2d…`）；`fatal` 串 **0** |
| `T3DOTM_001(25).log` | 58,058 B | 425 | 游戏侧日志（A 组：`08:17:39` → `08:21:29`） |
| `T3DOTM_002(23).log` | 57,813 B | 421 | 游戏侧日志（B 组：`08:22:49` → `08:26:41`） |

六份文件的落盘时间同为 `2026-11-02 08:25:06`（同一次导出）。两次运行的时长都是 **约 3 分 52 秒**（`08:17:39+3:54 ≈ 08:21:33`、`08:22:49+3:52 = 08:26:41`），与心跳 `233.8 s` 对得上。

### 1.2 版本判定：本批**实测就是 fix40**（不是推定）

- `[fix40] reintento sin mapeo real` = **569 条**，且**只出现在 `rex_stderr(26).log`**（同目录其余日志为 0）。
- `[fix41]` = **0 条**、`[fix42]` = **0 条**（全部 1,043 行里没有）⇒ 本批设备上**跑的是 fix40**；**fix41 至今尚未上机**。
- 因此本批的读数**只能**用来判 fix40 的行为，不能当 fix41 的验证。

### 1.3 关键读数与出处

| 读数 | 值 | 出处 |
| --- | --- | --- |
| 运行帧率 | 稳态 `fps=0.2–0.3`，心跳末段 `fps=1.0`；`pres=79` 帧 / `233.8 s` ⇒ 均值 **约 0.34 fps** | `rex_latido(15).log`（B 段）、`rex_perfil(25).log` |
| 卡顿时长 | 末行 `lento=218s`（占 233.8 s 的 **93%**） | `rex_latido(15).log` |
| CPU | `CPU total 236–245%`（400% = 4 核）；线程 `XThread42124AE0` **98.3–99.3%**、`Main XThread` **80.1–99.2%**、`XThread4250FDE0` **31.7–31.9%** | `rex_perfil(25).log` |
| 缺页处理速率 | `manejador SDK` 稳态 **5,991–8,079 次/s**（起步 22,896）；`lectura emulada 0`、`reintento emulado 0`、`SEH 0`、`fisica confirmada 0` | `rex_perfil(25).log` |
| `[fix40]` 洪流 | 569 条：`n` **0 → 2,211,840**；地址 **`0xBF9A0260` → `0xFF4FFCF8`**；字段一致性 **`manejador=0`、`permiso=1`、`acceso=3`、`real=0`、`via=sombra`、`servido=1`**（`escritura` 306 写 / 263 读） | `rex_stderr(26).log` |
| 采样规则 | `n < 32` 共 32 条 + `n % 4096 == 0` 共 537 条 = **569 条**（逐条核对吻合） | 同上 |
| 地址走势 | 相邻落盘地址差以 **+0x7900（30,976）** 为主（350 次）、次为 **+0x9C00**（89 次），另有 31 次 **+4** 与 3 次向低地址回绕；高字节分布 `0xBFF` 532 条、`0xBBF` 37 条 | 同上 |
| guest 内存 | B 段末 `invitado 214/234 MB`（剖析）→ `234/266 MB`；latido B 段 `map=185/204 ent=185/204 lib=23,23 pres=1 mg=1825/158678`；A 段末（`t=241.0 s`）`map=5247/1225 ent=5247/1225 lib=64,64 pres=1 ent_pag=4096 pags=4508 blq=8 mg=2525/165727` | `rex_perfil(25).log`、`rex_latido(15).log` |
| 进程内存 | `proceso 3185/3189 MB`（**只剩约 4 MB**），全程如此；`limite de mapeo 0/0 MB` | `rex_perfil(25).log`（23 个状态点） |
| 时钟 | `CPU 1020.0 MHz`、`GPU 460.8 MHz`、`memoria 1600.0 MHz`；`[apm]` 把 GPU 从 307.2 提到 460.8（`0x92220008`），内存保持 1600 | `rex_perfil(25).log`、`rex_stderr(26).log` |
| 温度 | Soc 45.6 → 46.8 °C、placa 44.2 → 44.6 °C（全程平稳，**不是过热降频**） | `rex_perfil(25).log` |
| 存储 | `[fix28] log del juego: ruta='/switch/Transformers3DOTM/logs/T3DOTM_002.log' largo=45 rc 0xFFFFFFFF errno=5`；心跳全程 `log=0KB(rc 0xFFFFFFFF)` | `rex_stderr(26).log`、`rex_latido(15).log` |
| 映射上限探针 | `[fix32] limite de mapeo: svcGetInfo(ResourceLimit) rc=0x0000E401 (no se pudo leer; tope no confirmado)` | `rex_stderr(26).log` |
| 剖析停顿窗口热点 | 33 帧 > 45 ms（合计 5,000 ms）；pc 采样集中在 **`imagen+0x5521xxx` 一小簇**（`0x55217F8` / `0x5521834` / `0x55218B8` / `0x55218E0`(33 次) / `0x552190C` / `0x5521938`），多个线程同簇 | `rex_perfil(25).log` |
| 游戏侧日志终点 | 两份都停在 **883 MB 预分配**那一行：`fix13: Using pre-allocated block at 46F90000 for size=0x37080000 (committed=true)` / `fix14 alloc: addr=46F90000 size=0x37080000 … page_size=0x10000` —— 之后再没有任何游戏日志 | `T3DOTM_001(25).log`、`T3DOTM_002(23).log` |

### 1.4 判定：**静默定格**，不是崩溃

| 判据 | 上一批（fix40 现场第一次，`rex_crash(25)`） | 本批（`rex_crash(26)`） |
| --- | --- | --- |
| 快照里的 `fatal` 串 | **2**（`[fix39] fatal sin servir`） | **0** |
| `rex_stderr` 里的致命串 | 有 | **`fatal` / `svcBreak` / `diagAbort` / `page fault` / `Data Abort` 全为 0** |
| Atmosphere 崩溃报告 | 两份 `0xCD59 (2345-0102)` `User Break` | **无** |
| 形态 | 一次**写**撞上「真映射、影子都没有」⇒ SEH ⇒ `svcBreak` | **不崩**：2,211,840 次访问逐个用影子服务，帧率掉到 0.2–0.3 |

⇒ 同一份 fix40 产物，换了一个现场（地址族从 `0x6D400000`/`0x6DA00000` 换成 `0xBF9A0260…0xFF4FFCF8`），失败形态就从**崩溃**变成了**定格**。**定格不属于「异常没被处理」，而属于「每次都处理得太贵」**。

### 1.5 定格的性质：按访问计费的模拟洪流

`[fix40]` 的六个字段在本批是**完全一致**的：

```
[fix40] reintento sin mapeo real: invitado=0xBF9A0260 manejador=0 escritura=1 permiso=1 acceso=3 real=0 via=sombra servido=1 (n=0)
[fix40] reintento sin mapeo real: invitado=0xFF4FF5A0 manejador=0 escritura=1 permiso=1 acceso=3 real=0 via=sombra servido=1 (n=2211840)
```

读法：

1. `manejador=0` ⇒ 这次访问**没**被 SDK 的页处理带住（不是内核缺页，是移植层自己的闸门）。
2. `real=0` ⇒ `svcQueryMemory` 说这一页**没有真实映射** —— 步(2)（`RexGmClearProtect` + `RexGmFaultIn`）每次都在这里失败。
3. `via=sombra`、`servido=1` ⇒ 只能靠**影子模拟**把这次访问「演」过去：指令看起来执行了，**页并没有落地**，下一个访问照旧重来。
4. `n` 一路涨到 **2,211,840**，且地址在 `0xBF9A0260…0xFF4FFCF8` 之间以大步（采样后为 `+0x7900`）推进 ⇒ 引擎在一个大区间上**反复走同一套地址**，每次都被影子接住。

量级换算（全部用本批实测数）：

```
2,211,840 次 ÷ 233.8 s ≈ 9,460 次/s          （与剖析的 manejador 6.0k–8.1k/s 同量级 ✅）
233.8 s ÷ 2,211,840 次 ≈ 106 µs / 次          （平均每次访问摊到的整机时间）
单线程采样 98.3–99.3% × 约 234 s ≈ 230 s+     （一个核几乎全花在这条通路上）
```

⇒ 这不是算力问题，也不是死锁：核在跑、访问在处理、帧偶尔出一张，**但引擎的推进速度被压到「看不见」**。要 60 帧，第一步是**把 `via=sombra` 换掉**。

---

## 二、fix42 改了什么（2 个文件 / 13 处 / `+180 −8` 行）

改动落在 `repos/nfsmw-nx/sdk/src/core/`（移植层，不是游戏本体）。

### 2.1 `guest_memory_switch.cpp`：`+165 −0`，7 处

| 落点（改后行号） | 内容 |
| --- | --- |
| 660–667 | `State` 新增 `std::set<std::pair<size_t,size_t>> paginas_vistas`（键 = `(视图序号, 页的映射偏移)`），带说明注释 |
| 1540–1542 | 前置声明 `void SoltarPaginasDeRango(State&, size_t, size_t);`（定义在后面） |
| **1990–2133** | **fix42 主体 144 行**：设计注释（把本批现场写进代码）、`constexpr size_t kPaginasVistasMax = 512`、`g_paginas42 / g_paginas42_neg / g_paginas42_soltadas` 三个原子计数、`SoltarUnaPaginaVista(State&)`、`SoltarPaginasDeRango(State&, size_t, size_t)`、`ServirPaginaDeBloque(uint64_t)` |
| 2135–2136 | `UnmapChunkFromViews` 挂点：块离开视图时清掉它的页登记 |
| 2208–2209 | `MapChunkIntoView` 挂点：整块进视图后，逐页登记作废（块级覆盖了页级） |
| 2216–2217 | 块出视图时同步清登记 |
| 3144–3147 | `extern "C" int RexGmServirPaginaDeBloque(void*)`（供异常处理器调用） |

`ServirPaginaDeBloque` 的六道前置检查（任一不满足即返回 `false`，不改变任何状态）：

1. 地址必须落在某个视图内（`ViewIndexFor`）。
2. **窗口偏移上有保护条目就不碰**（`s.protection.find(pagina_ventana)` 命中）—— 那是移植层的watch或 guest 自己的trampa页，映射它会破坏该条目的含义。
3. 那一页所在的块必须**已materializado且有影子**（`c->shadow`）。
4. 影子指针必须按 4 KB 对齐（否则不映射）。
5. 该 `(视图, 页)` 已在登记里 ⇒ **不重复映射**。
6. 预算：登记满 `512` 条，或 `EntradasTotales() + 1 > TopeTotal()` ⇒ **先 `SoltarUnaPaginaVista`**（挑**偏移最低**的那一页释放 —— guest 的游走是往上走的，最低偏移页最不可能回头），腾出位置再映射；腾不出就直接返回 `false`，**不让预算增长**。

失败与成功都会按「前 32 条 + 每 4096 条」限流落盘：

```
[fix42] pagina de bloque RECHAZADA rc=0x%08X off=0x… vista=… negadas=… entradas=…/…
[fix42] pagina de bloque servida off=0x… vista=… n=… entradas=…/… soltadas=…
Hito: primera pagina de bloque servida
```

### 2.2 `exception_handler_switch.cpp`：`+15 −8`，6 处

| 落点（改后行号） | 内容 |
| --- | --- |
| 117 | 声明 `extern "C" int RexGmServirPaginaDeBloque(void* address);` |
| 1410–1422 | 在步(2)（`ClearProtect` + `FaultIn`，行 1401–1409）之后、fix41 的 `suelta` 之前，插入 **fix42 一级**：`RexGmServirPaginaDeBloque` ⇒ `via40 = "pagina42"` |
| 1423–1429 / 1430–1433 | 原有 fix41 `suelta` 阶梯（保留，注释未改） |
| 1434–1438 | **删掉恒等的 `sombra41` 那一级**（其判断与 `EmulateViaShadow` 重复），保留 `via40 = "sombra"` 作为**最后的地板** |
| 1440–1445 | `[fix40]` 诊断串（未改），此后 `via=` 的取值集合变成 `prestamo / fault-in / pagina42 / suelta / sombra` |

阶梯最终形态：

```
prestamo（只读 watch）
  → fault-in（步(2)：ClearProtect + FaultIn）
    → 【fix42】pagina42（块已materializado，单独映射这一页）
      → suelta（fix41：整块不可用时的散页真映射）
        → sombra（地板：影子模拟）
```

### 2.3 为什么是「单页」、预算为什么**不涨**

- 上一批（fix41 那批）的思路是「**散页真映射**」，但散页走的是整块 alias 那一套，**要 2 个窗口条目**，而且要求块能塞进预算。
- 本批现场的特征是：**块的 alias 已经存在**（`[fix19] alias crudo … total=1/256`），缺的只是**这一页在窗口里的映射** ⇒ 只要 **1 个窗口条目**（对比：fix41 那一档按整页通道是 512 条目，整块更大）。
- 因此 fix42 **不申请新预算**：512 条页登记 ≈ 2 MB 窗口，写满就**释放最老（偏移最低）的一页**再放新的。这保证它在 `proceso 3185/3189 MB`（只剩约 4 MB）这种紧张现场**也敢用** —— 这正是本批的现场条件。
- 为什么不做「一次性 `RexSwitchDiagLine` 上报」（fix41 文档 §四预告的 fix42）？因为本批证明**致命那次已经不存在了**（`fatal` 串 0），新的瓶颈是**吞吐**而不是「看不看得见」；诊断串已经按 32+4096 限流落盘，够用。

---

## 三、构建与产物校验（实测）

```
入口：bash scripts/build_and_verify.sh
等价：cmake -S recomp -B recomp/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
             -DREXSDK_DIR=<sdk> -DT3DOTM_LTO=ON
      cmake --build recomp/out/sw8 -j 4        （ninja -j 4）
      python tools/verify_nro.py recomp/out/sw8/T3DOTM.nro
```

增量构建日志 `recomp/out/sw8/fix42_build.log`：

```
[1/3] Building CXX object …/guest_memory_switch.cpp.obj      ← 0 warning / 0 error
[2/3] Linking CXX static library …/librexruntime.a
[3/3] Linking CXX executable T3DOTM; Packaging T3DOTM.nro
lto-wrapper.exe: warning: using serial compilation of 128 LTRANS jobs   ← 历史固有
ld.exe: warning: creating DT_TEXTREL in a PIE                           ← 历史固有
```

**错误数 0**（`never defined` 那类告警在本轮已消除 —— 那正是本批两次落点事故留下的痕迹，见 §八）。全程**未删** `.ninja_log` / `.ninja_deps`（增量依赖完好）。

`tools/verify_nro.py` 判定 **PASS**：

```
File: T3DOTM.nro      size 0x686480e (109,463,566 bytes)
[0] text   file_off 0x00000000  mem_size 0x055d0000 [OK]
[1] rodata file_off 0x055d0000  mem_size 0x0056a000 [OK]
[2] data   file_off 0x05b3a000  mem_size 0x00d02000 [OK]
Total mapped 109,297,664 bytes (104.2 MiB)   MOD0 bss 0x634e18 (6.2 MiB)
```

产物内的新串与新符号（对产物本体、非源码，逐条核对）：

| 检查 | 结果 |
| --- | --- |
| `[fix42] pagina de bloque servida` | 命中 1 |
| `[fix42] pagina de bloque RECHAZADA` | 命中 1 |
| `"pagina42"` | 命中 1 |
| `primera pagina de bloque servida` | 命中 1 |
| `RexGmServirPaginaDeBloque` | `nm` 中 **`T`**（导出，可被异常处理器调用） |
| `ServirPaginaDeBloque` 的静态量 `aviso42` / `hito42` | `nm` 中为**匿名命名空间内符号**（`_ZZN12_GLOBAL__N_1L20ServirPaginaDeBloqueEmE…`）⇒ **证明确实落在匿名命名空间里**，作用域正确 |
| `RexGmServirPaginaSuelta`（fix41 遗留） | 仍在（`T`），未被覆盖 |

| 版本 | NRO 体积 | SHA-256 |
| --- | --- | --- |
| **fix42（本次交付）** | **109,463,566 B** | **`61590f8a9ce6bcab304d4b4b02b4d80dfce36bd5c49f415e8fa4a780c156a228`** |
| fix41 | 109,455,374 B | `f4cdab82c9639324…`（体积 +8,192 B ⇒ 本次新增代码与串的规模吻合） |

---

## 四、在你的设备上怎么复测

1. 备份旧档后放置新档（细节见 §七）：`T3DOTM.nro` → `sd:/switch/T3DOTM.nro`。
2. 先清掉上一批日志，避免混淆（或用 `[fix42]` 串区分，见 §五 第 1 项）。
3. **不要开超频**（见 §六）。
4. 冷启动后让现场**至少持续 5–10 分钟**（要看到 `[fix42]` 的计数成长）。
5. 退出后把 `sd:/switch/Transformers3DOTM/logs/` 下的 `rex_latido*.log`、`rex_perfil*.log`、`rex_stderr*.log`、`rex_crash*.log`（以及 `T3DOTM_00X.log`）打包回 `.dumate\inbox`。

**这一批要看三件事：`[fix42]` 串的成长、`[fix40]` 串里 `via=` 的比例、以及 `fps` 有没有动。**

---

## 五、本批判读表（下一批日志按这张表看）

| # | 串 / 读数 | 医好长什么样 | 本批（fix40 现场）读数 |
| --- | --- | --- | --- |
| 1 | `[fix42]` 串总数 | **出现**，且 `n` 持续增长 | **0**（本批跑的是 fix40） |
| 2 | `[fix40] … via=pagina42` | 占比**多数**（>90%） | 0（该级还不存在），**`via=sombra` 569/569** |
| 3 | `[fix40] … via=sombra` | **骤降** | 569 条（全部） |
| 4 | `[fix42] pagina de bloque RECHAZADA` | 不出现，或极少 | 0 |
| 5 | `[fix42] … entradas=x/…` 的 `x` | 稳定在 512 内、`soltadas` 同步增长 | 0 |
| 6 | `manejador SDK` 次/s | **降一个量级** | 6.0k–8.1k（起步 22.9k） |
| 7 | 心跳 `fps` | 稳定非 0，且随内容变化 | 0.0（末段 1.0），均值约 0.34 |
| 8 | 心跳 `lento` | 不再占满时长 | 218 s / 233.8 s（93%） |
| 9 | `[fix39] fatal sin servir` | 不出现 | 0（本批整体无 fatal） |
| 10 | `[fix28]` / 心跳 `log=` | `rc=0`、`log` 有 KB | `errno=5`、`log=0KB(rc 0xFFFFFFFF)` ❌ |
| 11 | `[fix32] limite de mapeo` | 能读到上限 | `rc=0x0000E401`（读不到）⚠️ |
| 12 | 时钟 `relojes:` | `CPU 1020 / GPU 460.8 / 1600` = **未超频** | 本批即此 ✅ |

## 五之二、崩溃若重现，按这条顺序读

1. `[fix42] pagina de bloque servida` 有、`n` 在涨，但仍崩 ⇒ **页落地了，还有别的形态**没被服务：核对新的 `[fix39] fatal sin servir` 里的 `off=` / `pag=` / `trozo=` 是否已不再是本批族（`0xBF9A0260…0xFF4FFCF8`）。
2. **一条 `[fix42]` 都没有**且仍崩 ⇒ 请求根本没走到新阶梯，看 `[fix40]` 的 `via=` 落到哪一级、以及 `[fix42] RECHAZADA` 的 `rc=`。
3. `RECHAZADA rc=0x0000CE01`（或类似内核拒绝码）大量出现 ⇒ **内核不让映射**，方向转为「降低同时驻留」或「回 Xbox 侧拆块」（见 §八）。
4. `RECHAZADA rc=0x00000000` ⇒ 拒绝不是内核给的，是前置检查挡的（保护条目 / 无影子 / 不对齐 / 已登记），看 `negadas` 与 `off=` 的分布。

---

## 六、性能目标与超频策略

- 目标（用户硬约束）：「**不需要超频**，流畅 60 帧」。
- 本批实测时钟：**CPU 1020.0 MHz / GPU 460.8 MHz / 内存 1600.0 MHz**，即 APM 官方口径，**没有超频**；SoC 温度 45.6→46.8 °C 全程平稳，**排除过热降频**。
- 当前 **0.2–0.3 fps** 距 60 帧差**两个数量级**，瓶颈是**「每次访问一次异常 + 一次影子模拟」的通路**：
  - 单核已被吃到 99%，再提频率只能**线性**改善（最多把 0.3 fps 抬到 0.4 fps 量级），**不会改变量级**；
  - 真正的量级变化只能来自**把 `via=sombra` 换成 `via=pagina42`**（每页一次映射，之后访问走正常内存，不再进异常）。
- 所以 fix42 之后**仍然不要超频**：先看到 `fps` 从 0.2 抬到「有帧率、且随内容变化」，再谈帧率优化。到那时若确有缺口，我方按 **CPU 1020 / GPU 460.8 / 1600** 这套官方上限口径继续优化（**不越界**）。

---

## 七、放置、回退与指纹表

- 交付件：**`T3DOTM.nro`**（109,463,566 B / `61590f8a9ce6bcab304d4b4b02b4d80dfce36bd5c49f415e8fa4a780c156a228`）+ 本说明文档。
- 放置：解压后把 `T3DOTM.nro` 放到 `sd:/switch/T3DOTM.nro`（覆盖旧档）。
- **先核对哈希**（Windows）：`certutil -hashfile T3DOTM.nro SHA256`。
- 回退：`T3DOTM_dispatch_fix40.zip` 内就是**本批实机运行的那一版**（`3ba8f408…`）—— 要复现本批「定格」现象就用它。

| 版本 | NRO 体积 | SHA-256（前缀） | 说明 |
| --- | --- | --- | --- |
| **fix42** | **109,463,566 B** | **`61590f8a9ce6bcab…`** | 本次交付 |
| fix41 | 109,455,374 B | `f4cdab82c9639324…` | **尚未上机** |
| fix40 | 109,451,278 B | `3ba8f408667b19e8…` | **本批实机运行的版本（实测 `[fix40]` 569 条）** |
| fix39 | 109,451,278 B | `29c14a1b…` | 体积相同，**只能按 SHA-256 认版本** |

源码指纹（改前基线都在同名 `.fix42.bak` 里）：

| 文件 | 改前（= fix41 版） | 改后（fix42） |
| --- | --- | --- |
| `guest_memory_switch.cpp` | 3,805 行 / 183,825 B / 纯 CRLF（3805/3805）/ 无 BOM / `06613b6e911d25a33b0c381fa60219e428823a996f551d24101275b604d0e6e0` | **3,970 行 / 191,890 B / 纯 CRLF（3970/3970）/ 无 BOM / `8f0244cdff1584f82f9b976f341932404bef6110244a6406be6ed8416c562c11`** |
| `exception_handler_switch.cpp` | 1,542 行 / 71,028 B / CRLF 1038 + 裸 LF 504 / 无 BOM / `028ad459885b15ac6099f6a8928613659ab09303e413c57abdf1b46eeedd42c2` | **1,549 行 / 71,697 B / CRLF 1045 + 裸 LF 504 / 无 BOM / `b90b02d630fe9d619bcc421527e20b196a128836f0a4e1168a89afd09f360ed6`** |
| `switch_perf.cpp`、`cmake/rexglue_switch.cmake` | — | **未改动** |

---

## 八、仍未完成 / 待确认（诚实清单）

1. **60 帧目标仍未验证**：fix42 还没上机；本批实测仍是 0.2–0.3 fps。
2. **fix41 至今未上机**：本批（`[fix41]` = 0）设备跑的是 fix40，所以本批读数**不能**用来判 fix41 是否有效；fix42 是在 fix41 之上叠加的，两者会一起接受第一次实机检验。
3. **步(2) 为什么每次失败，仍是两说**：① `RexGmFaultIn` 记账门陈旧（`view_mapped` 说块已进过视图）；② `MapWindowRange` 因 `proceso 3185/3189 MB`（只剩约 4 MB）被内核静默拒。
   - fix42 的 `[fix42] pagina de bloque RECHAZADA rc=0x…` **就是为分辨这两说设的**：若大量 `RECHAZADA` 且 `rc≠0` ⇒ 偏②；若 `RECHAZADA` 很少而 `via=sombra` 仍高 ⇒ 偏①。
4. **本次落地时踩了两次坑（已修，留档）**：fix42 代码块最初被插进 `/* … */` 注释内（`'/*' within comment`），重排后又落在 `} // namespace` 之外（`declared 'static' but never defined`）；最终搬到匿名命名空间内、`SoltarSueltasDeRango` 定义之前才通过编译。⇒ **改这两个文件不要用文本编辑器式的整段替换**，必须按字节精确插入并绕开注释与命名空间边界。
5. **剖析热点只定位到地址，没定位到函数名**：停顿窗口的 pc 集中在 `imagen+0x5521xxx` 簇，但我们手上**没有与 fix40 产物同期的符号表**（`recomp/out/sw8/syms.txt` 是 10-26 的旧档，重新解析出来的名字不可信），而 fix42 重新链接后**全镜像地址有位移** ⇒ 本轮**不给该簇命名**，宁缺勿错。
6. **两个地址族仍只定位到「哪个地址」，没定位到调用者**：`[fix15] far=0x260c401880 / 0x3282401880 invitado=0x10401880 escritura=0 reparado=0 bits=4 instr=0x3CE2483E`（A/B 两组各一条），以及 `[fix18] zero-load` 从 `#1` 到 `#53249`、`invitado=0x36A00000…0x36AD0000`、`instr=0xB9400002`（= `ldr w2,[x0]`）连续 14 条。
7. **`[fix28] errno=5` 与心跳 `log=0KB(rc 0xFFFFFFFF)` 是否同因未确证**：两份游戏日志本身各有 58 KB 内容（说明写入是**阶段性**失败，不是全程失败）。
8. **883 MB 块（guest `0x46F90000`，`len=0x37080000`）为什么一次性 commit 未查**：两份游戏日志的**最后一行**都停在 `fix13`/`fix14` 为这块做的预分配上，之后**没有任何**游戏日志 ⇒ 这块与定格的时间点重合，但「谁要的、为什么要整块」还没定位到调用者。
9. **快照 `rex_crash(26).log` 的触发条件未确证**：本批它不含 `fatal` 串（上一批含 2 条），所以「它是被谁写的」不能由内容推出；两组 pc（A `0x7159…`、B `0x3f2d…`）只能证明**两次独立运行**。
10. **`[fix32]` 读不到资源上限**（`rc=0x0000E401`）：所以「映射上限到底多少」至今是探针，不是事实；fix42 的预算策略因此**不敢靠上限做假设**，只用「满了就换最老的一页」这种自保策略。

---

## 九、交付物

| 文件 | 说明 |
| --- | --- |
| `T3DOTM.nro` | 109,463,566 B / `61590f8a9ce6bcab304d4b4b02b4d80dfce36bd5c49f415e8fa4a780c156a228` |
| `README_dispatch_fix42.md` | 本文档 |
| `T3DOTM_dispatch_fix42.zip` | 内层单一运行文件 `T3DOTM.nro`（已流式核对内层 SHA-256 = 上表值） |