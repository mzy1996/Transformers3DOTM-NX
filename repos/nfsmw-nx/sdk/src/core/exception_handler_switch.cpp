/**
 * @file        rex/core/exception_handler_switch.cpp
 * @brief       ExceptionHandler for Horizon, on top of a single __libnx_exception_handler
 *
 * How a fault arrives on Horizon, which is not how it does on POSIX
 *
 * All measured on the console (see docs/platform-notes.md), not assumed:
 *
 *  - There are no signals. A fault reaches __libnx_exception_handler, which libnx calls
 *    as a normal function on a stack of its own.
 *  - Returning from it kills the process. To continue, RexResumeFromException
 *    (resume_ctx.h) raises a trap exception and the entry below returns the corrected
 *    context with svcReturnFromException.
 *  - The ESR carries the WnR bit (read or write) but arrives with ISV = 0: it gives
 *    neither the register nor the size. Emulating an access requires decoding.
 *  - libnx accepts a single handler. What on POSIX are separate signal handlers (this
 *    one and the SEH one) must all go through here.
 *
 * The decision order, and why
 *
 *  0. Uncommitted guest physical memory -> 4 MB is committed and the access retried.
 *     Physical memory is no longer committed in full at startup: on Horizon those
 *     512 MB are counted once for each of the 360's five views and the kernel ends
 *     up refusing allocations (see xmemory.cpp).
 *
 *  1. Read from a watched guest page -> emulated through the shadow and never reaches
 *     the SDK. It has to be like this: on Switch a read-only page is unmapped, and if
 *     the fault reached the MMIO handler, it would see kReadOnly, conclude that another
 *     thread already removed the watch and retry the instruction... which would fault
 *     again, forever.
 *
 *  2. The handlers installed by the SDK (MMIO, write watching), in installation order,
 *     as on POSIX.
 *
 *     If one reports "handled" and leaves the PC where it was (that is, asks for a
 *     retry), whether the page ended up mapped is checked right then. If it did not,
 *     the retry would loop forever, so it is emulated through the shadow. This covers
 *     the case where remapping from the handler does not work on Horizon, without
 *     relying on heuristics.
 *
 *  3. SEH, if in use. See seh_switch.cpp.
 *
 *  4. Fatal: registers, address and stack to <NRO folder>/logs/rex/rex_crash.log, and
 *     error 2345-0102 on screen.
 *
 * Faults on several threads at once, and why they broke the game
 *
 * libnx has one exception stack and one dump, both global. With a single thread that
 * works; here it does not. The profiler counts hundreds or thousands of faults per
 * second spread over twenty-odd threads, so two coincide every so often, and then:
 *
 *  - the second thread writes its dump over the first one's, and the first resumes
 *    with the second one's registers (silent guest corruption, impossible to track);
 *  - both run on the same stack and overwrite each other's frames.
 *
 * Measured on the console while loading a race: two threads inside the handler with
 * their sp 0xD0 apart, one waiting for the guest memory lock and the other with a
 * mutex pointer turned into 1. The latter killed the process (report in
 * rex_crash.log, 2345-0102).
 *
 * Fix: REX_EXC_RANURAS stack + dump sets. The entry takes a free one with
 * ldaxr/stlxr, points the dump and sp at it, and stores the slot number in pad[0] of
 * the dump. On resume, the trap branch reads it from the copy and releases the slot:
 * at that point the thread no longer runs on that stack, so another one can take it
 * without overwriting anything. If none is free it waits; waiting is better than
 * corrupting.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include <rex/exception_handler.h>

#include <switch.h>

#include "guest_memory_switch.h"
#include "resume_ctx.h"
#include "rex_decode.h"

/* Provided by seh_switch.cpp. Returns true if it redirected execution. */
extern "C" bool RexSwitchSehHandleFault(ThreadExceptionDump* ctx);

/* Performance profiler counters (switch_perf.cpp). */
extern "C" void RexSwitchPerfCount(unsigned id);
// rexglue log folder, ending in '/' (switch_crash_hooks.c).
extern "C" const char* RexSwitchLogDir(void);

/* fix14 diagnostics: guest page table (xmemory.cpp) and window bookkeeping
 * (guest_memory_switch.cpp). Both are self-contained: they take only their own lock and never
 * touch the guest, which is what the handler needs. */
extern "C" int RexSwitchGuestPageInfo(uint32_t guest_address, char* out_text, size_t out_text_size);
/*
 * fix25: the same codes as RexSwitchGuestPageInfo (0 = not a guest virtual address, 1 = free,
 * 2 = allocated but not committed, 3 = committed read+write, 4 = committed read only,
 * 5 = committed no access), without building its text and without QueryRegionInfo walking the
 * page table to measure the size of the region: that walk scans to the end of the table when the
 * page is free, and this handler was paying it once per 4 KB page of every granule it checked.
 */
extern "C" int RexSwitchGuestPageState(uint32_t guest_address);
extern "C" uint32_t RexGmFaultDiag(uint64_t window_address, char* out, size_t out_size);
/* fix16: allocation ledger (xmemory.cpp): every address the port handed out, plus every
 * physical translation, so a refused fault has an owner for the address it faulted on. */
extern "C" void RexSwitchLedgerQuery(uint32_t guest_address, char* out_text,
                                      size_t out_text_size);
extern "C" void RexGmClearProtect(size_t offset, size_t length);
extern "C" int RexGmLiberarPresupuestoParaCarga(void);
extern "C" int RexGmServirPaginaSuelta(void* address);
extern "C" int RexGmServirPaginaDeBloque(void* address);

/*
 * The exception slots: one stack and one dump per thread faulting at the same time. See the header.
 * The sizes are powers of two on purpose, so the assembly reaches slot N with a shift rather than a
 * multiplication.
 *
 * Eight is plenty: measurements never showed more than two threads inside the handler at once, and
 * they are 512 KB of .bss, which take no space in the NRO.
 */
#define REX_EXC_RANURAS 8
#define REX_EXC_PILA 0x10000   /* 64 KB per slot -> shift of 16 */
#define REX_EXC_VOLCADO 0x400  /* the dump takes 0x340 -> shift of 10 */

extern "C" {
alignas(16) uint8_t g_rex_exc_pilas[REX_EXC_RANURAS][REX_EXC_PILA];
alignas(16) uint8_t g_rex_exc_volcados[REX_EXC_RANURAS][REX_EXC_VOLCADO];
alignas(16) uint32_t g_rex_exc_ocupadas[REX_EXC_RANURAS];
}

static_assert(sizeof(ThreadExceptionDump) <= REX_EXC_VOLCADO,
              "el volcado ya no cabe en la ranura");
static_assert(REX_EXC_RANURAS == 8, "el ensamblador compara contra 8 a mano");

/*
 * The exception entry, replacing libnx's
 *
 * libnx defines __libnx_exception_entry as a weak symbol, and crt0 jumps to it with x0 = exception
 * type and x1 = kernel frame. This is libnx 4.12's one instruction by instruction (read from the
 * linked binary), with one extra branch at the start: if the frame's pc is RexResumeTrap, the
 * exception is the resume from resume_ctx.h, not a fault. Then x0-x8, lr, sp, pc and pstate are
 * copied into the frame from the dump x8 points to, and it returns with svcReturnFromException
 * without touching x9-x29 or NEON, which already hold their values.
 *
 * The rest is libnx's: bail out if a debugger is attached (unless __nx_exception_ignoredebug), copy
 * the frame and the live registers to __nx_exceptiondump, and return from the kernel to
 * RexExceptionReturnEntry on the exception stack with x0 pointing to the dump.
 */
__asm__(".text\n"
        ".balign 4\n"
        ".global __libnx_exception_entry\n"
        ".type __libnx_exception_entry, %function\n"
        "__libnx_exception_entry:\n"
        "    cbz  x1, .Lrex_exc_abort\n"
        "    ldr  x2, [x1, #88]\n"
        "    adrp x3, RexResumeTrap\n"
        "    add  x3, x3, :lo12:RexResumeTrap\n"
        "    cmp  x2, x3\n"
        "    b.eq .Lrex_exc_resume\n"
        /* debugger? svcGetInfo(InfoType_DebuggerAttached) preserving x9-x21 */
        "    stp  x9,  x10, [sp, #-16]!\n"
        "    stp  x11, x12, [sp, #-16]!\n"
        "    stp  x13, x14, [sp, #-16]!\n"
        "    stp  x15, x16, [sp, #-16]!\n"
        "    stp  x17, x18, [sp, #-16]!\n"
        "    stp  x19, x20, [sp, #-16]!\n"
        "    str  x21, [sp, #-16]!\n"
        "    stp  x0,  x1,  [sp, #-16]!\n"
        "    sub  sp, sp, #16\n"
        "    mov  x0, sp\n"
        "    mov  x1, #8\n"
        "    mov  w2, wzr\n"
        "    mov  x3, #0\n"
        "    bl   svcGetInfo\n"
        "    mov  w6, w0\n"
        "    ldr  x7, [sp], #16\n"
        "    ldp  x0,  x1,  [sp], #16\n"
        "    ldr  x21, [sp], #16\n"
        "    ldp  x19, x20, [sp], #16\n"
        "    ldp  x17, x18, [sp], #16\n"
        "    ldp  x15, x16, [sp], #16\n"
        "    ldp  x13, x14, [sp], #16\n"
        "    ldp  x11, x12, [sp], #16\n"
        "    ldp  x9,  x10, [sp], #16\n"
        "    cbnz w6, .Lrex_exc_abort\n"
        "    adrp x6, __nx_exception_ignoredebug\n"
        "    ldr  w5, [x6, :lo12:__nx_exception_ignoredebug]\n"
        "    cbnz w5, .Lrex_exc_dump\n"
        "    cbnz x7, .Lrex_exc_abort\n"
        /*
         * Take a free slot. Only x3-x7 are touched: x0 holds the exception type and x1
         * the kernel frame, and both are needed below.
         */
        ".Lrex_exc_dump:\n"
        "    adrp x6, g_rex_exc_ocupadas\n"
        "    add  x6, x6, :lo12:g_rex_exc_ocupadas\n"
        "    mov  w7, wzr\n"
        ".Lrex_exc_pide:\n"
        "    add  x5, x6, x7, lsl #2\n"
        "    ldaxr w4, [x5]\n"
        "    cbnz w4, .Lrex_exc_siguiente\n"
        "    mov  w3, #1\n"
        "    stlxr w4, w3, [x5]\n"
        "    cbz  w4, .Lrex_exc_cogida\n"
        "    b    .Lrex_exc_pide\n"
        ".Lrex_exc_siguiente:\n"
        "    clrex\n"
        "    add  w7, w7, #1\n"
        "    cmp  w7, #8\n"
        "    b.lo .Lrex_exc_pide\n"
        /*
         * All taken: go back to the first one and wait. A thread stalled here is far
         * less harmful than two writing to the same dump.
         */
        "    mov  w7, wzr\n"
        "    b    .Lrex_exc_pide\n"
        ".Lrex_exc_cogida:\n"
        /*
         * This slot's dump, with the slot number in pad[0], which the resume branch
         * reads from the copy to release it.
         */
        "    adrp x2, g_rex_exc_volcados\n"
        "    add  x2, x2, :lo12:g_rex_exc_volcados\n"
        "    add  x2, x2, x7, lsl #10\n"
        "    mov  x5, x2\n"
        "    str  w0,  [x2], #4\n"
        "    str  w7,  [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    ldp  x3, x4, [x1]\n"
        "    str  x5, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  x9, [x2], #8\n"
        "    stp  x10, x11, [x2], #16\n"
        "    stp  x12, x13, [x2], #16\n"
        "    stp  x14, x15, [x2], #16\n"
        "    stp  x16, x17, [x2], #16\n"
        "    stp  x18, x19, [x2], #16\n"
        "    stp  x20, x21, [x2], #16\n"
        "    stp  x22, x23, [x2], #16\n"
        "    stp  x24, x25, [x2], #16\n"
        "    stp  x26, x27, [x2], #16\n"
        "    str  x28, [x2], #8\n"
        "    str  x29, [x2], #8\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        /* Top of this slot's stack: base + (ranura + 1) * 64 KB. */
        "    adrp x4, g_rex_exc_pilas\n"
        "    add  x4, x4, :lo12:g_rex_exc_pilas\n"
        "    add  x7, x7, #1\n"
        "    add  x4, x4, x7, lsl #16\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    adrp x4, RexExceptionReturnEntry\n"
        "    add  x4, x4, :lo12:RexExceptionReturnEntry\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  xzr, [x2], #8\n"
        "    stp  q0,  q1,  [x2], #32\n"
        "    stp  q2,  q3,  [x2], #32\n"
        "    stp  q4,  q5,  [x2], #32\n"
        "    stp  q6,  q7,  [x2], #32\n"
        "    stp  q8,  q9,  [x2], #32\n"
        "    stp  q10, q11, [x2], #32\n"
        "    stp  q12, q13, [x2], #32\n"
        "    stp  q14, q15, [x2], #32\n"
        "    stp  q16, q17, [x2], #32\n"
        "    stp  q18, q19, [x2], #32\n"
        "    stp  q20, q21, [x2], #32\n"
        "    stp  q22, q23, [x2], #32\n"
        "    stp  q24, q25, [x2], #32\n"
        "    stp  q26, q27, [x2], #32\n"
        "    stp  q28, q29, [x2], #32\n"
        "    stp  q30, q31, [x2], #32\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        /* resume: frame <- copied dump (x8 of the frame) */
        ".Lrex_exc_resume:\n"
        "    ldr  x2, [x1, #64]\n"
        /*
         * Release the slot. The thread no longer runs on the exception stack here
         * (it is inside the kernel, about to return to the faulting code), so another
         * thread can take it without overwriting anything. The mask is in case the
         * copied dump were damaged.
         */
        "    ldr  w3, [x2, #4]\n"
        "    and  w3, w3, #7\n"
        "    adrp x4, g_rex_exc_ocupadas\n"
        "    add  x4, x4, :lo12:g_rex_exc_ocupadas\n"
        "    add  x4, x4, x3, lsl #2\n"
        "    stlr wzr, [x4]\n"
        "    ldp  x3, x4, [x2, #16]\n"
        "    stp  x3, x4, [x1, #0]\n"
        "    ldp  x3, x4, [x2, #32]\n"
        "    stp  x3, x4, [x1, #16]\n"
        "    ldp  x3, x4, [x2, #48]\n"
        "    stp  x3, x4, [x1, #32]\n"
        "    ldp  x3, x4, [x2, #64]\n"
        "    stp  x3, x4, [x1, #48]\n"
        "    ldr  x3, [x2, #80]\n"
        "    str  x3, [x1, #64]\n"
        "    ldp  x3, x4, [x2, #256]\n"
        "    stp  x3, x4, [x1, #72]\n"
        "    ldr  x3, [x2, #272]\n"
        "    str  x3, [x1, #88]\n"
        "    ldr  w3, [x2, #800]\n"
        "    str  w3, [x1, #96]\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        ".Lrex_exc_abort:\n"
        "    mov  w0, #0xf801\n"
        ".Lrex_exc_end:\n"
        "    bl   svcReturnFromException\n"
        "    b    .\n"
        ".size __libnx_exception_entry, .-__libnx_exception_entry\n"
        "\n"
        ".balign 4\n"
        ".type RexExceptionReturnEntry, %function\n"
        "RexExceptionReturnEntry:\n"
        "    bl   __libnx_exception_handler\n"
        "    mov  w0, wzr\n"
        "    mov  x1, #0\n"
        "    mov  x2, #0\n"
        "    bl   svcBreak\n"
        "    b    .\n"
        ".size RexExceptionReturnEntry, .-RexExceptionReturnEntry\n");

/*
 * NFSMW's first boot went back to the HOME menu right after creating the GPU threads: no error on
 * screen, no Atmosphere report and the log cut off abruptly. The previous stop (a brk inside the
 * handler, with the data in x24..x27 for the report) left nothing to read. So the fault is written
 * right here, to <NRO folder>/logs/rex/rex_crash.log.
 *
 * It is written with the fs service directly, not with stdio: the faulting thread may have been left
 * halfway inside newlib, holding its locks.
 *
 * abort() and exit() use it too, see switch_crash_hooks.c. That is why ctx can be null: then there
 * are no registers, only the stack from 'stack'.
 */
extern "C" void _start(void);

namespace {

struct CrashBuf {
  char data[16384];
  size_t len = 0;
};

__attribute__((format(printf, 2, 3))) void Append(CrashBuf& b, const char* fmt, ...) {
  if (b.len + 1 >= sizeof(b.data)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(b.data + b.len, sizeof(b.data) - b.len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    b.len = std::min(sizeof(b.data) - 1, b.len + static_cast<size_t>(n));
  }
}

/* Region of 'addr' if it is readable and spans 'bytes'. */
bool QueryReadable(uint64_t addr, size_t bytes, MemoryInfo* out) {
  u32 page_info = 0;
  if (R_FAILED(svcQueryMemory(out, &page_info, addr))) {
    return false;
  }
  return out->type != MemType_Unmapped && (out->perm & Perm_R) &&
         addr + bytes <= out->addr + out->size;
}

/*
 * Without frame pointers (GCC omits them at -O2 on aarch64) there is no chain to follow. The stack
 * is walked and the values that fall in the executable's code right after a BL or BLR are recorded:
 * they are almost certainly return addresses. Some may be leftovers from an earlier call; the order
 * matters more than any single line.
 */
void ScanStack(CrashBuf& b, uint64_t stack, uint64_t base, uint64_t text_lo, uint64_t text_hi) {
  stack &= ~uint64_t(7);
  MemoryInfo mi;
  if (!QueryReadable(stack, 8, &mi)) {
    Append(b, "pila ilegible en 0x%016" PRIx64 "\n", stack);
    return;
  }
  const uint64_t end = std::min(mi.addr + mi.size, stack + 0x10000);
  Append(b, "posibles retornos en la pila (0x%016" PRIx64 "):\n", stack);
  int hits = 0;
  for (uint64_t p = stack; p + 8 <= end && hits < 48; p += 8) {
    const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
    if (v < text_lo + 4 || v >= text_hi || (v & 3)) {
      continue;
    }
    const uint32_t prev = *reinterpret_cast<const uint32_t*>(v - 4);
    const bool bl = (prev & 0xFC000000u) == 0x94000000u;
    const bool blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
    if (!bl && !blr) {
      continue;
    }
    Append(b, "  sp+0x%05" PRIx64 "  imagen+0x%" PRIx64 "\n", p - stack, v - base);
    ++hits;
  }
}

void WriteCrashFile(const char* data, size_t len) {
  FsFileSystem* fs = nullptr;
  char path[FS_MAX_PATH];
  // In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup, without allocating here).
  // If that path cannot be translated, the default one.
  char ruta[FS_MAX_PATH];
  std::snprintf(ruta, sizeof(ruta), "%srex_crash.log", RexSwitchLogDir());
  if ((fsdevTranslatePath(ruta, &fs, path) < 0 || !fs) &&
      (fsdevTranslatePath("sdmc:/switch/rex_crash.log", &fs, path) < 0 || !fs)) {
    return;
  }
  fsFsCreateFile(fs, path, 0, 0);  // fails if it already exists, which is fine
  FsFile f;
  if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
    return;
  }
  s64 size = 0;
  fsFileGetSize(&f, &size);
  fsFileWrite(&f, size, data, len, FsWriteOption_Flush);
  fsFileClose(&f);
}

}  // namespace

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx,
                                  uint64_t stack, uint64_t pc) {
  // A second fault while the first is being written (on this thread or another)
  // does not write: the first one is what explains what happened.
  static std::atomic_flag busy = ATOMIC_FLAG_INIT;
  if (busy.test_and_set()) {
    return;
  }
  static CrashBuf b;
  b.len = 0;

  const uint64_t base = reinterpret_cast<uint64_t>(&_start);
  uint64_t text_lo = base, text_hi = base;
  MemoryInfo mi;
  if (QueryReadable(base, 4, &mi)) {
    text_lo = mi.addr;
    text_hi = mi.addr + mi.size;
  }
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);

  Append(b, "==== %s ====\n", reason ? reason : "?");
  Append(b, "hilo %" PRIu64 ", imagen 0x%016" PRIx64 " (codigo hasta 0x%016" PRIx64 ")\n", tid,
         base, text_hi);
  Append(b, "pc 0x%016" PRIx64 " = imagen+0x%" PRIx64 "\n", pc, pc - base);

  if (ctx) {
    const uint32_t esr = ctx->esr;
    Append(b, "lr 0x%016" PRIx64 " = imagen+0x%" PRIx64 "\n", ctx->lr.x, ctx->lr.x - base);
    Append(b, "esr 0x%08x (EC 0x%02x)  far 0x%016" PRIx64 "  error_desc 0x%x  pstate 0x%08x\n",
           esr, esr >> 26, ctx->far.x, ctx->error_desc, ctx->pstate);
    if (QueryReadable(pc, 4, &mi) && (mi.perm & Perm_X)) {
      Append(b, "instruccion 0x%08x\n", *reinterpret_cast<const uint32_t*>(pc));
    }
    MemoryInfo fm;
    u32 page_info = 0;
    if (R_SUCCEEDED(svcQueryMemory(&fm, &page_info, ctx->far.x))) {
      Append(b,
             "far en region 0x%016" PRIx64 "+0x%" PRIx64 " tipo 0x%x permisos 0x%x\n",
             fm.addr, fm.size, fm.type, fm.perm);
    }
    const uint64_t gm = reinterpret_cast<uint64_t>(RexGmBase());
    if (gm && ctx->far.x >= gm && ctx->far.x < gm + RexGmSize()) {
      Append(b, "far dentro de la memoria del invitado: 0x%08" PRIx64 "\n", ctx->far.x - gm);
    }
    for (int i = 0; i < 29; ++i) {
      Append(b, "x%-2d 0x%016" PRIx64 "%s", i, ctx->cpu_gprs[i].x, (i % 3 == 2) ? "\n" : "  ");
    }
    Append(b, "fp  0x%016" PRIx64 "  sp  0x%016" PRIx64 "\n", ctx->fp.x, ctx->sp.x);
  }

  // Frame chain: the ELF keeps x29 as the frame pointer (the Atmosphere report of
  // the first abort walked it completely). More reliable than the scan below,
  // which remains as a fallback for when the chain breaks.
  Append(b, "cadena de marcos:\n");
  uint64_t frame = ctx ? ctx->fp.x : stack;
  for (int i = 0; i < 40 && (frame & 7) == 0 && QueryReadable(frame, 16, &mi); ++i) {
    const uint64_t next = reinterpret_cast<const uint64_t*>(frame)[0];
    const uint64_t ret = reinterpret_cast<const uint64_t*>(frame)[1];
    if (ret >= text_lo && ret < text_hi) {
      Append(b, "  #%02d imagen+0x%" PRIx64 "\n", i, ret - base);
    } else {
      Append(b, "  #%02d 0x%016" PRIx64 "\n", i, ret);
    }
    if (next <= frame) {
      break;
    }
    frame = next;
  }

  ScanStack(b, ctx ? ctx->sp.x : stack, base, text_lo, text_hi);
  Append(b, "\n");
  WriteCrashFile(b.data, b.len);
  busy.clear();
}

namespace rex::arch {

static_assert(sizeof(vec128_t) == 16, "vec128_t ya no mide 16 bytes");
static_assert(sizeof(FpuRegister) == 16, "FpuRegister ya no mide 16 bytes");

namespace {

// As on POSIX: few, because they are walked on every fault.
constexpr size_t kMaxHandlerCount = 8;
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

constexpr uint32_t kEsrEcShift = 26;
constexpr uint32_t kEcDataAbortMask = 0b111110;
constexpr uint32_t kEcDataAbort = 0b100100;  // 0x24 (lower EL) and 0x25 (same EL)
constexpr uint32_t kEcUnknown = 0x00;        // undefined instructions arrive here

bool InGuestWindow(uint64_t addr) {
  const uint8_t* base = RexGmBase();
  if (!base) {
    return false;
  }
  const uint64_t b = reinterpret_cast<uint64_t>(base);
  return addr >= b && addr < b + RexGmSize();
}

/*
 * Emulates the access of the instruction at PC by reading or writing through the shadow, and
 * advances the PC. Returns false if it could not: unrecognized instruction, no backing, or an
 * access that crosses into another backing chunk (the shadow is not contiguous across chunks, so it
 * cannot be emulated in one go).
 */
bool EmulateViaShadow(ThreadExceptionDump* ctx) {
  const uint32_t insn = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
  RexAccess acc;
  if (!RexDecodeAccess(ctx, insn, &acc)) {
    return false;
  }
  const size_t total = static_cast<size_t>(acc.bytes) * (acc.rt2 != 0xFF ? 2 : 1);
  void* ini = RexGmShadowFor(acc.address);
  void* fin = RexGmShadowFor(acc.address + total - 1);
  if (!ini || !fin || static_cast<uint8_t*>(fin) != static_cast<uint8_t*>(ini) + total - 1) {
    return false;
  }
  RexEmulateAccess(ctx, &acc, ini);
  ctx->pc.x += 4;
  return true;
}

/*
 * fix17: emulate a load instruction by writing zero to the destination register(s),
 * without requiring a shadow backing. This is a last-resort for read faults on
 * unallocated guest virtual pages where the guest is reading through a wrong pointer.
 * Instead of mapping zero pages (fix14 livelock) or fataling immediately (fix15), the
 * load is emulated as reading zero and execution continues, up to a strict limit.
 */
static bool EmulateZeroLoad(ThreadExceptionDump* ctx) {
  const uint32_t insn = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
  RexAccess acc;
  if (!RexDecodeAccess(ctx, insn, &acc)) {
    return false;
  }
  if (acc.kind != REX_ACC_LOAD) {
    return false;  // only loads
  }
  if (acc.is_simd) {
    uint8_t* dst = (uint8_t*)&ctx->fpu_gprs[acc.rt];
    memset(dst, 0, 16);
    if (acc.rt2 != 0xFF) {
      uint8_t* d2 = (uint8_t*)&ctx->fpu_gprs[acc.rt2];
      memset(d2, 0, 16);
    }
  } else {
    RexGprWrite(ctx, acc.rt, 0);
    if (acc.rt2 != 0xFF) {
      RexGprWrite(ctx, acc.rt2, 0);
    }
  }
  if (acc.writeback) {
    RexGprWriteSp(ctx, acc.rn, RexGprReadSp(ctx, acc.rn) + acc.writeback);
  }
  ctx->pc.x += 4;
  return true;
}

/*
 * fix19: serve a read of the raw physical range from physical memory itself.
 *
 * The guest page table reports the page as free, but that does not make the read a wild pointer: on
 * the console that address is not a virtual one at all, it is a *physical* one (the number
 * MmGetPhysicalAddress returns, the video front buffer, the XMA context array), and the console has
 * a view for it, the fifth one, over 0x00000000-0x1FFFFFFF. This port never registered that view
 * (the identity view holds that range for the v00000000 virtual heap), so the read faults.
 *
 * fix18 emulated those reads as zeros, one load at a time, and ran out of its budget at 65536 of
 * them: that is exactly the walk in its log, 0x10401880 -> 0x10501880 in 16-byte NEON loads, which
 * is 1 MB / 16 B. The zeros were wrong as well: they are not what the console returns.
 *
 * So before emulating, the granule is given a view of its own over physical memory. It is done for
 * the largest granule whose pages are *all* free in the guest page table, and a granule that fails
 * the check is remembered, so that the 2 MB scan is not repeated for every load of the same copy.
 *
 * Returns true when the fault has been served: the caller resumes the instruction.
 */
static bool TryRawPhysicalAlias(uint64_t far, uint32_t guest) {
  constexpr uint32_t kRawFin = 0x20000000u;
  if (guest >= kRawFin) return false;

  /*
   * 2 MB first: a single view and a single mapping for a whole copy (the fix18 log walked 1 MB in
   * one go). 64 KB after that, for the granules the game shares with something else.
   *
   * visto[] remembers what has already been tried, because this runs on every fault: 0 = not tried,
   * 1 = rejected (some page of the granule was not free, or no view was left). The granule number is
   * the index, so one byte per granule covers the whole raw range.
   */
  static const uint32_t tamanos[2] = {0x200000u, 0x10000u};
  static uint8_t visto_2m[kRawFin / 0x200000u] = {};
  static uint8_t visto_64k[kRawFin / 0x10000u] = {};

  for (size_t i = 0; i < 2; i++) {
    const uint32_t tam = tamanos[i];
    const uint32_t gran = guest & ~(tam - 1u);
    if (gran + tam > kRawFin) continue;

    uint8_t& marca = (i == 0) ? visto_2m[gran / 0x200000u] : visto_64k[gran / 0x10000u];
    if (marca == 1) continue;  /* already tried for this granule */

    /*
     * Every page of the granule has to be free: covering a page the game has allocated would have it
     * read physical bytes where it expects its own memory.
     */
    bool libre = true;
    for (uint32_t p = gran; p < gran + tam; p += 0x1000u) {
      /*
       * fix25: the state of the page is all this check needs. It used to ask
       * RexSwitchGuestPageInfo once per 4 KB page, and each of those calls scans the page table
       * to the end when the page is free: 512 calls for a 2 MB granule, about half a second per
       * granule, which is what kept the walk over free guest memory at 2 MB per second with the
       * screen at 0.0 fps.
       */
      if (RexSwitchGuestPageState(p) != 1) {
        libre = false;
        break;
      }
    }
    if (!libre) {
      marca = 1;
      continue;
    }

    if (!RexGmRawAlias(far, tam)) {
      marca = 1;
      continue;
    }

    /* The view is in place; what is missing is the chunk inside it. */
    if (RexGmFaultIn(far)) {
      RexSwitchPerfCount(30);
      return true;
    }
    return false;
  }
  return false;
}

/*
 * fix20: serve a read of free guest virtual memory with real backing, in one go.
 *
 * fix19 stopped the 16-byte walk inside the raw physical range (0x00000000-0x1FFFFFFF) by giving
 * the granules it went through a raw view. The run then showed the walk does not stop at the end of
 * that range: with 126 raw views live, covering 0x10400000-0x1FE00000, the copy went on into
 * 0x20000000, where the raw range ends. From there the only thing left was fix18's emulation, a
 * 16-byte load per exception and a budget of 65536 loads: the run reports 1 MB of them and then the
 * fatal abort.
 *
 * The guest virtual heap (the v00000000 view) is 1 GB, 0x00000000-0x3FFFFFFF, so 0x20000000 is
 * inside it, not outside every view. The page is free in the guest page table, that is, no
 * allocation owns it: giving it backing hides nothing of the game's. The bytes are the zeros
 * RexGmCommit writes when it allocates, the same ones fix18 emulated, only that from then on the
 * hardware serves them: one exception per 2 MB granule instead of one per load, and the copy runs
 * at native speed.
 *
 * This is not a return to fix14 (the live-locked silent walk): the backing is real, so each granule
 * is served once and the fault does not come back; every granule served is counted (id 33), so the
 * run says what the walk cost in memory; and the range is limited to the guest's own virtual window,
 * still gated by the page table check that fix15 refused to skip.
 *
 * Returns true when the fault has been served: the caller resumes the instruction.
 */
static bool TryFreePageBacking(uint64_t far, uint32_t guest) {
  constexpr uint32_t kVirtualFin = 0x40000000u;  /* the 1 GB guest virtual heap */
  if (guest >= kVirtualFin) return false;

  /*
   * 2 MB, the granule RexGmCommit allocates in. It only fills the gaps: an offset that already has a
   * chunk is skipped, so a page the game has allocated and that shares the granule with the free one
   * keeps its own backing and nothing is overwritten.
   */
  constexpr size_t kGran = 0x200000;
  size_t offset = 0;
  if (!RexGmWindowToOffset(far, &offset)) return false;
  if (offset >= kVirtualFin) return false;

  const size_t gran = offset & ~(kGran - 1);
  if (!RexGmCommit(gran, kGran, REX_GM_WRITE)) return false;

  /*
   * A page with logical protection is unmapped on purpose (it is being watched): RexGmFaultIn
   * refuses it, so this returns false and fix18's emulation serves the load as it did.
   */
  if (!RexGmFaultIn(far)) return false;

  RexSwitchPerfCount(33);
  return true;
}

void FillContext(HostThreadContext& tc, const ThreadExceptionDump* ctx) {
  for (int i = 0; i < 29; ++i) {
    tc.x[i] = ctx->cpu_gprs[i].x;
  }
  tc.x[29] = ctx->fp.x;
  tc.x[30] = ctx->lr.x;
  tc.sp = ctx->sp.x;
  tc.pc = ctx->pc.x;
  tc.pstate = ctx->pstate;

  // The Horizon dump has no FPSR or FPCR. They are thread registers and the
  // handler runs on the same thread, so they are read live.
  uint64_t fpsr = 0, fpcr = 0;
  __asm__ volatile("mrs %0, fpsr\n mrs %1, fpcr" : "=r"(fpsr), "=r"(fpcr));
  tc.fpsr = static_cast<uint32_t>(fpsr);
  tc.fpcr = static_cast<uint32_t>(fpcr);

  for (int i = 0; i < 32; ++i) {
    std::memcpy(&tc.v[i], &ctx->fpu_gprs[i], sizeof(vec128_t));
  }
}

/*
 * Copies to the dump what the SDK handler changed. Only the registers it marked as modified, as on
 * POSIX.
 */
void WriteBack(const Exception& ex, const HostThreadContext& tc, ThreadExceptionDump* ctx) {
  uint32_t xm = ex.modified_x_registers();
  while (xm) {
    const unsigned i = static_cast<unsigned>(__builtin_ctz(xm));
    xm &= xm - 1;
    if (i < 29) {
      ctx->cpu_gprs[i].x = tc.x[i];
    } else if (i == 29) {
      ctx->fp.x = tc.x[29];
    } else {
      ctx->lr.x = tc.x[30];
    }
  }
  uint32_t vm = ex.modified_v_registers();
  while (vm) {
    const unsigned i = static_cast<unsigned>(__builtin_ctz(vm));
    vm &= vm - 1;
    std::memcpy(&ctx->fpu_gprs[i], &tc.v[i], sizeof(FpuRegister));
  }
  ctx->sp.x = tc.sp;
  ctx->pc.x = tc.pc;
  ctx->pstate = static_cast<u32>(tc.pstate);
}

/*
 * fix14: append one line to rex_crash.log (and to stdout) from a path that is not a crash.
 * WriteCrashFile opens the file in append mode, so the fatal dump written later on does not
 * erase this line.
 */
extern "C" void RexSwitchDiagLine(const char* text) {
  char buffer[1024];
  const int n = std::snprintf(buffer, sizeof(buffer), "%s", text);
  if (n > 0) {
    const size_t len = static_cast<size_t>(n) < sizeof(buffer) ? static_cast<size_t>(n)
                                                              : sizeof(buffer) - 1;
    WriteCrashFile(buffer, len);
  }
  std::fprintf(stdout, "%s", text);
  std::fflush(stdout);
}

/*
 * fix20: what the watched-read emulation is asked to serve, sampled.
 *
 * The profile puts the port's hot path here: around 160,000 emulated loads per second with the CPU at
 * 200%, that is, the same fault served again and again in the shadow path. Neither the range of that
 * storm nor its caller was in the profile, so every 64th call writes one line with pc and lr, kept in
 * rex_crash.log together with the other diag lines.
 *
 * The line changes nothing: not the emulation, not the permissions, not any counter.
 */
/*
 * fix21: the sampling of these diag lines is bounded, and that is a fix in itself.
 *
 * fix20 wrote one line every 64 watched reads, and WriteCrashFile translates the path, creates the
 * file, opens it, writes with flush and closes it on every single line. In the storm run that meant
 * 130,006 lines: 130,006 open + write + flush + close cycles on the SD card, written from the very
 * thread that was faulting. rex_crash.log grew while the game crawled, and the crawl is partly this.
 *
 * Now the first lines of the run are kept (they are the forensic value: the address, the pc and the
 * lr of the storm, before anything else touches the file) and then one every 65536, capped for the
 * whole run. What the storm costs is told by the counters of the 10 s report, which write nothing
 * per event.
 */
constexpr uint32_t kWatchedDiagFirst = 16;
constexpr uint32_t kWatchedDiagStep = 65536;
constexpr uint32_t kWatchedDiagMax = 512;
constexpr uint32_t kBorrowDiagFirst = 8;
constexpr uint32_t kBorrowDiagStep = 4096;
constexpr uint32_t kBorrowDiagMax = 256;

static void RexObserveWatchedRead(const ThreadExceptionDump* ctx, uint64_t far) {
  static std::atomic<uint32_t> n{0};
  const uint32_t i = n.fetch_add(1) + 1;
  if (i > kWatchedDiagMax) return;
  if (i > kWatchedDiagFirst && (i % kWatchedDiagStep) != 0) return;
  char linea[256];
  std::snprintf(linea, sizeof(linea),
                "[fix20] lectura vigilada #%u invitado=0x%08X pc=0x%016llx lr=0x%016llx\n",
                static_cast<unsigned>(i),
                static_cast<unsigned>(far & 0xFFFFFFFFull),
                static_cast<unsigned long long>(ctx->pc.x),
                static_cast<unsigned long long>(ctx->lr.x));
  RexSwitchDiagLine(linea);
}

/*
 * fix21: how many reads have been served natively by lending the watched page its backing.
 *
 * The line is the evidence that the lending is what served the storm, and where the storm was (pc and
 * lr). It goes through the same bounded sampling as the one above, for the same reason.
 */
static void RexObserveBorrowedRead(const ThreadExceptionDump* ctx, uint64_t far) {
  static std::atomic<uint32_t> n{0};
  const uint32_t i = n.fetch_add(1) + 1;
  if (i > kBorrowDiagMax) return;
  if (i > kBorrowDiagFirst && (i % kBorrowDiagStep) != 0) return;
  char linea[256];
  std::snprintf(linea, sizeof(linea),
                "[fix21] lectura nativa #%u invitado=0x%08X pc=0x%016llx lr=0x%016llx\n",
                static_cast<unsigned>(i),
                static_cast<unsigned>(far & 0xFFFFFFFFull),
                static_cast<unsigned long long>(ctx->pc.x),
                static_cast<unsigned long long>(ctx->lr.x));
  RexSwitchDiagLine(linea);
}

/*
 * fix14/fix15: a guest *virtual* page (below 0x80000000 in window coordinates, so neither the
 * XEX image nor the guest physical ranges, which already have their own paths) is not present
 * in the window. Write down what both sides know about it, and repair it *only* when the guest
 * page table says the page is really allocated.
 *
 * Measured case this comes from: right after the 874 MB (0x37080000) allocation is served, a
 * recompiled std::wstring copy reads its source buffer at guest 0x10401880 and faults.
 *
 * fix15: fix14 wrote the line and then repaired everything except codigo 0 and 5, so a *free*
 * page (codigo 1: never allocated) was committed and mapped 2 MB at a time and the fault was
 * swallowed. Measured: about 110 repairs per run, guest 0x10401880 walking up to 0x1D000000
 * (about 210 MB of free address space read as zeroes), and then no fault at all. The guest ran
 * on with zeroes where its data should have been, at about 1.2 fps, and never faulted again, so
 * no report was produced: the failure turned from a hard data abort into a silent freeze. The
 * comment already stated the opposite contract ("if it says free or decommitted, the guest is
 * reading through a stale or wrong pointer and this line is the evidence"), and swallowing the
 * fault is what removes that evidence. So codigo 1 is never repaired here any more: it falls
 * through to the SDK handlers, to SEH and to the fatal path below, which writes the full report
 * (all registers, frame chain and stack scan).
 *
 * Repairing stays bounded on purpose: at most 256 repairs per run.
 */
static bool RepairMissingGuestVirtualPage(ThreadExceptionDump* ctx, uint64_t far, bool is_write) {
  size_t offset = 0;
  if (!RexGmWindowToOffset(far, &offset) || offset >= 0x80000000u) {
    return false;
  }
  const uint32_t guest = static_cast<uint32_t>(offset & 0xFFFFFFFFu);

  /*
   * fix25: the decision only needs the code. The two diagnostic texts (and RexGmFaultDiag,
   * which walks the views) are only needed when one of the two bounded lines below is
   * emitted, and asking for them here cost a full page-table scan on every fault.
   */
  const int codigo = RexSwitchGuestPageState(guest);

  static std::atomic<uint32_t> reparaciones{0};
  static std::atomic<uint32_t> avisos_reparado{0};
  static std::atomic<uint32_t> avisos_rechazado{0};
  const uint32_t hechas = reparaciones.load();

  /*
   * fix15: only what the guest page table confirms is really there is repaired.
   *
   *   1 = free (never allocated): the guest is reading through a wrong or stale pointer. Never
   *       repaired. Committing and mapping it hides the fault, turns a reportable data abort into
   *       a silent walk over free address space and destroys the evidence.
   *   2 = allocated but not committed (reserved, or decommitted after being committed): the guest
   *       asked for this page, so committing it here is the repair.
   *   3 = committed read+write; 4 = committed read only, reads only (a write to a page the guest
   *       asked for read-only is not repaired).
   *   0 (not a guest virtual page) and 5 (committed no-access: a guard page the guest protects
   *       itself, which the port must not map) are never repaired.
   */
  const bool asignada = (codigo == 2) || (codigo == 3) || (!is_write && codigo == 4);
  const bool reparar = asignada && hechas < 256;

  /*
   * fix15: one line for the first fault that is repaired and one for the first fault that is
   * refused, each with its own flag. fix14 used a single flag keyed off `fallos`, which only grew
   * on the branches that gave up: while repairs succeeded it stayed 0, so a wild pointer wrote
   * about 110 lines and buried the one that matters.
   *
   * pc, lr and the faulting instruction go in the line as well: the fatal dump that a refused
   * fault reaches has all of it, but a repaired fault never reaches a dump, and the first fault
   * of the run is the one that explains the run.
   */
  const bool primera =
      reparar ? (avisos_reparado.fetch_add(1) == 0) : (avisos_rechazado.fetch_add(1) == 0);
  if (primera) {
    char info_invitado[256];
    RexSwitchGuestPageInfo(guest, info_invitado, sizeof(info_invitado));
    char info_gm[256];
    const uint32_t bits = RexGmFaultDiag(far, info_gm, sizeof(info_gm));
    uint32_t instr = 0;
    MemoryInfo mi;
    if (QueryReadable(ctx->pc.x, 4, &mi) && (mi.perm & Perm_X)) {
      instr = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
    }
    char linea[1024];
    std::snprintf(linea, sizeof(linea),
                  "[fix15] far=0x%016llx invitado=0x%08X escritura=%d codigo=%d reparado=%d "
                  "bits=%u pc=0x%016llx lr=0x%016llx x2=0x%016llx instr=0x%08X reparaciones=%u | %s | %s\n",
                  static_cast<unsigned long long>(far), guest, is_write ? 1 : 0, codigo,
                  reparar ? 1 : 0, static_cast<unsigned>(bits),
                  static_cast<unsigned long long>(ctx->pc.x),
                  static_cast<unsigned long long>(ctx->lr.x),
                  static_cast<unsigned long long>(ctx->cpu_gprs[2].x), instr,
                  static_cast<unsigned>(hechas), info_invitado, info_gm);
    RexSwitchDiagLine(linea);
    /*
     * fix16: the ledger. Each RexSwitchDiagLine call is flushed to rex_crash.log and its
     * buffer is 1024 bytes, so the query text is split into one call per line.
     */
    {
      char libreta[4096];
      RexSwitchLedgerQuery(guest, libreta, sizeof(libreta));
      char* p = libreta;
      while (*p) {
        char* nl = std::strchr(p, '\n');
        if (!nl) {
          RexSwitchDiagLine(p);
          break;
        }
        *nl = 0;
        RexSwitchDiagLine(p);
        p = nl + 1;
      }
    }
  }

  if (!reparar) {
    return false;
  }

  constexpr size_t kTrozo = 0x100000;  // 1 MB: RexGmCommit rounds it up to its 2 MB granule
  /*
   * fix39: RexGmClearProtect erases by *window* offset, the key RexGmProtect writes and
   * RexGmFaultIn reads (the comment of RexGmCommit says why: the same mapping offset can be
   * mirrored in several views with different permissions). This call passed the mapping offset,
   * so for any view that does not sit at offset 0 it erased a page that had nothing to do with
   * the fault instead of the protection that was making RexGmFaultIn refuse the mapping.
   */
  RexGmClearProtect(static_cast<size_t>(far - reinterpret_cast<uint64_t>(RexGmBase())) &
                        ~size_t(0xFFF),
                    0x1000);
  if (!RexGmCommit(offset & ~(kTrozo - 1), kTrozo, REX_GM_WRITE)) {
    return false;
  }
  reparaciones.store(hechas + 1);
  if (RexGmFaultIn(far)) {
    return true;
  }
  /* Committed now, but it could not be mapped in this view: the chunk exists with a shadow. */
  return !is_write && EmulateViaShadow(ctx);
}

[[noreturn]] void HandleSwitchException(ThreadExceptionDump* ctx) {
  const uint64_t pc = ctx->pc.x;
  const uint64_t far = ctx->far.x;
  const uint32_t esr = ctx->esr;
  const uint32_t ec = esr >> kEsrEcShift;
  const bool data_abort = (ec & kEcDataAbortMask) == kEcDataAbort;
  const bool is_write = data_abort && ((esr >> 6) & 1);
  const bool in_window = data_abort && InGuestWindow(far);

  /*
   * The hot path, from five locks down to two
   *
   * Measured in a race: up to 113,000 read faults per second, and the thread that suffered most went
   * to 76% of a core. What took the time was not resolving the fault: it was asking.
   *
   * A read fault on a watched page used to go through here like this:
   *   1. RexGmFaultIn          -> lock, checks permissions, says no
   *   2. RexGmWindowToOffset   -> lock, looks up the view again
   *   3. RexGmQueryProtect     -> lock, looks up view and chunk again
   *   4. RexGmQueryProtect     -> lock, the same for the third time
   *   5. RexGmShadowFor        -> lock, looks up view and chunk again
   *
   * Five locks and a dozen tree lookups to always answer the same thing. And with three threads
   * faulting at once on the same lock, they queued up: total CPU stayed at 250% of 300% without any of
   * them reaching 100%.
   *
   * Now it asks once, at the top, and the common case (reading from a watched page) is resolved right
   * there with the shadow. Two remain: the query and the shadow.
   */
  if (data_abort && in_window) {
    size_t largo = 0x1000;
    RexGmAccess acceso = REX_GM_NONE;
    const bool hay_permiso =
        RexGmQueryProtect(reinterpret_cast<uint8_t*>(far), &largo, &acceso);

    /*
* (1) Read from a watched page. By far the common case, so it goes first and asks nothing
     * again. It is still emulated through the shadow: see below for why treating it as a write
     * does not work.
     *
     * fix21: before emulating, the page is offered its own backing. A page is unmapped because it is
     * watched, not because it does not exist, so the read can be served by the hardware if the page
     * is mapped back in: one exception per page instead of one per access. That is what the storm
     * needed, and what the emulation below cannot do: it serves the load but leaves the page
     * unmapped, so the next read of the same page faults again. Measured in the fix20 run: 8.32
     * million read faults over a 3 MB byte by byte copy, 0.1 fps, 126 s of log.
     *
     * See RexGmBorrowWatchForRead for exactly what is lent and what is given up (the write
     * notification of that page until the SDK installs the watch again). If there is nothing to lend
     * or the kernel refuses the mapping, it returns false and the emulation below serves the load as
     * it did before: no path is removed, one is added in front of it.
     */
    if (!is_write && hay_permiso && acceso == REX_GM_READ) {
      if (RexGmBorrowWatchForRead(far)) {
        RexSwitchPerfCount(34);
        RexObserveBorrowedRead(ctx, far);
        RexResumeFromException(ctx);
      }
      if (EmulateViaShadow(ctx)) {
        RexSwitchPerfCount(1);
        RexObserveWatchedRead(ctx, far);
        RexResumeFromException(ctx);
      }
    }

    /* (2) Committed, but not yet mapped in this view. */
    if (RexGmFaultIn(far)) {
      RexSwitchPerfCount(18);
      RexResumeFromException(ctx);
    }

    /*
     * (3) Uncommitted physical memory: it is committed and mapped.
     *
     * Guest physical memory is no longer committed in full at startup (see xmemory.cpp): it is
     * committed the first time it is touched. One 4 MB chunk per fault means at most 128 chunks, which
     * with the 360's five views are about 640 mappings (1 MB chunks gave about 2,500); with 64 KB pages
     * it would be 40,000 and the kernel runs out of blocks. The same instruction is retried, without touching the PC.
     *
     * fix39: the gate used to be `!hay_permiso`, and RexGmQueryProtect answers *true* with
     * REX_GM_NONE for a page that has no committed backing: it has to, because the SDK's MMIO
     * handler retries the instruction whenever the page looks accessible, and a page that is not
     * really there would retry forever (see its comment in guest_memory_switch.cpp). `!hay_permiso`
     * was therefore false for exactly the faults this step exists for, so the path had never run:
     * a read of a guest physical page that no chunk backed - the 0xA0000000, 0xC0000000 and
     * 0xE0000000 mirrors of the 360's physical memory - had no step left to serve it and went
     * straight to the fatal report. The batch this comes from died twice at the same guest address
     * 0xBA842718 (RtlEnterCriticalSection over the mirror of physical 0x1A842718), and the granule
     * that would have backed it had been handed back to the pool by fix38 as "nobody is reading
     * it": the rebuild that SoltarFisicoSinUso relies on is this step. The pool never takes a
     * granule of a page the guest is using through a mirror back again on its own, so the step is
     * now entered whenever the page has no backing to be served from, and a page that is unmapped
     * only because it is watched is given that backing and mapped (the trade fix21 already accepts
     * for a read: the write notification of that one page until the SDK installs the watch again)
     * when even the shadow cannot serve the load.
     *
     * fix39 (second part): the gate is not a condition any more. The batch this step was written
     * for came back with the step still skipped: the page that crashed had a chunk behind it (its
     * granule had never been handed back, fix19 had only unmapped it from the raw view it had
     * entered), so hay_permiso was true and the step never ran. Every mapping offset of the arena
     * behind the mirrors is guest physical memory: it exists, it is not a free guest virtual page,
     * and committing or mapping it can only help. What keeps this step off the other faults is the
     * range test below, and everything it does is idempotent (RexGmCommit over an existing chunk,
     * RexGmFaultIn over an already mapped one), so the cost for a fault it cannot serve is the same
     * lookups step (2) had already paid.
     */
    {
      constexpr size_t kFisicaIni = 0x100000000ull;
      constexpr size_t kFisicaFin = 0x120000000ull;
      constexpr size_t kTrozo = 0x400000;  // 4 MB: fewer chunks and fewer mappings
      size_t offset = 0;
      if (RexGmWindowToOffset(far, &offset) && offset >= kFisicaIni && offset < kFisicaFin) {
        const bool respaldo_previo = (RexGmShadowFor(far) != nullptr);
        bool servido =
            RexGmCommit(offset & ~(kTrozo - 1), kTrozo, REX_GM_WRITE) && RexGmFaultIn(far);
        if (!servido && acceso == REX_GM_READ) {
          /*
           * fix39: the page is unmapped because it is watched, and its chunk was handed back (or
           * never brought in), so there is no shadow to emulate from and the lending of fix21
           * cannot help either: it needs the chunk in the view already. The watch of this page is
           * given up so that the backing can be mapped, which is the difference between the load
           * being served and the run ending here.
           */
          RexGmClearProtect(static_cast<size_t>(far - reinterpret_cast<uint64_t>(RexGmBase())) &
                                ~size_t(0xFFF),
                            0x1000);
          servido = RexGmFaultIn(far);
        }
        if (!servido) {
          /*
           * fix39: the page has backing here but could not be mapped in this view. RexGmFaultIn
           * refuses in six ways and the fatal report only says which one after the run has ended;
           * the two a fault can still get out of are the mapping-entry budget and the physical
           * cache, and the reclaim of the load path hands both back (raw views first: they are the
           * only holder that returns entries). One retry: if it still refuses, the fatal report
           * says why, together with the diagnostic that accompanies it.
           */
          if (RexGmLiberarPresupuestoParaCarga() != 0) {
            servido = RexGmFaultIn(far);
          }
        }
        if (!respaldo_previo) {
          /*
           * fix39: this step had never run before this build, so a line (bounded: the first 48
           * pages) is worth it to see it working: a page that had no backing when it faulted and
           * has one now is the rebuild SoltarFisicoSinUso relies on, served from the fault path.
           */
          static std::atomic<uint32_t> fisica39{0};
          if (fisica39.fetch_add(1, std::memory_order_relaxed) < 48) {
            fprintf(stderr,
                    "[fix39] fisica a la primera invitado=0x%08llX off=0x%zX servido=%d "
                    "escritura=%d\n",
                    static_cast<unsigned long long>(far -
                                                   reinterpret_cast<uint64_t>(RexGmBase())),
                    offset, servido ? 1 : 0, is_write ? 1 : 0);
          }
        }
        if (servido) {
          RexSwitchPerfCount(17);
          RexResumeFromException(ctx);
        }
      }
    }

    /*
     * (4) fix14/fix15: guest virtual page that is not in the window at all. The steps above cover
     * the watched pages, the chunks committed but not mapped in this view and the uncommitted
     * physical memory; what is left over is the 1 GB guest virtual range, where a page the game
     * allocated can be missing here. Last resort, it only touches guest virtual pages, it writes
     * at most two [fix15] lines to rex_crash.log per run, and since fix15 it *refuses* to repair a
     * page the guest page table reports as free, so a wild pointer reaches the fatal report below
     * instead of walking free memory silently.
     */
    if (RepairMissingGuestVirtualPage(ctx, far, is_write)) {
      RexResumeFromException(ctx);
    }

    /*
     * (5) fix17: controlled zero-load emulation for codigo==1 read faults.
     * When the guest page table reports a page as free (never allocated) and the fault
     * is a read, instead of fataling immediately (fix15) we emulate the load as reading
     * zero and continue, up to a strict per-run limit. No memory is mapped, so this does
     * not repeat the fix14 livelock of silently walking free address space.
     */
    if (!is_write) {
      size_t offset_zl = 0;
      if (RexGmWindowToOffset(far, &offset_zl) && offset_zl < 0x80000000u) {
        const uint32_t guest_zl = static_cast<uint32_t>(offset_zl & 0xFFFFFFFFu);
        /* fix25: only the code is used here; the text was never read. */
        const int codigo_zl = RexSwitchGuestPageState(guest_zl);
        if (codigo_zl == 1) {
          /*
           * fix19: before emulating the read as zero, the granule is given a raw physical view.
           * When that works the load is served by the hardware on the retry, and the whole copy
           * stops faulting instead of being walked 16 bytes at a time.
           */
          if (TryRawPhysicalAlias(far, guest_zl)) {
            /*
             * The load is served by the alias on the retry. HandleSwitchException is noreturn,
             * so control never falls through into the fix18 emulation below.
             */
            RexResumeFromException(ctx);
          }
          /*
           * fix20: the same for the rest of the virtual range. The raw path above only covers
           * 0x00000000-0x1FFFFFFF of the guest; the 1 GB virtual heap goes up to 0x3FFFFFFF, and the
           * codigo_zl == 1 check above is the one that says the page is free. From here the read gets
           * backing in the view it was addressed through, instead of being emulated 16 bytes at a
           * time. HandleSwitchException is noreturn: when this serves the fault, control never
           * reaches the emulation below.
           */
          if (TryFreePageBacking(far, guest_zl)) {
            RexResumeFromException(ctx);
          }
          /*
           * fix18: fix17 worked but was silent, so a run could not tell a cap that had been
           * reached from a decoder that had failed. Both outcomes are now written once to
           * rex_crash.log.
           *
           * The run that motivated fix18 walked 0x10401a80 - 0x10401880 = 0x200 = 512 bytes in
           * 32 emulated 16-byte loads (a NEON LDR Q30, [X1, X2, UXTW] copying a wide string),
           * which is exactly the old cap of 32: the copy kept going one load past it. The cap
           * goes to 65536 (up to 1 MB of 16-byte loads); when it is reached it says so instead
           * of dying anonymously.
           */
          constexpr uint32_t kZeroLoadLimit = 65536;
          static std::atomic<uint32_t> cero_simulados{0};
          const uint32_t hechos = cero_simulados.load();
          if (hechos < kZeroLoadLimit) {
            if (EmulateZeroLoad(ctx)) {
              cero_simulados.store(hechos + 1);
              /*
               * fix19: id 29 for the zero-load emulations. It was 19, which the profile line
               * prints as "parones/s" and which command_processor.cpp also counts, so a run
               * could not tell emulated loads from a stalled GPU pipeline.
               */
              RexSwitchPerfCount(29);
              if (hechos == 0 || (hechos & 0xFFFu) == 0 || (hechos + 1) == kZeroLoadLimit) {
                uint32_t instr_zl = 0;
                MemoryInfo mi_zl;
                if (QueryReadable(ctx->pc.x, 4, &mi_zl) && (mi_zl.perm & Perm_X)) {
                  instr_zl = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
                }
                char linea[256];
                std::snprintf(linea, sizeof(linea),
                              "[fix18] zero-load #%u invitado=0x%08X pc=0x%016llx instr=0x%08X\n",
                              hechos + 1, guest_zl,
                              static_cast<unsigned long long>(ctx->pc.x), instr_zl);
                RexSwitchDiagLine(linea);
              }
              RexResumeFromException(ctx);
            } else {
              static std::atomic<uint32_t> aviso_fallo{0};
              if (aviso_fallo.fetch_add(1) == 0) {
                uint32_t instr_zl = 0;
                MemoryInfo mi_zl;
                if (QueryReadable(ctx->pc.x, 4, &mi_zl) && (mi_zl.perm & Perm_X)) {
                  instr_zl = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
                }
                char linea[256];
                std::snprintf(linea, sizeof(linea),
                              "[fix18] EmulateZeroLoad RECHAZADO invitado=0x%08X pc=0x%016llx instr=0x%08X\n",
                              guest_zl, static_cast<unsigned long long>(ctx->pc.x), instr_zl);
                RexSwitchDiagLine(linea);
              }
            }
          } else {
            static std::atomic<uint32_t> aviso_tope{0};
            if (aviso_tope.fetch_add(1) == 0) {
              char linea[256];
              std::snprintf(linea, sizeof(linea),
                            "[fix18] zero-load TOPE alcanzado: %u emulaciones, ultimo invitado=0x%08X pc=0x%016llx\n",
                            hechos, guest_zl,
                            static_cast<unsigned long long>(ctx->pc.x));
              RexSwitchDiagLine(linea);
            }
          }
        }
      }
    }
  }
  // --- 1. read from a watched page: emulated through the shadow -------------
  //
  // On Switch there is no such thing as a read-only page: as soon as the GPU watches a
  // chunk of physical memory, that chunk is unmapped, and then every guest read of it
  // faults. And reading does not remove the watch, so it faults again, and again, as
  // long as the game keeps reading from there. Measured in a race: 83,000 emulated
  // reads per second, more than half a core.
  //
  // Treating the read as a write was tried, and it does not work. The cost went away
  // (83,000 faults/s dropped to 21, and CPU from 292% to 118%) but the game froze while
  // loading a race, with all its threads asleep waiting for something that never came.
  // The reason is in PhysicalHeap::TriggerCallbacks: reporting a write does not only
  // unwatch the page that faulted but up to 4 MB around it (unwatch_exact_range =
  // false), and it also tells the GPU caches that the range changed. With that, write
  // watching was left almost empty (the SDK handler's faults dropped from 240/s to
  // 11/s) and coherence between the guest and the GPU broke.
  //
  // So it is still emulated through the shadow, which is correct even if expensive.
  // This is now resolved above, in point (1) of the hot path, without asking again.
  // The reasoning stays written here because it explains why it cannot be done any
  // other way.

  // --- 2. SDK handlers ----------------------------------------------
  HostThreadContext tc;
  FillContext(tc, ctx);
  Exception ex;
  bool clasificado = true;
  if (data_abort) {
    ex.InitializeAccessViolation(&tc, far,
                                 is_write ? Exception::AccessViolationOperation::kWrite
                                          : Exception::AccessViolationOperation::kRead);
  } else if (ec == kEcUnknown) {
    ex.InitializeIllegalInstruction(&tc);
  } else {
    clasificado = false;
  }

  if (clasificado) {
    for (size_t i = 0; i < kMaxHandlerCount && handlers_[i].first; ++i) {
      if (!handlers_[i].first(&ex, handlers_[i].second)) {
        continue;
      }
      WriteBack(ex, tc, ctx);

      // Did it ask for a retry? Then the page must already be mapped, or the retry
      // would keep faulting forever.
      if (in_window && ctx->pc.x == pc) {
        size_t len = 0x1000;
        RexGmAccess a = REX_GM_NONE;
        const bool ok = RexGmQueryProtect(reinterpret_cast<uint8_t*>(far), &len, &a);
        // A read can be retried on REX_GM_READ: it will fault again, but it will land
        // in step 1 and be emulated. A write needs WRITE.
        const bool reintentable = ok && (is_write ? a == REX_GM_WRITE : a != REX_GM_NONE);
        if (!reintentable) {
          if (EmulateViaShadow(ctx)) {
            RexSwitchPerfCount(3);
            RexResumeFromException(ctx);
          }
          break;  // ni reintentable ni emulable: sigue a SEH y a fatal
        }

        /*
         * fix40: el reintento solo es inocuo si el hardware puede ejecutar de verdad la
         * instruccion ahora mismo.
         *
         * Esta es la pantalla negra infinita de esta tanda: 56 400 fallos por segundo, 0.0 fps,
         * dos hilos de invitado a tope de un nucleo, sin caida, sin linea fatal y sin que la
         * escalera sirviera nada (prestamos 0, lecturas por sombra 0, fault-in 0, fisica 0,
         * alias 0, SEH 0). El fallo es una pagina cuya *contabilidad* dice REX_GM_WRITE -que
         * RexGmProtect registra como mapeada y accesible, ver guest_memory_switch.h- y cuyo
         * mapeo en la vista ya no esta: fix39 devuelve los alias crudos y desmapea lo que
         * llevan, y el permiso logico se queda detras.
         *
         * Con la entrada puesta, RexGmFaultIn rechaza la pagina (en guest_memory_switch.cpp
         * nunca se vuelve a traer lo que esta en el mapa de proteccion), el paso (1) no la
         * mira porque solo atiende REX_GM_READ, y este test lee la misma contabilidad que la
         * pagina lleva y contesta que el reintento es seguro. La instruccion reintenta, vuelve
         * a fallar y se reintenta: la partida no avanza y el informe de 10 s lo llama 0 fps.
         *
         * Asi que antes de reintentar se le pregunta al nucleo si la pagina esta realmente
         * mapeada con el acceso que pide la instruccion. Si lo esta, no cambia nada: el
         * reintento es exactamente el de antes. Si no lo esta, se suelta la entrada (que es lo
         * que bloquea el fault-in), se trae el respaldo y se mapea, y solo entonces se
         * reintenta la instruccion. Las lecturas se ofrecen antes a fix21, que es el camino
         * disenado para justo esta tormenta y al que la escalera no llegaba por solo reconocer
         * REX_GM_READ. Si nada de eso sirve el acceso, el fallo *no* se reintenta: sigue a SEH
         * y al informe fatal, que dice de que pagina se trataba, en vez de dejar la consola en
         * negro con la partida girando en el sitio.
         */
        MemoryInfo info40{};
        u32 pagina40 = 0;
        const bool real40 =
            R_SUCCEEDED(svcQueryMemory(&info40, &pagina40, far)) &&
            info40.type != MemType_Unmapped &&
            (info40.perm & (is_write ? Perm_W : Perm_R)) &&
            far < info40.addr + info40.size;
        if (!real40) {
          bool servido40 = false;
          const char* via40 = "ninguna";
          if (!is_write && RexGmBorrowWatchForRead(far)) {
            servido40 = true;
            via40 = "prestamo";
          }
          if (!servido40) {
            const size_t pago40 =
                static_cast<size_t>(far - reinterpret_cast<uint64_t>(RexGmBase())) & ~size_t(0xFFF);
            RexGmClearProtect(pago40, 0x1000);
            if (RexGmFaultIn(far)) {
              servido40 = true;
              via40 = "fault-in";
            }
          }
          /*
           * fix42: la pagina del bloque materializado, mapeada ella sola. El paso (2) de arriba
           * (ClearProtect + FaultIn) no la trae porque RexGmFaultIn sigue viendo el registro de
           * view_mapped del bloque entero, y la sombra es correcta pero cuesta una excepcion por
           * acceso: con 8.000 accesos por segundo la partida se queda quieta sin caerse. El alias
           * del bloque ya existe, asi que mapear esta pagina cuesta una entrada de ventana y el
           * presupuesto no crece (se suelta la pagina servida de offset mas bajo cuando hace falta
           * sitio). Ver RexGmServirPaginaDeBloque.
           */
          if (!servido40 && RexGmServirPaginaDeBloque(reinterpret_cast<void*>(far)) != 0) {
            servido40 = true;
            via40 = "pagina42";
          }
          /*
           * fix41: el ultimo escalon, y el unico que sirve un bloque comprometido que no cabe en
           * ningun sitio. Los intentos de arriba son los de fix40 y no llegan a ese caso: el
           * prestamo es para relojes de lectura, el fault-in se topa con su propia entrada de
           * proteccion y la sombra no tiene respaldo. Una pagina suelta cuesta dos entradas en vez de
           * las 512 de la via por paginas. Ver RexGmServirPaginaSuelta.
           */
          if (!servido40 && RexGmServirPaginaSuelta(reinterpret_cast<void*>(far)) != 0) {
            servido40 = true;
            via40 = "suelta";
          }
          /* fix42: y si nada de lo anterior sirvio la direccion, la sombra sigue siendo el suelo. */
          if (!servido40 && EmulateViaShadow(ctx)) {
            servido40 = true;
            via40 = "sombra";
          }
          static std::atomic<uint32_t> reintentos40{0};
          const uint32_t n40 = reintentos40.fetch_add(1, std::memory_order_relaxed);
          if (n40 < 32 || (n40 % 4096) == 0) {
            fprintf(stderr,
                    "[fix40] reintento sin mapeo real: invitado=0x%llX manejador=%zu "
                    "escritura=%d permiso=%d acceso=%d real=%d via=%s servido=%d (n=%u)\n",
                    static_cast<unsigned long long>(far - reinterpret_cast<uint64_t>(RexGmBase())),
                    i, is_write ? 1 : 0, ok ? 1 : 0, static_cast<int>(a), real40 ? 1 : 0, via40,
                    servido40 ? 1 : 0, n40);
          }
          if (servido40) {
            RexSwitchPerfCount(2);
            RexResumeFromException(ctx);
          }
          break;  // no hay quien lo sirva: mejor el informe que un bucle de reintentos
        }
      }
      RexSwitchPerfCount(2);
      RexResumeFromException(ctx);
    }
  }

  // --- 3. SEH ----------------------------------------------------------------
  if (RexSwitchSehHandleFault(ctx)) {
    RexSwitchPerfCount(4);
    RexResumeFromException(ctx);
  }

  // --- 4. fatal --------------------------------------------------------------
  // The report goes to <NRO folder>/logs/rex/rex_crash.log. diagAbortWithResult is a system call,
  // not another exception: it ends with error 2345-0102.
  (void)far;
  (void)esr;
  /*
   * fix39: what the ladder knew about the address that ended the run, written before the dump
   * (WriteCrashFile appends, so the dump does not erase it) and only once per run. This is the
   * line that says which link of the ladder refused a fault the resurrected step (3) could not
   * serve either: the window offset and whether it lies in the physical arena, the access the
   * port had recorded, whether any backing existed, and RexGmFaultDiag's view/chunk/protection
   * picture. Nothing here changes the dump that follows.
   */
  if (data_abort) {
    static bool dicho_fatal39 = false;
    if (!dicho_fatal39) {
      dicho_fatal39 = true;
      size_t off_fatal = 0;
      const bool hay_off = RexGmWindowToOffset(far, &off_fatal);
      size_t largo_fatal = 0x1000;
      RexGmAccess acceso_fatal = REX_GM_NONE;
      const bool permiso_fatal =
          RexGmQueryProtect(reinterpret_cast<uint8_t*>(far), &largo_fatal, &acceso_fatal);
      char info_fatal[256];
      RexGmFaultDiag(far, info_fatal, sizeof(info_fatal));
      char linea_fatal[768];
      std::snprintf(linea_fatal, sizeof(linea_fatal),
                    "[fix39] fatal sin servir en_ventana=%d offset=%d:0x%zX escritura=%d "
                    "permiso=%d acceso=%d respaldo=%d | %s\n",
                    in_window ? 1 : 0, hay_off ? 1 : 0, off_fatal, is_write ? 1 : 0,
                    permiso_fatal ? 1 : 0, static_cast<int>(acceso_fatal),
                    RexGmShadowFor(far) != nullptr ? 1 : 0, info_fatal);
      RexSwitchDiagLine(linea_fatal);
    }
  }
  RexSwitchCrashLog(data_abort ? "fallo de acceso a memoria" : "excepcion de CPU", ctx,
                    ctx->sp.x, pc);
  diagAbortWithResult(MAKERESULT(Module_Libnx, 102));
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  // There is nothing to install in the system: __libnx_exception_handler exists
  // once linked. This only records the handler.
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < kMaxHandlerCount - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }
}

}  // namespace rex::arch

/*
 * The libnx entry point.
 *
 * Careful when linking: in libnx this symbol is weak, and a weak reference does not pull an object
 * out of a static library. There is no problem here because it lives in the same object as
 * ExceptionHandler::Install, which the MMIO handler references strongly, so the whole object comes
 * in. If it is ever split out, it has to go directly into the executable, like runtime_switch.cpp.
 */
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  rex::arch::HandleSwitchException(ctx);
}

#endif  // REX_PLATFORM_SWITCH
