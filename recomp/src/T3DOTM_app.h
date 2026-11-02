// T3DOTM - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/switch_apm.h>

REXCVAR_DECLARE(std::string, gpu_plugin);
REXCVAR_DECLARE(uint32_t, rexcrt_heap_size_mb);

class T3DOTMApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<T3DOTMApp>(new T3DOTMApp(ctx, "T3DOTM",
        PPCImageConfig));
  }

  // On the Switch, request the official handheld performance profile through
  // Horizon's apm API: GPU 307.2 -> 460.8 MHz while the memory stays at 1331.2
  // MHz (configuration 0x92220008, the same pair a commercial game uses). This
  // is NOT an overclock - no KIP or sysmodule is touched, it goes through the
  // official API - and it is the single biggest step toward a steady 60 FPS at
  // stock clocks in handheld mode (+50 % GPU). RAM is left untouched on
  // purpose: our measurements say bandwidth is not the limit, and 1600 MHz
  // would only cost battery and heat.
  void OnPostInitLogging() override {
#if REX_PLATFORM_SWITCH
    // Guest heap: default 256 MB is exhausted during level load (258 MB
    // backup / 242 MB mapped seen in logs before C0000017). 768 MB leaves
    // headroom for UE3 texture streaming and TU-expanded code without
    // touching __nx_heap_size (1 GB via hbloader).
    REXCVAR_SET(rexcrt_heap_size_mb, 768);
    // Default GPU plugin for Switch (statically linked xenos -> Vulkan NVK)
    REXCVAR_SET(gpu_plugin, "xenos");
    RexSwitchApmPedirGpuMhz(460);
    RexSwitchApmPermitirRam1600(0);
    RexSwitchApmAplicar();
#endif
  }

  // If --game_data_root was not given, use the extracted game folder next to
  // the executable: on the Switch the NRO lives in
  // sdmc:/switch/T3DOTM/, with the UnleashedRecomp layout of a game/ folder
  // beside it containing default.xex (update/ is picked up automatically by
  // the Runtime's layered VFS, sibling of game/ so it never touches the base
  // files).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;  // given on the command line; it takes precedence.
    }
    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }
    const auto extraida = carpeta / "game";
    if (std::filesystem::is_directory(extraida, ec)) {
      paths.game_data_root = extraida;
    }

    // TU (update/) is a sibling of game/, not a child of it. The layered VFS
    // resolves game:\ paths update > base, so point update_data_root at the
    // sibling that holds default.xexp.
    const auto update_dir = carpeta / "update";
    if (std::filesystem::is_directory(update_dir, ec)) {
      paths.update_data_root = update_dir;
    }

    // Install-time fused XEX (patched/) is also a sibling of game/. PathConfig
    // has no field for it; the Runtime reads the patched_data_root cvar and
    // would otherwise default to <game>/patched, so set it explicitly to the
    // sibling directory. Layered VFS order is update > patched > DLC > game.
    const auto patched_dir = carpeta / "patched";
    if (std::filesystem::is_directory(patched_dir, ec)) {
      REXCVAR_SET(patched_data_root, patched_dir.string());
    }
  }
};