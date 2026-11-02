# T3DOTM Switch 移植 – fix36：给预留池加闲置上限，别再在 init 把宿主内存囤着——「前端进第二个 logo 崩溃退出」与「Sphiara 进入定格」是同一个机制

本轮对应你回传的 fix35 实机复测 6 份日志（`rex_crash(20)`、`rex_latido(9)`、`rex_perfil(19)`、`rex_stderr(20)`、`T3DOTM_001(19)`、`T3DOTM_002(17)`），以及你描述的两条现象：**「从前端启动时进到第二个 logo 动画就报错退出了」**、**「从 Sphiara 进入与先前一致停住」**。

一句话结论：**fix35 的三项改动全部生效了——池第一次填满（16 块 96 MB 板 = 1,536 MB）、阶梯真的逐级问到了 arena（`escalones saltados 0 intentados 4`）、`corte` 从 fix34 的 47 回到十万量级。但恰恰是「池第一次填满」把两类现象同源暴露出来：池在 init 一次性把 1,536 MB 拿走并一直攥着，而这台机器（进程上限 3,189 MB）已经没有第二份内存给 emulator 自己用。** 前端那次（run1）崩溃时池里 **1,388 MB 闲置**（`pisc=1536/148`），`arena` 只剩 59 MB 散在 7,316 片，Mesa 的 NIR/ralloc arena 分配器从自己的 arena 拿到 NULL 后直接解引用（`far=0x4`、`x0=0`）；Sphiara 那次（run2）池 **1,536 MB 全在用**、`proceso 3185/3189 MB` 顶格，80 个块的 2 MB backing 再也拿不到。**两条现象、一个机制：池抢走了 emulator 要用的宿主内存。** fix36 就让池只持有一个工作集（闲置上限 384 MB = 4×96 MB），其余在需求路径上按需再取。

---

## 一、上一批（fix35）实机日志的定性

### 1. 确证本批跑的是 fix35

- 字符串侧强证据（`rex_stderr(20)`）：`[fix35] piscina: sin losa nueva (` **16 条**（= 8 条 `piso` + 8 条 `escalera`），`escalones saltados` **16 条**、`[fix31] piscina: losa` **16 条**、`piscina: compactada` **1 条**。
- 反向核对：`[fix34]` **0 条**、`tope duro` **0 条**、`[fix33] por paginas: techo de entradas` **16 条**、`[fix27] 2 MB denegado` **4 条**、`[fix32] limite de mapeo` **1 条**、`liberadas … vistas crudas` **8 条** → 与本批判读键一致。
- NRO 指纹核对见第七节回退表（fix35 = `e9af9252…7724d239b3`）。

### 2. 本批现场（事实，全部来自本批 6 份日志）

`rex_latido(9).log` 一份里含**两段运行**：第 1–23 行是**前端那次（run1，t=1.0→25.1 s，随后崩溃）**，第 24–197 行是**Sphiara 那次（run2，t=1.0→171.3 s）**。

| 观测项 | 数值 / 形态 | 出处 |
| --- | --- | --- |
| **池第一次填满（本轮头号事实）** | `[fix31] piscina: losa 1…16`，每块都是 `96 MB llena`；`losa 16` 在 `0x381a200000`、`tomado 1536 MB, arena usado 2382 MB, libre 88 MB`。`piscina tomada 1536 MB` | `rex_stderr(20)` / `rex_perfil(19)` |
| 阶梯真的下探了（fix35 改动①生效） | `[fix35] … (escalera) arena libre 66 MB en ~10145 trozos, escalones saltados 0 intentados 4, tomado 1536 MB, en uso 1426→1440 MB, cortes 39→46, intentos 17→24` | `rex_stderr(20)` |
| 地板门仍命中（fix35 改动③未涉 96 MB 以外的况） | `[fix35] … (piso) arena libre 0/0/1/3/0/1/2/4 MB` **8 条**，全部落在启动前 8 次（`cortes 1..8`），此刻池还是 `0/0` | `rex_stderr(20)` |
| 上限门 | `[fix35] … (tope)` **0 条** —— 池停在 1,536 MB（< `kPiscinaTope` 1600），上限门一次没走到 | `rex_stderr(20)` |
| 压实（fix33 通路仍在） | `[fix33] piscina: compactada, 48 lonchas recuperadas (96 MB), en uso 1440 MB de 1536 MB tomados` | `rex_stderr(20)` |
| 页级通路仍撞条目天花板 | `[fix33] por paginas: techo de entradas (5775 + 512 > 6144) off=0x75800000` **×16** | `rex_stderr(20)` |
| **run1（前端→第二个 logo）崩溃前一刻** | `t=25.1 s`：`pisc=1536/148 MB`（**1,388 MB 闲置**）`corte=34 arena u=3013 f=59 t=96K n=7316`；此前一直 `fps≈18.9`（动画仍在跑） | `rex_latido(9)` 第 23 行 |
| **run2（Sphiara）定格前一刻** | `t=25.3 s`：`pisc=1536/1536 MB`（全在用），`fps` 从 4.0 掉到 `0.0`；此后定格到 `t=171.3 s`；`corte` 74 → **129,187**、`neg2m=43047 sin_back=43030`、`pres=298 lento=145s` | `rex_latido(9)` 第 46 / 197 行 |
| run2 终局账目 | `arena u=3021 f=50 t=8K n=14333`、`map=4901/877`、`ent=4901/877`、`lib=126,8`、`pres=1 ent_pag=4096`；perfil：`invitado 1626/1376 MB`、`materializados 813, pendientes 160 MB (80 bloques), parones 1`、`piscina tomada 1536 MB en uso 1536 MB cortes 128204` | `rex_latido(9)` / `rex_perfil(19)` |
| 宿主顶格（**本轮新墙**） | `limite de mapeo 0/0 MB, proceso 3185/3189 MB`（全程不动）；`arena usado 3021 MB libre 50 MB tope 8 KB trozos 14333` | `rex_perfil(19)` |
| 渲染侧同源报错（run2） | `T3DOTM_002(17)` 内 **74 条** `Shared memory: Failed to get a Vulkan upload buffer` / `Failed to request vertex buffer at 0x1FA11790 (size 80)` / `PM4_DRAW_INDX(4, 6, 2): Failed in backend`（首条 `04:33:06.620`） | `T3DOTM_002(17)` |
| 游戏日志停止点 | run2 仍停在同一个 880 MB 装载块：`Large page alloc failed bottom-up on 64KB heap … for size=0x37080000` → `BaseHeap::Alloc failed to find contiguous range` → `falling back to 4KB heap` → `BaseHeap::Alloc failed …` → `fix13: Using pre-allocated block at 46F90000 …` → `fix14 alloc: addr=46F90000 size=0x37080000`（`fix14 alloc` 计数 **102**，与上一批逐字相同） | `T3DOTM_002(17)` 第 578–579 行 |
| run1 游戏日志 | `T3DOTM_001(19)`，`fix14 alloc` 计数 **101**，末行 `04:32:28.109 … LENTO: abrir 'D:\TransGame\CookedXenon\UI_LobbyMaterials_p.xxx'` —— 前端正加载 UI 材质时崩溃 | `T3DOTM_001(19)` |
| 频率与温度 | `CPU 1785.0 MHz, GPU 921.6 MHz, memoria 1600.0 MHz`；`SoC 47.4–48.9 °C` | `rex_perfil(19)` |

### 3. 机制判定

**(a) fix35 的三项改动全部生效。** 池第一次取到板并填满 16 块（`pisc=1536`）；阶梯被真正走到底（`escalones saltados 0 intentados 4`：96 / 16 / 4 / 2 MB 四级全被 arena 拒，不是被自己的地板门跳过）；`corte` 从 fix34 的 47 回到十万量级（129,187）——「每次缺页都问池」恢复了。**fix34 那个「按次数等待」的自锁彻底解除。**

**(b) 但「池第一次填满」本身成了新墙，而且是两条现象的共同根因。** 池在 init 一次性把 1,536 MB 拿走。这台机器的进程容量是 3,189 MB（`proceso 3185/3189 MB` 顶格不动），于是：

- **run1：池大部分闲置 → emulator 挨饿 → 崩溃。** `t=25.1 s` 池 `pisc=1536/148`，即 **1,388 MB 闲置**，而 `arena` 只剩 `59 MB 散在 7,316 片`（均值 ~8 KB）。此时 GPU 线程在为一次 transfer/resolve 绘制预编译图形管线：SPIR-V → NIR 构建一路调用 Mesa 自己的 arena/ralloc 分配器，分配返回 NULL，调用者没判空就解引用。

**(c) run1 崩溃点的硬证据（可由日志逐字复现）。** `rex_crash(20)`：

```
hilo 1460, imagen 0x0000007aec5cb000 (codigo hasta 0x0000007af1b98000)
pc  0x0000007af19f13d4 = imagen+0x54263d4
lr  0x0000007af18867f8 = imagen+0x52bb7f8
esr 0x92000005 (EC 0x24)  far 0x0000000000000004  error_desc 0x101  pstate 0x60000000
instruccion 0x29409401
x0  0x0000000000000000  x1 0x0000000000000090  x2 0x00000000003201d5  x3 0x0000000000000097
```

- `esr 0x92000005` 的 `EC = 0x24` 是 **Data Abort from lower EL**；`far = 0x4` 且 `x0 = 0` ⇒ **对 NULL+4 的访存**。
- 故障指令 `0x29409401` 独立解码为 **`ldp w1, w5, [x0, #4]`**（opc=00、1010 装载对、imm7=1→偏移 4、Rt2=5、Rn=0、Rt=1），与 `far=0x4`、`x0=0` 完全吻合：从空指针读一对寄存器。**对该指令已在 fix35 包内的 NRO 上逐字节复核**（image 偏移 = ELF `.text` vaddr，`.text` 在 NRO 中 file_off=0），`0x54263d4` 处的 4 字节确为 `01 94 40 29`，即 `0x29409401`。
- 21 层调用链（在 fix35 版上用 `addr2line` 最近符号解析；Mesa 内部无 DWARF，故内层名字为「最近符号」，外层 rex 帧可靠）：

  `imagen+0x54263d4`（Mesa arena/ralloc 分配子函数，读空指针处）← `imagen+0x52bb7f8`（NIR 构建侧）← `imagen+0x52cbeb0`（`spirv_to_nir` 一侧）← `imagen+0x517e07c`（`vk_nir.c` 一侧）← `imagen+0x51841e0` ← `imagen+0x51846c4` ← `imagen+0x5187144 vk_create_graphics_pipeline` ← `imagen+0x5188130 vk_common_CreateGraphicsPipelines` ← `imagen+0x54e054 VulkanRenderTargetCache::GetTransferPipelines` ← `imagen+0x54fd74 / +0x5517f0 …PerformTransfersAndResolveClears` ← `imagen+0x51456c VulkanCommandProcessor::IssueDraw` ← `imagen+0x48db9c CommandProcessor::ExecutePacketType3Draw`。

  **结论：崩溃发生在 GPU 线程编译图形管线（由 transfer/resolve 绘制触发）时的 Mesa arena 分配路径上，根因是「宿主内存被池囤走、arena 分配返回 NULL」。**

**(d) run2：池全在用，但进程容量已到顶 → 缺块永远补不上。** 定格期 `pisc=1536/1536`、`corte` 一路涨（每 10 s 约 +4,700 次拒绝）、`neg2m` 涨到 43,047、`pendientes 160 MB (80 块)` 恒定不动、`proceso 3185/3189 MB` 顶格。这些 2 MB 块走池只差一条条目、走页级要 512 条（且页级已撞 `6144` 条目天花板、`techo de entradas` ×16），**没有第二份宿主内存，谁也接不住**。渲染侧的 74 条 upload buffer 失败正是同一件事在 GPU 侧的投影。

**(e) run1 与 run2 的差别只是池用到了哪里，机制完全相同。** run1 池的闲置部分饿死了 Mesa（崩溃）；run2 池被工作集占满后，进程再无余量给还要落位的块（定格）。**两个现象是同一个「池在 init 一次性囤走 1,536 MB」的下游后果。**

**(f) 缺口是硬账，且本轮已从「条目」移到「进程容量」。** 80 个块 × 2 MB = 160 MB；页级通路被 `6144` 条目天花板锁死；`limite de mapeo` 读不到（rc=0x0000E401）。**唯一还能拿出的内存，就是池里闲置的那部分。**

### 4. 与 fix34 批的差别

| 维度 | fix34 批 | fix35 批（本批） |
| --- | --- | --- |
| 池是否取到板 | 一块都没有（`pisc=0/0`） | **16 块 96 MB，填满 1,536 MB** |
| 阶梯语义 | 首走梯即 `break` 跳出（小阶也不试） | **`continue` 逐级下探**（`saltadas 0 / intentadas 4`） |
| 池拒绝总数 `corte` | 47（被等待闸门锁死） | **129,187（每缺页都问池）** |
| 未落位块 `pendientes` | 596 MB (298 块) | **160 MB (80 块)** |
| 新现象 | 无崩溃 | **run1：前端第二个 logo 处 NULL 崩溃（新）** |
| 定格位置 | 880 MB 块（`46F90000+0x37080000`） | **同位置、逐字相同（`fix14 alloc` 计数 102）** |
| 版本指纹 `[fix35]` | 0 | 16 |

---

## 二、fix36 的改动（2 个源码文件；构建配置未动）

### 1. `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`（3,074 → **3,120 行** / 145,435 → **148,457 B**，纯 CRLF、无 BOM、无孤立 LF，SHA-256 `5609022639ed7e2cf127f2a7a7cf3dfcc6e43c2fe88e58263a53c18e001e4eee`）

1. **新增闲置上限常量**（第 818 行）：
   `constexpr size_t kPiscinaInactivaMax = 4 * kPiscinaLosa;` —— **384 MB**。含义：池最多允许持有 4 块 96 MB 没人用的切片；再多就不再取板。
2. **`PiscinaReservar()` 的 `for(;;)` 顶部加「闲置检查」**（第 1003–1019 行）：
   - 计算 `tomado = g_piscina_bytes`、`en_uso = g_piscina_en_uso`、`inactiva = tomado > en_uso ? tomado - en_uso : 0`；
   - 若 `inactiva + kPiscinaLosa > kPiscinaInactivaMax`（即再加上一块就会超过 384 MB 上限），打印一次告警后 **`return`**（不再取板），把内存留给 emulator；
   - 告警（源码 `[fix36]` 运行时标记**恰好 1 处**，打印上限 8 条）：
     `"[fix36] piscina: no reservo mas, inactiva %zu MB de %zu MB tomados y %zu MB en uso; el arena queda para el emulador (uordblks %zu MB, libre %zu MB en %zu trozos)\n"`
   - **效果**：init 那次「一口气填到 `kPiscinaTope`（1600 MB）」不再发生——池只填到「工作集 + 384 MB 闲置」；需要更多时由 `PiscinaTomar()` 的需求路径**按需**补板（`PiscinaTomar` 本来就会在消费完当前板前请求下一块，所以按需取板仍然发生在 arena 还有空间的时刻）。
   - 新增计数器 `uint32_t g_piscina_avisos_inactiva = 0;`（第 862 行，落 bss）。
3. **`MaterializarBloque()` 里补一次重试**（第 1507–1510 行）：`PiscinaReservar();` 之后紧接
   `if (!backing) { backing = PiscinaTomar(); uso_2m = (backing != nullptr); }`
   —— 修掉「刚取到的新板给了下一个块、本块白落页级 512 条目路径」的浪费：`PiscinaReservar()` 刚补的板优先给当前这个块。
4. **未改动的常量**：`kPiscinaLosa=96 MB`、`kPiscinaLosasMax=24`、`kPiscinaTope=1600 MB`、`kPiscinaPisoLibre=48 MB`、`kPiscinaLosaMin=2 MB`、`kReusoMax=1024`、`kBloquesMargen=8`、阶梯 `{96, 16, 4, 2} MB`。

### 2. `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp`（1,746 → **1,748 行** / 78,663 → **78,751 B**，纯 CRLF、无 BOM、无孤立 LF，SHA-256 `6f291c40c46f044bc94aec5bb98e502990d1cc731680da5e3a350db569ef2e66`）

心跳格式串在 `pisc=%zu/%zu MB` 之后新增 `inactiva=%zu MB`：
`"… pisc=%zu/%zu MB inactiva=%zu MB corte=%zu arena u=… f=… t=…K n=… hilos=… …"`，参数行在 `RexGmPiscinaEnUso() >> 20,` 之后插 `(RexGmPiscinaBytes() - RexGmPiscinaEnUso()) >> 20,`。
**判读价值：`inactiva=` 一眼看出池此刻囤了多少闲置内存——这正是本轮两条现象的公共变量。**

### 3. 构建配置（本轮未动，但必须沿用）

`T3DOTM_LTO=ON` + `-flto=1`（最终 NRO 链接必须走 LTO）；两源码文件保持纯 CRLF、无 BOM、无孤立 LF；不改任何游戏行为（只有映射、记账、回收与日志）。

---

## 三、构建与产物校验（本轮实测记录）

- 命令：`bash scripts/build_and_verify.sh -j 4`（脚本默认 `LTO="-DT3DOTM_LTO=ON"`，三步走：CMake 重配置 → `cmake --build recomp/out/sw8 -j 4` → `python tools/verify_nro.py recomp/out/sw8/T3DOTM.nro`）。
- 单文件先验：两个 `.cpp` 各自 `ninja …obj -j 4` 通过，**无错误无警告**（obj 时间戳 06:21）。
- LTO 最终链接：`[1/1] Linking CXX executable T3DOTM; Packaging T3DOTM.nro`；`lto-wrapper.exe: warning: using serial compilation of 128 LTRANS jobs` 与 `ld.exe: warning: creating DT_TEXTREL in a PIE` 是历史各批的预期行为。
- 产物：`recomp/out/sw8/T3DOTM.nro`
  - 体积 **109,447,182 B（0x686080e）** —— 与 fix33 / fix34 / fix35 **同体积**（本工程自 fix33 起体积不再随改动变化，**切勿用体积判版本**）。
  - SHA-256 **`a271aa085cd2d42edfe105894670639c17918e1eb773cf5bed81e3433361e94a`**。
- `verify_nro.py` 输出（**Verdict: PASS**）：

```
File size: 0x686080e (109447182 bytes)
NRO header size: 0x6838000 (109281280 bytes)
Flags: 0x0
Segments:
  [0] text  : file_off=0x00000000, mem_size=0x055cd000 (89968640 bytes) [OK]
  [1] rodata: file_off=0x055cd000, mem_size=0x0056a000 (5677056 bytes) [OK]
  [2] data  : file_off=0x05b37000, mem_size=0x00d01000 (13635584 bytes) [OK]
Total mapped memory (text+ro+data): 0x6838000 (109281280 bytes = 104.2 MiB)
MOD0 bss: 0x634b98 (6507416 bytes = 6.2 MiB)
```

  - MOD0 bss 与 fix35 同为 `0x634b98`（本轮新增的 `g_piscina_avisos_inactiva` 只占 4 字节，落进既有对齐空隙，bss 页不变）。
- **产物字符串核对**：`[fix36] piscina: no reservo mas` **×1**、`pisc=%zu/%zu MB inactiva=%zu MB` 心跳格式 **×1**；既有仪表 `[fix31] piscina: losa`、`piscina: compactada`、`[fix33] piscina` / `por paginas`、`[fix35] piscina: sin losa nueva` 齐备。
- **产物符号核对**：`nm` 中 `g_piscina_avisos_inactiva` 落 bss（`0x68cb800`）；`PiscinaReservar()` / `PiscinaTomar()` 在位。

---

## 四、复测步骤（与前几轮相同，务必只换 NRO）

1. 只替换 SD 卡上的 `T3DOTM.nro`；`T3DOTM.toml` 以及其它文件**保持上一轮原样不动**。
2. 两条现象都请复现一次：**①从前端启动进到第二个 logo 动画**；**②从 Sphiara 进入**。若画面停住，请至少再等 25 秒再看日志（历史批次里定格判据与看门狗落盘需要这段时间）。
3. 取回这 6 份文件（文件名后缀会自增）：`rex_crash(*).log`、`rex_latido(*).log`、`rex_perfil(*).log`、`rex_stderr(*).log`、`T3DOTM_001(*).log`、`T3DOTM_002(*).log`。
4. 用 SHA-256 确证你跑的是本包：`T3DOTM.nro` = **第七节回退表**里的 fix36 值（体积判版本在本工程已失效）。

---

## 五、判读方法（本轮唯一重点：**池有没有停止囤积、闲置是否被压到 384 MB 以内、崩溃是否消失**）

| 判读键 | 期望（改动生效） | 若相反，说明 |
| --- | --- | --- |
| **先确版本**（放在最前） | `T3DOTM.nro` 的 SHA-256 等于第七节回退表里的 fix36 值；日志里出现 `[fix36] piscina: no reservo mas` 与心跳新字段 `inactiva=`。**注意 `grep [fix31]/[fix33]/[fix35]` 命中 ≠ 跑的是那一版**：fix36 有意保留历史全部旧字符串（让旧 README 的判读表继续对得上） | 一条 `[fix36]` 都没有 → 本批不是 fix36，先别按本节下结论 |
| **心跳 `inactiva=`（本轮第一优先）** | 应稳定 **≤ 384 MB**；`pisc=` 不再恒为 `1536/1536`，而应随工作集在 **~384–1,200 MB** 之间浮动。这正是本批要恢复的「池只持有工作集」 | `inactiva` 仍长期贴 1,388 MB（`pisc=1536/148` 那类）→ 闲置上限没进产物 |
| `[fix36] piscina: no reservo mas` 的出现 | init 后**不久**即出现（预期 ≤ 4–8 次，配额 8 条），随后靠需求路径按需补板 | 一次都不出现且 `pisc` 仍直冲 1,536 → 闲置检查没生效 |
| **前端崩溃（run1）是否消失** | 应**不再**出现 `rex_crash` 的 `imagen+0x54263d4` NULL 访存（`esr 0x92000005` / `far 0x4` / `x0 0`）；前端应越过第二个 logo | 仍在同地址崩溃 → emulator 仍缺宿主内存，需再降闲置上限（如 2×96=192 MB） |
| `pendientes … (…) bloques)` | 应从 160 MB (80 块) 大幅回落（池释放出的内存应让块落位） | 仍 160 MB / 80 块且 `proceso` 顶格 → 进程容量是真墙，需转「页级部分物化」或削减原始窗口收费 |
| 游戏日志停止点 | `T3DOTM_002` 是否**越过** `46F90000+0x37080000`（880 MB 块）；`fix14 alloc` 计数是否超过 102 | 仍停在同一行 → 装载处仍未过 |
| `corte=` 的增速 | 应仍为**十万量级**（fix34 的「安静型」冻结不应回来） | 掉回几十 → 又回到了闸门锁死 |
| 宿主账目 | `proceso …/… MB` 应**不再长期顶格** 3185/3189；若仍顶格且仍有 80 块 pending，请回传 | — |
| 帧率 | `fps=0.0` 是否消失；`lento=` 是否归零 | 仍 0.0 → 定格未解除（若池已收缩仍定格，则墙在进程容量/帧率侧） |

---

## 六、性能目标与超频策略（用户给定上限，与上一轮相同、未变）

目标仍是**无需超频在 Switch 上流畅 60 帧**；超频只是兜底，且只在画面不再定格之后才逐档启用：

- CPU ≤ 1785 MHz、GPU ≤ 921 MHz、内存 ≤ 1600 MHz，由 sys-clk 生效（不用 toml），一档一档试。
- **本批实测读数：`CPU 1785.0 MHz, GPU 921.6 MHz, memoria 1600.0 MHz`，SoC 47.4–48.9 °C。** 与 fix33 批起相同，**已在超频上限档上跑**。**定格/崩溃在最高档仍未解除 → 不是算力/带宽问题**（与「缺口是宿主内存而非速度」的判断一致）。
- **本轮仍未进入帧率阶段**：run1 在崩溃前只有约 18.9 fps（装载/前端期），run2 定格在 `fps=0.0`，没有可测的稳定游戏帧段。

---

## 七、放置方法与回退

- 放置：把包内 `T3DOTM.nro` 覆盖到 SD 卡上的 NRO 位置；`README_dispatch_fix36.md` 仅供阅读，不要放进 SD 卡。
- 回退指纹表（一律用 SHA-256；fix33 起体积恒为 109,447,182 B，体积已不能判版本）：

| 版本 | NRO SHA-256 | 体积 | 包名 |
| --- | --- | --- | --- |
| **fix36（本包）** | `a271aa085cd2d42edfe105894670639c17918e1eb773cf5bed81e3433361e94a` | 109,447,182 B | `T3DOTM_dispatch_fix36.zip` |
| fix35（上一批，可回退） | `e9af9252f9890ec7b1105acef52b4d3456b3d4547459876296f2597724d239b3` | 109,447,182 B | `T3DOTM_dispatch_fix35.zip` |
| fix34（可回退） | `d01a2630607cb9d4fffb70eb664d558ce3600b7f6b6d800c8f627fb9fbc396e7` | 109,447,182 B | `T3DOTM_dispatch_fix34.zip` |
| fix33（可回退） | `b5d1e05823b61b3f580868049a13aa88fecd49b2e781fe5a2b66fd61c1f3db3c` | 109,447,182 B | `T3DOTM_dispatch_fix33.zip` |
| fix32（可回退） | `7e8888189e8bee8879a2524a9a33cd25fdc89573d1e07b87846fd8b7119262a2` | 109,443,086 B | `T3DOTM_dispatch_fix32.zip` |

- 源码侧回退指纹（本轮改动：两个文件）：

| 文件 | 行数 / 大小 | SHA-256 |
| --- | --- | --- |
| `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`（**fix36**） | 3,120 行 / 148,457 B（纯 CRLF、无 BOM、无孤立 LF） | `5609022639ed7e2cf127f2a7a7cf3dfcc6e43c2fe88e58263a53c18e001e4eee` |
| `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`（fix35，可回退） | 3,074 行 / 145,435 B | `3d9dcd6a23e162bc3d2f76df0dd6104352842231774265151732dfbfd10b8cdb` |
| `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp`（**fix36**） | 1,748 行 / 78,751 B（纯 CRLF、无 BOM、无孤立 LF） | `6f291c40c46f044bc94aec5bb98e502990d1cc731680da5e3a350db569ef2e66` |
| `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp`（fix35，可回退） | 1,746 行 / 78,663 B | `5d9b0b6f6a89831c393ebbfb93261f26abdc22d7a3482854b0869300f3cec72f` |

- **不要动错文件**：实际参与编译的是 `repos/nfsmw-nx/sdk/src/…` 下的这两个 `.cpp`；工程里有同名的参考副本，改它没有任何效果。

---

## 八、仍未完成 / 待确认

1. **fix36 的净收益取决于一个 fix35 才第一次具备的前提**：池**能**填满（fix35 已证）。fix36 把「填满」改成「只持有工作集 + 384 MB 闲置」。若下一批 `inactiva` 稳定 ≤384 MB 且 `pendientes` 回落，则「池囤积」这条根因成立；若 `pendientes` 仍不降且 `proceso` 仍顶格，则进程容量是真墙，应转「页级部分物化」或「削减原始窗口收费」。
2. **4 板（384 MB）vs 2 板（192 MB）的闲置上限尚未定**：384 MB 是首值。若前端仍崩溃（run1 那个 NULL），说明 emulator 仍缺内存，下一步就降到 192 MB 再试。
3. **run1 崩溃是独立于定格的新回归**，但它与 run2 定格同源（池囤积），本轮按同一根因一并处理；下一批应能同时验证两条现象。
4. **`[fix27] 2 MB denegado` 的 4 个 off**（`0x59C00000 / 0x23200000 / 0x59E00000 / 0x23400000`）仍在对齐；池若因闲置上限而收缩，`neg2m` 增速是否变化值得一并观察。
5. **`limite de mapeo 0/0` 的根因已定但未解**：`svcGetInfo(InfoType_ResourceLimit)` rc=0x0000E401，「抬高内核限额」在当前启动器/环境下不可用。
6. **原始视图释放通路（fix30）已无余量**：`liberadas 126 vistas crudas en 8 eventos`、`crudas 0 MB`——这条通路已榨干，下一批别再指望它。
7. **T18 仍阻塞**：定格未解除前无法测稳定帧段帧率与 GPU fence 等待，超频兜底（CPU ≤1785 / GPU ≤921 / 内存 ≤1600 MHz）不动（且当前已在最高档，无更高可试）。