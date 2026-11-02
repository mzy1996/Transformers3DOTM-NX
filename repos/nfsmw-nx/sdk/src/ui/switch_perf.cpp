/**
 * @file        switch_perf.cpp
 * @brief       Performance measurement on the console, without a debugger
 *
 * The first boot that reached the title screen ran at 5-10 fps, with two cores at 97% and the
 * GPU at 99.7% according to the Horizon OC monitor. That does not tell what to optimize: it takes
 * knowing which thread spends the CPU and in which function. Horizon has no perf and no debugger
 * on a retail console, so it is measured from inside. Every 10 s a report is appended to
 * <NRO folder>/logs/rex/rex_perfil.log:
 *
 *  - Game FPS: guest presents (IssueSwap).
 *  - Faults per second by type: each one costs an exception, or two with the resume through the
 *    kernel.
 *  - Exact CPU of each thread: the kernel keeps each thread's CPU time (svcGetInfo with
 *    InfoType_ThreadTickCount, 13.0.0+), with its priority, its preferred core and its core mask.
 *  - Where it is spent: sampling at 1 kHz. A high-priority thread pauses a busy thread
 *    (svcSetThreadActivity), reads its pc, its lr and its stack through the x29 chain
 *    (svcGetThreadContext3) and resumes it. The stacks also tell what a thread stopped in the
 *    kernel is waiting for. Addresses come out as image+0x...; they are translated with addr2line
 *    on the ELF.
 *
 * Threads: by wrapping libnx's threadCreate and threadClose (--wrap in rexglue_switch.cmake). Every
 * thread goes through there, pthread ones included, so Mesa's and the SDK's show up too. The name
 * comes from pthread_setname_np (switch_libc_supplement.c).
 *
 * libnx calls: also with --wrap, the calls that cost IPC or walk memory are counted, along with the
 * time the calling thread spends inside: fences (zero-timeout queries and waits), submission
 * kickoffs, new NvMaps (with and without CPU cache), GPU addresses and mappings, armDCacheClean, the
 * window queue and svcSleepThread. Every fence query is at least one ioctl even when the GPU has
 * already finished (see docs/platform-notes.md), so the count per second is needed before changing
 * anything.
 *
 * Careful with the pause: while a thread is paused nothing is done that could take a lock (no
 * malloc, no stdio): if the paused thread held it, this one would wait forever. Between pause and
 * resume there are only system calls. If pausing fails (for example because the SDK had already
 * suspended the thread) it is left alone: resuming it would break that suspension.
 *
 * It goes in the executable (rexglue_switch_startup): the SDK calls RexSwitchPerfCount from
 * libraries, and the wrappers have to be defined before libnx.a enters the link.
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

#include <switch.h>

#include "rex/ui/switch_apm.h"
#include "rex/ui/switch_saltynx.h"
#include "rex/ui/switch_sysclk.h"

extern "C" {
void _start(void);
/* rexglue log folder, ending in '/' (switch_crash_hooks.c). */
const char* RexSwitchLogDir(void);
/*
 * From the guest memory core (guest_memory_switch.cpp): how much backing is
 * committed and how much is mapped in total, counting the 360 mirrors.
 */
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
/* 0 = sin probar, 1 = permisos (paginas vigiladas legibles), 2 = desmapeo. */
int RexGmModoProteccion(void);
/* fix20 diagnostics: watched pages, and guest bytes served by raw aliases. */
size_t RexGmProtectedPages(void);
size_t RexGmRawAliasBytes(void);
/* fix21 diagnostics: watched pages lent their backing to serve a read, and refusals. */
size_t RexGmBorrowedPages(void);
size_t RexGmBorrowRefused(void);
/* fix22 diagnostics: blocks paid for on their first touch, and blocks still only a descriptor. */
size_t RexGmMaterializedChunks(void);
size_t RexGmPendingChunks(void);
size_t RexGmPendingBytes(void);
/* fix23 diagnostics: alignment mode of the window, and whole blocks aliased on a 2 MB boundary. */
int RexGmLargePagesMode(void);
size_t RexGmLargePageAliases(void);
/*
 * fix26: from the memory core and from the log (guest_memory_switch.cpp, logging.cpp), for the watchdog.
 */
u64 RexGmLockHeldTicks(void);
void RexGmLockSnapshot(u64* max_ticks, u64* count);
const char* RexLogFilePath(void);
/* fix27: whether the heap refused a 2 MB backing (1 = the port is on 4 KB from then on), how many
 * refusals there were and how many blocks were left without backing (guest_memory_switch.cpp). */
int RexGmBajaAlineacion(void);
size_t RexGmDenegaciones2m(void);
size_t RexGmSinBacking(void);
size_t RexGmPiscinaBytes(void);
size_t RexGmPiscinaEnUso(void);
size_t RexGmPiscinaCortes(void);
/* fix29: the page-by-page fallback for the blocks the pool and libc both refuse. */
size_t RexGmPaginasEnUso(void);
size_t RexGmBloquesPorPaginas(void);
size_t RexGmPaginasReuso(void);
size_t RexGmPaginasSinSitio(void);
size_t RexGmEntradasPaginas(void);  /* fix31 */
/* fix30: the mapping budget: calls made, kernel entries they cost, what the emergency released. */
size_t RexGmMapeosCodigo(void);
size_t RexGmMapeosCodigoMb(void);
size_t RexGmEntradasCodigo(void);
size_t RexGmMapeosVentana(void);
size_t RexGmMapeosVentanaMb(void);
size_t RexGmEntradasVentana(void);
size_t RexGmLiberacionesVistas(void);
size_t RexGmLiberacionesEventos(void);
int RexGmPresionMapeo(void);
/*
 * fix37: the heap histogram (guest_memory_switch.cpp), at the end of the heartbeat line, so a freeze
 * can be read against the memory the process is holding second by second.
 */
size_t RexGmMallocVivo(void);
size_t RexGmMallocBloques(void);
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset);
void __real_armDCacheClean(void* addr, size_t size);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence);
void __real_svcSleepThread(s64 nano);
}

namespace {

constexpr size_t kMaxThreads = 160;
constexpr u64 kSampleIntervalNs = 1000000ULL;  // 1 ms
constexpr u64 kSegundoNs = 1000000000ULL;      // tick for the console overlays

/* internal resolution published to the overlays. The game sets it when choosing the video mode. */
std::atomic<uint32_t> g_resolucion{(1280u << 16) | 720u};
constexpr u64 kPassiveIntervalNs = 100000000ULL;  // 100 ms, without pausing threads
// Next to the rexglue logs, in <NRO folder>/logs/rex/.
std::string StackFlagPath() {
  return std::string(RexSwitchLogDir()) + "perfil_pilas.flag";
}
constexpr u64 kReportSeconds = 10;
/*
 * Let the game start first. This only delays stack sampling and the first report, which read the
 * SD and pause threads. The console overlays (FPS and resolution) are published from second zero:
 * see the warm-up loop in ProfilerMain.
 */
constexpr u64 kStartDelayNs = 8000000000ULL;
constexpr size_t kMaxSamples = 1 << 15;
constexpr size_t kFrames = 10;
/* 34 = fix21: reads served natively by lending the watched page its backing. */
constexpr unsigned kCounterCount = 35;
// In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup).
std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_perfil.log";
}

/*
 * fix28: the arena, as the watchdog and the report have to see it. arena/free tell memory running
 * out (free near zero) from memory merely fragmented (free high, no big run); keepcost is the top
 * chunk and ordblks the number of pieces the free space is cut into. O(ordblks) per call, once a
 * second and once per report, which is what the pool has to be read against.
 */
struct Arena {
  size_t uord;
  size_t ford;
  size_t keep;
  size_t ord;
};

Arena ArenaAhora() {
  const struct mallinfo mi = mallinfo();
  Arena a{};
  a.uord = static_cast<size_t>(mi.uordblks);
  a.ford = static_cast<size_t>(mi.fordblks);
  a.keep = static_cast<size_t>(mi.keepcost);
  a.ord = static_cast<size_t>(mi.ordblks);
  return a;
}

/*
 * fix26: what the watchdog leaves behind. Its own comment (VigiaMain, at the bottom of the file) says
 * what each field is for.
 */
constexpr u64 kVigiaPeriodoNs = 1000000000ULL;
constexpr u64 kVigiaFpsLento = 3;
constexpr u32 kVigiaSegundosLento = 4;
constexpr u32 kVigiaEpisodiosMax = 8;
constexpr u32 kVigiaLogCadaLatidos = 5;
/*
 * fix27: how many watchdog lines are mirrored to stderr. In fix26 the latido file went through the
 * raw fsdev interface and stayed at zero bytes for the whole run: the watchdog wrote and nothing
 * arrived - and that file is the channel that has to arrive. stderr never failed in this port, so
 * every line goes out through it as well while this budget lasts.
 */
constexpr u32 kLatidoEspejoMax = 900;
std::atomic<u64> g_perf_loop{0};
std::atomic<u32> g_perf_stage{0};
std::atomic<u64> g_report_ok{0};
std::atomic<u64> g_report_fail{0};

struct Slot {
  std::atomic<u32> handle{0};
  char name[32]{};
  // Only the profiler thread touches these:
  u32 seen = 0;
  u64 last_ticks = 0;
  bool busy = true;
  double cpu = 0.0;
};
Slot g_slots[kMaxThreads];

struct Sample {
  u64 pc;
  u64 lr;
  u64 frames[kFrames];  // return addresses along the x29 chain
  u16 slot;
  u64 tick;  // when it was taken (armGetSystemTick), to separate those from long frames
};

/*
 * Long frame windows. The renderer calls RexSwitchPerfTiron with the interval of every frame over
 * 45 ms; the report separates the samples taken inside those intervals and says what each thread was
 * doing right then (section "durante los tirones"). Only works with perfil_pilas.flag.
 */
constexpr size_t kMaxTirones = 256;
/*
 * fix22: stuck-screen detector (see ProfilerMain). After kParonSegundos seconds without a presented
 * frame, invasive sampling switches itself on for kParonMuestreoSegundos seconds: it is the only way
 * to know where the CPU is burning during a freeze, and perfil_pilas.flag cannot be created on a
 * console. The whole thing is bounded, at most kParonForzadosMax bursts per run.
 */
constexpr unsigned kParonSegundos = 4;
constexpr unsigned kParonMuestreoSegundos = 3;
constexpr unsigned kParonForzadosMax = 8;
/* fix22: the freeze detector of ProfilerMain calls it before its definition, which lives in the
 * extern "C" block at the end of the file. */
extern "C" void RexSwitchPerfTiron(u64 inicio, u64 fin);

struct VentanaTiron {
  u64 inicio;
  u64 fin;
};
VentanaTiron g_tirones[kMaxTirones];
std::atomic<u32> g_num_tirones{0};
std::atomic<u32> g_parones{0};
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;

std::atomic<u64> g_counters[kCounterCount];

/*
 * Wrapped libnx calls (see the file header): how many, how much time inside and, where it makes
 * sense, how many bytes. They are constant-initialized, so they are valid even if someone calls
 * before the constructors.
 */
enum : unsigned {
  kFenceConsulta,     // nvFenceWait con espera 0
  kFenceEspera,       // nvFenceWait con espera
  kKickoff,           // nvGpuChannelKickoff
  kNvMapCacheada,     // nvMapCreate with CPU cache
  kNvMapSinCache,     // nvMapCreate without CPU cache
  kReservaDireccion,  // nvAddressSpaceAllocFixed
  kMapeoGpu,          // nvioctlNvhostAsGpu_MapBufferEx
  kCacheClean,        // armDCacheClean
  kQueueBuffer,       // nwindowQueueBuffer
  kDequeueBuffer,     // bqDequeueBuffer
  kSleep0,            // svcSleepThread(0)
  kSleepCeder,        // svcSleepThread(-1 o -2)
  kSleepCorto,        // up to 1 ms
  kSleepLargo,        // over 1 ms
  kLlamadasCount,
};
struct Llamadas {
  std::atomic<u64> n{0};
  std::atomic<u64> ticks{0};
  std::atomic<u64> bytes{0};
};
Llamadas g_llamadas[kLlamadasCount];

inline void Anotar(unsigned id, u64 desde, u64 bytes = 0) {
  const u64 ticks = armGetSystemTick() - desde;
  Llamadas& l = g_llamadas[id];
  l.n.fetch_add(1, std::memory_order_relaxed);
  l.ticks.fetch_add(ticks, std::memory_order_relaxed);
  if (bytes) {
    l.bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

/*
 * Automatic A/B tests. Each report (10 s) removes one part of the GPU work: those commands (draw or
 * dispatch) stop being recorded, but everything else stays the same, so the state does not break.
 * The image looks wrong while the mode lasts; that is expected. If with "sin nada" it is just as
 * slow, the cost is not in the work but in something fixed (submissions, driver).
 */
struct Mode {
  u32 mask;
  const char* name;
};
constexpr Mode kModes[] = {
    {0, "normal"},
    {1, "sin dibujados"},
    {2, "sin transferencias"},
    {4, "sin resolves"},
    {8, "sin cargas de texturas"},
    {16, "sin efecto de presentacion"},
    {32, "sin despacho por baldosas"},
    {63, "sin nada"},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/*
 * A/B tests, started by hand
 *
 * They used to be enabled with this constant and rotate on their own from startup. That is no good:
 * the game takes a while to reach a race, and on the way it spent its time in modes with drawing
 * turned off. Once they were left on by mistake and the game did not even reach the title screen.
 *
 * Now the player starts the rotation with L+R+ZL from inside the race, which is the only place where
 * the measurement is useful. From then on it removes one part of the GPU work per report, goes once
 * through the eight modes and returns to "normal".
 *
 * How to read it. If with "sin nada" the game is just as slow, the cost is not in the GPU work but
 * in something fixed: submissions, the driver, or the guest itself. If FPS shoots up, the mode where
 * it rises most points to the culprit.
 */
std::atomic<bool> g_ab_activas{false};
std::atomic<u32> g_skip_mask{0};

extern "C" void RexSwitchPerfToggleAb(void) {
  const bool nuevo = !g_ab_activas.load(std::memory_order_relaxed);
  g_ab_activas.store(nuevo, std::memory_order_relaxed);
  if (!nuevo) {
    g_skip_mask.store(0, std::memory_order_relaxed);
  }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory). Our
 * guest memory model maps each chunk several times (the shadow and once per 360
 * view), so that limit is the real ceiling, not free memory. When it runs out,
 * even committing 4 KB fails with 2001-0103 (resource exhausted).
 */
void LimiteDeMapeo(u64* usado_mb, u64* tope_mb, u64* proceso_mb, u64* total_mb) {
  *usado_mb = *tope_mb = *proceso_mb = *total_mb = 0;
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *proceso_mb = v >> 20;
  }
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *total_mb = v >> 20;
  }
Handle reslimit = INVALID_HANDLE;
  const Result rc = svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
  if (R_FAILED(rc)) {
    /*
     * fix32: say it once. A "0/0" in the report is not "no limit"; it is this read failing, and the
     * run that froze here had a 0/0 while the drawing of the budget mattered. Whoever reads the next
     * report now sees which of the two it is, and with what code.
     */
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      std::fprintf(stderr,
                   "[fix32] limite de mapeo: svcGetInfo(ResourceLimit) rc=0x%08X (no se pudo leer; "
                   "tope no confirmado)\n",
                   static_cast<unsigned>(rc));
    }
    return;
  }
  reslimit = static_cast<Handle>(v);
  s64 actual = 0, tope = 0;
  if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&actual, reslimit, LimitableResource_Memory))) {
    *usado_mb = static_cast<u64>(actual) >> 20;
  }
  if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&tope, reslimit, LimitableResource_Memory))) {
    *tope_mb = static_cast<u64>(tope) >> 20;
  }
  svcCloseHandle(reslimit);
}

u64 g_base = 0, g_text_lo = 0, g_text_hi = 0;
Thread g_thread;
Thread g_vigia;

void Register(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = 0;
    if (s.handle.compare_exchange_strong(expected, h)) {
      s.name[0] = 0;
      return;
    }
  }
}

void Unregister(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = h;
    if (s.handle.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

bool InText(u64 addr) { return addr >= g_text_lo && addr < g_text_hi; }

void PrintAddr(FILE* f, u64 addr) {
  if (InText(addr)) {
    std::fprintf(f, "imagen+0x%" PRIx64, addr - g_base);
  } else {
    std::fprintf(f, "0x%016" PRIx64, addr);
  }
}

/*
 * Is the pc right after a system call? Then the thread is waiting in the kernel,
 * not using CPU.
 */
bool AfterSvc(u64 pc) {
  if (!InText(pc) || pc < g_text_lo + 4) {
    return false;
  }
  const u32 insn = *reinterpret_cast<const u32*>(pc - 4);
  return (insn & 0xFFE0001Fu) == 0xD4000001u;
}

struct Count {
  u64 addr;
  u32 n;
};

std::vector<Count> Histogram(std::vector<u64>& addrs) {
  std::sort(addrs.begin(), addrs.end());
  std::vector<Count> out;
  for (u64 a : addrs) {
    if (out.empty() || out.back().addr != a) {
      out.push_back({a, 1});
    } else {
      ++out.back().n;
    }
  }
  std::sort(out.begin(), out.end(), [](const Count& x, const Count& y) { return x.n > y.n; });
  return out;
}

void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  /*
   * fix26: stage 4 of the watchdog. This is the first thing the report does and it is the phase that
   * can block (one svcGetInfo per live thread).
   */
  g_perf_stage.store(4, std::memory_order_relaxed);
  const double seconds = double(elapsed_ticks) / double(tick_freq);

  u64 counters_now[kCounterCount];
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_now[i] = g_counters[i].load(std::memory_order_relaxed);
  }

  // CPU of each thread over the interval, as % of one core.
  std::vector<size_t> order;
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    const u32 h = s.handle.load();
    if (!h) {
      s.seen = 0;
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    if (s.seen != h) {  // new thread: no interval yet
      s.seen = h;
      s.last_ticks = ticks;
      s.busy = true;
      s.cpu = 0.0;
      continue;
    }
    s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed_ticks);
    s.last_ticks = ticks;
    /*
     * All threads are sampled, including those that use no CPU. In a hang caused by
     * a wait, the thread that matters is exactly that one: the stopped one.
     */
    s.busy = true;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

  /*
   * fix26: a report that could not open its file used to be skipped in silence, which is exactly what
   * cannot be seen in a run that stops: if the profiler thread stops writing, the file says nothing
   * about it and there is no way to tell "fopen failed" from "the thread is not running".
   * It is counted (the watchdog prints the counter), said once on stderr, retried once, and on failure
   * the counter baseline is still updated so that the next report's per-second rates stay right. The
   * samples are kept: they are still good evidence of where the CPU was.
   */
  g_perf_stage.store(5, std::memory_order_relaxed);
  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    g_report_fail.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> avisado{false};
    if (!avisado.exchange(true, std::memory_order_relaxed)) {
      std::fprintf(stderr, "[fix26] informes: no se pudo abrir %s (%s), se sigue intentando\n",
                   ReportPath().c_str(), std::strerror(errno));
    }
    __real_svcSleepThread(20000000ULL);
    f = std::fopen(ReportPath().c_str(), "a");
    if (!f) {
      for (unsigned i = 0; i < kCounterCount; ++i) {
        counters_last[i] = counters_now[i];
      }
      g_perf_stage.store(1, std::memory_order_relaxed);
      return;
    }
  }
  g_report_ok.fetch_add(1, std::memory_order_relaxed);
  g_perf_stage.store(6, std::memory_order_relaxed);

  u64 lim_usado = 0, lim_tope = 0, proc_usado = 0, proc_total = 0;
  LimiteDeMapeo(&lim_usado, &lim_tope, &proc_usado, &proc_total);
  const Arena arena_rep = ArenaAhora();

  double total_cpu = 0.0;
  for (size_t i : order) {
    total_cpu += g_slots[i].cpu;
  }
  std::fprintf(f,
               "==== %.1f s | modo: %s | juego %.1f fps | CPU total %.0f%% (400%% = 4 nucleos) | fallos/s: "
               "lectura emulada %.0f, manejador SDK %.0f, reintento emulado %.0f, SEH %.0f, "
               "fisica confirmada %.0f, vistas %.0f | invitado %zu/%zu MB (respaldo/mapeado) | "
               "limite de mapeo %llu/%llu MB, proceso %llu/%llu MB | "
               "muestras %zu | vigilancia: %s | samplers: %.0f nuevos/s, %.0f parones/s | "
"zero-load %.0f/s, alias fisico %.0f/s, relleno nativo %.0f/s | "
               "lectura nativa %.0f/s | vigiladas %zu pags, crudas %zu MB, prestadas %zu pags "
               "(%zu rechazadas) | materializados %zu, pendientes %zu MB (%zu bloques), parones %u | "
               "paginas grandes modo %d, bloques 2 MB %zu | arena usado %zu MB libre %zu MB tope "
               "%zu KB trozos %zu | piscina tomada %zu MB, en uso %zu MB, cortes %zu | paginas en "
               "uso %zu, bloques por paginas %zu, en cache %zu, sin sitio %zu | mapeos: codigo %zu "
               "(~%zu entradas, %zu MB), ventana %zu (~%zu entradas, %zu MB) | liberadas %zu "
               "vistas crudas en %zu eventos, presion %d\n",
               seconds, mode, double(counters_now[0] - counters_last[0]) / seconds, total_cpu,
               double(counters_now[1] - counters_last[1]) / seconds,
               double(counters_now[2] - counters_last[2]) / seconds,
               double(counters_now[3] - counters_last[3]) / seconds,
               double(counters_now[4] - counters_last[4]) / seconds,
               double(counters_now[17] - counters_last[17]) / seconds,
               double(counters_now[18] - counters_last[18]) / seconds,
               RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20,
               (u64)lim_usado, (u64)lim_tope, (u64)proc_usado, (u64)proc_total, g_sample_count,
                (RexGmModoProteccion() == 1   ? "permisos (paginas legibles)"
                 : RexGmModoProteccion() == 2 ? "desmapeo (cada lectura falla)"
                                              : "sin probar"),
                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds,
                double(counters_now[29] - counters_last[29]) / seconds,
                double(counters_now[30] - counters_last[30]) / seconds,
double(counters_now[33] - counters_last[33]) / seconds,
                double(counters_now[34] - counters_last[34]) / seconds,
                RexGmProtectedPages(), RexGmRawAliasBytes() >> 20,
                RexGmBorrowedPages(), RexGmBorrowRefused(), RexGmMaterializedChunks(),
                RexGmPendingBytes() >> 20, RexGmPendingChunks(),
                (unsigned)g_parones.load(std::memory_order_relaxed), RexGmLargePagesMode(),
                RexGmLargePageAliases(), arena_rep.uord >> 20, arena_rep.ford >> 20,
                arena_rep.keep >> 10, arena_rep.ord, RexGmPiscinaBytes() >> 20,
                RexGmPiscinaEnUso() >> 20, RexGmPiscinaCortes(), RexGmPaginasEnUso(),
                RexGmBloquesPorPaginas(), RexGmPaginasReuso(), RexGmPaginasSinSitio(),
                RexGmMapeosCodigo(), RexGmEntradasCodigo(), RexGmMapeosCodigoMb(),
                RexGmMapeosVentana(), RexGmEntradasVentana(), RexGmMapeosVentanaMb(),
                RexGmLiberacionesVistas(), RexGmLiberacionesEventos(), RexGmPresionMapeo());
  // Work the game sends to the GPU, per presented frame. One screen is
  // 1280x720 = 921,600 pixels.
  const auto delta = [&](unsigned id) { return double(counters_now[id] - counters_last[id]); };
  const double frames = delta(0);
  // Mix clipping before conversion to 16 bits (21 samples above 1.0, 22 buffers with a peak of 0.98 or
  // more, 23 maximum peak of the interval in ten-thousandths) and the console mode, for the crackling
  // in handheld mode.
  const u64 pico_audio = g_counters[23].exchange(0, std::memory_order_relaxed);
  std::fprintf(f,
               "     audio: %.0f bloques de cliente mezclados, %.0f solicitudes sin datos, "
               "%.0f/%.0f buffers audout con PCM no nulo, %.0f muestras saturadas (mas de 1,0 antes de recortar), "
               "%.0f buffers con pico de 0,98 o mas, pico maximo %.3f | consola en modo %s\n",
               delta(24), delta(25), delta(26), delta(27), delta(21), delta(22), double(pico_audio) / 10000.0,
               rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console)
                   ? "sobremesa"
                   : "portatil");
  // Reverse-NX state as is, without interpretation. A whole test session was wasted because its
  // overlay said "Docked" while "Controlled by system" was Yes, and in that case its mode does not
  // rule: it only mirrors the console's. This shows at a glance which of the two is happening.
  {
    const auto reverse = rex::ui::switch_saltynx::EstadoReverse();
    const bool real = appletGetOperationMode() == AppletOperationMode_Console;
    if (!reverse.hay) {
      std::fprintf(f, "     Reverse-NX: sin bloque (consola %s, manda ella)\n",
                   real ? "en la base" : "en las manos");
    } else {
      std::fprintf(f,
                   "     Reverse-NX: dice %s, manda %s (Controlled by system %s), el juego ha preguntado: %s; "
                   "consola de verdad %s -> se obedece %s\n",
                   reverse.en_base ? "sobremesa" : "portatil", reverse.por_defecto ? "la consola" : "Reverse-NX",
                   reverse.por_defecto ? "Yes" : "No", reverse.plugin_activo ? "si" : "no",
                   real ? "en la base" : "en las manos",
                   rex::ui::switch_saltynx::ModoBase(real) ? "sobremesa" : "portatil");
    }
  }
  // Real console clocks (clkrst, 8.0.0+) and the cores the process may use. It tells whether the
  // session ran overclocked and with which CPU limit it was measured: performance tests run without
  // overclock.
  {
    static bool iniciado = false;
    static bool hay_clkrst = false;
    static ClkrstSession sesiones[3]{};
    static u64 mascara_nucleos = 0;
    if (!iniciado) {
      iniciado = true;
      if (R_FAILED(svcGetInfo(&mascara_nucleos, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        mascara_nucleos = 0;
      }
      if (R_SUCCEEDED(clkrstInitialize())) {
        static const PcvModuleId modulos[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        hay_clkrst = true;
        for (unsigned i = 0; i < 3; ++i) {
          if (R_FAILED(clkrstOpenSession(&sesiones[i], modulos[i], 3))) {
            hay_clkrst = false;
          }
        }
      }
    }
    u32 hz[3] = {0, 0, 0};
    if (hay_clkrst) {
      for (unsigned i = 0; i < 3; ++i) {
        clkrstGetClockRate(&sesiones[i], &hz[i]);
      }
    }
    unsigned nucleos = 0;
    for (unsigned i = 0; i < 64; ++i) {
      nucleos += unsigned((mascara_nucleos >> i) & 1);
    }
    // SoC and board temperature (ts service, sessions on [10.0.0+]). The system lowers clocks around
    // 70 degrees and Erista gets there before Mariko: without this, "it is slow" cannot be told apart
    // from "it is hot". On the OLED model the board sensor reads wrong, because it sits next to the
    // charging IC.
    static bool ts_iniciado = false;
    static bool hay_ts = false;
    static TsSession ts_soc{}, ts_placa{};
    if (!ts_iniciado) {
      ts_iniciado = true;
      if (R_SUCCEEDED(tsInitialize())) {
        hay_ts = R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal)) &&
                 R_SUCCEEDED(tsOpenSession(&ts_placa, TsDeviceCode_LocationInternal));
      }
    }
    float grados_soc = 0.0f, grados_placa = 0.0f;
    if (hay_ts) {
      tsSessionGetTemperature(&ts_soc, &grados_soc);
      tsSessionGetTemperature(&ts_placa, &grados_placa);
    }
    std::fprintf(f,
                 "     relojes: CPU %.1f MHz, GPU %.1f MHz, memoria %.1f MHz%s | nucleos del proceso %u "
                 "(mascara 0x%llX) | SoC %.1f C, placa %.1f C%s\n",
                 double(hz[0]) / 1.0e6, double(hz[1]) / 1.0e6, double(hz[2]) / 1.0e6,
                 hay_clkrst ? "" : " (clkrst no disponible)", nucleos,
                 (unsigned long long)mascara_nucleos, double(grados_soc), double(grados_placa),
                 hay_ts ? "" : " (ts no disponible)");
  }
  // libnx calls: per second, with the ms per second the calling threads spend inside.
  {
    static u64 ultimo[kLlamadasCount][3];
    double n[kLlamadasCount], ms[kLlamadasCount], mb[kLlamadasCount];
    for (unsigned i = 0; i < kLlamadasCount; ++i) {
      const u64 vn = g_llamadas[i].n.load(std::memory_order_relaxed);
      const u64 vt = g_llamadas[i].ticks.load(std::memory_order_relaxed);
      const u64 vb = g_llamadas[i].bytes.load(std::memory_order_relaxed);
      n[i] = double(vn - ultimo[i][0]) / seconds;
      ms[i] = double(vt - ultimo[i][1]) * 1000.0 / double(tick_freq) / seconds;
      mb[i] = double(vb - ultimo[i][2]) / 1048576.0 / seconds;
      ultimo[i][0] = vn;
      ultimo[i][1] = vt;
      ultimo[i][2] = vb;
    }
    std::fprintf(f,
                 "     libnx por segundo (llamadas y ms dentro): fences consultadas %.0f (%.1f ms), esperadas %.0f "
                 "(%.1f ms) | kickoff %.0f (%.1f ms) | NvMap nuevos %.1f con cache (%.1f MB) y %.1f sin cache "
                 "(%.1f MB), %.1f ms | direcciones de GPU %.1f (%.1f ms), mapeos %.1f (%.1f MB, %.1f ms) | "
                 "armDCacheClean %.0f (%.1f MB, %.1f ms) | cola de la ventana %.1f (%.1f ms), sacar buffer %.1f "
                 "(%.1f ms) | svcSleepThread: 0 %.0f, ceder %.0f, hasta 1 ms %.0f (%.1f ms), mas %.0f\n",
                 n[kFenceConsulta], ms[kFenceConsulta], n[kFenceEspera], ms[kFenceEspera], n[kKickoff],
                 ms[kKickoff], n[kNvMapCacheada], mb[kNvMapCacheada], n[kNvMapSinCache], mb[kNvMapSinCache],
                 ms[kNvMapCacheada] + ms[kNvMapSinCache], n[kReservaDireccion], ms[kReservaDireccion],
                 n[kMapeoGpu], mb[kMapeoGpu], ms[kMapeoGpu], n[kCacheClean], mb[kCacheClean], ms[kCacheClean],
                 n[kQueueBuffer], ms[kQueueBuffer], n[kDequeueBuffer], ms[kDequeueBuffer], n[kSleep0],
                 n[kSleepCeder], n[kSleepCorto], ms[kSleepCorto], n[kSleepLargo]);
  }
  if (frames > 0) {
    std::fprintf(f,
                 "     por fotograma: %.0f dibujados | tijera %.1f pantallas | %.1f envios | "
                 "%.1f resolves (%.2f pantallas) | transferencias: %.1f llamadas, %.1f render "
                 "targets, %.1f dibujados | %.1f texturas cargadas | %.2f MB de memoria "
                 "compartida subida | pipelines creados en el intervalo: %.0f\n",
                 delta(5) / frames, delta(6) / frames / 921600.0, delta(7) / frames,
                 delta(9) / frames, delta(10) / frames / 921600.0, delta(11) / frames,
                 delta(12) / frames, delta(13) / frames, delta(14) / frames,
                 delta(15) / frames / 1048576.0, delta(16));
    std::fprintf(f,
                 "     dibujados por ancho de superficie: 1600+ %.0f | 1280-1599 %.0f | "
                 "640-1279 %.0f | 256-639 %.0f | menos de 256 %.0f\n",
                 delta(28) / frames, delta(29) / frames, delta(30) / frames, delta(31) / frames,
                 delta(32) / frames);
  }

  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = counters_now[i];
  }

  std::vector<Sample> samples(g_samples, g_samples + g_sample_count);
  g_sample_count = 0;
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.slot < b.slot; });

  // What each thread was doing during the long frames of this interval.
  {
    const u32 nt = std::min<u32>(g_num_tirones.exchange(0), kMaxTirones);
    if (nt && !samples.empty()) {
      std::vector<VentanaTiron> v(g_tirones, g_tirones + nt);
      std::sort(v.begin(), v.end(), [](const VentanaTiron& a, const VentanaTiron& b) { return a.inicio < b.inicio; });
      double ms_total = 0.0;
      for (const VentanaTiron& w : v) {
        ms_total += double(w.fin - w.inicio) * 1000.0 / double(tick_freq);
      }
      const auto dentro = [&v](u64 t) {
        auto it = std::upper_bound(v.begin(), v.end(), t, [](u64 x, const VentanaTiron& w) { return x < w.inicio; });
        return it != v.begin() && t <= (it - 1)->fin;
      };
      std::fprintf(f, "\n== durante los tirones: %u fotogramas de mas de 45 ms (%.0f ms en total) ==\n", nt, ms_total);
      for (size_t i : order) {
        std::vector<u64> pcs;
        std::vector<const Sample*> todas;
        size_t en_svc = 0;
        for (const Sample& m : samples) {
          if (m.slot != i || !dentro(m.tick)) {
            continue;
          }
          todas.push_back(&m);
          if (AfterSvc(m.pc)) {
            ++en_svc;
          } else {
            pcs.push_back(m.pc);
          }
        }
        if (todas.size() < 5) {
          continue;
        }
        const Slot& s = g_slots[i];
        std::fprintf(f, "-- hilo \"%s\": %zu muestras en los tirones, %.0f%% esperando en el kernel\n",
                     s.name[0] ? s.name : "?", todas.size(), double(en_svc) * 100.0 / double(todas.size()));
        const auto h = Histogram(pcs);
        for (size_t k = 0; k < h.size() && k < 25; ++k) {
          std::fprintf(f, "   %5.1f%%  pc ", double(h[k].n) * 100.0 / double(todas.size()));
          PrintAddr(f, h[k].addr);
          std::fputc('\n', f);
        }
        std::sort(todas.begin(), todas.end(), [](const Sample* a, const Sample* b) {
          return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
        });
        std::vector<std::pair<const Sample*, u32>> pilas;
        for (const Sample* m : todas) {
          if (pilas.empty() || std::memcmp(pilas.back().first->frames, m->frames, sizeof(m->frames)) != 0) {
            pilas.push_back({m, 1});
          } else {
            ++pilas.back().second;
          }
        }
        std::sort(pilas.begin(), pilas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < pilas.size() && k < 10; ++k) {
          std::fprintf(f, "   %5.1f%%  pila", double(pilas[k].second) * 100.0 / double(todas.size()));
          for (size_t j = 0; j < kFrames && pilas[k].first->frames[j]; ++j) {
            std::fputs(j == 0 ? " " : " <- ", f);
            PrintAddr(f, pilas[k].first->frames[j]);
          }
          std::fputc('\n', f);
        }
      }
    }
  }

  for (size_t i : order) {
    const Slot& s = g_slots[i];
    /*
     * Threads that use no CPU are listed too, with less detail: in a hang their
     * stacks are needed to know what they are waiting for.
     */
    const bool activo = s.cpu >= 1.0;
    u64 tid = 0;
    svcGetThreadId(&tid, s.handle.load());
    // Horizon priority, preferred core and allowed cores: who can take the CPU from whom.
    s32 prioridad = -1;
    s32 preferido = -1;
    u64 mascara = 0;
    svcGetThreadPriority(&prioridad, s.handle.load());
    svcGetThreadCoreMask(&preferido, &mascara, s.handle.load());
    std::fprintf(f,
                 "\n-- hilo %" PRIu64 " \"%s\": CPU %.1f%% | prioridad 0x%X, nucleo preferido %d, "
                 "mascara 0x%llX\n",
                 tid, s.name[0] ? s.name : "?", s.cpu, static_cast<unsigned>(prioridad),
                 static_cast<int>(preferido), static_cast<unsigned long long>(mascara));

    auto lo = std::lower_bound(samples.begin(), samples.end(), i,
                               [](const Sample& a, size_t v) { return a.slot < v; });
    auto hi = std::upper_bound(samples.begin(), samples.end(), i,
                               [](size_t v, const Sample& a) { return v < a.slot; });
    const size_t n = size_t(hi - lo);
    if (n == 0) {
      std::fprintf(f, "   sin muestras\n");
      continue;
    }
    std::vector<u64> pcs, lrs;
    size_t in_svc = 0;
    for (auto it = lo; it != hi; ++it) {
      if (AfterSvc(it->pc)) {
        ++in_svc;
        continue;
      }
      pcs.push_back(it->pc);
      lrs.push_back(it->lr);
    }
    std::fprintf(f, "   %zu muestras, %.0f%% esperando en el kernel\n", n,
                 double(in_svc) * 100.0 / double(n));
    const auto pc_hist = Histogram(pcs);
    const size_t busy = pcs.size();
    for (size_t k = 0; k < pc_hist.size() && k < (activo ? 80u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, pc_hist[k].addr);
      std::fputc('\n', f);
    }
    const auto lr_hist = Histogram(lrs);
    for (size_t k = 0; k < lr_hist.size() && k < (activo ? 40u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  lr ", double(lr_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, lr_hist[k].addr);
      std::fputc('\n', f);
    }

    // Full stacks, waits included: they say what the thread is waiting for.
    std::vector<const Sample*> by_stack;
    for (auto it = lo; it != hi; ++it) {
      by_stack.push_back(&*it);
    }
    std::sort(by_stack.begin(), by_stack.end(), [](const Sample* a, const Sample* b) {
      return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
    });
    struct StackCount {
      const Sample* s;
      u32 n;
    };
    std::vector<StackCount> stacks;
    for (const Sample* s : by_stack) {
      if (stacks.empty() ||
          std::memcmp(stacks.back().s->frames, s->frames, sizeof(s->frames)) != 0) {
        stacks.push_back({s, 1});
      } else {
        ++stacks.back().n;
      }
    }
    std::sort(stacks.begin(), stacks.end(),
              [](const StackCount& a, const StackCount& b) { return a.n > b.n; });
    for (size_t k = 0; k < stacks.size() && k < (activo ? 30u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pila", double(stacks[k].n) * 100.0 / double(n));
      for (size_t j = 0; j < kFrames && stacks[k].s->frames[j]; ++j) {
        std::fputs(j == 0 ? " " : " <- ", f);
        PrintAddr(f, stacks[k].s->frames[j]);
      }
      std::fputc('\n', f);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
  g_perf_stage.store(1, std::memory_order_relaxed);
}

/*
 * The one-second tick for the console overlays, separate from the profile loop.
 *
 * Two loops use it: the warm-up one (from second zero) and the normal one (from kStartDelayNs on).
 * When it lived only in the normal loop there was neither FPS nor resolution until 8 seconds had
 * passed: they appeared with the EA logo instead of with the Most Wanted logo, which comes much
 * earlier.
 *
 * It is cheap: two counter reads and a few bytes in shared memory. Actualizar also re-attaches the
 * block if it is missing, so the overlay can be opened at any time.
 */
struct TicOverlay {
  u64 tic = 0;
  u64 presentados = 0;
  double media[10] = {};
  size_t pos = 0;

  void Arrancar(u64 ahora) {
    tic = ahora;
    presentados = g_counters[0].load(std::memory_order_relaxed);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolucion
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Actualizar(0.0, 0.0, res >> 16, res & 0xFFFF, presentados);
  }

  // Once per second. `con_relojes` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Paso(u64 ahora, u64 freq, bool con_relojes) {
    if (ahora - tic < freq) {
      return;
    }
    const double segundos = double(ahora - tic) / double(freq);
    const u64 ahora_presentados = g_counters[0].load(std::memory_order_relaxed);
    const double fps = segundos > 0.0 ? double(ahora_presentados - presentados) / segundos : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double suma = 0.0;
    size_t cuantos = 0;
    for (double v : media) {
      if (v > 0.0) {
        suma += v;
        ++cuantos;
      }
    }
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Actualizar(fps, cuantos ? suma / double(cuantos) : fps, res >> 16, res & 0xFFFF,
                                        ahora_presentados);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (con_relojes) {
      const bool sobremesa_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool sobremesa = rex::ui::switch_saltynx::ModoBase(sobremesa_real);
      rex::ui::switch_sysclk::SeguirModo(sobremesa, sobremesa_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when con_relojes is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmVigilar();
    presentados = ahora_presentados;
    tic = ahora;
  }
};

void ProfilerMain(void*) {
  const u64 tick_freq = armGetSystemTickFreq();

  /*
   * The overlay first, without waiting for kStartDelayNs. Those 8 seconds are for stack sampling and
   * the first report (which read the SD and pause threads), not for this: publishing FPS and
   * resolution is a few bytes in shared memory, and it has to show already at the Most Wanted logo.
   * During warm-up the tick keeps running every second, so attaching the block is also retried if
   * SaltyNX is slow.
   */
  TicOverlay overlay;
  const u64 inicio = armGetSystemTick();
  overlay.Arrancar(inicio);
  const u64 calentamiento = (kStartDelayNs / 1000000ULL) * tick_freq / 1000ULL;
  while (armGetSystemTick() - inicio < calentamiento) {
    __real_svcSleepThread(kPassiveIntervalNs);
    overlay.Paso(armGetSystemTick(), tick_freq, false);
  }

  // Invasive sampling is only enabled explicitly, for diagnostics.
  // The normal mode keeps FPS, CPU and counters, without stopping the game.
  bool sample_stacks = false;
  if (FILE* flag = std::fopen(StackFlagPath().c_str(), "r")) {
    sample_stacks = true;
    std::fclose(flag);
  }
  /* fix22: was invasive sampling asked for by hand? the detector must not switch it off. */
  const bool flag_pilas = sample_stacks;

  u64 counters_last[kCounterCount]{};
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
    std::fprintf(f, "imagen 0x%016" PRIx64 ", codigo hasta 0x%016" PRIx64 "\n\n", g_base,
                 g_text_hi);
    std::fprintf(f, "Muestreo de pilas: %s\n", sample_stacks ? "activo (1 ms)" : "desactivado");
    std::fclose(f);
  }

  // The console overlays read from SaltyNX's shared memory. That is already running from second
  // zero (see the warm-up loop above); only the normal loop continues here.

  // A short first report just to set the starting CPU time.
  u64 report_start = armGetSystemTick();
  Report(1, tick_freq, counters_last, "arranque");
  report_start = armGetSystemTick();

  size_t rr = 0;
  size_t mode_index = 0;
  while (true) {
    __real_svcSleepThread(sample_stacks ? kSampleIntervalNs : kPassiveIntervalNs);

    /*
     * fix22: stuck screen. The 10 s report was not enough to catch the loading freeze: it showed
     * 0.0 fps and the main thread at 98.9% of CPU, but with sampling off nothing said where those
     * cycles were going (the guest walk over its physical window, throttled by the mapping
     * limiter). If frames stop being presented, sampling turns itself on for a bounded window and
     * the stall is registered as a long frame, so the report prints its PCs and stacks in
     * "durante los tirones"; it is off again as soon as frames come back.
     */
    g_perf_stage.store(7, std::memory_order_relaxed);
    {
      static u64 tic_lento = 0;
      static u64 pres_lento = 0;
      static u64 paron_ini = 0;
      static u32 seg_sin = 0;
      static u32 forzados = 0;
      static bool registrado = true;
      const u64 ahora_det = armGetSystemTick();
      const u64 pres = g_counters[0].load(std::memory_order_relaxed);
      if (tic_lento == 0) {
        tic_lento = ahora_det;
        pres_lento = pres;
      } else if (ahora_det - tic_lento >= tick_freq) {
        tic_lento = ahora_det;
        /*
         * fix26: the criterion used to be "not a single frame presented in the whole second". A screen
         * that presents one or two frames and spends the rest of the time stuck is the same problem and
         * was not being registered; the runs that stop do it after falling to 1.0 and then 0.0 fps, and
         * the loading screen itself sits at 7-10 fps. kVigiaFpsLento covers both, and it is the same
         * criterion the watchdog counts with.
         */
        if (pres - pres_lento <= kVigiaFpsLento) {
          ++seg_sin;
          if (seg_sin == kParonSegundos && forzados < kParonForzadosMax) {
            ++forzados;
            registrado = false;
            paron_ini = ahora_det - static_cast<u64>(kParonSegundos) * tick_freq;
            sample_stacks = true;
            g_parones.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[fix22] paron: %u s sin fotogramas, muestreo forzado\n", seg_sin);
          }
          /*
           * fix24: dump the freeze on the spot. Registering the stall and waiting for the next
           * 10 s report was too late: the console is switched off before that report is written,
           * and the run ends with no evidence of where the CPU was. One second after the stall
           * starts there is already a second of samples inside it, and Report takes the locked
           * frames out of g_samples while they are still there ("durante los tirones").
           * No thread is paused here (the pause lives in the sampling block below), so the
           * stdio of Report is safe.
           */
          if (seg_sin == kParonSegundos + 1) {
            if (!registrado) {
              RexSwitchPerfTiron(paron_ini, ahora_det);
              registrado = true;
              std::fprintf(stderr, "[fix24] paron: volcado inmediato a los %u s\n", seg_sin);
              Report(ahora_det - report_start, tick_freq, counters_last, "tiron");
              report_start = armGetSystemTick();
            }
          }
          if (seg_sin == kParonSegundos + kParonMuestreoSegundos) {
            if (!flag_pilas) sample_stacks = false;
            if (!registrado) {
              RexSwitchPerfTiron(paron_ini, ahora_det);
              registrado = true;
            }
          }
        } else {
          if (seg_sin >= kParonSegundos && !registrado) {
            RexSwitchPerfTiron(paron_ini, ahora_det);
            registrado = true;
            std::fprintf(stderr, "[fix22] paron fin: %u s (tiron registrado)\n", seg_sin);
          }
          if (!flag_pilas) sample_stacks = false;
          seg_sin = 0;
        }
        pres_lento = pres;
      }
    }

    g_perf_stage.store(2, std::memory_order_relaxed);
    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {
      const size_t idx = rr;
      rr = (rr + 1) % kMaxThreads;
      Slot& s = g_slots[idx];
      const u32 h = s.handle.load();
      if (!h || !s.busy) {
        continue;
      }
      if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        break;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, h);
      if (R_SUCCEEDED(rc) && g_sample_count < kMaxSamples) {
        Sample& out = g_samples[g_sample_count++];
        out = {};
        out.pc = ctx.pc.x;
        out.lr = ctx.lr;
        out.slot = static_cast<u16>(idx);
        out.tick = armGetSystemTick();
        // The stack, with the thread still paused: it is our own memory and
        // there are no locks. Only frames within the region of its sp.
        MemoryInfo smi;
        u32 spi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
          const u64 lo = smi.addr, hi = smi.addr + smi.size;
          u64 fp = ctx.fp;
          for (size_t k = 0; k < kFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
            out.frames[k] = reinterpret_cast<const u64*>(fp)[1];
            const u64 next = reinterpret_cast<const u64*>(fp)[0];
            if (next <= fp) {
              break;
            }
            fp = next;
          }
        }
      }
      svcSetThreadActivity(h, ThreadActivity_Runnable);
      break;
    }

    g_perf_stage.store(3, std::memory_order_relaxed);
    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).
    overlay.Paso(now, tick_freq, true);
    if (now - report_start >= kReportSeconds * tick_freq) {
      Report(now - report_start, tick_freq, counters_last, kModes[mode_index].name);
      if (g_ab_activas.load(std::memory_order_relaxed)) {
        mode_index = (mode_index + 1) % kModeCount;
        g_skip_mask.store(kModes[mode_index].mask, std::memory_order_relaxed);
        if (mode_index == 0) {
          // Full cycle: it turns itself off and the game looks right again.
          g_ab_activas.store(false, std::memory_order_relaxed);
        }
      }
      report_start = armGetSystemTick();
    }
    /*
     * fix26: the loop counter is the watchdog's proof that this thread is still running, and 1 is the
     * stage it should be in at the top of the loop (sleeping its 100 ms or about to).
     */
    g_perf_stage.store(1, std::memory_order_relaxed);
    g_perf_loop.fetch_add(1, std::memory_order_relaxed);
  }
}

/*
 * fix26: watchdog.
 *
 * The last runs ended with a screen that stops, and what they left behind could not say why: the report
 * is written every 10 s by the profiler thread, so if that thread stops writing there is no way to know
 * whether it died, whether it is blocked opening the file, whether the card is blocked or whether the
 * whole process stopped; and the stall detector of ProfilerMain also depended on a report of its own
 * succeeding.
 *
 * This thread exists only to leave evidence that depends on nothing else. It runs at priority 0x29,
 * above the profiler (0x2A) and everything in the game, and every second appends a line to
 * <NRO folder>/logs/rex/rex_latido.log through its own raw fsdev handle: opened once, flushed on every
 * write, never closed. No stdio, no spdlog and no lock is used here: sharing a lock with a thread that
 * is suspended while holding it is exactly what must not happen (see the mutex note in
 * guest_memory_switch.cpp).
 *
 * Each line carries:
 *   t         seconds since this thread started, fps of the last second, pres = total presented frames.
 *   lento     consecutive seconds with kVigiaFpsLento frames or fewer: the run of a freeze.
 *   perf_loop / perf_st
 *             iterations of the profiler loop, how many since the previous line, and the stage it is in
 *             right now: 0 sleeping, 1 loop, 2 sampling threads, 3 overlay, 4 report: CPU per thread,
 *             5 report: opening the file, 6 report: writing it, 7 stall detector. If perf_loop does not
 *             advance, the profiler thread is blocked and perf_st says where it stopped.
 *   rep / repfail
 *             reports written, and reports whose fopen failed (they used to be skipped in silence).
 *   lock      how long the mapping lock has been held right now, the longest hold and the number of
 *             acquisitions of the last second. Seconds with the lock held mean a thread is stuck inside
 *             the mapping with everything else queued behind it.
 *   log       size of the game log and the Result of reading it, every kVigiaLogCadaLatidos lines. If
 *             this grows while the game is stuck, the card is fine and it is the game that stopped.
 *   sd        how long the previous write to this file took and how it ended. Heartbeats missing here
 *             while the game is stuck mean the card stopped accepting writes.
 *   hilos     threads alive.
 *   cpu       the three threads using the most CPU, measured here every second with
 *             svcGetInfo(ThreadTickCount), so it survives a profiler thread that is blocked.
 *
 * When the low-fps run reaches kVigiaSegundosLento seconds, an episode block with all of it is appended
 * (at most kVigiaEpisodiosMax per run) plus a line on stderr the first time. No thread is paused and no
 * lock is taken to write it.
 */
struct VigiaCpu {
  char nombre[32];
  double pct;
};

unsigned VigiaHilos() {
  unsigned n = 0;
  for (const auto& s : g_slots) {
    if (s.handle.load(std::memory_order_relaxed)) {
      ++n;
    }
  }
  return n;
}

class Vigia {
 public:
  bool Abrir() {
    char ruta[FS_MAX_PATH];
    std::snprintf(ruta, sizeof(ruta), "%srex_latido.log", RexSwitchLogDir());
    /*
     * fix27: fix26 opened this file with the raw fsdev interface and every write was refused: the
     * file was created and stayed at zero bytes, and nothing said so, because the Result only
     * travelled inside the line that never got written. The likely reason is the open mode:
     * FsOpenMode_Write without FsOpenMode_Append refuses to grow a zero length file, and the
     * append fallback only ran when the first open failed, which it did not.
     *
     * stdio is the channel that has never failed here: rex_perfil.log and rex_crash.log are written
     * through it, one of them open at the same time as this file. The latido goes through it now,
     * every write is checked, and a refused write closes the handle so the loop opens it again.
     */
    f = std::fopen(ruta, "ab");
    if (!f) {
      f = std::fopen(ruta, "wb");
    }
    abierto = (f != nullptr);
    return abierto;
  }

  /* One line per second. Measured so that the line after it can say what the card took. */
  void Escribir(const char* datos, size_t n) {
    if (!n) {
      return;
    }
    const u64 t0 = armGetSystemTick();
    if (f) {
      const size_t escrito = std::fwrite(datos, 1, n, f);
      const int rc = std::fflush(f);
      ultimo_rc = (escrito == n && rc == 0) ? 0u : static_cast<u32>(errno ? errno : 1);
      if (ultimo_rc) {
        std::fclose(f);
        f = nullptr;
        abierto = false;
      }
    } else {
      ultimo_rc = 0xFFFF0000u;  /* no handle: it is the mirror that carries the line */
    }
    /* fix27: mirrored to stderr while the budget lasts. One small write per second. */
    if (espejo < kLatidoEspejoMax) {
      ++espejo;
      std::fwrite(datos, 1, n, stderr);
    }
    const u64 t1 = armGetSystemTick();
    lat_us = freq ? (t1 - t0) * 1000000ULL / freq : 0;
  }

  void MarcarFreq(u64 f) { freq = f; }
  bool abierto = false;
  u64 lat_us = 0;
  u32 ultimo_rc = 0;
  u32 espejo = 0;
  FILE* f = nullptr;

  /*
   * CPU per second of the busiest threads, measured here and not by the profiler. Threads paused by the
   * sampler count as idle (their counter does not advance), which is what we want: the point is to see
   * who is burning CPU while the screen is stuck.
   */
  void Cpu(u64 elapsed_ticks, VigiaCpu* out, size_t max_out, size_t* cuantos) {
    struct Cuenta {
      double pct;
      size_t idx;
    };
    Cuenta orden[kMaxThreads];
    size_t n_orden = 0;
    for (size_t i = 0; i < kMaxThreads; ++i) {
      const u32 h = g_slots[i].handle.load(std::memory_order_relaxed);
      if (!h) {
        visto[i] = 0;
        continue;
      }
      u64 ticks = 0;
      if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
        continue;
      }
      if (visto[i] != h || ticks < marcas[i]) {  // first sighting, or the thread restarted
        visto[i] = h;
        marcas[i] = ticks;
        continue;
      }
      orden[n_orden].pct = double(ticks - marcas[i]) * 100.0 / double(elapsed_ticks);
      orden[n_orden].idx = i;
      ++n_orden;
      marcas[i] = ticks;
    }
    std::sort(orden, orden + n_orden,
              [](const Cuenta& a, const Cuenta& b) { return a.pct > b.pct; });
    const size_t n = n_orden < max_out ? n_orden : max_out;
    *cuantos = n;
    for (size_t k = 0; k < n; ++k) {
      const Slot& s = g_slots[orden[k].idx];
      std::strncpy(out[k].nombre, s.name[0] ? s.name : "?", sizeof(out[k].nombre) - 1);
      out[k].nombre[sizeof(out[k].nombre) - 1] = 0;
      out[k].pct = orden[k].pct;
    }
  }

 private:
  u64 freq = 0;
  u32 visto[kMaxThreads]{};
  u64 marcas[kMaxThreads]{};
};

void VigiaMain(void*) {
  Vigia v;
  const u64 freq = armGetSystemTickFreq();
  v.MarcarFreq(freq);
  const u64 inicio = armGetSystemTick();
  bool avisado = false;
  if (v.Abrir()) {
    std::fprintf(stderr, "[fix27] vigia: latido por stdio, espejo en stderr hasta %u lineas\n",
                 kLatidoEspejoMax);
  } else {
    std::fprintf(stderr, "[fix27] vigia: no se pudo abrir %srex_latido.log, el latido va a stderr\n",
                 RexSwitchLogDir());
    avisado = true;
  }
  u64 ult_pres = g_counters[0].load(std::memory_order_relaxed);
  u64 ult_tic = armGetSystemTick();
  u64 ult_loop = g_perf_loop.load(std::memory_order_relaxed);
  u32 seg_lento = 0;
  u32 episodios = 0;
  u32 latidos = 0;
  bool ya_rendio = false;      // do not judge before the game has presented once
  s64 log_bytes = 0;
  u32 log_rc = 0;
  for (;;) {
    __real_svcSleepThread(kVigiaPeriodoNs);
    if (!v.abierto) {
      if (!avisado) {
        std::fprintf(stderr, "[fix27] vigia: sin %srex_latido.log, el latido sigue por stderr\n",
                     RexSwitchLogDir());
        avisado = true;
      }
      /* fix27: no continue: without the file the line has to go out through the mirror anyway. */
      v.Abrir();
    }
    const u64 ahora = armGetSystemTick();
    const double t = double(ahora - inicio) / double(freq);
    const double dt = double(ahora - ult_tic) / double(freq);
    const u64 pres = g_counters[0].load(std::memory_order_relaxed);
    const u64 datos = pres - ult_pres;
    if (datos > kVigiaFpsLento) {
      ya_rendio = true;
    }
    const bool lento = ya_rendio && datos <= kVigiaFpsLento;
    const double fps = dt > 0.0 ? double(datos) / dt : 0.0;

    const u64 loop = g_perf_loop.load(std::memory_order_relaxed);
    const u32 st = g_perf_stage.load(std::memory_order_relaxed);
    const u64 rep_ok = g_report_ok.load(std::memory_order_relaxed);
    const u64 rep_fail = g_report_fail.load(std::memory_order_relaxed);
    u64 lock_max = 0, lock_n = 0;
    RexGmLockSnapshot(&lock_max, &lock_n);
    const u64 lock_ahora = RexGmLockHeldTicks();
    VigiaCpu cpu[3]{};
    size_t n_cpu = 0;
    v.Cpu(ahora - ult_tic, cpu, 3, &n_cpu);

    /* The game log, every kVigiaLogCadaLatidos lines, through stdio like everything else: the size
     * says whether the game is still writing and errno says whether the card is taking it. */
    if (latidos % kVigiaLogCadaLatidos == 0) {
      const char* ruta = RexLogFilePath();
      log_rc = 0xFFFFFFFFu;
      if (ruta && ruta[0]) {
        if (FILE* g = std::fopen(ruta, "rb")) {
          if (std::fseek(g, 0, SEEK_END) == 0) {
            const long tam = std::ftell(g);
            if (tam >= 0) {
              log_bytes = static_cast<s64>(tam);
              log_rc = 0;
            }
          } else {
            log_rc = static_cast<u32>(errno ? errno : 1);
          }
          std::fclose(g);
        }
      }
    }
      /*
       * fix28: rc 0xFFFFFFFF only says "the log could not be read", never which path was tried or
       * why. One line, once, is enough to stop spending sessions on the wrong suspicion.
       */
      static bool ruta_avisada = false;
      if (!ruta_avisada && log_rc != 0) {
        ruta_avisada = true;
        const char* p = RexLogFilePath();
        std::fprintf(stderr, "[fix28] log del juego: ruta='%s' largo=%zu rc 0x%X errno=%d\n",
                     p ? p : "(nula)", p ? std::strlen(p) : 0u, static_cast<unsigned>(log_rc), errno);
      }

    const Arena arena_hb = ArenaAhora();
    char linea[1024];
    int n = std::snprintf(
        linea, sizeof(linea),
        "[fix27] t=%6.1fs fps=%5.1f pres=%llu lento=%us perf_loop=%llu(%+lld) perf_st=%u "
        "rep=%llu repfail=%llu lock=%llums(max %llums/%llu) log=%lldKB(rc 0x%X) "
        "sd=%lluus(rc 0x%X) al4k=%d neg2m=%zu sin_back=%zu pisc=%zu/%zu MB inactiva=%zu MB "
        "corte=%zu "
        "arena u=%zu f=%zu t=%zuK n=%zu hilos=%u pags=%zu blq=%zu cache=%zu sin_sitio=%zu "
        "map=%zu/%zu ent=%zu/%zu lib=%zu,%zu pres=%d ent_pag=%zu mg=%zu/%zu",
        t, fps, static_cast<unsigned long long>(pres), static_cast<unsigned>(seg_lento),
        static_cast<unsigned long long>(loop), static_cast<long long>(loop - ult_loop), st,
        static_cast<unsigned long long>(rep_ok), static_cast<unsigned long long>(rep_fail),
        static_cast<unsigned long long>(lock_ahora * 1000ULL / freq),
        static_cast<unsigned long long>(lock_max * 1000ULL / freq),
        static_cast<unsigned long long>(lock_n), static_cast<long long>(log_bytes >> 10),
        static_cast<unsigned>(log_rc), static_cast<unsigned long long>(v.lat_us),
        static_cast<unsigned>(v.ultimo_rc), RexGmBajaAlineacion(), RexGmDenegaciones2m(),
        RexGmSinBacking(), RexGmPiscinaBytes() >> 20, RexGmPiscinaEnUso() >> 20,
        (RexGmPiscinaBytes() - RexGmPiscinaEnUso()) >> 20,
        RexGmPiscinaCortes(), arena_hb.uord >> 20, arena_hb.ford >> 20,
        arena_hb.keep >> 10, arena_hb.ord, VigiaHilos(), RexGmPaginasEnUso(),
        RexGmBloquesPorPaginas(), RexGmPaginasReuso(), RexGmPaginasSinSitio(),
        RexGmMapeosCodigo(), RexGmMapeosVentana(), RexGmEntradasCodigo(), RexGmEntradasVentana(),
        RexGmLiberacionesVistas(), RexGmLiberacionesEventos(), RexGmPresionMapeo(),
        RexGmEntradasPaginas(), RexGmMallocVivo() >> 20, RexGmMallocBloques());
    for (size_t k = 0; k < n_cpu && n > 0 && n < static_cast<int>(sizeof(linea)) - 64; ++k) {
      n += std::snprintf(linea + n, sizeof(linea) - static_cast<size_t>(n), " | %s %.1f%%",
                         cpu[k].nombre, cpu[k].pct);
    }
    if (n > 0 && n < static_cast<int>(sizeof(linea)) - 2) {
      linea[n++] = '\n';
      linea[n] = 0;
      v.Escribir(linea, static_cast<size_t>(n));
    }

    if (lento) {
      if (++seg_lento == kVigiaSegundosLento && episodios < kVigiaEpisodiosMax) {
        ++episodios;
        char bloque[2048];
        int m = std::snprintf(
            bloque, sizeof(bloque),
            "-- [fix27] episodio %u a los %.1f s: %u s seguidos con %llu fotogramas o menos por "
            "segundo\n"
            "   perf_loop=%llu perf_st=%u informes=%llu fallidos=%llu hilos=%u\n"
            "   cerrojo de mapeo: %llums ahora, maximo %llums en %llu adquisiciones\n"
            "   log del juego: %lld KB (rc 0x%X) | ultimo latido: %lluus (rc 0x%X)\n"
            "   arena: usado %zu MB libre %zu MB tope %zu KB trozos %zu | piscina tomada "
            "%zu MB, en uso %zu MB, cortes %zu | paginas en uso %zu, bloques %zu, cache %zu, "
            "sin sitio %zu | map=%zu/%zu ent=%zu/%zu lib=%zu,%zu pres=%d\n"
            "   cpu:",
            episodios, t, static_cast<unsigned>(seg_lento), static_cast<unsigned long long>(datos),
            static_cast<unsigned long long>(loop), st, static_cast<unsigned long long>(rep_ok),
            static_cast<unsigned long long>(rep_fail), VigiaHilos(),
            static_cast<unsigned long long>(lock_ahora * 1000ULL / freq),
            static_cast<unsigned long long>(lock_max * 1000ULL / freq),
            static_cast<unsigned long long>(lock_n), static_cast<long long>(log_bytes >> 10),
            static_cast<unsigned>(log_rc), static_cast<unsigned long long>(v.lat_us),
            static_cast<unsigned>(v.ultimo_rc), arena_hb.uord >> 20, arena_hb.ford >> 20,
            arena_hb.keep >> 10, arena_hb.ord, RexGmPiscinaBytes() >> 20,
            RexGmPiscinaEnUso() >> 20, RexGmPiscinaCortes(), RexGmPaginasEnUso(),
            RexGmBloquesPorPaginas(), RexGmPaginasReuso(), RexGmPaginasSinSitio(),
            RexGmMapeosCodigo(), RexGmMapeosVentana(), RexGmEntradasCodigo(), RexGmEntradasVentana(),
            RexGmLiberacionesVistas(), RexGmLiberacionesEventos(), RexGmPresionMapeo());
        for (size_t k = 0; k < n_cpu && m > 0 && m < static_cast<int>(sizeof(bloque)) - 64; ++k) {
          m += std::snprintf(bloque + m, sizeof(bloque) - static_cast<size_t>(m), " %s %.1f%%",
                             cpu[k].nombre, cpu[k].pct);
        }
        if (m > 0 && m < static_cast<int>(sizeof(bloque)) - 2) {
          bloque[m++] = '\n';
          bloque[m] = 0;
          v.Escribir(bloque, static_cast<size_t>(m));
        }
        /*
         * fix27: every episode, not only the first, and with the whole payload. The latido file
         * cannot be trusted to arrive (fix26 left it at zero bytes) and this is the line that has
         * to be read afterwards, so it carries what the run needs: whether the perf loop is still
         * turning, whether the reports are being written, how long the mapping lock is held, how
         * the guest memory is doing, whether the heap refused the 2 MB backing, the card latency
         * and who is burning CPU.
         */
        std::fprintf(stderr,
                     "[fix27] episodio %u: t=%.1f s, %u s con %llu fps o menos | perf_loop=%llu "
                     "perf_st=%u rep=%llu repfail=%llu lock=%llums(max %llums/%llu) "
                     "log=%lldKB(rc 0x%X) sd=%lluus(rc 0x%X) al4k=%d neg2m=%zu sin_back=%zu "
                     "hilos=%u arena u=%zuMB f=%zuMB t=%zuKB n=%zu pisc=%zu/%zuMB "
                     "corte=%zu pags=%zu blq=%zu cache=%zu sin_sitio=%zu "
                     "map=%zu/%zu ent=%zu/%zu lib=%zu,%zu pres=%d\n",
                     episodios, t, static_cast<unsigned>(seg_lento),
                     static_cast<unsigned long long>(datos),
                     static_cast<unsigned long long>(loop), st,
                     static_cast<unsigned long long>(rep_ok),
                     static_cast<unsigned long long>(rep_fail),
                     static_cast<unsigned long long>(lock_ahora * 1000ULL / freq),
                     static_cast<unsigned long long>(lock_max * 1000ULL / freq),
                     static_cast<unsigned long long>(lock_n), static_cast<long long>(log_bytes >> 10),
                     static_cast<unsigned>(log_rc), static_cast<unsigned long long>(v.lat_us),
                     static_cast<unsigned>(v.ultimo_rc), RexGmBajaAlineacion(),
                     RexGmDenegaciones2m(), RexGmSinBacking(), VigiaHilos(),
                     arena_hb.uord >> 20, arena_hb.ford >> 20, arena_hb.keep >> 10,
                     arena_hb.ord, RexGmPiscinaBytes() >> 20, RexGmPiscinaEnUso() >> 20,
                     RexGmPiscinaCortes(), RexGmPaginasEnUso(), RexGmBloquesPorPaginas(),
                     RexGmPaginasReuso(), RexGmPaginasSinSitio(), RexGmMapeosCodigo(), RexGmMapeosVentana(),
                     RexGmEntradasCodigo(), RexGmEntradasVentana(), RexGmLiberacionesVistas(),
                     RexGmLiberacionesEventos(), RexGmPresionMapeo());
      }
    } else {
      if (seg_lento >= kVigiaSegundosLento) {
        char fin[256];
        const int m = std::snprintf(fin, sizeof(fin), "-- [fix27] fin del episodio %u: %.1f s, %u s "
                                                      "lentos\n",
                                    episodios, t, static_cast<unsigned>(seg_lento));
        if (m > 0 && m < static_cast<int>(sizeof(fin))) {
          v.Escribir(fin, static_cast<size_t>(m));
        }
      }
      seg_lento = 0;
    }
    ult_pres = pres;
    ult_tic = ahora;
    ult_loop = loop;
    ++latidos;
  }
}

/* Priority 102: after switch_crash_hooks.c, before the SDK. */
__attribute__((constructor(102))) void StartProfiler() {
  g_base = reinterpret_cast<u64>(&_start);
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, g_base))) {
    g_text_lo = mi.addr;
    g_text_hi = mi.addr + mi.size;
  }
  Register(envGetMainThreadHandle());
  std::strncpy(g_slots[0].name, "main (interfaz)", sizeof(g_slots[0].name) - 1);
  // 0x2A: above everything in the game (audio runs at 0x2B), so the samples
  // are taken on time. It sleeps almost all the time.
  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
  /*
   * fix26: the watchdog, one step above the profiler (0x29 against 0x2A) so that it still runs when the
   * profiler thread is the one that stops. See the comment on VigiaMain for what it leaves behind.
   */
  if (R_SUCCEEDED(__real_threadCreate(&g_vigia, VigiaMain, nullptr, nullptr, 0x10000, 0x29, -2))) {
    threadStart(&g_vigia);
  }
}

}  // namespace

extern "C" {

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    Register(t->handle);
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    Unregister(t->handle);
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See g_llamadas. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  Anotar(timeout_us == 0 ? kFenceConsulta : kFenceEspera, desde);
  return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvGpuChannelKickoff(c);
  Anotar(kKickoff, desde);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  Anotar(is_cpu_cacheable ? kNvMapCacheada : kNvMapSinCache, desde, size);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  Anotar(kReservaDireccion, desde, size);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  Anotar(kMapeoGpu, desde, mapping_size);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  const u64 desde = armGetSystemTick();
  __real_armDCacheClean(addr, size);
  Anotar(kCacheClean, desde, size);
}

/*
 * The presentation interval (nfsmw_intervalo_swap).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_intervalo_swap{0};

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const u64 desde = armGetSystemTick();
  const unsigned pedido = g_intervalo_swap.load(std::memory_order_relaxed);
  if (pedido != 0 && nw != nullptr && nw->swap_interval != pedido) {
    nwindowSetSwapInterval(nw, pedido);
  }
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  Anotar(kQueueBuffer, desde);
  return rc;
}

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const u64 desde = armGetSystemTick();
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  Anotar(kDequeueBuffer, desde);
  return rc;
}

void __wrap_svcSleepThread(s64 nano) {
  const u64 desde = armGetSystemTick();
  __real_svcSleepThread(nano);
  Anotar(nano == 0 ? kSleep0 : nano < 0 ? kSleepCeder : nano <= 1000000 ? kSleepCorto : kSleepLargo, desde);
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
* 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 * 17 = physical chunks committed on touch, 18 = chunks mapped into a view,
 * 29 = zero-load emulated, 30 = raw physical alias, 33 = free page given real
 * backing (fix20), 34 = read served natively by a lent page (fix21).
 */
// A long frame, in armGetSystemTick ticks (see VentanaTiron).
void RexSwitchPerfTiron(u64 inicio, u64 fin) {
  const u32 i = g_num_tirones.fetch_add(1);
  if (i < kMaxTirones) {
    g_tirones[i] = {inicio, fin};
  }
}

void RexSwitchPerfResolution(unsigned ancho, unsigned alto) {
  g_resolucion.store(((ancho & 0xFFFF) << 16) | (alto & 0xFFFF), std::memory_order_relaxed);
}

// See g_intervalo_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfIntervaloSwap(unsigned vblanks) {
  g_intervalo_swap.store(vblanks, std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(1, std::memory_order_relaxed);
  }
  if (id == 0) {
    // Counter 0 is presents. The console overlay clears the "alive" mark and the resolution mark and
    // only waits 100 ms, so it has to be answered on every frame, not once per second. It is writing a
    // few bytes to shared memory.
    const uint32_t res = g_resolucion.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Latir(res >> 16, res & 0xFFFF);
  }
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(value, std::memory_order_relaxed);
  }
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  if (id >= kCounterCount) {
    return;
  }
  u64 actual = g_counters[id].load(std::memory_order_relaxed);
  while (value > actual && !g_counters[id].compare_exchange_weak(actual, value, std::memory_order_relaxed)) {
  }
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
bool RexSwitchPerfSkip(unsigned bit) {
  return (g_skip_mask.load(std::memory_order_relaxed) & bit) != 0;
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  for (auto& s : g_slots) {
    if (s.handle.load() == handle) {
      char tmp[sizeof(s.name)]{};
      std::strncpy(tmp, name, sizeof(tmp) - 1);
      std::memcpy(s.name, tmp, sizeof(tmp));
      return;
    }
  }
}

}  // extern "C"
