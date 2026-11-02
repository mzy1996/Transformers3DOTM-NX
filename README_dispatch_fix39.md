# T3DOTM Switch 移植 – fix39：让复活阶梯真正服务「落在 arena 的缺页」（无条件进入 + 步内清保护 + fault 路径按需回收）

本轮依据：你回传的 fix38 实机复测 6 份日志 —— `.dumate/inbox/rex_crash(23).log`、`rex_latido(12).log`、`rex_perfil(22).log`、`rex_stderr(23).log`、`T3DOTM_001(22).log`、`T3DOTM_002(20).log`。

**一句话结论**：fix38 的归还**按设计发生了**（`[fix38]` 17 条打印 = **64 颗粒 / 128 MB**，地址 `0x10400000 → 0x18200000` 连续 2 MB 步进），而且这次**池真的收下了**——本批首次出现 `pisc=1256/1152 MB inactiva=104 MB`（上批恒 `1256/1256 inactiva=0`），未落位队列从 **290 MB/145 块**降到 **≤54 MB/27 块**，`neg2m=0 / sin_back=0 / al4k=0`（上批 33→36 / 17→20），页级硬闸门从 **17 次**降到 **6 次**，`pags=0` 说明页级通路本批**根本没被用**。**但进程内存没松**（`proceso 3185/3189 MB` 全程钉死）、装载仍停在 `fix14 alloc: addr=46F90000 size=0x37080000`（880 MB 连续分配），两次 run 仍**同址崩溃**：`pc = imagen+0x4441e4`、`esr 0x92000006 (EC 0x24)`、`far = <窗口基址> + 0xBA842730`。本轮把这条崩溃的机制查穿了——缺页地址经镜像窗换算后的**映射偏移 `0x11A842730` 落在 arena 区间**，而复活阶梯 **3/4/5 三级没有一级能服务它**（阶梯 3 的门 `!hay_permiso || RexGmShadowFor(far)==nullptr` 对「未提交页」恒为 false；阶梯 4/5 对 `offset >= 0x80000000` 提前 return）⇒ **确定性 fatal**。fix39 让阶梯 3 **无条件进入**、在步内**清掉陈旧保护条目后重试**，并在 fault 路径上新增一条**按需回收**通路（先归还映射条目/无人读物理页，再重试缺页），另补一条只打一次的 `[fix39] fatal sin servir …` 取证行，好让下一批日志直接说出「六种拒绝里是哪一条」拒掉了这次缺页。

---

## 一、本批（fix38）实机日志的定性

### 1. 确证本批跑的是 fix38

| 判据 | 本批命中 | 出处 |
| --- | --- | --- |
| `[fix38] fisico liberado invitado=` 独占串 | **17 条**（`total=1`…`total=16` 连续 + `total=64`） | `rex_stderr(23).log:263,264,…,278,…,336` |
| `[fix39]` 独占串 | **0 条**（本批不是 fix39） | 6 份日志 |
| 归还地址序列 | `0x10400000`(total=1) → `0x12200000`(total=16) → `0x18200000`(total=64)，步长 `0x200000` | `rex_stderr(23).log:263,278,336` |
| 池在用时序（归还打印内） | `1254 → 1252 → … → 1224 MB de 1256 MB tomados` | `rex_stderr(23).log` |
| 页级硬闸门 | `tope duro` **6 次**（fix37 批 17 次） | `rex_stderr(23).log` |
| 崩溃点 | 两次 run **同为** `imagen+0x4441e4`、`esr 0x92000006` | `rex_crash(23).log:13,78` |
| 游戏日志停止点 | 仍停在 `fix14 alloc: addr=46F90000 size=0x37080000` | `T3DOTM_001(22)` / `T3DOTM_002(20)` |

⚠️ **版本只能用实机 NRO 的 SHA-256 定**（见 §七）；字符串命中 `[fix27]`（心跳行的前缀就是它）说明不了版本，`[fix38]` 只说明「至少是 fix38 及以后」。

### 2. 本批现场（全部实测自本批 6 份日志）

| 观测项 | 本批读数 | 出处 |
| --- | --- | --- |
| 帧率轨迹 | `9.9 → 9.9 → 9.9 → 2.0 → 2.0 → 6.0 → 0.0`（t=39–42 s 尾段），`pres` 260→266 | `rex_latido(12)` |
| 主线程占用 | `Main XThread` **94.9% / 97.7%**（同刻 `XThread12F721C0` 29–31%） | `rex_latido(12)` |
| 缺页拒绝 | **`corte=47` 全程恒定**（不再增长） | `rex_latido(12)` / `rex_perfil(22)` |
| 2 MB 拒绝 / 回退 | **`neg2m=0 / sin_back=0 / al4k=0`** | `rex_latido(12)` |
| 池两本账 | **`pisc=1256/1152 MB inactiva=104 MB`**（本批首次出现闲置） | `rex_latido(12)` t=42.1 s |
| 宿主 arena | `u=2991 f=80 t=44K n=10202`（上批 `u=3009 f=62 n=15553`） | `rex_latido(12)` t=42.1 s |
| 未落位块 | `pendientes` 18 MB(9) → 18(9) → 24(12) → **54 MB (27 块)** | `rex_perfil(22)` |
| 宿主账目 | **`proceso 3185/3189 MB` 全程恒定**；`invitado` 8/7 → 104/117 → 164/181 → **230/262 MB** | `rex_perfil(22)` |
| guest 映射 | `map=622/688 ent=622/688`；**`pags=0 blq=0 ent_pag=0`**（页级通路未使用） | `rex_latido(12)` t=42.1 s |
| 等待/锁 | `repfail=0`、`lock=0ms(max 13ms/1279)`、`sd=2581us(rc 0x0)`、`lento=1s` | `rex_latido(12)` |
| 时钟 | `relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1600.0 MHz`（**未超频**） | `rex_perfil(22)` / `rex_stderr(23)` |
| 崩溃 | 见 §3(d)：两次 run 同 `pc` 同 `guest` 地址 | `rex_crash(23).log:12–15,77–80` |
| 游戏日志停止点 | 两份逐行一致，仍停在 `fix14 alloc: addr=46F90000 size=0x37080000`（23:30:29 / 23:31:29） | `T3DOTM_001(22)` / `T3DOTM_002(20)` |

### 3. 机制判定

**(a) 归还按设计发生，而且这次池真的收下了。** 17 条打印、64 颗粒、每颗粒 `0x200000`、累计 128 MB；池的在用第一颗粒后由 `1254` 落到 `1252`、第 16 颗粒（32 MB）时到 `1224 MB`。更关键的是心跳里首次出现 **`pisc=1256/1152 MB inactiva=104 MB`** ⇒ 池的在用 < 总量、有 104 MB 可让 —— **fix38 检查键 #2（「取用 < 在用」）在实机命中**（上批恒 `1256/1256 inactiva=0`）。

**(b) 队列、闸门、2 MB 拒绝都在回落。** `pendientes` 290 MB/145 块 → ≤54 MB/27 块；`tope duro` 17 → 6 次；`neg2m / sin_back / al4k` 全部 0；`corte=47` 不再涨（上批自锁期 780 次/s → 3–5 次/s）；`pags=0 blq=0 ent_pag=0` 说明**页级通路本批根本没被用**——2 MB 板这一路已经能把队列喂上。

**(c) 但进程内存没松，装载仍然没过那道门槛。** `proceso 3185/3189 MB` 与上批**完全一致**、钉死不动；`arena u` 只微降 18 MB；`Main XThread` 在定格期占 94.9–97.7%（在等）。游戏日志两份都停在 `fix14 alloc: addr=46F90000 size=0x37080000`（**880 MB 连续分配**）。这把上批 §八.2 的疑问坐实了：**归还让池有了闲置，却没有把内存还给进程**——`svcGetInfo(ResourceLimit)` 那堵墙仍在。

**(d) 崩溃机制（本轮的决定性发现）：缺页落在 arena，而阶梯 3/4/5 没有一级能服务它。**

| 行 | run 1 | run 2 |
| --- | --- | --- |
| `hilo` | 1344 | 1421 |
| `imagen` | `0x61de9d5000` | `0x0f0c123000` |
| `pc` | `0x61dee191e4` = **`imagen+0x4441e4`** | `0x0f0c5671e4` = **`imagen+0x4441e4`** |
| `esr` | `0x92000006 (EC 0x24)` | `0x92000006 (EC 0x24)` |
| `far` | `0x30b4842730` | `0x474c842730` |

- **同一个 guest 地址**：`far` 减去各自的窗口基址（run 1 `0x2FFA000000`、run 2 `0x4692000000`）都得 **`0xBA842730`**；再按 `RexGmWindowToOffset`（`v->offset + (window_address - v->base)`，镜像窗常数 `0x60000000`）换算 ⇒ **映射偏移 `0x60000000 + 0xBA842730 = 0x11A842730`，落在 `[0x100000000, 0x120000000)` 即 arena（guest 物理内存区）**。
- **崩的是 guest 的一页物理内存**（arena 那块 2 MB 板上的 `+0x42730`），不是宿主指针。
- 该颗粒的来历查得到：`[fix19] alias crudo invitado=0x1A800000 arena=0x11A800000 len=0x200000 total=83/256`（`rex_stderr(23).log:163`）与 `[fix19] alias liberado invitado=0x1A800000 len=0x200000`（`:358`）。`SoltarVistaCruda` 只 `UnmapChunkFromViews` + `r.viva=false`，**块本身仍留着**（源码注释明写 "the chunks behind them stay"）⇒ 缺的是**视图/保护条目**，不是块。
- **阶梯 3 本该处理这种情形**（「页在、视图被丢、块还在」），但它的门 `if (!hay_permiso || RexGmShadowFor(far) == nullptr)` 对未提交页恒为 **false**（`RexGmQueryProtect` 对未提交页返回 `true + REX_GM_NONE`；`IsCommittedWindowAddress` 判的是 `ChunkAt != nullptr`）⇒ **阶梯 3 在本项目里是死代码**；阶梯 4（`RepairMissingGuestVirtualPage`）与阶梯 5（@1158–1160）又以 `offset >= 0x80000000u` 提前 `return`。三条路都够不着 `0x11A842730` ⇒ 必然 fatal（两次 run 同址同 `pc` 就是它的签名）。
- **与 fix38 无因果**：归还列表是 `0x10400000–0x18200000` 的连续 64 颗粒，**不含**崩溃颗粒 `0x1A800000`；两者只是同一预算耗尽的两种表现。

### 4. 与 fix37 批的差别

| 项 | fix37 批 | fix38 批（本批） |
| --- | --- | --- |
| 池两本账 | 恒 `1256/1256 inactiva=0` | **`1256/1152 inactiva=104`**（首次出现闲置） |
| 未落位块 | 290 MB (145 块) | **≤54 MB (27 块)** |
| `neg2m` / `sin_back` | 33→36 / 17→20 | **0 / 0** |
| 页级顶格 | `[fix32] tope duro` 17 次 | **6 次** |
| 页级通路 | `entradas pag=4608` 顶格 | **`pags=0 blq=0`（未使用）** |
| 宿主 arena | `u=3009 f=62 n=15553` | `u=2991 f=80 n=10202` |
| `proceso` | 3185/3189 MB | 3185/3189 MB（**不变**） |
| 崩溃 | 同址 `imagen+0x4441e4`（机制未定） | 同址，**机制已定（arena 缺页无人服务）** |
| 新增仪表 | `[fix37] hito ×4`、`heap clases ×4` | `[fix38] fisico liberado`；下批看 `[fix39]` |

---

## 二、fix39 的改动（2 个源码文件，5 处）

### (1) `repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp`（1,409 → 1,450 行；63,457 → 65,971 B）

- 文件作用域新增 `extern "C" int RexGmLiberarPresupuestoParaCarga(void);`（**@115**，紧挨已有的 `RexGmClearProtect` 声明 @114）；
- **(C) 键空间修正**：`RexGmClearProtect` 的键是**窗口偏移**（`RexGmProtect` 写入、`RexGmFaultIn` 命中判定都按窗口偏移），原调用点传的却是**映射偏移** ⇒ @995–1001（说明+调用）与 @1132–1143（步内重试）统一改成 `far - RexGmBase()`；
- **(A) 阶梯 3 无条件进入 + 步内清保护重试**：原门 `if (!hay_permiso || RexGmShadowFor(far) == nullptr)` 在「块还在、只是视图丢了」时恒 false ⇒ 改为**无条件执行该步**（@1094 说明、**@1111** 起「第二半：门不再是条件」注释），并在步内先 `RexGmClearProtect(窗口偏移 & ~0xFFF, 0x1000)` 清掉陈旧保护条目、再重试映射（@1127 记录 `respaldo_previo`、@1132–1143 重试块）。区间判定（`offset ∈ arena`）负责把这步限制在本类缺页上；操作幂等；
- **(E) 未服务后的按需回收 + 重试**：阶梯仍未服务时调用 `RexGmLiberarPresupuestoParaCarga()`，返回非 0 ⇒ 再 `RexGmFaultIn(far)` 重试一次（@1145 说明、**@1152** 调用与重试）；
- **(B) 取证行（每次 run 一条）**：`[fix39] fatal sin servir en_ventana=%d offset=%d:0x%zX escritura=%d permiso=%d acceso=%d respaldo=%d | <RexGmFaultDiag>`（@1374 说明、**@1395** 打印），紧接在原有崩溃转储之前 —— 这是回答「六种拒绝里是哪一条」的那一行；
- 另有 `[fix39] fisica a la primera invitado=0x%08llX off=0x%zX servido=%d escritura=%d`：只在「该地址此前没有 backing」时打印，有界（前 48 次）（@1156–1170）。

### (2) `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`（3,495 → 3,534 行；168,311 → 170,511 B）

- **(D) 新增 C 链接导出 `int RexGmLiberarPresupuestoParaCarga(void)`**（**@2685** 说明、**@2701–2721** 定义、@2714 打印）：
  - 自己取 `State& s = S()` 与 `GmLock lock(s.mutex)`（fault 路径从异常上下文进来，不能再假定已持锁）；
  - 若事件数未到上限且仍有活着的原始视图 ⇒ `g_lib_eventos++`、`g_presion_mapeo = true`、`LiberarVistasCrudas(kLibVistasPorEvento)`（唯一会归还**映射条目**的那一支）、`g_lib_vistas += …`，并打
    `[fix39] presupuesto para el fault: liberadas %zu vistas crudas (%zu -> %zu vivas) en el evento %zu/%zu`；
  - 随后 `SoltarFisicoSinUso(s, nullptr)`（fix38 那条「把无人读的原始物理页还给池」）——**先归还映射条目、后归还物理页**（与 `LiberarPresupuestoParaCarga` 的顺序相反，因为 fault 路径先要的是**条目**）；
  - 两者之一非 0 ⇒ 返回 1（「有预算了」）。
- **为什么可逆**：归还的 granule 一旦再被要，`RexGmCommit` 幂等、`MaterializarBloque` 会重新物化；被清的只有**陈旧保护条目**（其页已不映射），重建路径与首次缺页完全相同。**边界**：每次事件的视图数有上限、事件总数有上限（`kLibEventosMax`），且每一笔都记账打印，便于复测核「fault 路径会不会把回收变成抖动」。
- ⚠️ 本轮**只动上面两个文件**；`repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp` 与 `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` 保持原样（心跳格式不变，仍以 `[fix27]` 前缀、`mg=` 结尾）。
- ⚠️ **行尾铁律**：`guest_memory_switch.cpp` 必须**纯 CRLF、无 BOM**；`exception_handler_switch.cpp` 是**混合行尾**文件（CRLF 946 行 / LF 504 行）——**不要用会自动归一化行尾的编辑器改它**（会整文件变成 CRLF），要按「逐行保留各自 EOL」的脚本改。本轮 `scripts/patch_fix39.py`（第一半）与 `scripts/patch_fix39b.py`（第二半）就是这么写的。

---

## 三、构建与产物校验（本轮实测）

- 构建：`bash scripts/build_and_verify.sh`（`T3DOTM_LTO=ON`、`JOBS=4`；`export DEVKITPRO=/opt/devkitpro`；`cmake` 用非 sandbox 桩的那一份）。
- **新增前置验证（本轮引入，建议沿用）**：先只编译被改动的两个 TU —— `ninja rexglue-sdk/src/core/CMakeFiles/rexcore.dir/{exception_handler_switch,guest_memory_switch}.cpp.obj` ⇒ `rc=0`、无警告；确认语法/格式串/占位符无误**再**进 100 分钟量级的 LTO 链接。
- 编译阶段：configure + 2 个重编 + 静态库，随后最终 LTO 链接：`[1/1] Linking CXX executable T3DOTM; Packaging T3DOTM.nro`；全量日志 `recomp/out/sw8/fix39_build.log`；`warning:` **2 条**（`using serial compilation of 128 LTRANS jobs`、`ld.exe: warning: creating DT_TEXTREL in a PIE`），`error:` **0**。
- 链接耗时：`recomp/out/sw8/.ninja_log` 末行 `6465 2660325 … T3DOTM 5e25abb899b2d73e` ⇒ 最终链接 **2,660,325 ms ≈ 44.3 min**。
  ⚠️ **判进度看 `.ninja_log` 末行，不要看 build log 尾部**（stdio 缓冲会让它长时间停在上一条不动）。
- 后台运行方式：本轮的构建用**真正分离**的 `Start-Process` 拉起（脚本内 `cd` + 相对路径调用，避免含空格路径的参数引号问题）；`setsid` 逃不出 Windows Job Object，中断工具调用会连根杀进程树，不可用。
- `verify_nro.py`：**Verdict: PASS**（text `0x055ce000` / rodata `0x0056a000` / data `0x00d01000` / MOD0 bss `0x634d58`；`Total mapped 0x6839000` = 104.2 MiB）。
- 产物：
  - `recomp/out/sw8/T3DOTM.nro` = **109,451,278 B (0x686180E)**，SHA-256 **`29c14a1b7e28b305ac59e32a50048a4e7f579e13c6171700c2f67dfab83def96`**；
  - `T3DOTM`（未 strip ELF）= 128,148,880 B；`T3DOTM.stripped.elf` = 109,352,072 B。
  - ⚠️ **体积不再固定**（自 fix33 起长期为 109,447,182 B，本轮因新增代码与字符串长到 **109,451,278 B**）⇒ **只能按 SHA-256 认版本**。
- 字符串 / 符号核对：对 `T3DOTM.nro` 逐条计数——`[fix39] fisica a la primera invitado=`、`[fix39] fatal sin servir en_ventana=`、`[fix39] presupuesto para el fault: liberadas` 各 **1 条**、`[fix38] fisico liberado invitado=` 保留 **1 条**，**0 缺失**；
  ELF 符号（`aarch64-none-elf-nm recomp/out/sw8/T3DOTM`）：`RexGmLiberarPresupuestoParaCarga`（`T`，**@`0x15c4b0`**）、`RexGmFaultDiag`（`T`，@`0x160e70`）、`SoltarFisicoSinUso…part.0`（`t`，@`0x15a6f0`）。
  注：函数**符号名**不会出现在 `T3DOTM.nro` 里（NRO 由 stripped ELF 打包），所以要在**未 strip 的 ELF** 上核。

---

## 四、复测步骤

1. 把 `T3DOTM.nro` 放到 `/switch/T3DOTM/T3DOTM.nro`（覆盖旧文件），保留 `romfs` 与 TU 覆盖层不变；
2. 冷启动进游戏，**完整跑一次前端动画 + 进第一个可玩画面**，等画面静止 30 s 以上再取日志；
3. 采集 6 份日志（`rex_crash`、`rex_latido`、`rex_perfil`、`rex_stderr`、`T3DOTM_001`、`T3DOTM_002`）回传；
4. 想确认跑的是 fix39：先算 NRO 的 SHA-256 对 `29c14a1b…3def96`（§七）——**不要**用体积（本轮起已不固定）、也不要用字符串命中 `[fix27]/[fix38]`。

---

## 五、判读方法（本轮唯一重点：这次缺页有没有被服务、是哪条拒绝）

| # | 判读键 | 期望（fix39 生效） | 若不符 |
| --- | --- | --- | --- |
| 1 | **`[fix39] fatal sin servir …`** | **不应出现** ⇒ 缺页被阶梯 3 / 回收重试服务掉了 | 出现 ⇒ 它同时给出 `permiso/acceso/respaldo` 与 `RexGmFaultDiag` 文本，直接定位六种拒绝中的哪一条（上批唯一未知量） |
| 2 | **`[fix39] fisica a la primera invitado=… servido=?`** | 应出现，且 `servido=1` ⇒ 回收后重试成功 | `servido=0` ⇒ 回收没换来可用板/条目，转 §八.2 |
| 3 | **`[fix39] presupuesto para el fault: liberadas N vistas crudas (A -> B vivas) en el evento N/M`** | 应出现若干条，`N`>0 且 `B`<`A` | 0 条 ⇒ fault 路径没走到回收（查 `kLibEventosMax` 与活视图数） |
| 4 | 崩溃点 | `pc = imagen+0x4441e4`、`esr 0x92000006`、`far = 窗口基址+0xBA842730` **应消失** | 仍出现 ⇒ 阶梯 3 的无条件化没接管（对照第 1 项是否伴随出现） |
| 5 | `corte` | 应继续不涨（本批恒 47） | 开始涨 ⇒ 回收变成了抖动（每次缺页都回收+重试） |
| 6 | `pisc= / inactiva=` | 闲置应继续存在或变大 | 又回 `1256/1256 inactiva=0` ⇒ 回收的片被瞬间吃光 |
| 7 | `pendientes` | 应从 ≤54 MB (27 块) 继续向 0 落 | 不落 ⇒ 队列没被喂上 |
| 8 | 游戏日志 | 是否越过 `fix14 alloc: addr=46F90000 size=0x37080000` | 未越过 ⇒ 880 MB 那道墙仍在（§八.1） |
| 9 | `proceso` | 是否从 3185/3189 MB 松开 | 不变 ⇒ 见 §八.2 |
| 10 | `fps` / 主线程 | `fps` 是否首次脱离 0.0 并稳住一段；`Main XThread` 是否从 95–98% 降下来 | 已稳一段 ⇒ 转 T18 帧率测定 |
| 11 | 旧仪表 | `neg2m/sin_back/al4k`、`arena u/f/t/n`、`map/ent`、`pags/ent_pag/blq`、`mg=`、`lento/parones`、`repfail/lock/sd`、`[fix38] fisico liberado` 一并对读 |  |

### 本批新增观察项（阶梯 3 无条件化的语义边界，必看）

阶梯 3 原来是死代码，这轮**第一次真正跑起来**：它会把「页在、视图没了、块还在」的那一类**一律**重建。风险面是**误伤**——若某块其实被 guest 通过非 `view_mapped` 的路径读着，重建/清保护会与它并发。请一并记录**零数据症状**（贴图错色 / 几何缺块 / 贴地异常）出现在哪一屏、是否可复现；第 1 项的 `RexGmFaultDiag` 文本请**整行**回传。

---

## 六、性能目标与超频策略

- 本批时钟：`relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1600.0 MHz` ⇒ 三档都**未启用**超频兜底。
- 定格**不是算力问题**（本批已证是「等 880 MB 连续块 + 预算耗尽」），本轮**不动任何频率**；等装载越过、进入稳定帧段后，再按 T18 规则逐档评估（CPU ≤1785 / GPU ≤921 / 内存 ≤1600 MHz），目标仍是**不超频稳 60 帧**。
- 帧率基线（本批实测）：尾段 **9.9 fps → 2.0 → 6.0 → 0.0**（`pres` 260→266）—— 这就是「越过装载后要往上顶」的起点。

---

## 七、放置方法与回退

### 1. 本批交付物

| 文件 | 内容 |
| --- | --- |
| `T3DOTM.nro` | 109,451,278 B，SHA-256 `29c14a1b7e28b305ac59e32a50048a4e7f579e13c6171700c2f67dfab83def96` |
| `README_dispatch_fix39.md` | 本文件 |

放置：`/switch/T3DOTM/T3DOTM.nro`。

### 2. NRO 回退指纹表

| 版本 | NRO SHA-256 | 说明 |
| --- | --- | --- |
| **fix39** | `29c14a1b7e28b305ac59e32a50048a4e7f579e13c6171700c2f67dfab83def96` | 本批（阶梯 3 无条件 + 步内清保护 + fault 路径回收 + 取证行） |
| fix38 | `3492f0266b83df22e87b0b346bc0e0a3f4addf774c6654c5fb230c497b9af9d8` | 归还无人读的原始物理页 |
| fix37 | `29f212782c9e1c21088e37eb1276dc4b25465009a6af6e37ace7f2efb0d3ef70` | 缺页限流 + 直方图 + hito |
| fix36 | `a271aa085cd2d42edfe105894670639c17918e1eb773cf5bed81e3433361e94a` | 池闲置上限 384 MB |
| fix35 | `e9af9252…` | 阶梯 `break → continue` |
| fix34 | `d01a2630…` | 8192 次等待 |
| fix33 | `b5d1e058…` | 池重组 |

⚠️ 体积自本批起**又变了**（109,447,182 → 109,451,278 B）⇒ **回退只能按 SHA-256 认**。

### 3. 源码指纹表

| 文件 | fix39（本轮） | fix38 |
| --- | --- | --- |
| `repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp` | 1,450 行 / 65,971 B / SHA-256 `390a79d008ef5a9a8a10089e9526459a0d68d02228300793847bcdf094de91d1` / CRLF=946、LF=504 / 无 BOM | 1,409 行 / 63,457 B / `08ce6d11dc5f265c80e3a45dc121bbf2d0481447434cce216450999be7c0556f` |
| `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp` | 3,534 行 / 170,511 B / SHA-256 `773b1939bd07a8782ea935ddc9d602a9cb88a856e0c6fa7e1d61479362ad222a` / CRLF=3,534 / 无 BOM | 3,495 行 / 168,311 B / `b0766d14ed46cd97b429cbe6d9a9595f43ec5e1a311cf72cfad87818df830c1b` |
| `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp` | **本轮未改**：1,754 行 / 79,062 B / `6e180117d51b63ccead1c5d3db583d040215318036cbf76b7039102b2b0181af` / CRLF=1,754 / 无 BOM | 同左 |
| `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` | **本轮未改**：8,505 B / `81baa7e6d4030e3ef5693db3e1d7719fe44b26e5347798b3bc0f80db9338e7ce` / CRLF=151、LF=10 | 同左 |

⚠️ `repos/` 下各有一份**同名参考副本**，**不要动它们、也不要用它们的指纹比对**：`masseffect-nx/sdk/src/ui/switch_perf.cpp`（1,725 行 / 74,415 B / `ba3eafb7eb7a5922cf6cd35444b3b29c5739806750ccb45615a7ad3390754ac9`）、`masseffect-nx/sdk/cmake/rexglue_switch.cmake`（8,464 B / `eddc194288614db3524515feb6d750bb7aeaf036a38a13ea393dfff90a9965a7`）。

---

## 八、仍未完成 / 待确认

1. **880 MB 连续块那道墙**（`fix14 alloc: addr=46F90000 size=0x37080000`）：本批两次 run 都没越过，仍是最前面的门槛；fix38 让池有了闲置（104 MB），但离 880 MB 连续还差得远 ⇒ 下一步要么让这笔分配**分段落位**（拆成可服务的小块），要么继续回收进程内存。
2. **`proceso 3185/3189 MB` 贴顶**：归还的片回到池、没还给进程（本批 §一.3(c) 已坐实）⇒ 下一步要么直接向 libc 归还（`free`），要么改「原始 alias 用池的片、不用 libc 块」。
3. **六种拒绝里到底是哪一条拒了这次缺页**：上批唯一未知量，本批由 `[fix39] fatal sin servir …` 回答。
4. **fault 路径回收的抖动风险**：每次缺页都「回收 + 重试」，若 `corte` 开始涨或 `fps` 反而更差，就要把回收**限流**（对齐 fix37 的时间窗做法）。
5. **`limite de mapeo 0/0 MB`**（`svcGetInfo(InfoType_ResourceLimit)` rc `0x0000E401`）根因已定、尚未解决。
6. **T18 仍阻塞**：装载处尚未越过，还没有稳定帧段可测。

---

*本文档与 `T3DOTM_dispatch_fix39.zip`（内层仅 `T3DOTM.nro`）同步放入 `.dumate/inbox/` 与工程根目录。*