# Transformers3DOTM-NX

A native Nintendo Switch port of the Xbox360 version of Transformers: Dark of the Moon by static recompile with ReXGlue

> 本仓库为移植工程的**源码快照**（首次同步时间 2026-11-02，fix42 阶段）。技术路线：`rexglue-sdk` / `XenosRecomp` 静态重编译 + `mesa-switch` 图形后端，经 devkitPro + libnx 构建为 Switch NRO。

## 仓库内容与排除项

### 已纳入

| 路径 | 内容 |
| --- | --- |
| `recomp/` | 工程构建配置与自有源码：`CMakeLists.txt`、`CMakePresets.json`、`src/`（app 头、startup 标记、stdio 锁桩）、`templates/`、`assets/`、`overrides.toml`（重编译函数覆盖表）、`t3dotm_manifest.toml`（codegen 清单）、`codegen_t3dotm.sh` |
| `repos/nfsmw-nx/sdk/` | 移植层 SDK 源码（fix36–fix42 的落点：`src/core/guest_memory_switch.cpp`、`src/core/exception_handler_switch.cpp` 等）；已剔除 `out/` 与 `thirdparty/` |
| `tools/` | 移植与取证工具：`xex_analyze.py`、`xex_dump.py`、`xenia_live_unpack.py`、`verify_nro.py`、`pe_verify.py`、打补丁脚本、`xex_delta_patch/`、`xex_patcher_cli/`、`switch/`（链接脚本与 NRO rodata 补丁） |
| `scripts/` | 构建、打包与批量打补丁脚本（`build_and_verify.sh`、`build_switch.sh`、`package.sh`、`setup_update.sh`、`patch_fix39*.py` 等） |
| `README_dispatch_fix36.md` … `README_dispatch_fix42.md` | 各轮修复交付说明：实机日志实测读数、改动点行号与 SHA-256 指纹、复测步骤与判读表 |
| `README.md`（本文件）、`T3DOTM.toml`、`icon_T3.png` | 工程说明、构建配置与图标 |

### 未纳入（由 `.gitignore` 硬排除）

- **游戏原始素材与提取产物**：`game/`、`extracted/`、`patched/`、`patched_flat.bin`、`update/`（TU / DLC 数据 `default.xexp`、`TransGame`）
- **构建与代码生成产物**：`recomp/out/`、`recomp/generated/`（由 `codegen_t3dotm.sh` 从游戏 XEX 重新生成）、`repos/nfsmw-nx/sdk/out/`、`*.nro`、`*.elf`、`*.o`
- **vendored 第三方依赖**：`repos/nfsmw-nx/sdk/thirdparty/`（FFmpeg、catch2、cli11、dxc 等，可由上游还原）
- **交付压缩包与日志**：`*.zip`、`*.log`、`*.bak*`、`.dumate/`

> 行尾说明：仓库内文件按**逐字节原样**保存（`.gitattributes: * -text`，关闭行尾归一化），因此入库文件的 SHA-256 与工程内基线指纹一致；`repos/nfsmw-nx/sdk/src/core/*.cpp` 为 CRLF。

---
# T3DOTM — Transformers: Dark of the Moon（Xbox 360）→ Nintendo Switch

《变形金刚3：月黑之时》Xbox 360 版到 Nintendo Switch 的 ReXGlue 重编译移植工程。

- 运行平台：Nintendo Switch（自制系统，Homebrew Menu / 直接运行 NRO）
- 图形后端：Mesa NVK（Switch 上的 Vulkan 驱动，基于官方 NVN 路径）
- 内容组织：原版 game/ + TU 升级档（update/）+ **安装期融合 xex（patched/）** 兄弟目录、
  运行时层叠加载，不覆盖 base 文件
- 性能目标：不超频（仅官方 APM 性能档），稳定 60 FPS（口径见第 5 节）

## 1. 目录布局（SD 卡）

把以下内容放到 SD 卡的 `sdmc:/switch/Transformers3DOTM/` 下：

```
sdmc:/switch/Transformers3DOTM/
├── T3DOTM.nro          # 可执行文件（recomp/out/sw8/T3DOTM.nro）
├── game/               # 原版 ISO 提取的完整游戏文件（default.xex、TransGame/ 等）
├── update/             # TU 数据覆盖（可选；无 TU 数据时仅占位）
└── patched/            # 安装期融合的 TU 代码补丁（default.xex，由 setup_update.sh 生成）
```

- `game/`、`update/`、`patched/` 是**兄弟目录**，都不是彼此的父目录。
- 运行时层叠查找顺序为 **update/ > patched/ > DLC > game/**：
  - TU 数据覆盖（camera.lxb、tuning.lxb、text/、ui/ 等）由运行时 layered VFS 优先命中 `update/` 层；
  - TU 代码补丁**不需要在运行时应用**：在 PC 安装期已由 `setup_update.sh` 用
    `xex_patcher` 把 base `default.xex` 与 TU `default.xexp` **融合**为 `patched/default.xex`
    （已解密、已解压、TU 代码生效），运行时 `UserModule` 检测到 fused XEX
    （FILE_FORMAT_INFO 为 NONE/NONE）后跳过 sibling patch 查找。
  - **`game/` 原始文件全程只读，不被修改或覆盖**。
- 本工程 `recomp/T3DOTM_manifest.toml` 的 `game_root = "../game"`，codegen 时以
  `--update_data_root="../update"` 传入，使 manifest、codegen 与运行时都按同一布局工作。
- NRO 会自动发现同目录下的 `game/`、`update/`、`patched/`（见 `recomp/src/T3DOTM_app.h`
  `OnConfigurePaths`），SD 卡上目录名按上面布局摆放即可。

> 游戏数据（game/、update/）约 7 GB 级，不属于工程压缩包；从你已有的 Xbox 360 镜像/升级档提取。
> TU 容器用 `python tools/xenia_live_unpack.py` 解包到 `extracted/TU` 后，
> 执行 `bash scripts/setup_update.sh` 生成 `update/` 与 `patched/`。

## 2. 构建步骤

### 前提

- Windows（本工程在 Windows + Git Bash 下构建），或任意能运行 devkitPro 的环境
- devkitPro + devkitA64 + libnx（`DEVKITPRO` 环境变量有效；本机为 `C:\devkitPro`）
- CMake ≥ 3.25、Ninja、Python 3
- 依赖（已在 `repos/` 内就位）：
  - `repos/nfsmw-nx/` — Switch 版 rexglue-sdk 基座（layered VFS、输入映射、性能剖析等），
    `sdk/out/win-amd64/rexglue.exe` 为 host 端 codegen 工具
  - `repos/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch` — 构建好的 NVK Horizon SDK
- `recomp/generated/` 为 codegen 输出（首次需执行 codegen，见下）

### Codegen（首次 / TU 变更后）

```bash
bash recomp/codegen_t3dotm.sh    # 需要 rexglue.exe（脚本默认指向 repos/nfsmw-nx/...）
```

### 构建

```bash
bash scripts/build_switch.sh -j 8          # LTO 构建（60 帧性能目标）
bash scripts/build_switch.sh --no-lto -j 8 # 仅诊断/快速验证用
```

产物：`recomp/out/sw8/T3DOTM.nro`

说明：

- 默认启用 LTO（`-flto=2 -flto-partition=balanced`，链接按 `switch.specs` 带
  `--gc-sections`）。**不要用 `--no-lto` 作为常规构建**：其产物仅用于快速验证流程，
  不满足 60 帧性能目标。
- LTO 链接已修复：GCC LTO 大分区（127+ 分区）下，原 `DEFINE_REX_FUNC` 的 asm
  `.weak /.set` 别名符号会被分区裁掉，导致 `PPCFuncMappings` 引用悬空、链接报
  `undefined reference to 'sub_...'`。现改为 **thunk 方案**：`name` 为真实 IR 弱函数
  （`used, weak, noinline`），函数体为 `[[gnu::musttail]]` 尾调用 `__imp__name`，
  保证符号随 LTO 分区输出且可被 hook 覆盖（模板 `pch_h.inja` 与生成文件
  `T3DOTM_pch.h` 需同步修改）。修复后开关验证：已定义 `sub_*` 111,011 个、
  未定义 0 个、注册表（`T3DOTM_register.cpp`）全部命中。
- LTO 链接必须带 `-z notext`（`tools/switch/cmake/switch-devkitA64.cmake` 的
  `_SWITCH_LTO_DIAG` 选项默认开启）：LTO 全程序阶段产生的相对重定位会落到只读段
  （实测 596 个在 `.rodata`，来源为 GCC switch 跳转表 `CSWTCH.*` 与 SDK 内部
  iconv 编码表；`.text` 本身 0 个），严格 `-z text` 会报
  `read-only segment has dynamic relocations` 而链接失败。产物因此带
  `DT_TEXTREL` 标记：仅影响加载期写少量指针，不影响运行帧率；如追求无 textrel
  的严格产物，需进一步把这些表搬离只读段（当前不做，作为已知事项）。
- `T3DOTM_register.cpp`（约 11 万行、纯符号注册表、无热代码）单独以 `-O0` 编译
  （`recomp/CMakeLists.txt` 中 `set_property(... COMPILE_OPTIONS "-O0")`），
  避免 GCC 在 `-O3` 下对其花费数小时；不影响产物行为。
- 图标/标题/作者：`icon_T3.png`（可选，缺失时 elf2nro 不带图标）、
  `Transformers: Dark of the Moon`、`mzy1996`（见 `recomp/CMakeLists.txt` 的
  `rexglue_switch_add_nro`）。

## 3. 工程结构

| 路径 | 说明 |
|---|---|
| `recomp/` | 主工程：manifest（`T3DOTM_manifest.toml`）、codegen 脚本、应用代码（`src/`）、codegen 输出（`generated/`）、构建输出（`out/`） |
| `recomp/src/T3DOTM_app.h` | 应用钩子：路径自动发现（NRO 旁 game/、update/、patched/）、APM 性能档请求（GPU 460.8 MHz、内存 1331.2 MHz，官方 API 非超频） |
| `repos/nfsmw-nx/` | Switch 版 rexglue-sdk 基座（layered VFS 等；`user_module.cpp` 含 fused XEX 跳过运行时补丁检测） |
| `repos/mesa-switch/` | NVK Horizon SDK（已构建） |
| `scripts/` | build_switch.sh（构建）、setup_update.sh（TU 解包数据 + 融合 xex → update/、patched/）、package.sh（原地重建交付 zip） |
| `tools/` | 辅助工具（xenia_live_unpack.py、xex_analyze.py、pe_verify.py、xex_patcher_cli、xex_delta_patch、switch/cmake 工具链文件等） |
| `docs/` | 工程文档（XEX/TU 分析记录、性能验证方法等，按需生成） |

## 4. 当前状态

- [x] 输入盘点（ISO、TU、工具链）
- [x] 参考仓库与依赖调研（rexglue-sdk / nfsmw-nx / mesa-switch 已就位）
- [x] Switch 工程骨架（目录结构、CMake、工具链、layer 布局、update/ 运行时加载设计）
- [x] codegen（default.xex → C++；TU 已纳入 codegen）
- [x] TU 升级档解包与 update/ + patched/ 落地（安装期融合流程实跑通过；
      `patched/default.xex` 28,393,472 B，XEX2/PE 校验通过）
- [x] 渲染/性能接入与构建（NVK 链接通过；NRO 已产出）
- [x] LTO 链接修复（thunk 方案：`DEFINE_REX_FUNC` 改为 IR 弱函数 + musttail 尾调用；
      LTO NRO 已产出并通过链接校验，待实机验证崩溃与 60 帧）
- [x] 打包交付（README + 工程包 zip）
- [x] 实机启动崩溃定位（dispatch 线程 TLS/EH 问题）
- [x] 实机再次启动验证（TLS 修复生效：游戏进入 normal 模式渲染约 33 秒，
      新崩溃定位为 guest 地址 0x8328B5C8 未注册）
- [x] 第 18 轮崩溃修复（0x8328B5C8 this-adjustment thunk 注册进 overrides.toml，
      codegen 重建后 NRO 已产出并通过 verify_nro.py）
- [x] 第 19 轮实机复测（确认已越过 0x8328B5C8 间接分派点；但停止于第一幕后的
      新崩溃 `0x8255B3D8`，见下方 2026-10-19 batch 15 记录）
- [x] 第 19 轮崩溃修复（0x8255B3D8 vtable 间接分派入口注册进 overrides.toml，
      codegen 重建后 NRO 已产出并通过 verify_nro.py）
- [x] 第 20 轮实机复测（确认越过 0x8255B3D8 后，统一停止于 0x830EA420 未注册；
      根因为 overrides 误把函数体中段声明为 stub 入口，导致该地址未注册；
      已修正 overrides 并重建 codegen + NRO，verify_nro.py PASS）
- [x] 第 21 轮实机复测（确认越过 0x830EA420 后，第一幕后触发新崩溃 `0x833CF038`，见下方 2026-10-19 batch 19 记录）
- [x] 第 22 轮实机复测与修复（确认越过 0x833CF038 后，统一停止于新崩溃 `0x8334D6A0`；已按 batch 20 注册并重建，见下方 2026-10-19 batch 20 记录）
- [x] 第 23 轮实机复测与修复（确认越过 0x8334D6A0 后，统一停止于新崩溃 `0x832F7068`；已按 batch 21 注册并重建，见下方 2026-10-19 batch 21 记录）
- [x] 第 24 轮实机复测与修复（确认越过 0x832F7068 后，统一停止于新崩溃 `0x8334D5C8`；已按 batch 22 注册并重建，见下方 2026-10-19 batch 22 记录）
- [x] 第 25 轮实机复测与修复（确认越过 0x8334D5C8 后，统一停止于新崩溃 `0x833D6FC8`；已按 batch 23 注册并重建，见下方 2026-10-19 batch 23 记录）
- [x] 第 26 轮崩溃修复（batch 24：0x832F5648 大函数内部 48 处 unresolved branch，
      删除 0x832F5810/0x832F5884 内嵌声明让 Discover 扩展大函数，见下方
      2026-10-20 batch 24 记录；NRO 已验证通过构建）
- [x] 视频播放逻辑调研（确认 T3DOTM 使用 Bink (.bik) 视频而非 WMV；参考
      nfsmw-nx WMV+FFmpeg 与 UnleashedRecomp-NX 渲染补丁的结论与可选方案，
      见下方 2026-10-20 视频播放调研记录）
- [x] 第 27/28 轮日志分析（第 27 轮 FATAL 0x833D6FC8 为旧构建复现，当前
      batch 24 已注册；第 28 轮访问违例读 guest 0x5660 为新崩溃类型，非视频
      路径，需 batch 24 实机复测确认是否复现，见下方 2026-10-20 记录）
- [x] 视频加载修复（在 update/TransGame/Movies/ 补齐游戏查找的语言变体
      .bik 别名，消除启动/加载画面 NtCreateFile FAILED，让开场与加载画面
      视频可真正加载、可实测，见下方 2026-10-20 记录）
- [x] 第 29 轮实机复测与修复（batch 25：确认越过 0x82E869A0 后，统一停止于
      新崩溃 `0x827DC208`；已按 batch 26 注册并重建，见下方 2026-10-21
      batch 26 记录 / README_dispatch_fix2.md）

### 崩溃修复记录（2026-10-18）

**现象**：`modo: normal` 启动后 Data Abort，PC `imagen+0x556a48c`，
fault 地址 `0xffffffcf2c6ae551`（内核无效区域）。崩溃线程 "Kernel Dispatch"（`dispatch_thread_`）。

**根因**：`std::condition_variable_any::wait` 内部调用 `std::uncaught_exception()`，
后者走 `__cxa_get_globals()` → `pthread_getspecific()` 读取 EH TLS slot。在 libnx 的
TLS 实现中，key 分配上限为 0..26，且仅初始化**调用线程**的 slot；dispatch 线程在该 key
分配后才创建，其 slot 为垃圾值 → 访问 `0xffffffcf2c6ae551` 崩溃。

**修复**：将 `kernel_state.h:396` 的 `std::condition_variable_any dispatch_cond_`
替换为普通 `std::condition_variable dispatch_cond_` + 专用 `std::mutex dispatch_mutex_`；
同步修改 `kernel_state.cpp:650-679`（消费者，锁改用 `unique_lock<std::mutex>`）与
`1223-1246`（生产者，`push_back` 包在 `lock_guard<std::mutex>` 内，`notify_all` 放锁外）。
反汇编已确认普通 `std::condition_variable::wait` 直接 `b pthread_cond_wait`，不调用
`uncaught_exception()`，完全绕开 EH TLS 读取。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-18 23:58，非 LTO 诊断构建，验证通过）。
**实测结论**：TLS 修复生效，游戏进入 `modo: normal`、20 draws、渲染约 33 秒后触发
新一轮崩溃 `[FATAL] Call to invalid or unregistered function at guest address 0x8328B5C8`。

### 崩溃修复记录（2026-10-19）

**现象**：`[FATAL] Call to invalid or unregistered function at guest address 0x8328B5C8`
（第 18 轮实机日志，游戏已进入 normal 模式渲染约 33 秒）。

**根因**：0x8328B5C8 是一段 12 字节的 this-adjustment thunk
（`addi r5,r5,-16; addi r3,r3,-16; b 0x82F2FA70`），位于已注册函数
`sub_8328B5C0`（结束于 0xC8）与 `sub_8328B5D8`（起始于 0xD8）之间，
codegen 未将其识别为独立函数。游戏经 UE3 虚表间接分派（bctr）调用该地址时，
运行时派发表中无对应项 → FATAL。同类问题此前以 overrides.toml batch 1-13 的方式
逐轮修复。

**修复**：在 `recomp/overrides.toml` 追加 batch 14：
`"0x8328B5C8" = { size = 0x10 }`（覆盖 12 字节 thunk + padding）。
重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen 生成
`DEFINE_REX_FUNC(sub_8328B5C8)`（`T3DOTM_recomp.58.cpp`）并在注册表登记
`SetFunction(0x8328B5C8, sub_8328B5C8)`。其真实尾调用目标 `sub_82F2FA70`
本就已注册，无新增未解析跳转。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 01:37，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x8328B5C8 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 16）

**现象**：两次独立实机复测均越过上一轮修复的 0x8255B3D8，但统一停止于第一幕
之后的 `[FATAL] Call to invalid or unregistered function at guest address 0x830EA420`
（记录于 `T3DOTM_001(20)`、`T3DOTM_002(18)` 日志；Atmosphere crash report 均为 `Type: User Break`，
PC = `T3DOTM + 0x5559e34`，即 recomp 运行时 REX_FATAL 断点，属预期行为，
确认两起崩溃同源）。

**根因**：0x830EA420 位于函数 `sub_830EA3B0`（`mflr r12`/`stwu` 入口）体中段。
此前 overrides 错误地把 `0x830EA410`（`rlwinm` 指令）声明为独立 stub 入口
（`"0x830EA410" = { size = 0x18 }`），导致 codegen 仅注册到 0x830EA410 为止，
0x830EA420 落在未注册区域。运行时经间接分派（bctr）调用 0x830EA420 时
派发表无对应项 → FATAL。

同时，`0x830EA378`（`lwz r11,80(r31)`）位于 `sub_830EA368` 体中段，
被 `0x83269FB8` 处的 `b 0x830EA378` 无条件尾调用引用；此前误删该声明
codegen Validate 报 `UnresolvedCall` 中止，已加回。

**修复**：
- 删除 `recomp/overrides.toml` 中的错误声明 `"0x830EA410" = { size = 0x18 }`
  （该地址为函数体中间指令，非入口）。
- 保留 `"0x830EA378" = { size = 0x40 }`（被 `0x83269FB8` 的 `b` 跳转引用，
  需作为独立函数入口注册）。
- 重新运行 `bash recomp/codegen_t3dotm.sh`（已移除错误的 `--update_data_root`
  参数，确认 codegen 以 `patched/default.xex` 为唯一入口）。codegen 生成
  `DEFINE_REX_FUNC(sub_830EA420)`（`T3DOTM_recomp.77.cpp`）并在注册表登记
  `SetFunction(0x830EA420, sub_830EA420)`，Validate 通过（"6 written, 157 unchanged"）。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 05:30，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x830EA420 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 19）

**现象**：第 21 轮实机复测越过 0x830EA420 后，第一幕之后触发
`[FATAL] Call to invalid or unregistered function at guest address 0x833CF038`
（记录于 `T3DOTM_001(23)`、`T3DOTM_002(21)` 日志；Atmosphere crash report 均为
`Type: User Break`，PC = `T3DOTM + 0x5559870`，确认同源）。

**根因**：0x833CF038 是一段 this-adjustment thunk 入口，位于已注册函数
`sub_833CF030`（结束于 0x833CF038，8 字节：`mr r3,r4; b 0x833ce938`）与
`sub_833CF04C`（起始于 0x833CF04C，12 字节：`mr r3,r4; lwz r4,4(r5); b 0x833cef78`）
之间的 20 字节间隙中，codegen 未将其识别为独立函数。运行时经间接分派调用该地址时
派发表无对应项 → FATAL。同类问题此前以 overrides.toml batch 8（0x82AF9408）的
小 thunk 模式修复。

**修复**：在 `recomp/overrides.toml` 追加 batch 19：
`"0x833CF038" = { size = 0x14 }`（覆盖 20 字节间隙至下一函数入口）。
重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen 生成
`DEFINE_REX_FUNC(sub_833CF038)`（`T3DOTM_recomp.63.cpp`）并在注册表登记
`SetFunction(0x833CF038, sub_833CF038)`。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 18:53，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x833CF038 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 20）

**现象**：第 22 轮实机复测确认越过上一轮修复的 0x833CF038（该地址不再出现在
崩溃报告中），但统一停止于新的 `[FATAL] Call to invalid or unregistered function
at guest address 0x8334D6A0`（记录于 `T3DOTM_001(24)`、`T3DOTM_002(22)` 日志；
Atmosphere crash report 均为 `Type: User Break`，PC = `T3DOTM + 0x5559770`，
LR = `T3DOTM + 0x555506c`，确认同源）。

**根因**：0x8334D6A0 位于已注册函数 `sub_8334D670`（结束于 0x8334D670+体长）与
`sub_8334D6C0`（起始于 0x8334D6C0）之间的 0x50 字节间隙中，codegen 未将其识别
为独立函数。运行时经 UE3 虚表间接分派（bctr）调用该地址时派发表无对应项 → FATAL。
同类问题与此前 overrides.toml batch 8/14/19 的小 thunk/间隙函数模式一致。

**修复**：在 `recomp/overrides.toml` 追加 batch 20：
`"0x8334D6A0" = { size = 0x20 }`（覆盖至下一已注册函数 `sub_8334D6C0` 之前）。
重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen 生成对应
`DEFINE_REX_FUNC(sub_8334D6A0)`（`T3DOTM_recomp.47.cpp`）并在注册表登记
`SetFunction(0x8334D6A0, sub_8334D6A0)`（"5 written, 158 unchanged"，Validate 通过）。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 20:16，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x8334D6A0 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 21）

**现象**：第 23 轮实机复测确认越过上一轮修复的 0x8334D6A0（该地址不再出现在
崩溃报告中），但统一停止于新的 `[FATAL] Call to invalid or unregistered function
at guest address 0x832F7068`（记录于 `T3DOTM_001(25)`、`T3DOTM_002(23)` 日志；
Atmosphere crash report 均为 `Type: User Break`，PC = 宿主重编译地址
`T3DOTM + 0x55597f4`，确认同源）。

**根因**：0x832F7068 恰好位于已注册函数 `sub_832F7058`
（经典虚表 dispatch thunk：`lwz r11,0(r3); lwz r11,148(r11); mtctr r11; bctr`，
结束于 0x832F7068）与下一已注册函数 `sub_832F7070`（起始于 0x832F7070）
之间的 0x08 字节间隙中，codegen 未将其识别为独立函数。运行时经 UE3 虚表
间接分派（bctr）调用该地址时派发表无对应项 → FATAL。同类问题与此前
overrides.toml batch 8/14/19/20 的小 thunk/间隙函数模式一致。

**修复**：在 `recomp/overrides.toml` 追加 batch 21：
`"0x832F7068" = { size = 0x08 }`（覆盖 8 字节间隙至下一已注册函数
`sub_832F7070` 之前；不可加大，否则与 sub_832F7070 重叠）。
重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen 生成对应
`DEFINE_REX_FUNC(sub_832F7068)`（`T3DOTM_recomp.42.cpp`）并在注册表登记
`SetFunction(0x832F7068, sub_832F7068)`（"5 written, 158 unchanged"，Validate 通过）。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 21:39，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x832F7068 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 22）

**现象**：第 24 轮实机复测确认越过上一轮修复的 0x832F7068（该地址不再出现在
崩溃报告中），但统一停止于新的 `[FATAL] Call to invalid or unregistered function
at guest address 0x8334D5C8`（记录于 `T3DOTM_001(26)`、`T3DOTM_002(24)` 日志；
Atmosphere crash report 均为 `Type: User Break`，PC = 宿主重编译地址
`T3DOTM + 0x55597f4`，确认同源）。

**根因**：0x8334D5C8 恰好位于已注册函数 `sub_8334D5B0`
（经典虚表 dispatch thunk：`lwz r11,28(r3); lwz r3,12(r11); lwz r11,0(r3);
lwz r11,40(r11); mtctr r11; bctr`，结束于 0x8334D5C8）与下一已注册函数
`sub_8334D5E0`（起始于 0x8334D5E0）之间的 0x18 字节间隙中，codegen
未将其识别为独立函数。运行时经 UE3 虚表间接分派（bctr）调用该地址时
派发表无对应项 → FATAL。同类问题与此前 overrides.toml batch 8/14/19/20/21 的
小 thunk/间隙函数模式一致。

**修复**：在 `recomp/overrides.toml` 追加 batch 22：
`"0x8334D5C8" = { size = 0x18 }`（覆盖 24 字节间隙至下一已注册函数
`sub_8334D5E0` 之前）。重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen
生成对应 `DEFINE_REX_FUNC(sub_8334D5C8)`（`T3DOTM_recomp.45.cpp`）并在注册表登记
`SetFunction(0x8334D5C8, sub_8334D5C8)`。Validate 通过。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 23:44，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x8334D5C8 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 23）

**现象**：第 25 轮实机复测确认越过上一轮修复的 0x8334D5C8（该地址不再出现在
崩溃报告中），但统一停止于新的 `[FATAL] Call to invalid or unregistered function
at guest address 0x833D6FC8`（记录于 `T3DOTM_001(27)`、`T3DOTM_002(25)` 日志；
Atmosphere crash report 均为 `Type: User Break`，PC = 宿主重编译地址
`T3DOTM + 0x55597f4`，确认同源）。

**根因**：0x833D6FC8 恰好位于已注册函数 `sub_833D6FB0`
（经典虚表 dispatch thunk，结束于 0x833D6FC8）与下一已注册函数
`sub_833D6FE8`（起始于 0x833D6FE8）之间的 0x20 字节间隙中，codegen
未将其识别为独立函数。运行时经 UE3 虚表间接分派（bctr）调用该地址时
派发表无对应项 → FATAL。同类问题与此前 overrides.toml batch 8/14/19/20/21/22 的
小 thunk/间隙函数模式一致。

**修复**：在 `recomp/overrides.toml` 追加 batch 23：
`"0x833D6FC8" = { size = 0x20 }`（覆盖 32 字节间隙至下一已注册函数
`sub_833D6FE8` 之前）。重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen
生成对应 `DEFINE_REX_FUNC(sub_833D6FC8)`（`T3DOTM_recomp.58.cpp`）并在注册表登记
`SetFunction(0x833D6FC8, sub_833D6FC8)`。Validate 通过。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-20 00:38，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x833D6FC8 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-19，batch 15）

**现象**：两次独立实机复测均越过上一轮修复的 0x8328B5C8，但统一停止于第一幕
之后的 `[FATAL] Call to invalid or unregistered function at guest address 0x8255B3D8`
（记录于 `T3DOTM_001/002` 日志；Atmosphere crash report 均为 `Type: User Break`，
PC = `T3DOTM + 0x5559e34`，即 recomp 运行时 REX_FATAL 断点，属预期行为，
确认两起崩溃同源）。

**根因**：0x8255B3D8 是完整函数入口（`mr r4,r3; cmpwi r3,0; beqlr; lis/lwz;
lwz r10,0(r3); lwz r9,0x14(r10); mtctr r9; bctr; blr`，空指针保护 + vtable
间接分派模式），位于已注册 `sub_8255B388`（结束于 ~0x8255B3A8，bctr 尾部）
与 `sub_8255B400` 之间，codegen 未识别为独立函数。游戏经 UE3 虚表间接分派
（bctr）调用该地址时派发表无对应项 → FATAL。

**修复**：在 `recomp/overrides.toml` 追加 batch 15：
`"0x8255B3D8" = { size = 0x28 }`（覆盖该间接分派尾部，到 `0x8255B400`）。
重新运行 `bash recomp/codegen_t3dotm.sh` 后，codegen 生成
`DEFINE_REX_FUNC(sub_8255B3D8)` 并在注册表登记
`SetFunction(0x8255B3D8, sub_8255B3D8)`。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-19 02:53，非 LTO 诊断构建，
verify_nro.py Verdict: PASS）。
**待验证**：实机复测，确认越过 0x8255B3D8 间接分派点后继续推进；若出现新的
未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 崩溃修复记录（2026-10-20，batch 24）

**现象**：第 26 轮 7 份日志（`T3DOTM_002(26).log` 等）确认越过上一轮修复的
0x833D6FC8（该地址不再出现在崩溃报告中），但统一停止于 0x832F5648 大函数内部
的 48 处 unresolved branch（首爆点为 `FATAL: Unresolved branch from 0x832F5838
to 0x832F5840`，另有 0x832F583C/0x832F5884/0x832F5888/0x832F5890/0x832F59C0
/0x832F58D8 等内部目标；Atmosphere crash report 均为 `Type: User Break`，
PC = 宿主重编译地址 REX_FATAL 断点，确认同源）。

**根因**：0x832F5648 是一个从未声明 size 的大函数，codegen 未把它当作整体
覆盖，导致函数内部多处分支目标（0x832F5xxx）被当作未知地址，生成代码残留
48 处 `REX_FATAL("Unresolved branch from ...")`。此前在 overrides.toml 中
保留的内嵌声明 `0x832F5810`（`b 0x832F5884` 薄壳）与 `0x832F5884` 会与
大函数 span 重叠或把父函数切碎，使 Discover 无法把内部目标解析为函数内
`loc_` 标签。

**修复**：从 `recomp/overrides.toml` **彻底删除** `"0x832F5810"` 与
`"0x832F5884"` 的声明（包括 batch 24 早期注释），仅保留
`"0x832F5648" = { }` 空声明，让 Discover 把它扩展到下一个已注册函数
`0x832F5CBC`（`{ size = 0x40 }` 保留）。codegen 后 0x832F5xxx 区域的
48 处 FATAL 全部消失：`sub_832F5648`（`T3DOTM_recomp.57.cpp` ~110164 行）
内部 0x832F5814/0x832F5828/0x832F583C/0x832F5840/0x832F5884/0x832F5888/
0x832F5890/0x832F59BC/0x832F59C0/0x832F59C8/0x832F5A1C/0x832F5A38/
0x832F5A3C/0x832F5A64 等全部解析为 `loc_` 标签；register.cpp 中
`SetFunction(0x832F5648, sub_832F5648)` 保留，0x832F5810/0x832F5884
被大函数吸收不再单独注册。

> 备注（本批 3 次无效尝试，避免回退）：
> 1. `0x832F5810 = { size = 0x20 }` + `0x832F5648 = { size = 0x674 }` →
>    manifest 报 `Overlapping boundaries` 失败；
> 2. 0x832F5810 空声明 + 0x832F5648 固定 size → 仍重叠失败（空声明 size=0
>    也参与重叠检测）；
> 3. 两者全空声明 → codegen 通过但 Discover 把 0x832F5810 切成独立节点，
>    生成代码残留 48 处 unresolved branch。必须整体删除内嵌声明。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-20 07:44，非 LTO 诊断构建，
109,557,774 B / 0x687B80E，verify_nro.py Verdict: PASS；`build_b24.log`）。
**已知遗留**：0x832F5810 被大函数吸收后不再是独立注册入口；静态 grep 无
`bl 0x832F5810` 直跳，但若运行中有 `bctr` 间接分派到该地址会导致
"Call to invalid or unregistered function" FATAL，需实机验证。
**待验证**：实机复测，确认越过 0x832F5648 大函数内部区域后继续推进；若出现
新的未注册间接分派地址，按同一流程追加 overrides.toml batch 并重建。

### 视频播放调研记录（2026-10-20）

**背景**：可能因视频播放导致的崩溃需要参考同为 XBOX360→Switch 移植项目的
视频播放逻辑。本地参考仓库：
- `repos/nfsmw-nx`（`https://github.com/StevensND/nfsmw-nx`，仅含 sdk 基座）
- `repos/unleashednx-tu-ref-chansey`（`https://github.com/ChanseyIsTheBest/UnleashedRecomp-NX`）
- `repos/masseffect-nx`（同 rexglue-sdk 架构、UE3 + Bink，含完整 app 层源码）

**结论：T3DOTM 使用 Bink (.bik) 视频。与 nfsmw-nx 的 WMV 方案格式不同，
WMV+FFmpeg 路径不能直接套用；最佳参考是 masseffect-nx 的“保留重编译
Bink 解码器、SDK 只适配渲染”路线。**

1. **nfsmw-nx（WMV）**：重编译的 XDK WMV 解码器在 Switch 上无法实时运行，
   因此在 IO 层（`sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp` 的
   `NfsmwAnotarApertura`/`NfsmwAnotarLectura`）记录游戏打开/读取的 `.wmv`
   路径，应用层再用 FFmpeg 重新打开并原生解码（配置启用
   `wmav2, wmv3, xmaframes` 解码器）。
2. **UnleashedRecomp-NX**：`UnleashedRecomp/patches/video_patches.cpp` 处理
   **渲染**侧（`CPlayMovieWrapper::CRender` 宽高比、运动模糊、材质修复），
   `gpu/video.cpp` 是 Switch 上 NVK 视频渲染器实现；不负责解码。
3. **masseffect-nx（最接近的可工作参考）**：`docs/audio-and-video.md`
   明确记载——Mass Effect 的电影同样是 Bink（`Movies/*.bik`），**由游戏
   自身重编译的播放器播放，SDK 不在视频路径上**（解码器随 XEX 重编译后在
   Switch 上可运行，开场 Logo 实测约 30 fps）。SDK 需要处理的是**渲染侧**
   纹理适配：Bink 输出的是 LINEAR、CPU 每帧重写的纹理平面
   （k_8 1280x720 / 640x360），`masseffect_native_draws.cpp` 为这类
   “无 coherency 事件的 CPU-written 视频平面”保持每帧完整哈希/上传，
   不参与纹理一致性跳过；`me_resolution.cpp` 明确 Bink 电影不做内部分辨率
   缩放。这套“保留原解码器 + 纹理上传适配”路线与 T3DOTM 同架构、同引擎，
   是最直接可复用的方案。
4. **T3DOTM 视频资产**：`game/TransGame/Movies/` 下 38 个文件（约 30 个
   `.bik`）：开场 Logo（Activision/Hasbro/HighMoon/Paramount）、启动画面
   （TF_InitialStartup + 多语言）、过场动画 FMV_* 与 L6_OptimusBroadcast、
   加载画面（LoadingScreenAlpha / TF_LoadingScreen + 多语言）。
   生成代码与注册表中**无** Bink/WMV/PlayMovie/ShowMovie/MoviePlayer/
   CutScene/Cinematic 等可识别符号（只有 `sub_xxxxxxxx` 与
   `VdSetDisplayMode`/`VdQueryVideoMode`/`XGetVideoMode` 等显示模式 API），
   Bink 解码器以无符号内部函数形式存在。

**可选方案（当前未实施，待实机确认视频播放是否真的崩溃后选择）**：
- **A（推荐，参照 masseffect-nx）保留重编译 Bink 解码器，补渲染侧适配**：
  视频解码大概率能随重编译代码在 Switch 上运行；若实测 Bink 纹理上传/绘制
  异常，参照 `masseffect_native_draws.cpp` 对每帧重写的线性视频平面做
  强制哈希/上传，并检查视频平面裁剪与视口（pixel-space draws）。
- **B. 跳过/禁用视频播放**：定位 Movies 播放入口（UE3 MoviePlayer/Bink
  调用）后直接跳过，最快规避解码性能与兼容性问题；需要先确认这些调用不在
  加载关键路径上（masseffect 的 Coalesced.ini 跳开场即为同类做法）。
- **C. 原生 Bink 解码替换**：参考 nfsmw-nx 思路，在 IO 层或应用层拦截
  `.bik` 打开/读取，用 FFmpeg 的 Bink 解码器在 Switch 端解码并回填帧；
  需引入 FFmpeg（当前 config 中 `BINK_DECODER=0` 未启用）并实现渲染桥接，
  工作量最大，仅在 A 路线失败时考虑。
- 当前第 26 轮 FATAL 均为普通未注册间接分派点（batch 24 已覆盖 0x832F5648
  区域），尚未出现明确指向 `.bik` 解码路径的崩溃证据；视频相关崩溃需带着
  视频播放场景（开场 Logo / 过场 / 加载画面）的具体日志再定位。

### 第 27/28 轮日志分析与视频加载修复（2026-10-20）

**第 27 轮（T3DOTM_001(27).log，2026-10-19 23:58）**：FATAL
`Call to invalid or unregistered function at guest address 0x833D6FC8`。
该地址已在 batch 23 注册（`overrides.toml` 第 166 行，
`SetFunction(0x833D6FC8, sub_833D6FC8)`，`T3DOTM_recomp.58.cpp`），且
第 26 轮（22:10）日志已越过该点——因此第 27 轮应为**旧构建复现**（SD 上
仍放置 batch 23 之前的 NRO），当前 batch 24 构建已覆盖，无需新增声明。

**第 28 轮（T3DOTM_001(28).log，2026-10-20 05:47）**：新崩溃类型
`Unhandled guest access violation: read of guest 0x00005660 (host
0x0000003275FD3660) on thread 0xF8000028`。这是 guest 代码读取接近 NULL 的
地址（0x5660 = NULL + 0x5660 类字段偏移），不是未注册函数 FATAL，overrides
无法覆盖。时间线：视频尝试加载（05:47:20-21，全部 FAILED）→ 流式加载
游戏资产（WL_* / TR_* / WEP_*，05:47:22-29）→ 15 秒 IO 统计（:35）→
崩溃（:37），**崩溃点与 .bik 视频解码路径无关**（视频文件根本未打开成功）。
根因大概率是前端加载路径上的空指针/堆状态问题，与第 26/27 轮同窗口
（UI 加载后 17-30 秒）内出现不同崩溃点的情况一致，疑似多线程竞态或堆
损坏；当前 batch 24 版本与 alias 修复是否消除该路径，需实机复测确认。
若复现，下一步需给 SDK 的 `Memory::AccessViolationCallback`
（`sdk/src/system/xmemory.cpp:558`）增加 guest PC 记录后再构建，才能定位
具体函数。

**视频加载修复（update/ 层 .bik 别名）**：实机日志显示游戏启动按
`TF_InitialStartup_360_INT.bik` → `_NA_INT` → `_INT` → `_360`（加载画面同
规则）的顺序查找开场/加载视频，但磁盘 `game/TransGame/Movies/` 只有基本名
与 DEU/ESN/FRA/ITA 变体，没有 INT/360 变体，导致每次启动约 250+ 次
`NtCreateFile FAILED`（游戏容错跳过，因此此前视频从未真正播放、也从未
进入 Bink 解码路径）。本批在 `update/TransGame/Movies/` 下新增 8 个别名
文件（TF_InitialStartup / TF_LoadingScreen × 360_INT / NA_INT / INT /
360，内容为基本名 .bik 的拷贝，md5 一致，共约 11.4 MB）。VFS 为分层
overlay（update/ → patched/ → DLC/ → game/，见
`sdk/src/system/runtime.cpp:297` SetupVfs），因此 update/ 层命中即可让
游戏真正加载并播放开场与加载画面视频：既消除 FAILED 日志噪音，也让
masseffect-nx 结论（重编译 Bink 解码器可在 Switch 运行，~30 fps）能在此
游戏上实测。若实机视频播放崩溃，按上文方案 A（渲染侧纹理适配）继续。
如需临时关闭视频（回到跳过状态），删除 SD 上 `update/TransGame/Movies/`
目录即可，不改动 game/ 数据。

## 7. 实机测试指引（NRO 复测）

当前 NRO（`recomp/out/sw8/T3DOTM.nro`，非 LTO 诊断构建，2026-10-20 07:44，
batch 24 修复版，109,557,774 B / 0x687B80E，verify_nro.py Verdict: PASS）
为第 26 轮崩溃修复后的版本；**第 27/28 轮日志均早于该构建**（分别为
23:58 前夜 / 05:47 今晨，而 NRO 于 07:44 构建），请务必用本包内 NRO 复测，
不要沿用 SD 上的旧 NRO。

- 把 `recomp/out/sw8/T3DOTM.nro` 放到 SD 卡 `sdmc:/switch/Transformers3DOTM/`，
  与 `game/`、`update/`、`patched/` 兄弟目录同放（见第 1 节布局）。
  `update/` 同时含本批新增的 `TransGame/Movies/` 别名 .bik（开场/加载视频
  修复）；若不希望视频播放，删除该目录即可。
- 经 Homebrew Menu 启动；预期：进入渲染（`modo: normal`）并越过 0x833CF038、
  0x8334D6A0、0x832F7068、0x8334D5C8、0x833D6FC8、0x832F5648 大函数区域。
- **视频播放（本批新增验证点）**：开场 Publisher Logo / `TF_InitialStartup`
  与加载画面 `TF_LoadingScreen` 将真正加载并播放（此前因文件名不匹配全部
  被跳过）。留意视频是否黑屏/崩溃/卡顿；masseffect-nx 同架构实测 Bink
  播放约 30 fps，若 T3DOTM 播放异常按"视频播放调研记录"方案 A 适配渲染。
- 若复现第 28 轮类崩溃（`Unhandled guest access violation: read of guest
  0x00005660`），请回报日志并备注运行到崩溃前看到的画面/阶段；若新崩溃为
  普通未注册函数 FATAL（`Call to invalid or unregistered function at guest
  address 0xXXXXXXXX`），把地址加入 `overrides.toml` 重建即可。
- 特别留意开场 Logo / 过场动画 / 加载画面等视频播放场景：若在这些时刻
  崩溃，记录崩溃场景并回报日志（见上方"视频播放调研记录"）。
- 复测回报内容：
  - 程序运行目录下新的 `rex_crash.log` / `rex_perfil.log` / `rex_stderr.log` /
    `T3DOTM_*.log`；
  - `sdmc:/atmosphere/crash_reports/` 与 `sdmc:/atmosphere/fatal_errors/`
    是否有新报告（faulting PC 可直接定根因）。
- 若新崩溃仍为 `Call to invalid or unregistered function at guest address 0xXXXXXXXX`，
  说明是新的未注册间接分派地址：把该地址加入 `recomp/overrides.toml`
  （参考 batch 14 注释的判定方法：patch xex 中该地址是否为真实 thunk/函数体），
  重新 `bash recomp/codegen_t3dotm.sh` + `bash scripts/build_and_verify.sh --no-lto -j 4`。

## 5. 性能验证口径

- **不超频**：仅通过官方 APM API 请求手持模式性能档（GPU 307.2 → 460.8 MHz，
  RAM 保持 1331.2 MHz，配置 0x92220008），不触碰 KIP / sysmodule；内存频率不拉高。
- **内部分辨率**：1280x720（guest 视频模式默认 `video_mode_width=1280`、
  `video_mode_height=720`），输出随实际显示分辨率（docked 1920x1080 / handheld 1280x720）。
- **验证方式**：实机帧时间统计，目标稳定 60 FPS —— 关注 >33.3 ms（掉到 30 FPS 档）与
  >50 ms（掉到 20 FPS 档）帧的占比，判定性能是否达标。

## 6. 已知注意事项

- TU 容器为 LIVE STFS 格式，工程自带 `tools/xenia_live_unpack.py` 可解包（xextool 不行）。
- XEX 解密参数需以本工程 `tools/xex_analyze.py` 对 `game/default.xex` 与 TU 容器的实测结果为准，
  不要沿用其他来源的预分析参数。
- xex_patcher 是 Windows 原生程序，由 `setup_update.sh` 经 `cygpath -w` 传入路径；
  header delta patch 的补丁长度必须用 `size - offsetof(Xex2OptDeltaPatchDescriptor, info)`
  （见 `tools/xex_patcher_cli/src/xex_patcher.cpp`），否则融合会失败。
- 融合后 xex 的特征：XEX2 magic；headerSize=0x4000；FILE_FORMAT_INFO 为 encryption/compression
  NONE/NONE；PE DOS header `MZ\x90\x00`、machine 0x1F2（PPC）、15 sections；
  `security.image_size + header_size == file_size`。

### 崩溃修复记录（2026-10-21，batch 26）

**现象**：第 29 轮 7 份日志（`T3DOTM_001(35).log` 17:58:42、
`T3DOTM_002(33).log` 18:02:20 + 2 份 Atmosphere 崩溃报告 + rex_crash +
rex_perfil + rex_stderr）确认越过 batch 25 的 0x82E869A0，但统一停止于
新地址 `0x827DC208`：两份 T3DOTM 日志均记录
`[FATAL] Call to invalid or unregistered function at guest address 0x827DC208`；
两份 Atmosphere 报告（Program ID `0540c4720b100000` / `05e2b6d3eb447000`）
Type `User Break`，PC = `T3DOTM + 0x547dea4`，ReturnAddress[01] =
`T3DOTM + 0x2a61c8`，与 rex_crash 栈顶 `imagen+0x2a61c8` 一致，断点 4 字节
（`svcBreak(0x0, addr, 4)` → runtime handle_fatal），确认仍为未注册间接
分派 FATAL、地址前进。

**根因**：`0x827DC208` 落在已注册 `sub_827DC1D8`（UE3 对象引用/计数 thunk，
结尾 bctr 间接分派）与 `sub_827DC2D8` 之间的空隙；patched/default.xex
中该地址是一条独立的 `blr`（4 字节空返回 stub，紧跟 sub_827DC1D8 尾部
之后），属 UE3 虚表/分发表中的空槽位入口（empty virtual method slot），
与 batch 23 注册的 0x833D6FC8 同类。运行时 bctr 间接分派落在此处即触发
FATAL。

**修复**：`recomp/overrides.toml` 末尾追加 batch 26 声明
`"0x827DC208" = { size = 0x04 }`，codegen 后生成
`DECLARE_REX_FUNC(sub_827DC208)` / `SetFunction(0x827DC208, sub_827DC208)` /
`DEFINE_REX_FUNC(sub_827DC208)`（blr 空返回）。

**产物**：`recomp/out/sw8/T3DOTM.nro`（2026-10-21 21:11，108,668,942 B，
verify_nro.py Verdict：PASS；aarch64-none-elf-nm 确认 sub_827DC208 /
sub_827DC1D8 / sub_827DC2D8 符号均已编入 ELF）。交付副本
`T3DOTM_dispatch_fix2.nro` + `README_dispatch_fix2.md`。
**已知观察项**：rex_stderr 中 `RexGmInit: Direction F map FAILED
(test_dst=0x1e92cf9000, rc=0x0000D401)` 为 code-memory 整段映射不可用、
退化为按页 fault-in 的既有 fallback，与本次 FATAL 无直接因果，列入性能
观察项。
**待验证**：实机复测确认越过 0x827DC208；若出现新的未注册间接分派地址，
按同一流程追加 overrides.toml batch 并重建。