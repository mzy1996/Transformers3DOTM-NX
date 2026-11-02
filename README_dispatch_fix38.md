# T3DOTM Switch 移植 – fix38：把 guest 原始物理缓存里「无人读」的页交还给池

本轮依据：你回传的 fix37 实机复测 6 份日志 —— `.dumate/inbox/rex_crash(22).log`、`rex_latido(11).log`、`rex_perfil(21).log`、`rex_stderr(22).log`、`T3DOTM_001(21).log`、`T3DOTM_002(19).log`。

**一句话结论**：fix37 的缺页限流**全生效**（`corte` 增速从 ~780 次/s 掉到 **~3–5 次/s**，`perf_loop` 增量从 +921/+989 掉到 **+10/s**），画面却仍定格——因为限流只是把「每次都付扫描代价」改成了「每 2 秒试一次」，而那个「每次」现在**注定失败**：装载处还剩 **145 块（290 MB）** 排在门外，`piscina 1256/1256 MB`（`inactiva=0`，池无闲置可让）、`arena u=3009 f=62 MB` 而空闲被切成 **15,551 片**、`proceso 3185/3189 MB` 贴着内核上限，2 MB 板哪一路都取不到；页级通路也满（`entradas pag=4608` 顶格，`[fix32] por paginas: tope duro` 17 次）。fix37 新装的直方图同时把这笔账定名了：`vivos 2,529 MB / 164,466 块` 里，**110 个 >512K 的块独占 2,346,062 KB（≈2.29 GB）**；而 guest 的**原始物理 alias 区**在 fix19 建起来之后，已经「建了又丢」126 次，`crudas 0 MB` —— 那些当初 `RexGmCommit` 提交过的 guest 物理页，**现在既没人读、也没归还，白占着池以外的进程内存**。fix38 就是让 `MaterializarBloque` 在向池要板之前，先把这些「无人读」的原始物理页**交还给池**（每次 ≤16 页 = 32 MB，全程上限 2,048 页），并让 `LiberarPresupuestoParaCarga` 也先走这条通路。

---

## 一、上一批（fix37）实机日志的定性

### 1. 确证本批跑的是 fix37

| 判据 | 本批命中 | 出处 |
| --- | --- | --- |
| `[fix37]` 独占串 | **8 条** = 4 条 `hito` + 4 条 `heap clases` | `rex_stderr(22).log` |
| 4 条 hito | `hito fin de RexGmInit` / `hito primer rechazo 2 MB` / `hito primer tope duro de paginas` / `hito primer bloque sin backing` | `rex_stderr(22).log` |
| `[fix38]` 独占串 | **0 条**（本批不是 fix38） | `rex_stderr(22).log` / `rex_crash(22).log` |
| 崩溃种类 | `Failed to get a Vulkan upload buffer` **0 条**；`hilo `/`esr ` **0 条**（fix35 批的 NULL 崩溃未复现） | `rex_crash(22).log` |
| 游戏日志 | 停止点仍是 `fix14 alloc: addr=46F90000 size=0x37080000` | `T3DOTM_001(21)` / `T3DOTM_002(19)` |

⚠️ **版本只能用实机 NRO 的 SHA-256 定**（见 §七）；字符串命中 `[fix27]/[fix31]/[fix32]/[fix33]/[fix35]/[fix36]` 都说明不了版本——本批心跳行的前缀就是历史遗留的 `[fix27]`。

### 2. 本批现场（全部实测自本批 6 份日志）

| 观测项 | 本批读数 | 出处 |
| --- | --- | --- |
| 帧率轨迹 | 第二段 10 s 处 **10.2 fps** → 进入 `modo: tiron` 后 **1.1 fps** → 定格后 `fps=0.0` | `rex_perfil(21)` |
| 定格起点 | `lento` 由 0 涨到 32 s（t=74.5 s 时 `lento=32s`）⇒ ≈**t=42.2 s 起定格**，`parones 1` | `rex_latido(11)` + `rex_perfil(21)` |
| 主线程占用 | `Main XThread` **99.6%**（同刻 `XThread89E9CF00 28.2%`） | `rex_latido(11)` |
| 缺页拒绝增速 | `corte` t=69.4 s **216** → t=74.5 s **231** ⇒ **~3–5 次/s** | `rex_latido(11)` |
| 性能线程节拍 | `perf_loop` 增量 **+10 / +11 每秒**（fix36 批是 +921/+989） | `rex_latido(11)` |
| 2 MB 拒绝 / 回退 | `neg2m` 33→36、`sin_back` 17→20（定格期仍在缓涨） | `rex_latido(11)` |
| 池两本账 | **`pisc=1256/1256 MB inactiva=0`** ⇒ 池里没有闲置可让 | `rex_latido(11)` |
| 宿主 arena | `u=3009 MB f=62 MB t=44K n=15553`（空闲切成 ~15,553 段） | `rex_latido(11)` |
| 未落位块 | **`pendientes 290 MB (145 bloques)`**，全程恒定不降 | `rex_perfil(21)` |
| 页级窗口 | `pags=4608 blq=9 ent_pag=4608`（**双顶格**）、`map/ent=5282/752`、`lib=126,8` | `rex_latido(11)` |
| 宿主账目 | `proceso 3185/3189 MB`；`invitado 1366/1116 MB` | `rex_perfil(21)` |
| guest 映射 | `mapeos: codigo 5282 (~5282 entradas, 1366 MB), ventana 752 (~752 entradas, 1501 MB)`；`liberadas 126 vistas crudas en 8 eventos, presion 1` | `rex_perfil(21)` |
| 原始视图 | `crudas 0 MB`（活着的原始视图 **0**）| `rex_perfil(21)` |
| 三个拒绝出口是否出现 | `fix18=0 / TOPE=0 / RECHAZADO=0`（全 6 份日志）⇒ 排除「零读仿真顶格」与「内核拒绝」 | 6 份日志 |
| 页级硬闸门 | `[fix32] por paginas: tope duro` **17 次** | `rex_stderr(22)` |
| 卡死块 | `fix15 … invitado=0x6D600000 escritura=1 … bloque=46F90000+37080000 trozo=6D600000+200000 mapeado=si confirmado=no` | `rex_crash(22)` |
| 时钟 | `CPU 1020.0 / GPU 460.8 / memoria 1600.0 MHz`（未启用超频兜底） | `rex_perfil(21)` |
| 游戏日志停止点 | 两份逐行一致，仍停在 `fix14 alloc: addr=46F90000 size=0x37080000` | `T3DOTM_001(21)` / `T3DOTM_002(19)` |

### 3. 机制判定

**(a) fix37 的两条改动都生效了，画面仍定格。** `corte` 增速 780 → 3–5 次/s、`perf_loop` +921/+989 → +10/s，说明「同一块每 2 s 才放行一次」的时间窗限流确实把「每次都完整跑一遍 `memalign(2 MB)` + `mallinfo()` 堆走查 + 阶梯扫描、且全程持映射锁」的自锁解掉了。**但限流换不来板**：被放行的块照样取不到 2 MB，于是装载线程（`Main XThread` 99.6%）就停在 guest 的「等这块内存」上转，`fps` 归 0。**定格的性质从「自锁」变成了「纯等待」**。

**(b) 三处墙同时顶格，且都不是 guest 自己的账。**

- `piscina 1256/1256 MB`、`inactiva=0`：池的板全在用，没闲置可让；
- `arena u=3009 MB f=62 MB`、空闲切成 **15,551 片**：libc 给不出连续 2 MB；
- `proceso 3185/3189 MB`：已贴内核上限；而 `fix18=0 / TOPE=0 / RECHAZADO=0` 说明**既不是零读仿真顶格、也不是内核拒绝**，就是「没有可用的板」。

**(c) 页级通路也满。** `entradas pag=4608` 顶格（天花板 4,608），`[fix32] por paginas: tope duro (4608 + 512 > 4608)` 出现 **17 次**——2 MB 拿不到，页级也不收，145 块无处落位。

**(d) 账目定名：那 ≈2.29 GB 落在宿主第 2 层堆的大块上。** fix37 的 `[fix37] heap clases` 给出：`vivos 2,529 MB / 164,466 块`，其中 **`>512K 110 块 = 2,346,062 KB ≈ 2.29 GB`**（`<=512K 194,294 KB / 695 块`，其余六个尺寸类合计 <70 MB）。而 guest 的 `crudas 0 MB`：**活着的原始物理视图为 0**。合起来就是本批的病灶——fix19 建原始 alias 时会 `RexGmCommit` 一整块 guest 物理页；这些页在视图被丢掉之后**仍保持已提交**，既不参与池的账，也无人读，最后把 `proceso` 顶到 3185/3189 MB 而 145 块排在外面。

### 4. 与 fix36 批的差别

| 项 | fix36 批 | fix37 批（本批） |
| --- | --- | --- |
| `corte` 增速 | **~780 次/s**（自锁） | **~3–5 次/s**（限流生效，改为「等板」） |
| `perf_loop` 增量 | +921 / +989 | **+10 / +11** |
| `Main XThread` | 97.5–97.6% | **99.6%** |
| 池两本账 | `pisc=384/…` → `1270/1270` | **恒 `1256/1256`、`inactiva=0`** |
| 未落位块 | 288 MB (144 块) | **290 MB (145 块)** |
| 前端 | 不崩 | **不崩**（`hilo `/`esr ` 0 条） |
| 新增仪表 | 无 | `[fix37] hito ×4`、`[fix37] heap clases ×4`、心跳 `mg=` |

---

## 二、fix38 的改动（1 个源码文件）

### `guest_memory_switch.cpp`（3,377 → 3,495 行；161,781 → 168,311 B）

**(D) 向池交还「无人读」的原始物理页（核心）**

- 文件作用域声明 `extern "C" int RexSwitchGuestPageState(uint32_t guest_address);`（@38–43）——与 `core/exception_handler_switch.cpp`、`system/xmemory.cpp` 中的定义**同 C 链接**，用的是 fix25 那条**只查状态、不扫文本**的快路径；
- 新增 `std::atomic<size_t> g_fisicos_lib{0}`（本轮累计归还数）、`constexpr size_t kFisicosPorEvento = 16`（每次 ≤16 页 = 32 MB）、`constexpr size_t kFisicosMax = 2048`（**全程上限**，@348–350，防止误判把整个原始区间吃光）；
- 前向声明 `size_t SoltarFisicoSinUso(State& s, const Chunk* excluir);`（@1686–1687，它与原始视图机制同处下方，调用时已持状态锁）；
- **插入点**：`MaterializarBloque` 里，在 `PiscinaReservar();` **之前**调用 `SoltarFisicoSinUso(s, &c);`（@1776）——即在向池要新板之前，先把能还的还掉；紧随其后的 `PiscinaTomar()` 重试才是把还回来的板落到本块上的那一步；
- `SoltarFisicoSinUso` 定义（@2595–2646）：
  - 只挑落在原始物理区间（`kRawFisicoBase .. kRawFisicoBase+kRawFisicoTam`）、**页对齐**、`c.shadow` 为真、且**不是本次要物化的那块**（`&c == excluir` 直接跳过）；
  - **视图判据**：遍历 `s.view_mapped`，若 guest 自己的视图或原始视图仍映射着它（`ViewOf(s, par.first) != nullptr`）⇒ `referenciado`，跳过（有人在读，不能还）；
  - **guest 页表判据**：对块内每一页问 `RexSwitchGuestPageState(invitado + p) != 1` ⇒ 只要有一页是 guest 自己分配的，就 `libre=false`，跳过。**归还 guest 自己的内存不是这块缓存能做的事**；
  - 两条都过才 `UnmapChunkFromViews → ReleaseChunk → 修正 s.committed → s.chunks.erase(it)`，`++hechas`、`g_fisicos_lib.fetch_add(1)`；
  - 打印（前 16 条与每 64 条）：`[fix38] fisico liberado invitado=0x%08zX len=0x%zX total=%zu (%zu MB) | piscina en uso %zu MB de %zu MB tomados`；
- `LiberarPresupuestoParaCarga` 开头新增：`if (SoltarFisicoSinUso(s, nullptr) != 0) return true;`（@2659）——**即使原始视图已全部消失**（本批正是 `crudas 0 MB`），那些被它们提交过的块仍在占内存，所以先走这条通路，且成功即返回「有预算了」。

**为什么可逆**：被归还的 granule 一旦 `RexGmRawAlias` 再次要它，`RexGmCommit` 是**幂等**的（会重写 descriptor），`MaterializarBloque` 会重新把它物化；所以在「活视图为 0」时归还，不影响任何正在读它的东西。**边界**：每次 ≤16 页、全程 ≤2,048 页，且每一笔都记账打印，便于复测核「guest 是否又把同一批要回去」。

⚠️ 本轮**只动 `guest_memory_switch.cpp`**；`switch_perf.cpp` 与 `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` 保持 fix37 原样（心跳格式不变，仍以 `[fix27]` 前缀 + `mg=` 结尾）。

---

## 三、构建与产物校验（本轮实测）

- 构建：`bash scripts/build_and_verify.sh -j 4`（`T3DOTM_LTO=ON`；`export DEVKITPRO=/opt/devkitpro`）。
- 编译阶段：configure + 22 个编译/静态库步骤完成后，**最终 LTO 链接另起一轮收官**：`[1/1] Linking CXX executable T3DOTM; Packaging T3DOTM.nro`，全量日志 `.dumate/logs/build_fix38d.log`；只有两条常态 warning（`lto-wrapper.exe: warning: using serial compilation of 128 LTRANS jobs`、`ld.exe: warning: creating DT_TEXTREL in a PIE`），`error:` = **0**。
- 链接耗时：`recomp/out/sw8/.ninja_log` 末行 `46 6221545 … T3DOTM 5e25abb899b2d73e` ⇒ 最终链接 **6,221,545 ms ≈ 103.7 min**。
  ⚠️ **判进度看 `recomp/out/sw8/.ninja_log` 末行，不要看 build log 尾部**——stdio 缓冲会让它长时间停在上一条不动。
- `verify_nro.py`：**Verdict: PASS**（text `0x055cd000` / rodata `0x0056a000` / data `0x00d01000` / MOD0 bss `0x634d58`）。
- 产物：
  - `recomp/out/sw8/T3DOTM.nro` = **109,447,182 B (0x686080E)**，SHA-256 **`3492f0266b83df22e87b0b346bc0e0a3f4addf774c6654c5fb230c497b9af9d8`**；
  - `T3DOTM`（未 strip ELF）= 128,144,408 B；`T3DOTM.stripped.elf` = 109,347,976 B。
  - ⚠️ **体积自 fix33 起固定为 109,447,182 B，不再随改动变化——绝不能用体积判版本，只能看 SHA-256。**
- 字符串 / 符号核对（对 `T3DOTM.nro` 逐条计数）：**9 个模式全部在位、0 缺失**——
  `[fix38] fisico liberado invitado=0x%08zX len=0x%zX total=%zu (%zu MB) | piscina en uso %zu MB de %zu MB tomados`、`[fix37] hito %s:`、`[fix37] heap clases (KB vivos/bloques):`、心跳尾 `mg=%zu/%zu`、`[fix36] piscina: no reservo mas`、`[fix31] piscina: losa`、`inactiva=`、`[fix32] por paginas: tope duro`；
  ELF 符号：`SoltarFisicoSinUso`（`t`，局部，@`0x15a510`）与 `RexSwitchGuestPageState`（`T`，全局，@`0x280bd0`）均在（`aarch64-none-elf-nm recomp/out/sw8/T3DOTM`）。

---

## 四、复测步骤

1. 把 `T3DOTM.nro` 放到 `/switch/T3DOTM/T3DOTM.nro`（覆盖旧文件），保留 `romfs` 与 TU 覆盖层不变；
2. 冷启动进游戏，**完整跑一次前端动画 + 进第一个可玩画面**，等画面静止 30 s 以上再取日志；
3. 采集 6 份日志（`rex_crash`、`rex_latido`、`rex_perfil`、`rex_stderr`、`T3DOTM_001`、`T3DOTM_002`）回传；
4. 想确认跑的是 fix38，先算 NRO 的 SHA-256 对 `3492f026…9af9d8`（§七）——**不要**用体积、也不要用字符串命中 `[fix27]/[fix36]`。

---

## 五、判读方法（本轮唯一重点：归还物理页有没有让池取回板、145 块有没有放行）

| # | 判读键 | 期望（fix38 生效） | 若不符 |
| --- | --- | --- | --- |
| 1 | **`[fix38] fisico liberado`** | 应出现若干条，`len` 为 `0x200000`（2 MB），`total` 累加且 **≤2,048** | 0 条 ⇒ 归还判据（视图/页表）把每一块都挡了，查 `view_mapped` 与 `RexSwitchGuestPageState` 口径 |
| 2 | **`pisc=` 两本账** | 应重新出现「取用 < 在用」（如 `1256/1152`）或 `[fix31] piscina: losa` 行 ⇒ 池重新拿到板 | 仍恒 `1256/1256` ⇒ 还回来的片没有被重新登记进池 |
| 3 | **`pendientes`** | 应从 **290 MB (145 块)** 开始回落 | 仍恒定 ⇒ 板还是不够，转下一处（见 §八.1） |
| 4 | **游戏日志** | 是否越过 `fix14 alloc: addr=46F90000 size=0x37080000`（本批仍停在这里） | 未越过 ⇒ 装载处仍未过去 |
| 5 | **`corte` / `neg2m` / `sin_back`** | 增速应随队列消化进一步回落 | 反涨 ⇒ 归还的 granule 又被要回并重新提交 |
| 6 | **`arena u/f/t/n`** | 归还释放后 `u` 应下降、`f` 上升、`trozos` 应变少 | 不变 ⇒ `ReleaseChunk` 未真正把内存还给 libc |
| 7 | **`proceso`** | 应从 3185/3189 MB 松开 | 不变 ⇒ 还回的是池的片、不是进程内存（见 §八.2） |
| 8 | **副作用：同一批会不会被要回** | `[fix38]` 里 `invitado=` 地址**不应大量重复**出现 | 重复多 ⇒ 要么判据误放，要么 alias 重建逻辑要复核 |
| 9 | **`fps`** | 是否在第一个可玩画面段首次脱离 0.0 并稳住一段 | 已稳一段 ⇒ 转 T18 帧率测定 |
| 10 | 旧仪表 | `pisc= / corte= / inactiva=`、`mg=`、`arena u/f/t/n`、`proceso`、`invitado`、`pags/ent_pag/blq`、`neg2m/sin_back`、`map/ent/lib`、`[fix37] hito` 一并对读 | |

### 本批新增观察项（(D) 的语义边界，必看）

归还动作是「活视图为 0 时把已提交的 granule 交回池」。若某块其实被 guest **间接**使用（例如 guest 通过非 `view_mapped` 登记的路径读它），归还后下一次访问会触发缺页重建——表现为**零数据症状**（贴图错色 / 几何缺块 / 贴地异常）。这与「有没有画面」是两件事，请一并记录（出现在哪一屏、是否可复现）。`RexSwitchGuestPageState` 那道 guest 页表检查正是为了挡住「guest 自己分配的页」，但它的口径是否正确、也要靠实机数据核。

---

## 六、性能目标与超频策略

- 本批时钟：`CPU 1020.0 / GPU 460.8 / memoria 1600.0 MHz` ⇒ 三档都**未启用**超频兜底。
- 定格**不是算力问题**（§一.3 已证是「等板」），本轮**不动任何频率**；等画面不再定格、进入稳定帧段后，再按 T18 规则逐档评估（CPU ≤1785 / GPU ≤921 / 内存 ≤1600 MHz），目标仍是**不超频稳 60 帧**。
- 现有性能基线（fix37 批实测）：第二段 10 s 处 **10.2 fps**、`tiron` 段 1.1 fps、定格段 0.0 fps —— 这些就是「进入稳定帧段后要往上顶」的起点。

---

## 七、放置方法与回退

### 1. 本批交付物

| 文件 | 内容 |
| --- | --- |
| `T3DOTM.nro` | 109,447,182 B，SHA-256 `3492f0266b83df22e87b0b346bc0e0a3f4addf774c6654c5fb230c497b9af9d8` |
| `README_dispatch_fix38.md` | 本文件 |

放置：`/switch/T3DOTM/T3DOTM.nro`。

### 2. NRO 回退指纹表

| 版本 | NRO SHA-256 | 说明 |
| --- | --- | --- |
| **fix38** | `3492f0266b83df22e87b0b346bc0e0a3f4addf774c6654c5fb230c497b9af9d8` | 本批（归还无人读的原始物理页） |
| fix37 | `29f212782c9e1c21088e37eb1276dc4b25465009a6af6e37ace7f2efb0d3ef70` | 缺页限流 + 直方图 + hito |
| fix36 | `a271aa085cd2d42edfe105894670639c17918e1eb773cf5bed81e3433361e94a` | 池闲置上限 384 MB |
| fix35 | `e9af9252…` | 阶梯 `break → continue` |
| fix34 | `d01a2630…` | 8192 次等待 |
| fix33 | `b5d1e058…` | 池重组 |

⚠️ fix33 起体积固定 109,447,182 B ⇒ **回退只能按 SHA-256 认**。

### 3. 源码指纹表

| 文件 | fix38（本轮） | fix37 |
| --- | --- | --- |
| `guest_memory_switch.cpp` | 3,495 行 / 168,311 B / SHA-256 `b0766d14ed46cd97b429cbe6d9a9595f43ec5e1a311cf72cfad87818df830c1b` / CRLF=3,495 / 无 BOM | 3,377 行 / 161,781 B / `d55a25c2…` |
| `switch_perf.cpp` | **本轮未改**：1,754 行 / 79,062 B / `6e180117d51b63ccead1c5d3db583d040215318036cbf76b7039102b2b0181af` / 无 BOM | 同左 |
| `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` | **本轮未改**：8,505 B / `81baa7e6d4030e3ef5693db3e1d7719fe44b26e5347798b3bc0f80db9338e7ce` | 同左 |

⚠️ `repos/masseffect-nx/sdk/cmake/rexglue_switch.cmake`（8,464 B）是同名**参考副本**，**不要动它、也不要用它的指纹比对**。

---

## 八、仍未完成 / 待确认

1. **归还能否真腾出「可用的 2 MB 板」**：本批 `piscina 1256/1256`、`inactiva=0`，归还的每个原始 granule = 池的一「片」（切片结构，天然 2 MB 对齐）。预期能把 145 块的队列喂上，但**必须实机确认**；若 `pisc=` 仍恒 1256/1256，说明「还回来的片没有重新登记进池」，下一步是查池的免费链表回填路径。
2. **归还的是「进程内存」还是「池的片」**：`ReleaseChunk` 把块还给池（切片），而 `proceso 3185/3189 MB` 是本进程的内存账。若 §五.7 显示 `proceso` 不松，则下一步要么直接向 libc 归还（`free`），要么改「原始 alias 用池的片、不用 libc 块」。
3. **`(D)` 的语义边界**：同一批 granule 会不会被要回、会不会出零数据症状，本批只能靠复测核（§五「新增观察项」）。
4. **`pendientes` 与 `entradas pag=4608` 的耦合**：页级通路在天花板（4,608 条 / 4608+512>4608）被 `[fix32] tope duro` 挡了 17 次；若 fix38 之后仍不够，就要么抬高这条天花板、要么让 `ventana`（752 条 / 1501 MB 这本最贵的账）少收一点。
5. **T18 仍阻塞**：装载处（`46F90000 + 0x37080000`）尚未越过，还没有稳定帧段可测。
6. **`limite de mapeo 0/0 MB`**（`svcGetInfo(InfoType_ResourceLimit)` rc `0x0000E401`）根因已定、尚未解决。
7. **≈2.29 GB 的细分归属**：本批已定名到「宿主第 2 层堆的 110 个 >512K 块」，但还没细分（guest alias / 渲染 / 音频）。需要时再继续。

---

*本文档与 `T3DOTM_dispatch_fix38.zip`（内层仅 `T3DOTM.nro`）同步放入 `.dumate/inbox/` 与工程根目录。*