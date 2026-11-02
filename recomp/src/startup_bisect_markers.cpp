// T3DOTM - startup bisect markers
//
// Diagnostic-only build: writes a marker file per constructor-priority
// reached, to pinpoint where the process silently dies before the crash
// hooks (priority 101) are installed.
//
// Verified .init_array order on the failing ELF:
//   90  _GLOBAL__sub_I.00090_globals_io.cc   (libstdc++ ios_base::Init)
//   98  init_have_lse_atomics / __init_cpu_features (libgcc / Rust compiler-builtins)
//   99+ std::sys::args::unix::imp::ARGV_INIT_ARRAY (Rust std)
//  101  RexSwitchInstallTerminate / RexSwitchCrashHooksInit
//
// Markers therefore go at 89 (before iostream), 91 (after iostream),
// 97 (before libgcc), 99 (after libgcc / before Rust argv+hooks).
// __appInit runs before constructors, so sdmc: is mounted and fopen works.

#include <cstdio>

extern "C" void T3DOTM_BisectMarker(const char* tag)
{
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/T3DOTM_mk_%s.txt", tag);
    FILE* f = fopen(path, "w");
    if (f) {
        fputs("ok\n", f);
        fclose(f);
    }
}

__attribute__((constructor(89))) static void mk_089() { T3DOTM_BisectMarker("089"); }
__attribute__((constructor(91))) static void mk_091() { T3DOTM_BisectMarker("091"); }
__attribute__((constructor(97))) static void mk_097() { T3DOTM_BisectMarker("097"); }
__attribute__((constructor(99))) static void mk_099() { T3DOTM_BisectMarker("099"); }