/**
 * @file        rex/os/switch/runtime_switch.cpp
 * @brief       libnx startup constants for the Switch executable
 *
 * This file is compiled directly into the final executable, not into a
 * library.
 *
 * Everything here replaces weak libnx symbols, and crt0 only references them
 * weakly. A weak reference does not pull an object out of a static library, so
 * if this ended up in rexcore.a the linker would silently ignore it and the
 * defaults would be used: no error, no warning, and the process starting in a
 * different state than expected. skate3recomp-nx solves it the same way, adding
 * it to the game target.
 *
 * The values are those of MarathonRecomp-NX, a finished Horizon port.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <cxxabi.h>

#include <switch.h>

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack,
                                  u64 pc);

namespace {

/*
 * An uncaught exception. devkitA64's libstdc++ handler writes nothing
 * (rex_stderr.log came out empty when SetupVfs let a filesystem_error
 * escape), so the type and what() are recorded here; for std::filesystem
 * errors what() includes the path. It ends with error 2345-0104.
 */
[[noreturn]] void RexSwitchTerminate() {
  char reason[768] = "std::terminate sin excepcion activa";
  if (std::exception_ptr current = std::current_exception()) {
    const std::type_info* type = abi::__cxa_current_exception_type();
    int status = -1;
    char* demangled = type ? abi::__cxa_demangle(type->name(), nullptr, nullptr, &status) : nullptr;
    const char* type_name = demangled ? demangled : (type ? type->name() : "?");
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s: %s", type_name, e.what());
    } catch (...) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s", type_name);
    }
    std::free(demangled);
  }
  RexSwitchCrashLog(reason, nullptr, reinterpret_cast<u64>(__builtin_frame_address(0)),
                    reinterpret_cast<u64>(__builtin_return_address(0)));
  diagAbortWithResult(MAKERESULT(Module_Libnx, 104));
}

__attribute__((constructor(101))) void RexSwitchInstallTerminate() {
  std::set_terminate(RexSwitchTerminate);
}

}  // namespace

extern "C" {

/* libnx internal weak symbols called by the default __appInit */
void __attribute__((weak)) __nx_win_init(void);
void __attribute__((weak)) userAppInit(void);
void __libnx_init_time(void);
void __libnx_init_cwd(void);

/*
 * Declared as an application instead of an applet. It goes with title takeover,
 * which was measured to be mandatory: in applet mode the process gets 400 MB, and
 * guest physical memory alone is 512.
 */
u32 __nx_applet_type = AppletType_Application;

/*
 * hbloader pre-inflates the heap to ~3.2 GB before the NRO entry point.
 * That consumes almost all LimitableResource_Memory mapping quota and leaves
 * almost nothing for svcMapProcessCodeMemory, which later fails with 0xCE01
 * (KERNELERROR_RESOURCE_EXHAUSTED). Shrinking to 1.5 GB releases ~1.7 GB of
 * mapping quota and physical memory for guest memory shadow mappings.
 *
 * Mesa/NVK allocations use Vulkan memory allocators, not libc heap, so they are
 * unaffected by this shrink. The remaining 1.5 GB covers host reserve,
 * 128 MB prealloc backing and on-demand commit backing.
 */
size_t __nx_heap_size = (1ull * 1024 * 1024 * 1024) + (512ull * 1024 * 1024);   /* 1.5 GB */

/*
 * libnx's __libnx_initheap already calls svcSetHeapSize(__nx_heap_size) before
 * __appInit (see _start -> __libnx_init -> __libnx_initheap), so shrinking the
 * heap to 1.5 GB happens there and frees the mapping quota for guest memory
 * shadow mappings. Calling svcSetHeapSize a second time here leaves libc's
 * arena pointers (__libnx_heap_base/limit) stale and every malloc/memalign
 * after __libc_init_array faults with a Data Abort (EC 0x24) at the old heap
 * base (measured v6.27: _memalign_r in RexGmCommit, far=heap_base+0xac048).
 * Do NOT re-add a svcSetHeapSize call in this override.
 *
 * The rest must match what libnx's default __appInit does: spin up the
 * services and mount the SD. Without sm/fs init the first mkdir()/fopen() in
 * a constructor faults inside newlib/fsdev with a data abort (measured on
 * v6.20..v6.24: RexSwitchCrashHooksInit+0x168, the bl mkdir at 0xd9404),
 * and every log write fails silently, so only the Atmosphere report came
 * out.
 */
Result __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }

    rc = appletInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    rc = hidInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    rc = timeInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    __libnx_init_time();

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        return rc;
    }

    __libnx_init_cwd();

    if (&__nx_win_init) __nx_win_init();
    if (&userAppInit) userAppInit();

    return 0;
}

/*
 * The exception handler stack is no longer declared here.
 *
 * libnx has a single one for the whole process, and with twenty-odd threads
 * faulting at the same time they overwrote each other. There are now eight
 * stack and dump sets in exception_handler_switch.cpp, which hands them out.
 */

}  // extern "C"
