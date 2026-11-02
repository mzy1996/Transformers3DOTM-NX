/* See guest_memory_switch.h for the design and the reason behind each decision. */

#if defined(__SWITCH__)

#include "guest_memory_switch.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include <algorithm>
#include <map>
#include <set>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

#include <rex/cvar.h>

/*
 * fix23: alignment mode of the guest window (see the note on kWindowAlign below). It is read when
 * the window is reserved, so it is a decision of the run and not a runtime switch:
 *   0 = 4 KB base: what this port did up to fix22 (4 KB pages everywhere)
 *   1 = 32 MB aligned base: every view congruent except 0xE0000000 (default)
 *   2 = as 1, plus the 4 KB phase of 0xE0000000 on the grid of the physical arena
 */
REXCVAR_DEFINE_INT32(
    guest_memory_large_pages, 1, "Memory",
    "Switch: align the guest window so the kernel can map whole chunks with 2 MB block descriptors "
    "instead of 4 KB page tables. 0 = off, 1 = 32 MB aligned window, 2 = as 1 plus the 4 KB phase "
    "of the 0xE0000000 view")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * fix38: the state-only page lookup of fix25 (defined in xmemory.cpp), the one the fault path
 * already uses: 1 = the page is free in the guest page table, 0 = not a guest virtual page.
 * SoltarFisicoSinUso asks it before taking a granule back, and it is the fast one: no text is
 * built and no region is walked.
 */
extern "C" int RexSwitchGuestPageState(uint32_t guest_address);

/*
 * fix37: what the heap holds, named by size class.
 *
 * The batch this comes from leaves an arena of 3015 MB used while the guest only answers for 1372 MB
 * (the slices the pool took plus the blocks it backs), the process sits at 3185/3189 MB and the 144
 * blocks the game is still waiting for (288 MB) never come. Who holds the other ~1.6 GB is what the
 * next batch has to answer, and mallinfo cannot: it gives one total, not a histogram.
 *
 * These wrappers give the histogram. They do not allocate, they do not free and they do not change
 * what a caller gets: they count and hand the call straight to the real function that --wrap (see
 * rexglue_switch.cmake) renames here. malloc_usable_size gives the size of each block exactly as
 * newlib metered it, so the live side of each class is the same figure mallinfo would add up, and the
 * two can be read against each other in the heartbeat (mg= against arena u=).
 *
 * The wrappers themselves are at file scope with C linkage (the linker resolves __wrap_malloc by
 * name, so they cannot live in the anonymous namespace below). The counters and the bookkeeping sit
 * in that namespace, where the rest of this file can read them.
 */
namespace {

constexpr size_t kCajaLimites[7] = {32, 128, 512, 2048, 8192, 65536, 524288};
constexpr size_t kCajas = 8;  /* seven "at most" classes and one for everything above them */

std::atomic<unsigned long long> g_caja_vivo_bytes[kCajas];
std::atomic<unsigned long long> g_caja_vivo_bloques[kCajas];
std::atomic<unsigned long long> g_caja_asignado_bytes[kCajas];
std::atomic<unsigned long long> g_caja_liberado_por_clase[kCajas];
std::atomic<unsigned long long> g_caja_llamadas[kCajas];
std::atomic<unsigned long long> g_malloc_asignado_total{0};
std::atomic<unsigned long long> g_malloc_liberado_total{0};
std::atomic<unsigned long long> g_malloc_llamadas_total{0};
std::atomic<unsigned long long> g_malloc_vivos_total{0};
std::atomic<unsigned long long> g_malloc_bloques_total{0};

size_t CajaDe(size_t bytes) {
    for (size_t i = 0; i < 7; ++i) {
        if (bytes <= kCajaLimites[i]) return i;
    }
    return 7;
}

void CajaAlta(void* p) {
    if (!p) return;
    const size_t s = malloc_usable_size(p);
    const size_t c = CajaDe(s);
    g_caja_vivo_bytes[c].fetch_add(s, std::memory_order_relaxed);
    g_caja_vivo_bloques[c].fetch_add(1, std::memory_order_relaxed);
    g_caja_asignado_bytes[c].fetch_add(s, std::memory_order_relaxed);
    g_caja_llamadas[c].fetch_add(1, std::memory_order_relaxed);
    g_malloc_asignado_total.fetch_add(s, std::memory_order_relaxed);
    g_malloc_llamadas_total.fetch_add(1, std::memory_order_relaxed);
    g_malloc_vivos_total.fetch_add(s, std::memory_order_relaxed);
    g_malloc_bloques_total.fetch_add(1, std::memory_order_relaxed);
}

/* Saturating on the way down: an internal free that never came through here must not underflow. */
void CajaBaja(size_t s) {
    if (s == 0) return;
    const size_t c = CajaDe(s);
    const unsigned long long vivo = g_caja_vivo_bytes[c].load(std::memory_order_relaxed);
    g_caja_vivo_bytes[c].fetch_sub(s <= vivo ? s : vivo, std::memory_order_relaxed);
    const unsigned long long nb = g_caja_vivo_bloques[c].load(std::memory_order_relaxed);
    if (nb != 0) g_caja_vivo_bloques[c].fetch_sub(1, std::memory_order_relaxed);
    g_caja_liberado_por_clase[c].fetch_add(s, std::memory_order_relaxed);
    g_malloc_liberado_total.fetch_add(s, std::memory_order_relaxed);
    const unsigned long long vt = g_malloc_vivos_total.load(std::memory_order_relaxed);
    g_malloc_vivos_total.fetch_sub(s <= vt ? s : vt, std::memory_order_relaxed);
    const unsigned long long bt = g_malloc_bloques_total.load(std::memory_order_relaxed);
    if (bt != 0) g_malloc_bloques_total.fetch_sub(1, std::memory_order_relaxed);
}

}  /* namespace */

extern "C" {

void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void  __real_free(void*);
void* __real_memalign(size_t, size_t);

void* __wrap_malloc(size_t n) {
    void* p = __real_malloc(n);
    CajaAlta(p);
    return p;
}

void* __wrap_calloc(size_t n, size_t m) {
    void* p = __real_calloc(n, m);
    CajaAlta(p);
    return p;
}

void* __wrap_realloc(void* p, size_t n) {
    const size_t viejo = p ? malloc_usable_size(p) : 0;
    void* q = __real_realloc(p, n);
    if (q) {
        /*
         * The old block was released inside the real realloc and did not pass through __wrap_free:
         * the loss is taken here so the live side stays exact.
         */
        CajaBaja(viejo);
        CajaAlta(q);
    }
    return q;
}

void __wrap_free(void* p) {
    if (p) CajaBaja(malloc_usable_size(p));
    __real_free(p);
}

void* __wrap_memalign(size_t alineacion, size_t n) {
    void* p = __real_memalign(alineacion, n);
    CajaAlta(p);
    return p;
}

}  /* extern "C" */

namespace {

constexpr size_t kPageSize  = 0x1000;
/*
 * Large chunks are requested 2 MB aligned, so the kernel can use block descriptors instead of a
 * page table.
 */
constexpr size_t kBlockSize = 0x200000;
/*
 * Smallest commit (Torchlight/T2ROTF): each chunk costs the kernel a code alias plus one
 * mapping per view, and a game that commits its physical memory 4 KB at a time
 * runs out of kernel memory blocks (0xCE01, OutOfResource) with only ~300 MB
 * committed. Rounding commits to 64 KB divides the number of chunks by up to 16.
 * The extra pages are committed zeroed, which the guest cannot tell apart.
 *
 * 64 KB was still not enough: Torchlight's physical heap hands out small blocks
 * (8 to 160 KB) one after another, top down, and each one became its own chunk.
 * On another console the game hit 0xCE01 again after a town -> mine -> town trip,
 * with 1,135 chunks for 395 MB (left part of the town black, then the whole console
 * hung: the kernel memory blocks may be shared with the system). Committing whole
 * 2 MB blocks puts those neighbours in a single chunk (at most 256 for the 512 MB
 * of the 360) and lets the kernel use block descriptors. The cost is at most 2 MB
 * per committed run, and a chunk only partly decommitted stays committed, which
 * the 512 MB of guest physical memory bounds.
 */
constexpr size_t kCommitGranule = kBlockSize;

/* Direction B: pre-allocate backing in heap, shadow in code-memory via
 * virtmemFindCodeMemory / svcMapProcessCodeMemory / svcSetProcessMemoryPermission.
 * Done before NVK init to grab code-memory VA while it is still free. */
static void* g_prealloc_backing = nullptr;
static size_t g_prealloc_size = 0;
static void* g_prealloc_shadow = nullptr;      /* code-memory alias of the prealloc backing */
static VirtmemReservation* g_prealloc_shadow_res = nullptr;

/* Direction F: if svcSetMemoryAttribute can make heap pages MapProcessAllowed,
 * we map backing directly into the window without a code-memory shadow. */
static bool g_f_mode_enabled = false;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/*
 * fix23: the window is mapped with 2 MB block descriptors instead of 4 KB pages.
 *
 * Horizon's kernel only uses a block descriptor where the virtual address and the physical address
 * are congruent modulo the block size (KPageTable::MapContiguous climbs the block sizes while
 * (virt & (size - 1)) == (phys & (size - 1))). The backing of a chunk is memalign'd, so its
 * alignment decides the physical side; the alignment of the window decides the virtual side, and
 * that is the side this port was losing: virtmemFindAslr(size, kPageSize) takes a *guard size*,
 * not an alignment, so the base came out 4 KB aligned (measured on the console:
 * "[DIAG] RexGmInit: return base=0x6154a3000", that is 0xa3000 modulo 2 MB). With a base like that
 * no mapping can ever be congruent, every one falls to 4 KB pages, and each 2 MB chunk costs 512
 * page table entries instead of one descriptor.
 *
 * That is the ceiling that runs out first: the kernel answers 0xCE01 (OutOfResource) with RAM to
 * spare (a sibling port measured a 64 KB commit refused with only 329 MB of backing). Here it is
 * where fix22 stopped: the loading screen commits 0x37080000 (883 MB) and, with the commit
 * deferred to its first touch, that touch was refused 14 times
 * ("[fix22] materializar RECHAZADO rc=0x0000CE01" at 0x5AC00000 and 0x24200000, alternating), the
 * profile fell 8.4 -> 0.8 -> 0.0 fps and the game froze after TF_LoadingScreen.bik without ever
 * showing the first logo animation.
 *
 * The fix is to put both sides of every mapping on the same 2 MB square:
 *   - the window base is aligned to kWindowAlign (32 MB),
 *   - every committed chunk is a square of the grid (RexGmCommit, GridDown/GridUp),
 *   - the backing of a whole block and its code alias are 2 MB aligned (MaterializarBloque).
 *
 * The views this port registers are 0x00000000 (offset 0), 0x40000000, 0x7F000000 (0x100000000),
 * 0x80000000, 0x90000000, 0xA0000000 (0x100000000), 0xC0000000 (0x100000000), 0xE0000000
 * (0x100001000) and the fix19 raw range (0x100000000 + granule). Every one of them has its arena
 * offset on the same 2 MB square as its window address *except* 0xE0000000, whose offset carries
 * the 4 KB phase the console emulates. Hence the three modes:
 *
 *   mode 0: what this port did before - 4 KB base, 4 KB pages in every view.
 *   mode 1: 32 MB aligned base (default). All views congruent except 0xE0000000, which stays at
 *           4 KB pages as it was: no regression anywhere, and the heaps - where this game spends
 *           its memory and where the 0x37080000 commit lands (0x40000000) - stop paying 512
 *           entries per 2 MB.
 *   mode 2: as 1 plus kFaseE0 (4 KB) on the grid of the physical arena, which makes 0xE0000000
 *           congruent instead, at the price of the raw view, 0xA0000000 and 0xC0000000 falling to
 *           4 KB. It is there to be measured against mode 1, not as the default: the commit that
 *           fails here is in 0x40000000.
 *
 * Changing the alignment of the window changes nothing the guest sees: the base is a host-side
 * value and every address the SDK handles is base + guest offset.
 */
constexpr size_t kWindowAlign = 0x2000000;       /* 32 MB: square big enough to hold any view phase */
constexpr size_t kPhysStart   = 0x100000000ull;  /* first mapping offset of the physical arena */
constexpr size_t kFaseE0      = 0x1000;          /* 4 KB phase of the 0xE0000000 view */

/* fix23: mode in force, and the phase mode 2 adds to the grid of the physical arena. Set by
 * RexGmConfigure before the window is reserved, read by everything else after that. */
int    g_paginas_grandes = 1;
size_t g_fase_fisica = 0;

/* fix23: whole blocks whose alias went in on 2 MB, that is, the ones the kernel can map with a
 * block descriptor. Read by the 10 s report to tell one mode from another. */
std::atomic<size_t> g_aliases_grandes{0};

/*
 * fix27: the 2 MB alignment of the backing is a wish, not a requirement: it only saves the kernel
 * 512 page table entries per block. Under fragmentation memalign(2 MB, 2 MB) fails while the arena
 * still has free space, and from then on every fault on that block pays the failing search again:
 * 2045 refusals on the same offset in a 30 s run of this game, with the screen at 0.0 fps.
 *
 * The port used to retry on every fault. Now the first refusal drops the whole process to the 4 KB
 * alignment every block had before fix23, and 2 MB is not asked for again in this run: what costs
 * is the refusal, because the search for an aligned hole walks the whole free list. The alignment
 * that reaches the kernel follows the backing that was really granted (see MaterializarBloque), so a
 * refused 2 MB does not leave the port asking for a 2 MB alias of a 4 KB backing. A block that cannot
 * be had either way is retried kIntentosBackingMax times and then left alone.
 */
std::atomic<int> g_alineacion_baja{0};
std::atomic<size_t> g_denegaciones_2m{0};
std::atomic<size_t> g_sin_backing{0};
constexpr unsigned kIntentosBackingMax = 4;
/*
 * fix37: the window a block is retried in once it has reached the cap, and the system tick of the
 * Switch (19.2 MHz, the same figure the heartbeat of switch_perf.cpp uses). See the note on
 * Chunk::ultimo_intento.
 */
constexpr unsigned kRetiradaEsperaMs = 2000;
constexpr u64 kTicksPorMs = 19200ull;
/*
 * fix30: the mapping budget, counted and defended.
 *
 * fix29 stopped at 0xCE01 = KERNELERROR_RESOURCE_EXHAUSTED: ten 2 MB blocks went in page by page
 * (512 mappings each) and the eleventh was refused. While the guest went round that same page for
 * 260 s the port kept creating raw physical aliases (crudas 0 -> 252 MB inside the stall), and every
 * raw alias commits a chunk (a code mapping) and maps it into its own view: the run was eating the
 * budget it had just run out of. And a block refused by the pool, by libc and by the pages was
 * written off, so the same fault came back for ever.
 *
 * So: count the mapping calls and the entries the kernel needs for them, stop the raw aliases from
 * growing while the budget is refusing, and - the point of it - hand part of the raw-view cache back
 * when a block cannot be materialised, then ask for that block again on the next fault. Everything
 * goes out in the heartbeat and in the 10 s report, so the next run says whether 0xCE01 arrives at a
 * fixed entry count (a ceiling) and whether the release moved it.
 */
std::atomic<bool> g_presion_mapeo{false};
std::atomic<size_t> g_mapeos_codigo_n{0};
std::atomic<size_t> g_mapeos_codigo_bytes{0};
std::atomic<size_t> g_entradas_codigo{0};
std::atomic<size_t> g_mapeos_ventana_n{0};
std::atomic<size_t> g_mapeos_ventana_bytes{0};
std::atomic<size_t> g_entradas_ventana{0};
std::atomic<size_t> g_lib_eventos{0};
std::atomic<size_t> g_lib_vistas{0};
/*
 * fix31: the entry price is the whole story of this batch. The table charges one entry per 4 KB of
 * every mapping that is not a whole 2 MB square, and exactly one entry for an aligned 2 MB one: the
 * page-by-page path costs 512 entries per block whatever its runs look like, and the run that froze
 * here spent 5120 of them (ten blocks) while the whole cheap side - 508 aliases - spent 508. So the
 * liberator is allowed to move more of the raw window per event, and the page path is rationed.
 */
std::atomic<size_t> g_entradas_paginas{0};   /* fix31: entries the page path has alive right now */
constexpr size_t kLibVistasPorEvento = 16;   /* fix31: 8 -> 16 hand-backs per event */
constexpr size_t kLibEventosMax = 64;        /* fix32: 16 -> 64 events per run, the reclaim is now on the fault path */
/*
 * fix38: the guest physical cache fix19 built has no owner the moment nobody is reading it.
 *
 * The batch this comes from leaves the bill plain. Its raw physical views had been created and
 * dropped 126 times, and every creation committed a whole granule of guest physical memory under
 * the arena at kRawFisicoBase (RexGmRawAlias -> RexGmCommit). By the time the loading screen asked
 * for its last 145 blocks (290 MB) that memory was still committed and nobody was reading it: live
 * raw views, zero. The pool sat at 1256/1256 MB, the arena had 62 MB free cut into 15554 pieces,
 * the slab ladder was refused at all four steps and the process was at 3185/3189 MB of the kernel
 * limit - so 145 blocks queued behind a 2 MB slice that no longer existed, and the whole process
 * (the game's own threads included) stayed at 0.0 fps while the port retried every 2 s.
 *
 * A materialised chunk inside the raw range that no view - neither one of the guest's own nor a
 * raw one - has mapped is memory the guest is not reading right now, and the port rebuilds it the
 * moment RexGmRawAlias asks for that granule again (RexGmCommit is idempotent and rewrites the
 * descriptor, MaterializarBloque backs it again). Handing it back is therefore reversible, and
 * each one returns a whole slice to the pool's free list: exactly the backing the refused blocks
 * are queued for, and without taking a byte of process memory the kernel is no longer granting.
 *
 * The guest page table is asked too, because releasing a granule the guest has allocated would be
 * giving back memory that is not this cache's to give. Bounded per call and per run, so a run
 * that misclassifies one granule cannot eat the raw window, and every release is printed: the
 * next log says whether the guest asked for the same granules back.
 */
std::atomic<size_t> g_fisicos_lib{0};     /* fix38: raw physical granules handed back this run */
constexpr size_t kFisicosPorEvento = 16;  /* fix38: granules handed back per call (32 MB) */
constexpr size_t kFisicosMax = 2048;      /* fix38: ceiling per run, so the raw range cannot be eaten by mistake */
/*
 * fix32: the page path stays rationed at the same 4096 entries it had, and what changes is that a
 * refusal is no longer free: the reason is what the batch that froze here said. The run reached 4096
 * entries with sixteen "entrada racionada" lines all on the same offset 0x32000000, while the kernel
 * had refused *nothing* - "RECHAZADO" does not appear once in that stderr - and the liberator then
 * drained the raw window (126 -> 0 views in 8 events) and still would not let that one block in. So
 * the block the guest was asking for cost 512 entries and the cheap side of the same run was 1 entry
 * per 2 MB: the page path is the wrong tool for a load that still has a few hundred blocks pending,
 * and the pool is the right one. Raising this ration would spend the kernel's headroom on the
 * expensive side. What fix32 does here:
 *  - kEntradasTotalesTope is the ceiling the kernel really showed: the earliest refusal on record
 *    (fix29) happened with 7071 + 4146 = 11,217 entries alive, and this batch sat at 4696 + 4748 =
 *    9,444 with no refusal. The cap is set below the earliest refusal, so no block of this port goes
 *    in past it however much the reclaim pays.
 *  - Over the soft tier, the raw window is handed back first and, if that bought anything and the
 *    total is still under the ceiling, the block goes in anyway; the hard tier keeps a runaway out.
 * Both tiers and the ceiling go out in the log, so the next run says where the load stops and
 * whether the kernel starts refusing at last.
 */
/*
 * fix33: the entry table was charging the window for pages it never mapped one by one, and that
 * arithmetic is what stopped the load.
 *
 * The batch that froze at 0x70800000 left the three numbers side by side: ventana 801 calls,
 * 1599 MB and 5894 entries. A window call is one svcMapProcessMemory of a whole view, so it is one
 * mapping; the 512-entry charge only describes the page path, whose runs really are one page each.
 * The formula charged len/4KB whenever dst was not on the 2 MB grid, and about ten of those calls
 * were not (the 880 MB block starts at 0x46F90000, 0x90000 into its square), so 10 x 512 = 5120 of
 * the 5894 entries were arithmetic rather than mappings: half the ceiling spent on a number. The
 * kernel never saw them - "RECHAZADO" is absent from that whole stderr, and the refused word was
 * this port's own ceiling - and the block the load needed went unbacked. From here a window call
 * costs one entry, the number the kernel really has to take, and the ceilings below are read in
 * that unit: code (one per alias, 512 per page-path block) plus window (one per call).
 *
 * The ceiling itself stops being a guess:
 *  - fix32 read the wall off two batches and set 10240: (fix29) refused at 7071 + 4146 = 11,217
 *    accounted entries and (fix32) sat at 4696 + 4748 = 9,444 with no refusal. In the corrected
 *    unit those two runs read 5986 + 586 = 6,572 refused and 4821 + 801 = 5,622 taken, so the
 *    ceiling of this batch is 6144: above what the kernel took, below what it refused.
 *  - g_techo_aprendido is the wall as the kernel reports it: the first 0xCE01 at a total this port
 *    has counted lowers the ceiling to that total for the rest of the run, so the next run says the
 *    number instead of the port having to guess it a third time.
 */
constexpr size_t kEntradasPaginasTope = 4096;       /* fix33: soft tier, 8 blocks at 512 entries */
constexpr size_t kEntradasPaginasTopeDuro = 4608;   /* fix33: hard tier, 9 blocks at 512 entries */
constexpr size_t kEntradasTotalesTope = 6144;       /* fix33: 5622 taken, 6572 refused */

std::atomic<size_t> g_techo_aprendido{static_cast<size_t>(-1)};  /* fix33: the wall, once */

size_t EntradasDeMapeo(u64 dst, size_t len) {
    if (len >= kBlockSize && (len % kBlockSize) == 0 && (dst % kBlockSize) == 0) return len / kBlockSize;
    return len / kPageSize;
}
void AnotarMapeoCodigo(u64 dst, size_t len) {
    g_mapeos_codigo_n.fetch_add(1, std::memory_order_relaxed);
    g_mapeos_codigo_bytes.fetch_add(len, std::memory_order_relaxed);
    g_entradas_codigo.fetch_add(EntradasDeMapeo(dst, len), std::memory_order_relaxed);
}
/* fix33: one call of the window is one mapping, whatever the alignment of the address it lands on. */
void AnotarMapeoVentana(u64 dst, size_t len) {
    (void)dst;
    g_mapeos_ventana_n.fetch_add(1, std::memory_order_relaxed);
    g_mapeos_ventana_bytes.fetch_add(len, std::memory_order_relaxed);
    g_entradas_ventana.fetch_add(1, std::memory_order_relaxed);
}
size_t EntradasTotales(void) {
    return g_entradas_codigo.load(std::memory_order_relaxed) +
           g_entradas_ventana.load(std::memory_order_relaxed);
}
size_t TopeTotal(void) {
    const size_t aprendido = g_techo_aprendido.load(std::memory_order_relaxed);
    return aprendido < kEntradasTotalesTope ? aprendido : kEntradasTotalesTope;
}
/* fix33: the kernel has the last word on the wall; from the first 0xCE01 on, the port stays under it. */
void AnotarRechazoKernel(Result rc) {
    if (rc != 0x0000CE01u) return;
    const size_t total = EntradasTotales();
    size_t prev = g_techo_aprendido.load(std::memory_order_relaxed);
    while (total < prev && !g_techo_aprendido.compare_exchange_weak(prev, total)) {
    }
    static uint32_t aviso = 0;
    if (total < prev && aviso < 8) {
        ++aviso;
        fprintf(stderr, "[fix33] techo aprendido: el kernel rechazo con %zu entradas totales\n", total);
    }
}

/*
 * fix23: the grid of the chunk offsets. 2 MB squares, shifted by kFaseE0 from kPhysStart in mode 2
 * so that the 0xE0000000 view is the congruent one. A chunk that does not start and end on this
 * grid cannot be congruent with any view, so RexGmCommit works on the grid and not on the request
 * boundaries.
 */
size_t GridDown(size_t off) {
    if (g_fase_fisica != 0 && off >= kPhysStart) {
        return kPhysStart + AlignDown(off - kPhysStart, kCommitGranule);
    }
    return AlignDown(off, kCommitGranule);
}
size_t GridUp(size_t off) {
    if (g_fase_fisica != 0 && off >= kPhysStart) {
        return kPhysStart + AlignUp(off - kPhysStart, kCommitGranule);
    }
    return AlignUp(off, kCommitGranule);
}

/* A committed chunk: heap backing, its code alias (the shadow), and the span of the mapping it covers. */
struct Chunk {
    size_t offset;      /* offset in the mapping */
    size_t length;
    void*  backing;     /* from the heap. Inaccessible after mapping it as code */
    void*  shadow;      /* the codeAlias: always RW, a back door */
    VirtmemReservation* shadow_res; /* virtmem reservation of the shadow (nullptr for prealloc slices) */
    unsigned fallos = 0;            /* fix27: failed materialisations; capped by kIntentosBackingMax */
    /*
     * fix37: reaching the cap no longer writes the block off for ever, and it no longer means the
     * port pays for the search on every fault. The batch this comes from shows the bill: the pool
     * refusal line went from cortes 38 to 18568 while the game sat at 0.0 fps, and each of those
     * attempts walks the arena and the slab ladder with the mapping lock held, which is what keeps
     * the rest of the process (the game's own threads) from running. From here the block is asked
     * again only when this window has gone by, so the memory can still come back to it (a slab, a
     * freed view, a chunk that unloads) without the storm.
     */
    u64 ultimo_intento = 0;         /* fix37: tick of the last retry after the cap, 0 = none yet */
    /*
     * fix29: when neither the pool nor the two contiguous requests can back a block, the backing is
     * one page at a time and cannot be a single buffer: `paginas` holds them (and is what tells this
     * kind of chunk from the others in ReleaseChunk), `n_paginas` how many. Both null for every
     * normal chunk, `backing` is null for these.
     */
    void** paginas = nullptr;
    size_t n_paginas = 0;
};

/* A view: a span of the window that mirrors the mapping from `offset`. */
struct View {
    uint8_t* base;
    size_t   offset;
    size_t   length;
};

/*
 * fix19: a view of its own over the raw physical range.
 *
 * On the console the 512 MB of physical memory also has the "raw physical" view: the
 * physical address itself, dereferenceable as it comes. That is the address the guest holds
 * when it is handed one - MmGetPhysicalAddress, the video front buffer, the XMA context
 * array -, and this port never registered that view: the identity view already holds
 * 0x00000000-0x3FFFFFFF for the v00000000 virtual heap, so every access to a raw physical
 * address used to fault.
 *
 * These views are not registered at startup; they would double the accounting of the whole
 * arena for nothing. They are created on demand, over the granules the guest actually
 * touches, and only over granules that the guest page table reports as free, so they never
 * cover a virtual allocation that is in use.
 *
 * They live in their own list because their index space must not collide with the fixed
 * views of `views`: from kRawViewBase on, index = kRawViewBase + position in raw_views. A
 * dead one (viva == false) is never erased, so the indexes of the ones created after it do
 * not move.
 */
struct VistaCruda {
    View v;
    bool viva;
};

/* Index space of the raw views, far from any possible position in `views`. */
constexpr size_t kRawViewBase = 1u << 24;

/* The raw physical range of the guest: 512 MB, seen at its own address. */
constexpr size_t kRawFisicoTam = 0x20000000ull;

/* Mapping offset of physical memory (the arena behind the mirrors). */
constexpr size_t kRawFisicoBase = 0x100000000ull;

/*
 * Budget of live raw views: each one costs a mapping when it is entered.
 *
 * fix20: it used to be 128 and the fix19 run filled 126 of them with the 2 MB granules of
 * 0x10400000-0x1FE00000, that is, the first half of the raw range, because the copy that walks it
 * starts where it starts. The raw range is 512 MB, 256 granules: at 128 anything that began lower
 * than that copy would have found no view left halfway. 256 covers the raw range once.
 */
constexpr size_t kRawVistasMax = 256;

/* A span already mapped in this operation, so it can be undone if another fails. */
struct Tramo {
    size_t   win_off;
    uint8_t* src;
    size_t   len;
};

/*
 * The window size, without the lock
 *
 * InGuestWindow() is called on every fault (tens of thousands per second in a race) and only needs
 * to know whether the address falls inside the window. Reading it from State meant taking the global
 * lock, the same one the mappings use, and in the profile the thread that faulted most spent 22.8%
 * of its time in svcArbitrateLock because of this.
 *
 * base and size are set once in RexGmInit and only change again in RexGmShutdown, so an atomic copy
 * is enough and cannot get out of sync.
 */
std::atomic<size_t> g_size_rapido{0};

/*
 * The base goes in its own atomic and is published after the size. That way, if a thread sees the
 * base, the size is already set: the other way round (as it used to be) the window could be seen
 * half-initialized, with a valid base and size 0, and then InGuestWindow said no and a legitimate
 * guest fault ended in a fatal stop.
 */
std::atomic<uint8_t*> g_base_rapido{nullptr};

/*
 * fix22: blocks that had to be materialised (backing + zeroing + code alias) on their first touch.
 *
 * It is the figure that says how much of a big commit was really paid for: a commit of 883 MB
 * leaves 442 descriptors of 2 MB and only the blocks the game touches become entries here.
 */
static std::atomic<size_t> g_bloques_materializados{0};

/*
 * fix41: the last resort of the fault path - one page at a time.
 *
 * The batch this comes from closes the story the fix22 ledger had been telling. The first touch of
 * the deferred commit 0x46F90000+0x37080000 (883 MB behind heap 0x40000000+0x3F000000) wrote to
 * 0x6D400000 and the run aborted - report 2345-0102, "User Break" at T3DOTM+0x551feec - with the
 * block committed ("[fix16] cubre op=1") and no backing at all: the pool sat at 1268/1268 MB, the
 * arena had its 64 MB of free space cut into 14657 pieces so no 2 MB slice was left to memalign, and
 * the page path could not pay its 512 entries either ("[fix33] por paginas: techo de entradas (5658 +
 * 512 > 6144)"), the whole run at 0.0 fps. The block was written in the table as mapped
 * (prot_bloque 3, bits 5, estado 3) but "real=0 respaldo=0", and no rung of fix40 serves that: the
 * lending is for read watches, the fault-in meets its own protection entry and the shadow emulation
 * has no shadow to lean on, so the access walks into the fatal report.
 *
 * One page is the shape of the problem: the guest touches one page and needs four kilobytes, while
 * the two prices of this port are 512 entries for a page-path block and two for a page of its own
 * (one 4 KB code alias plus one window call). The page is served through the same calls as the page
 * path, the ones this console has already accepted, and it lives in State::sueltas until the block
 * can be materialised - then its content moves into the block backing and the page is given back - or
 * until the chunk is released.
 *
 * The gate does not refuse on the port's own arithmetic: over the ceiling the raw window is handed
 * back first, which is the headroom the kernel really sees, and the attempt goes ahead anyway,
 * because the alternative to trying is the abort. What decides is the kernel, with its own 0xCE01.
 */
struct PaginaSuelta {
    void* pagina;  /* the heap page of this run */
    void* alias;   /* its code alias: the source of the window and the door of the emulation */
    VirtmemReservation* res;
};
constexpr size_t kSueltasMax = 2048;  /* loose pages at once: 8 MB, 4096 entries in the worst case */

/* fix41: what the last rung did this run, for the next log to read. */
std::atomic<size_t> g_sueltas_n{0};       /* pages served loose */
std::atomic<size_t> g_sueltas_vistas{0};  /* window calls they have cost */
std::atomic<size_t> g_sueltas_neg{0};     /* pages this path could not serve */

struct State {
    /*
     * Exclusive lock, and it cannot be replaced by a reader/writer one.
     *
     * shared_mutex was tried and it hung the game. The idea was sound on paper: the three calls of
     * the fault path only read, so they could run concurrently. But the performance profiler suspends
     * threads to sample them (svcSetThreadActivity in switch_perf.cpp) and then takes this same lock
     * to read the memory counters.
     *
     * With shared_mutex that is fatal: libstdc++ gives writers preference, so if a thread holding the
     * shared lock is suspended, the first writer to arrive queues up, and all following readers queue
     * behind it. The whole process stops. It hung exactly like that in a test: all 17 threads idle and
     * the profiler unable to write a single report.
     *
     * What did pay off from that attempt is in exception_handler_switch.cpp: asking once per fault
     * instead of five times. That reduces lock traffic without changing how it blocks.
     */
    std::mutex mutex;
    uint8_t* base = nullptr;
    size_t   size = 0;
    VirtmemReservation* reservation = nullptr;

    std::vector<View> views;
    /* fix19: raw physical views, created on demand. See VistaCruda. */
    std::vector<VistaCruda> raw_views;
    size_t   raw_bytes = 0;

    /* chunks sorted by offset: allows finding the one that covers an offset */
    std::map<size_t, Chunk> chunks;
    /*
     * Logical permissions per page of the window, keyed by offset from base.
     * Only pages that are not REX_GM_WRITE are stored; REX_GM_WRITE is the common
     * case, so the table stays small. A page with an entry here is unmapped from
     * the window, whether it is committed or not.
     */
    std::map<size_t, RexGmAccess> protection;
    /*
     * Which (view index, chunk offset) pairs are mapped. A chunk is not mapped into
     * all five views when it is committed: it enters each view the first time it is
     * accessed there. The real ceiling on Horizon is the process limit on mappable
     * memory, and mapping too much exhausts it.
     */
    std::set<std::pair<size_t, size_t>> view_mapped;
    /*
     * fix41: pages served one by one when their block could not be materialised. Keyed by mapping
     * offset of the page, exactly like chunks. They go away when the block is materialised (see
     * AbsorberSueltas) and when the chunk is released (see SoltarSueltasDeRango).
     */
    std::map<size_t, PaginaSuelta> sueltas;
    /* fix41: which (view index, page offset) pairs a loose page has been mapped into. */
    std::set<std::pair<size_t, size_t>> sueltas_vistas;

    /*
     * fix42: which (view index, page mapping offset) pairs a page of a materialised block has
     * been mapped into on its own. They are dropped when the whole block enters the view
     * (MapChunkIntoView) and when it leaves it (UnmapChunkFromViews), which are the two ways
     * these addresses change hands.
     */
    std::set<std::pair<size_t, size_t>> paginas_vistas;

    size_t   committed = 0;
    size_t   mapped = 0;
    uint32_t last_result = 0;
};

State& S() {
    static State s;
    return s;
}

/*
 * fix26: mapping-lock tracker.
 *
 * The last runs ended with a screen that stops and with nothing written afterwards, and there was no way
 * to tell whether that was the card, the process or this lock. The three fault-path calls and every
 * commit take this lock, and the whole memory model queues behind it, so a thread that stays inside it
 * stops the game exactly like a blocked card would.
 *
 * It costs two atomic stores and one maximum per acquisition, no system calls:
 *   - g_lock_ini is the tick when the current holder entered (0 = free), so "how long has it been held"
 *     can be read at any moment from another thread (RexGmLockHeldTicks).
 *   - g_lock_max / g_lock_n are the longest hold and the number of acquisitions since the last
 *     snapshot (RexGmLockSnapshot); the watchdog of switch_perf.cpp reads them once per second.
 *
 * GmLock replaces std::lock_guard<std::mutex> at every use of this mutex: same blocking, plus the
 * counters. Nothing else changes, and a lock_guard could not be used because there is no hook to
 * measure how long it was held.
 */
static std::atomic<u64> g_lock_ini{0};
static std::atomic<u64> g_lock_max{0};
static std::atomic<u64> g_lock_n{0};

struct GmLock {
    std::mutex& mutex;
    u64 ini;
    explicit GmLock(std::mutex& m) : mutex(m), ini(0) {
        mutex.lock();
        ini = armGetSystemTick();
        g_lock_ini.store(ini, std::memory_order_relaxed);
    }
    ~GmLock() {
        const u64 dt = armGetSystemTick() - ini;
        g_lock_ini.store(0, std::memory_order_relaxed);
        g_lock_n.fetch_add(1, std::memory_order_relaxed);
        u64 visto = g_lock_max.load(std::memory_order_relaxed);
        while (dt > visto &&
               !g_lock_max.compare_exchange_weak(visto, dt, std::memory_order_relaxed)) {
        }
        mutex.unlock();
    }
    GmLock(const GmLock&) = delete;
    GmLock& operator=(const GmLock&) = delete;
};

/*
 * fix19: the view of an index, in either index space. It returns NULL if the index is not a
 * live view, which cannot happen for an index that ViewIndexFor has just returned, but it keeps
 * the callers honest.
 */
const View* ViewOf(State& s, size_t vi) {
    if (vi >= kRawViewBase) {
        const size_t i = vi - kRawViewBase;
        if (i >= s.raw_views.size() || !s.raw_views[i].viva) return nullptr;
        return &s.raw_views[i].v;
    }
    if (vi >= s.views.size()) return nullptr;
    return &s.views[vi];
}

size_t VistasCrudasVivas(State& s) {
    size_t n = 0;
    for (const VistaCruda& r : s.raw_views) {
        if (r.viva) n++;
    }
    return n;
}

Handle Proc() { return envGetOwnProcessHandle(); }

bool Intersect(size_t a, size_t al, size_t b, size_t bl, size_t* lo, size_t* hi) {
    const size_t l = a > b ? a : b;
    const size_t ea = a + al, eb = b + bl;
    const size_t h = ea < eb ? ea : eb;
    if (l >= h) return false;
    *lo = l;
    *hi = h;
    return true;
}

/* Finds the chunk that contains an offset of the mapping. */
Chunk* ChunkAt(State& s, size_t offset) {
    auto it = s.chunks.upper_bound(offset);
    if (it == s.chunks.begin()) return nullptr;
    --it;
    Chunk& c = it->second;
    return (offset >= c.offset && offset < c.offset + c.length) ? &c : nullptr;
}

/*
 * The view that contains a window address. The raw physical ones are asked first: inside the raw
 * range they are the most specific, and they are the only ones that describe what the guest meant
 * when it used a physical address as a pointer.
 */
const View* ViewFor(State& s, uint64_t addr) {
    for (const VistaCruda& r : s.raw_views) {
        if (!r.viva) continue;
        const uint64_t lo = reinterpret_cast<uint64_t>(r.v.base);
        if (addr >= lo && addr < lo + r.v.length) return &r.v;
    }
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}

RexGmAccess AccessOf(State& s, size_t page_off) {
    auto it = s.protection.find(page_off);
    return it == s.protection.end() ? REX_GM_WRITE : it->second;
}

/* Is there committed backing behind this window address? */
bool IsCommittedWindowAddress(State& s, uint64_t addr) {
    const View* v = ViewFor(s, addr);
    if (!v) return false;
    const size_t off = v->offset + static_cast<size_t>(addr - reinterpret_cast<uint64_t>(v->base));
    return ChunkAt(s, off) != nullptr;
}

/*
 * Unmaps [win_off, win_off+len) from the window, skipping pages with logical
 * protection, which are already unmapped. `src` is the shadow address that
 * corresponds to win_off.
 *
 * Walk: `it` always points to the first protected page >= cur. Keys are
 * page-aligned and increasing, so skipping a protected page and advancing the
 * iterator keeps that invariant, and the loop always makes progress.
 */
/*
 * Read-only page: try it, and if it is refused, go back to unmapping
 *
 * Why. Watching writes by unmapping the page has an expensive side effect: the
 * guest cannot read from it either, and every read is an exception. Measured in
 * a race: 108,000 emulated reads per second at about 9 us each, which is
 * practically the whole core of the main thread.
 *
 * With the page read-only, watching works the same (the write faults and the
 * handler services it) but reads are free.
 *
 * Why it is not a given. svcSetProcessMemoryPermission only accepts memory whose
 * state allows permission changes. The shadow is mapped with svcMapProcessMemory
 * from the heap backing, and whether Horizon allows permission changes there too
 * is not documented anywhere.
 *
 * So it is tried once. The first page that needs watching is tried with
 * permissions; if the kernel refuses, that is recorded and from then on pages
 * are unmapped as before. Nothing is left half done: if the call fails, it has
 * not changed anything.
 */
enum class SoloLectura { kSinProbar, kSi, kNo };
SoloLectura g_solo_lectura = SoloLectura::kSinProbar;

/* Returns true if the page was protected through permissions. */
bool ProtegerConPermisos(uint8_t* pagina, bool solo_lectura) {
    if (g_solo_lectura == SoloLectura::kNo) return false;

    const Result rc = svcSetProcessMemoryPermission(
        Proc(), reinterpret_cast<u64>(pagina), kPageSize, solo_lectura ? Perm_R : Perm_Rw);
    if (R_FAILED(rc)) {
        if (g_solo_lectura == SoloLectura::kSinProbar) {
            g_solo_lectura = SoloLectura::kNo;  // no se vuelve a intentar
            /*
             * fix20: the code was never written down, and the whole profile hangs on it. With the
             * call refused, "modo: desmapeo" is not a choice of the port any more but the only thing
             * left, and every read of a watched page costs an exception: 160,000 per second in the
             * fix19 run, which is where the frames went.
             */
            fprintf(stderr, "[fix20] solo-lectura RECHAZADA rc=0x%08X: vigilancia por desmapeo\n",
                    static_cast<unsigned>(rc));
        }
        return false;
    }
    g_solo_lectura = SoloLectura::kSi;
    return true;
}
void UnmapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        /* No holes here: everything was mapped, watched or not. */
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + win_off, Proc(),
                                              reinterpret_cast<u64>(src), len))) {
            s.mapped -= len;
        }
        return;
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur))) {
            s.mapped -= stop - cur;
        }
        cur = stop;
    }
}

/*
 * When watched pages are read-only instead of unmapped, the whole range is mapped
 * at once and the watched pages then get their permission lowered. Simpler and
 * faster than skipping them.
 */
bool MapWindowRangeSoloLectura(State& s, size_t win_off, uint8_t* src, size_t len) {
    const Result rc =
        svcMapProcessMemory(s.base + win_off, Proc(), reinterpret_cast<u64>(src), len);
    if (R_FAILED(rc)) {
        s.last_result = rc;
        return false;
    }
    s.mapped += len;
    for (auto it = s.protection.lower_bound(win_off);
         it != s.protection.end() && it->first < win_off + len; ++it) {
        ProtegerConPermisos(s.base + it->first, true);
    }
    return true;
}

/*
 * Maps [win_off, win_off+len) honoring the logical protection. If a call fails,
 * it undoes what this call had mapped and returns false: it never leaves a range
 * half mapped.
 */
bool MapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        return MapWindowRangeSoloLectura(s, win_off, src, len);
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        const Result rc = svcMapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur);
        if (R_SUCCEEDED(rc)) AnotarMapeoVentana(reinterpret_cast<u64>(s.base + cur), stop - cur);
        if (R_FAILED(rc)) {
            s.last_result = rc;
            /*
             * The unmap walks the same way as this loop, so it removes exactly what was
             * mapped, no more and no less.
             */
            UnmapWindowRange(s, win_off, src, cur - win_off);
            return false;
        }
        s.mapped += stop - cur;
        cur = stop;
    }
    return true;
}

/*
 * Maps the range [offset, offset+length) of a chunk into every view that covers
 * it. This is where the 360 mirrors come from.
 *
 * All or nothing: if it fails in one view, it undoes the previous ones. The
 * alternative (leaving a view unmapped and carrying on) would make every access
 * to it be emulated one by one by the exception handler: it would work, but at a
 * fraction of the speed and without any warning. That is exactly the bug the
 * first version had.
 */
bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
    /* fix19: the raw views first, as in ViewFor. Their index is kRawViewBase + position. */
    for (size_t i = 0; i < s.raw_views.size(); i++) {
        if (!s.raw_views[i].viva) continue;
        const uint64_t lo = reinterpret_cast<uint64_t>(s.raw_views[i].v.base);
        if (addr >= lo && addr < lo + s.raw_views[i].v.length) {
            *out = kRawViewBase + i;
            return true;
        }
    }
    for (size_t i = 0; i < s.views.size(); i++) {
        const uint64_t lo = reinterpret_cast<uint64_t>(s.views[i].base);
        if (addr >= lo && addr < lo + s.views[i].length) { *out = i; return true; }
    }
    return false;
}

/*
 * fix22: materialisation of a committed chunk, deferred to its first touch.
 *
 * Before, RexGmCommit did all of this at commit time and for the whole request: it allocated the
 * heap backing, zeroed it and created its code-memory alias. The game asks for 0x37080000 (883 MB)
 * in a single commit while the loading screen is up, and that one request took the process to
 * 3185/3189 MB of Horizon's mapping limit: from there on every 2 MB alias went from 25-80 ms to
 * about 500 ms, the guest walk over its physical window (the fix19 raw aliases) fell to 4 MB/s and
 * the screen froze at 0.0 fps with the main thread at 98.9% of CPU.
 *
 * Now the chunk is registered as a bare range (no backing, no alias) and it is materialised here,
 * from MapChunkIntoView, on the first access that really needs the backing. The chunking is the
 * same as before (kBlockSize granules, see RexGmCommit), so a commit of 883 MB leaves 442
 * descriptors of 2 MB and only the blocks that are touched are paid for.
 *
 * `c.shadow` is the marker: NULL means "registered, not materialised". Every path that only reads
 * bookkeeping treats it as it did before: RexGmShadowFor and RexGmBorrowWatchForRead refuse a chunk
 * without a shadow, and UnmapChunkFromViews finds no entry in view_mapped for it.
 */
/*
 * fix27: what the failing path leaves behind, once. mallinfo tells a fragmented arena (free space
 * with no run big enough: fordblks high while keepcost is small) from a really exhausted one
 * (fordblks near zero), and the State says how far the guest got. It is called with the mapping
 * lock already held, so it reads the counters straight from the State and takes no other lock:
 * RexGmCommittedBytes and friends would deadlock here.
 */
void DiagnosticoSinBacking(const State& s, Chunk& c, size_t block, bool fallo_2m) {
    if (g_sin_backing.fetch_add(1, std::memory_order_relaxed) != 0) return;
    const struct mallinfo mi = mallinfo();
    fprintf(stderr,
            "[fix27] sin backing: off=0x%zX len=0x%zX fallo 2MB=%d intentos=%u previos=%zu\n"
            "[fix27] arena: arena=%llu fordblks=%llu keepcost=%llu uordblks=%llu ordblks=%llu\n"
            "[fix27] invitado: comprometido=%llu MB mapeado=%llu MB bloques=%zu alias 2MB=%llu "
            "denegaciones 2MB=%llu\n",
            c.offset, block, fallo_2m ? 1 : 0, c.fallos,
            g_sin_backing.load(std::memory_order_relaxed) - 1,
            static_cast<unsigned long long>(mi.arena), static_cast<unsigned long long>(mi.fordblks),
            static_cast<unsigned long long>(mi.keepcost),
            static_cast<unsigned long long>(mi.uordblks),
            static_cast<unsigned long long>(mi.ordblks),
            static_cast<unsigned long long>(s.committed >> 20),
            static_cast<unsigned long long>(s.mapped >> 20), s.chunks.size(),
            static_cast<unsigned long long>(g_aliases_grandes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_denegaciones_2m.load(std::memory_order_relaxed)));
}

/*
 * fix28: the commit that froze the port did not stop because of the limit but because of the shape
 * of the arena. At the stall mallinfo said arena 3071 MB, 690 MB free spread over 10523 pieces and
 * a top chunk of 24 KB, so neither memalign(2 MB, 2 MB) nor its 4 KB fallback (which still asks for
 * 2 MB of contiguous bytes) could be granted, and the guest spun for ever on the first block it
 * could not get (main thread 99.6%, 0.0 fps, "[fix27] sin backing ... intentos=1").
 *
 * The pool takes its memory while the arena is still a few big runs and hands it out in 2 MB
 * slices: a slab is one memalign(2 MB, kPiscinaLosa) and every slice inside it is 2 MB aligned by
 * construction, so a slice is there while the slab has room, whatever the arena looks like
 * afterwards. Slices released by a chunk go to a free list and come back for the next block, which
 * keeps a session that loads and unloads from growing the footprint. The pool stops taking slabs
 * while the arena still has kPiscinaPisoLibre for everyone else and never goes past kPiscinaTope:
 * beyond that the block falls back to libc, exactly as it did before.
 */
constexpr size_t kPiscinaLosa = 96ull * 1024 * 1024;
constexpr size_t kPiscinaLosasMax = 24;   /* fix34: room for the small slabs the ladder asks for */
constexpr size_t kPiscinaTope = 1600ull * 1024 * 1024;  /* fix34: the cap was the only gate that ever ran */
/*
 * fix36: the pool took its memory at init and kept it idle, and this batch shows the bill in one
 * and the same run. At the crash (t=25.1 s) the pool holds 1536 MB of which 1388 are idle, while
 * the arena has 59 MB libre in 7316 pieces and the pipeline the GPU thread is compiling gets a NULL
 * from its own arena and dies on it. In the frozen run the pool is 1536 MB and all of it in use, the
 * process sits at 3185/3189 MB and the 80 blocks the guest still needs are never backed. The two
 * phenomena are one mechanism: the pool is keeping the host memory the emulator needs, and it keeps
 * it for blocks that may never come. From here the pool holds a working set and asks for the rest on
 * demand: it stops taking slabs while it has kPiscinaInactivaMax idle, and demand tops it up again
 * from PiscinaTomar, which already asks for the next slab before the current one runs out, so a slab
 * taken on demand is still taken while the arena has room.
 */
constexpr size_t kPiscinaInactivaMax = 4 * kPiscinaLosa;  /* fix36: 384 MB of idle slices is enough */
/*
 * fix31: two things were wrong with the pool in the batch that froze here. The floor was one of them:
 * at t=20 s the arena had 69 MB libre and a slab was refused for it, with later slices that the guest
 * needed and the page path had to buy at 512 entries each. The other is the size of a slab: the run
 * froze with 386 MB libre and slab 7 refused, not for the floor (386 >= 256 + 96) but because a 96 MB
 * contiguous run cannot be formed out of 16308 pieces, so the pool stopped at 576 MB while the guest
 * still had 14 blocks to back. 64 MB is the floor from here, and the slab is asked for in a ladder:
 * 96 MB first, then 16, then 4, then 2. A 2 MB slab is worth having even when a big one is not: two
 * slices, two entries, against 1024 for the same 2 MB through the page path.
 */
constexpr size_t kPiscinaPisoLibre = 48ull * 1024 * 1024;
constexpr size_t kPiscinaLosaMin = 2ull * 1024 * 1024;  /* fix31: smallest slab the ladder asks for */
constexpr size_t kReusoMax = 1024;  /* fix33: room for the slices the compactor hands back */
constexpr size_t kBloquesMargen = 8;
/*
 * fix35: the wait of fix34 cost the pool its window, and this batch says so in one line: the pool
 * never took a slab. [fix31] piscina: losa is absent from the whole run, every heartbeat reads
 * pisc=0/0, and the blocks the guest is stuck on had to be asked of libc, which cannot form a 2 MB
 * square out of an arena holding 672 MB in 15958 pieces.
 *
 * The walk of fix34 came back every 8192 requests, and this run made about fifteen thousand of
 * them, so it walked the ladder once early and once late. Early was the one that mattered: at
 * cortes 39 the arena had 59 MB libre, the floor of the 96 MB step answered (59 < 48 + 96) and the
 * ladder broke out of the loop, so the 4 MB step - which 59 MB does afford - was never asked and the
 * walk left with nothing. From there the wait held the pool off until the arena was 672 MB spread
 * over 15958 pieces, where no step can be granted. In between lay the window fix33 used: at
 * t=20.8 s the arena had 166 MB libre, over the 144 MB the 96 MB step needs, and each of the
 * fourteen slabs of that batch was granted exactly there (arena usado rose 96 MB per slab while
 * libre stayed between 160 and 187 MB: the slab is a fresh mmap, not arena free space). The wait
 * skipped it and the pending list grew to 298 blocks, against the 96 of the batch that had the pool.
 *
 * So the wait goes: the pool is asked on every request again, exactly as it was when it filled to
 * 1344 MB. A failed walk costs four memalign calls - the 96 MB one is a single mmap the kernel
 * refuses - next to a guest that is going round the same page for ever.
 *
 * And the walk no longer walks out on a step it cannot afford: a step above the floor is skipped and
 * the next one down is asked, which is what the ladder was written for. The two counts in the
 * refusal line say which steps were skipped and which ones the arena really turned down.
 */
std::atomic<size_t> g_piscina_intentos{0};  /* fix35: ladder calls, reported, never a gate */
uint32_t g_piscina_avisos_tope = 0;
uint32_t g_piscina_avisos_piso = 0;
uint32_t g_piscina_avisos_esc = 0;
uint32_t g_piscina_avisos_inactiva = 0;  /* fix36: the idle cap reports itself */

struct Piscina {
    uint8_t* losas[kPiscinaLosasMax]{};
    size_t losa_bytes[kPiscinaLosasMax]{};  /* fix31: how big each slab really is (ladder) */
    size_t n_losas = 0;
    size_t en_curso = 0;            /* slab being handed out */
    size_t cursor = 0;              /* bytes already handed out of losas[en_curso] */
    void* reuso[kReusoMax]{};
    size_t n_reuso = 0;
    /*
     * fix33: one bit per slice, set while the slice is handed out or waiting on the free list. A
     * cession that could not queue a slice used to lose it: the free list was the only record of
     * what could be handed out again, so a slice that came back while the list was full stayed out
     * of reach for the rest of the session. The bit is what lets the compactor find those.
     */
    uint8_t usadas[kPiscinaLosasMax][kPiscinaLosa / kBlockSize]{};
};
Piscina g_piscina;
std::atomic<size_t> g_piscina_bytes{0};       /* taken from the arena, in use or not */
std::atomic<size_t> g_piscina_en_uso{0};      /* bytes of slices backing a chunk right now */
std::atomic<size_t> g_piscina_cortes{0};      /* slab requests refused (cap, floor or denied) */

void PiscinaRefusar(size_t fordblks, const char* motivo, size_t saltadas = 0,
                    size_t intentadas = 0) {
    const size_t n = g_piscina_cortes.fetch_add(1, std::memory_order_relaxed);
    const bool es_tope = motivo[0] == 't';
    const bool es_escalera = motivo[0] == 'e';
    uint32_t* avisos = es_tope ? &g_piscina_avisos_tope
                               : (es_escalera ? &g_piscina_avisos_esc : &g_piscina_avisos_piso);
    if (*avisos >= 8) return;
    ++*avisos;
    /*
     * fix35: the two counts say why the ladder came back empty. "saltadas" are the steps the floor
     * turned down without asking - a small libre makes the 96 MB step unaffordable while the 4 MB one
     * is still there - and "intentadas" the ones the arena was really asked for. The batch that froze
     * here skipped the 96 MB step and walked out with nothing, four steps below the one line.
     */
    fprintf(stderr,
            "[fix35] piscina: sin losa nueva (%s) arena libre %zu MB en %zu trozos, escalones saltados "
            "%zu intentados %zu, tomado %zu MB, en uso %zu MB, cortes %zu, intentos %zu\n",
            motivo, fordblks >> 20, static_cast<size_t>(mallinfo().ordblks), saltadas, intentadas,
            g_piscina_bytes.load(std::memory_order_relaxed) >> 20,
            g_piscina_en_uso.load(std::memory_order_relaxed) >> 20, n + 1,
            g_piscina_intentos.load(std::memory_order_relaxed));
}

/*
 * fix37: the arena query of this path was the hot one. Every fault that reaches the pool asks libc
 * for the free-list figures, and mallinfo walks the heap: in the stall of this batch "cortes" rose
 * from 38 to 18568, one refusal per fault, so that walk was paid with the mapping lock held, by the
 * whole process. The figure is a gate, not a decision that has to be fresh to the byte, so it is
 * cached for kArenaCacheMs and the one-shot diagnostics keep their own live calls
 * (DiagnosticoSinBacking, the pool's own lines and the pages path).
 */
constexpr u64 kArenaCacheMs = 250;
size_t ArenaLibreCacheado(void) {
    static u64 ultima = 0;
    static size_t libre = 0;
    const u64 ahora = svcGetSystemTick();
    if (ultima == 0 || (ahora - ultima) >= kArenaCacheMs * kTicksPorMs) {
        libre = static_cast<size_t>(mallinfo().fordblks);
        ultima = ahora;
    }
    return libre;
}

/*
 * fix37: the milestones. Four snapshots of the figures the heartbeat already carries, taken where the
 * memory question splits: the end of the init, the first 2 MB refusal, the first time the page path
 * hits its hard tier, and the first block left without backing (the moment the screen freezes).
 * Between them they say whether the ~1.6 GB the arena reports and the guest cannot account for was
 * already there before the game ran, or arrived with the load.
 */
void Hito(const char* donde) {
    const struct mallinfo mi = mallinfo();
    unsigned long long vivos = 0, bloques = 0;
    for (size_t i = 0; i < kCajas; ++i) {
        vivos += g_caja_vivo_bytes[i].load(std::memory_order_relaxed);
        bloques += g_caja_vivo_bloques[i].load(std::memory_order_relaxed);
    }
    fprintf(stderr,
            "[fix37] hito %s: arena u=%zu MB f=%zu MB t=%zu KB n=%zu | piscina %zu/%zu MB cortes=%zu "
            "| entradas pag=%zu neg2m=%zu sin_back=%zu | heap asignado %llu MB liberado %llu MB vivos "
            "%llu MB en %llu bloques | mallinfo uordblks=%llu MB\n",
            donde, static_cast<size_t>(mi.uordblks) >> 20, static_cast<size_t>(mi.fordblks) >> 20,
            static_cast<size_t>(mi.keepcost) >> 10, static_cast<size_t>(mi.ordblks),
            g_piscina_bytes.load(std::memory_order_relaxed) >> 20,
            g_piscina_en_uso.load(std::memory_order_relaxed) >> 20,
            g_piscina_cortes.load(std::memory_order_relaxed),
            g_entradas_paginas.load(std::memory_order_relaxed),
            g_denegaciones_2m.load(std::memory_order_relaxed),
            g_sin_backing.load(std::memory_order_relaxed),
            g_malloc_asignado_total.load(std::memory_order_relaxed) >> 20,
            g_malloc_liberado_total.load(std::memory_order_relaxed) >> 20, vivos >> 20, bloques,
            static_cast<unsigned long long>(mi.uordblks) >> 20);
    fprintf(stderr,
            "[fix37] heap clases (KB vivos/bloques): <=32B %llu/%llu | <=128B %llu/%llu | <=512B "
            "%llu/%llu | <=2K %llu/%llu | <=8K %llu/%llu | <=64K %llu/%llu | <=512K %llu/%llu | "
            ">512K %llu/%llu | llamadas %llu | asignado KB %llu | liberado KB %llu\n",
            g_caja_vivo_bytes[0].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[0].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[1].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[1].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[2].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[2].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[3].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[3].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[4].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[4].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[5].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[5].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[6].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[6].load(std::memory_order_relaxed),
            g_caja_vivo_bytes[7].load(std::memory_order_relaxed) >> 10,
            g_caja_vivo_bloques[7].load(std::memory_order_relaxed),
            g_malloc_llamadas_total.load(std::memory_order_relaxed),
            g_malloc_asignado_total.load(std::memory_order_relaxed) >> 10,
            g_malloc_liberado_total.load(std::memory_order_relaxed) >> 10);
}

bool PiscinaTomarLosa(void) {
    if (g_piscina.n_losas >= kPiscinaLosasMax) return false;
/* fix31: these two gates are the smallest slab's, not the 96 MB one: the ladder below decides. */
    if (g_piscina_bytes.load(std::memory_order_relaxed) + kPiscinaLosaMin > kPiscinaTope) {
        PiscinaRefusar(0, "tope");
        return false;
    }
    const size_t libre = ArenaLibreCacheado();  /* fix37: cached, see the note on the helper */
    if (libre < kPiscinaPisoLibre + kPiscinaLosaMin) {
        PiscinaRefusar(libre, "piso");
        return false;
    }
    /*
     * fix31: the ladder. A 96 MB slab is the one that keeps the pool out of the arena's way, but it is
     * also the one a fragmented arena cannot form, and at the freeze it was 96 MB or nothing. The size
     * drops down the steps until one is granted, and each slab keeps its own size, so the handing out
     * below never walks off the end of a small one.
     */
    /*
     * fix35: the walk is asked on every request again (fix34's wait is gone, see the block above the
     * counters) and it no longer breaks out on a step it cannot afford: the floor test belongs to the
     * step, and a step above it is skipped for the next one down.
     */
    static const size_t kEscalones[] = {kPiscinaLosa, 16ull * 1024 * 1024, 4ull * 1024 * 1024,
                                        kPiscinaLosaMin};
    /*
     * fix35: the 96 MB step is the one the arena's free list does not pay for. 96 MB is past every
     * mmap threshold glibc will raise, so the slab is a fresh mmap and fordblks never moves: in the
     * batch that filled this pool arena usado rose 96 MB with each slab while libre stayed between
     * 160 and 187 MB. The floor is there to keep the pool out of the arena's free list, this step
     * does not touch it, and a small libre is therefore not a reason to leave the pool empty: it is
     * asked as soon as the pool's own floor is met. The smaller steps can be served out of the arena
     * and they keep the full floor.
     */
    g_piscina_intentos.fetch_add(1, std::memory_order_relaxed);
    size_t saltadas = 0;
    size_t intentadas = 0;
    size_t tam = 0;
    void* losa = nullptr;
    for (size_t i = 0; i < sizeof(kEscalones) / sizeof(kEscalones[0]); ++i) {
        const size_t escala = kEscalones[i];
        const size_t piso = i == 0 ? kPiscinaPisoLibre : kPiscinaPisoLibre + escala;
        if (libre < piso) {
            ++saltadas;
            continue;
        }
        ++intentadas;
        losa = memalign(kBlockSize, escala);
        if (losa) {
            tam = escala;
            break;
        }
    }
    if (!losa) {
        /* fix34: the ladder failed with the arena above its floor: the pool is not full, the arena
           cannot form the square. fix35: the line carries the two counts that say which steps were
           never asked and which ones the arena turned down. */
        PiscinaRefusar(libre, "escalera", saltadas, intentadas);
        return false;
    }
    g_piscina.losas[g_piscina.n_losas] = static_cast<uint8_t*>(losa);
    g_piscina.losa_bytes[g_piscina.n_losas] = tam;
    ++g_piscina.n_losas;
    g_piscina_bytes.fetch_add(tam, std::memory_order_relaxed);
    fprintf(stderr,
            "[fix31] piscina: losa %zu (%zu MB %s) en %p, tomado %zu MB, arena usado %zu MB, libre "
            "%zu MB\n",
            g_piscina.n_losas, tam >> 20, tam == kPiscinaLosa ? "llena" : "corta", losa,
            g_piscina_bytes.load(std::memory_order_relaxed) >> 20,
            static_cast<size_t>(mallinfo().uordblks) >> 20, libre >> 20);
    return true;
}

/*
 * fix32: the pool is filled up to its cap while the arena can still form 96 MB squares, instead of one
 * slab at a time. It was the shape of the batch that froze here that asked for this: the pool sat on
 * 992 MB (ten full slabs and two 16 MB ones) while 183 blocks - 366 MB of guest memory - were still
 * pending, and the arena, by the time the last slab was asked for, could no longer form anything big.
 * A slice out of a slab costs one entry; the same 2 MB through the page path costs 512, and the run
 * was already up against the entry count. Taking the slabs early, while the arena is still clean, is
 * what turns those 183 blocks from 512 entries each into 1 each.
 *
 * It stops at the first slab that is not full: once the arena has to drop down the ladder the free
 * space is already in pieces, and asking again on every fault would only pay the search twice.
 */
void PiscinaReservar(void) {
    for (;;) {
        /*
         * fix36: the pool stops here while it holds kPiscinaInactivaMax slices nobody is using. The
         * call that fills to kPiscinaTope runs at init, and it is what left the pool holding memory
         * the emulator needed later; the demand path tops the pool up again as soon as the working
         * set falls below the cap, so no burst is left without a fresh slab.
         */
        const size_t tomado = g_piscina_bytes.load(std::memory_order_relaxed);
        const size_t en_uso = g_piscina_en_uso.load(std::memory_order_relaxed);
        const size_t inactiva = tomado > en_uso ? tomado - en_uso : 0;
        if (inactiva + kPiscinaLosa > kPiscinaInactivaMax) {
            if (g_piscina_avisos_inactiva < 8) {
                ++g_piscina_avisos_inactiva;
                fprintf(stderr,
                        "[fix36] piscina: no reservo mas, inactiva %zu MB de %zu MB tomados y %zu MB "
                        "en uso; el arena queda para el emulador (uordblks %zu MB, libre %zu MB en %zu "
                        "trozos)\n",
                        inactiva >> 20, tomado >> 20, en_uso >> 20,
                        static_cast<size_t>(mallinfo().uordblks) >> 20,
                        static_cast<size_t>(mallinfo().fordblks) >> 20,
                        static_cast<size_t>(mallinfo().ordblks));
            }
            return;
        }
        if (!PiscinaTomarLosa()) return;
        if (g_piscina.losa_bytes[g_piscina.n_losas - 1] < kPiscinaLosa) return;
        if (g_piscina.n_losas >= kPiscinaLosasMax) return;
        if (g_piscina_bytes.load(std::memory_order_relaxed) + kPiscinaLosa > kPiscinaTope) return;
    }
}

/*
 * fix33: the pool keeps a bit per slice, because the free list was not the whole truth.
 *
 * The batch that froze here left the two numbers one line apart: 1344 MB taken from the arena and
 * 1248 MB in use, with no slice on the free list and the guest still 120 blocks (240 MB) short. The
 * 96 MB in between - 48 slices - were inside slabs the pool had walked past and no list entry
 * pointed at any of them: a cession queues a slice only while kReusoMax has room, and the slices
 * that came back when it did not were dropped out of reach and out of the accounting at once, so
 * the pool said no for ever while its own slabs still had room. With a bit per slice a cession that
 * could not queue is still free, and the compactor below finds it.
 *
 * Invariant: a bit is set while the slice is handed out to a chunk or waiting on the free list; it
 * is clear only for a slice of a slab the cursor has already walked past, which is exactly what the
 * compactor may take. Slices the cursor has not reached are never touched there, or the same slice
 * would come out twice.
 */
void PiscinaMarcar(size_t losa, size_t idx, bool en_uso) {
    if (losa >= kPiscinaLosasMax || idx >= kPiscinaLosa / kBlockSize) return;
    g_piscina.usadas[losa][idx] = en_uso ? 1 : 0;
}

bool PiscinaSituar(void* p, size_t& losa, size_t& idx) {
    if (!p || g_piscina.n_losas == 0) return false;
    const uint8_t* q = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < g_piscina.n_losas; ++i) {
        const uint8_t* b = g_piscina.losas[i];
        if (q >= b && q < b + g_piscina.losa_bytes[i]) {
            losa = i;
            idx = static_cast<size_t>(q - b) / kBlockSize;
            return true;
        }
    }
    return false;
}

bool PiscinaCompactar(void) {
    if (g_piscina.n_losas == 0) return false;
    size_t recuperadas = 0;
    for (size_t i = 0; i < g_piscina.n_losas && g_piscina.n_reuso < kReusoMax; ++i) {
        const size_t vistas = g_piscina.losa_bytes[i] / kBlockSize;
        const size_t limite = (i == g_piscina.en_curso) ? (g_piscina.cursor / kBlockSize) : vistas;
        for (size_t j = 0; j < limite && g_piscina.n_reuso < kReusoMax; ++j) {
            if (g_piscina.usadas[i][j]) continue;
            g_piscina.usadas[i][j] = 1;  /* on the list from here: same invariant */
            g_piscina.reuso[g_piscina.n_reuso++] = g_piscina.losas[i] + j * kBlockSize;
            ++recuperadas;
        }
    }
    if (recuperadas > 0) {
        static uint32_t aviso = 0;
        if (aviso < 8) {
            ++aviso;
            fprintf(stderr,
                    "[fix33] piscina: compactada, %zu lonchas recuperadas (%zu MB), en uso %zu MB de "
                    "%zu MB tomados\n",
                    recuperadas, (recuperadas * kBlockSize) >> 20,
                    g_piscina_en_uso.load(std::memory_order_relaxed) >> 20,
                    g_piscina_bytes.load(std::memory_order_relaxed) >> 20);
        }
    }
    return recuperadas > 0;
}

void* PiscinaTomar(void) {
    if (g_piscina.n_reuso > 0) {
        --g_piscina.n_reuso;
        void* p = g_piscina.reuso[g_piscina.n_reuso];
        g_piscina.reuso[g_piscina.n_reuso] = nullptr;
        g_piscina_en_uso.fetch_add(kBlockSize, std::memory_order_relaxed);
        return p;
    }
    if (g_piscina.n_losas == 0 ||
        g_piscina.cursor + kBlockSize > g_piscina.losa_bytes[g_piscina.en_curso]) {
        /* The current slab is full: move on to the one that was asked for in advance. */
        if (g_piscina.en_curso + 1 < g_piscina.n_losas) {
            ++g_piscina.en_curso;
            g_piscina.cursor = 0;
        } else {
            PiscinaReservar();
            if (g_piscina.en_curso + 1 < g_piscina.n_losas) {
                ++g_piscina.en_curso;
                g_piscina.cursor = 0;
            }
        }
    }
    /*
     * Ask for the next slab before this one runs out, so that a refusal does not leave a block
     * waiting: the refusals repeat on every fault, so a pool that could not grow once can still
     * grow later, when the arena has room again.
     */
    const size_t bytes_curso =
        g_piscina.n_losas > 0 ? g_piscina.losa_bytes[g_piscina.en_curso] : 0;
    if (g_piscina.n_losas > 0 && g_piscina.en_curso + 1 >= g_piscina.n_losas &&
        g_piscina.cursor + kBloquesMargen * kBlockSize > bytes_curso) {
        PiscinaReservar();
    }
/*
     * fix33: the slab is exhausted, but the pool may still be holding slices it walked past. Take
     * them back before saying no. The compactor only touches slices behind the cursor, so a slice it
     * hands out here and one the cursor would reach later can never be the same.
     */
    if (g_piscina.n_losas == 0 || g_piscina.en_curso >= g_piscina.n_losas ||
        g_piscina.cursor + kBlockSize > g_piscina.losa_bytes[g_piscina.en_curso]) {
        if (PiscinaCompactar() && g_piscina.n_reuso > 0) {
            --g_piscina.n_reuso;
            void* q = g_piscina.reuso[g_piscina.n_reuso];
            g_piscina.reuso[g_piscina.n_reuso] = nullptr;
            g_piscina_en_uso.fetch_add(kBlockSize, std::memory_order_relaxed);
            return q;
        }
        return nullptr;
    }
    const size_t idx = g_piscina.cursor / kBlockSize;
    void* p = g_piscina.losas[g_piscina.en_curso] + g_piscina.cursor;
    g_piscina.cursor += kBlockSize;
    PiscinaMarcar(g_piscina.en_curso, idx, true);
    g_piscina_en_uso.fetch_add(kBlockSize, std::memory_order_relaxed);
    return p;
}

bool PiscinaCeder(void* p) {
    size_t losa = 0;
    size_t idx = 0;
    if (!PiscinaSituar(p, losa, idx)) return false;
    if (g_piscina.n_reuso < kReusoMax) {
        /* On the list from here: the bit stays set while the slice waits there. */
        PiscinaMarcar(losa, idx, true);
        g_piscina.reuso[g_piscina.n_reuso++] = p;
    } else {
        /*
         * fix33: the list is full, so the slice goes back to its place instead of out of reach. This
         * is the 96 MB that never came back: dropping the slice here left it neither on the list nor
         * in the accounting, and the pool went on refusing while its own slabs were half empty. With
         * the bit cleared, the compactor finds it the next time the pool runs out of slabs.
         */
        PiscinaMarcar(losa, idx, false);
    }
    g_piscina_en_uso.fetch_sub(kBlockSize, std::memory_order_relaxed);
    return true;
}

/* A backing goes back to the pool when it came from it, and to libc when it did not. */
void CederBacking(void* p) {
    if (!p) return;
    if (PiscinaCeder(p)) return;
    free(p);
}

/*
 * fix29: the last resource, for the block the pool and libc both refuse.
 *
 * This batch's stall said it plainly: arena 3071 MB with 2707 MB handed out, 364 MB free cut into
 * 10490 pieces and a top chunk of 52 KB. Neither memalign(2 MB, 2 MB) nor its 4 KB-aligned 2 MB
 * fallback could be granted (the second one still asks for 2 MB of contiguous bytes), the pool could
 * not take another 96 MB slab (301 refusals) and the guest went round the same page for ever
 * (manejador SDK 74668/s, main thread at 99.5%, 0.0 fps). The free space was not missing, it was
 * small: a 4 KB page is never in short supply, a 2 MB run is.
 *
 * So a block that everything else refused is rebuilt page by page. The pages come from a small free
 * list (a block that is released gives its pages back to the next one, so a session that loads and
 * unloads does not grow the footprint) or from the heap one at a time, and the shadow goes in run by
 * run: a run is a stretch of pages that happens to be contiguous, so the usual case costs a handful
 * of svcMapProcessCodeMemory calls instead of 512, and a run of 512 pages on a 2 MB boundary would
 * still give the block its single descriptor. Nothing is taken that a contiguous request could have
 * used: this only runs after the pool, the 2 MB request and the 4 KB-aligned 2 MB request have all
 * refused.
 *
 * Everything here runs under the state mutex (the fault path and the release path both hold it), so
 * the free list is plain data, exactly like the pool's.
 */
constexpr size_t kPaginasReusoMax = 4096;  /* pages kept for reuse: 16 MB, enough for the churn */
constexpr unsigned kPaginasAvisos = 16;    /* bounded lines: what the first blocks of this kind cost */

void* g_paginas_reuso[kPaginasReusoMax]{};
std::atomic<size_t> g_paginas_reuso_n{0};        /* pages in the free list (read by the report) */
std::atomic<size_t> g_paginas_en_uso{0};         /* pages backing a chunk right now */
std::atomic<size_t> g_bloques_con_paginas{0};    /* blocks materialised page by page */
std::atomic<size_t> g_paginas_sin_sitio{0};      /* blocks this path could not back either */

void* PaginaTomar(void) {
    const size_t n = g_paginas_reuso_n.load(std::memory_order_relaxed);
    if (n > 0) {
        g_paginas_reuso_n.store(n - 1, std::memory_order_relaxed);
        void* p = g_paginas_reuso[n - 1];
        g_paginas_reuso[n - 1] = nullptr;
        g_paginas_en_uso.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void* p = memalign(kPageSize, kPageSize);
    if (p) g_paginas_en_uso.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void PaginaCeder(void* p) {
    if (!p) return;
    g_paginas_en_uso.fetch_sub(1, std::memory_order_relaxed);
    const size_t n = g_paginas_reuso_n.load(std::memory_order_relaxed);
    if (n < kPaginasReusoMax) {
        g_paginas_reuso[n] = p;
        g_paginas_reuso_n.store(n + 1, std::memory_order_relaxed);
        return;
    }
    free(p);
}

/* The run a page starts: the pages that follow it and are contiguous with it. */
size_t FinDeRun(void* const* paginas, size_t n, size_t desde) {
    size_t j = desde;
    while (j + 1 < n && static_cast<uint8_t*>(paginas[j]) + kPageSize == paginas[j + 1]) ++j;
    return j;
}

/* fix32: defined with the raw-view machinery further down; the fault path holds the state mutex. */
bool LiberarPresupuestoParaCarga(State& s);

/* fix42: defined with the block and view machinery further down. */
void SoltarPaginasDeRango(State& s, size_t inicio, size_t largo);

bool MaterializarPorPaginas(State& s, Chunk& c, size_t block, size_t alineacion) {
    const u64 desde = svcGetSystemTick();
    const size_t n = block / kPageSize;
    /*
     * fix31: this path is the expensive one and it is rationed. Every page costs the kernel one entry
     * (a run is contiguous but it is not a 2 MB square), so a block costs 512 entries, and the batch
     * that froze here had ten blocks of this kind going in (5120 entries) while the whole cheap side
     * of the run was 508 one-entry aliases. Past the ration it refuses without writing the block off,
     * so the liberator can still hand part of the raw window back and the block is asked again on the
     * next fault.
     */
size_t ya = g_entradas_paginas.load(std::memory_order_relaxed);
    if (ya + n > kEntradasPaginasTope) {
/*
         * fix32: a refusal here is no longer free. The batch that froze here sat on exactly 4096
         * entries with sixteen "racionada" lines on the same offset and not one "RECHAZADO": the wall
         * was this ration. Over it the raw window is handed back first, and the block goes in when
         * that bought anything - but never past the two ceilings, the hard tier of this path and the
         * total the kernel has really shown it will take.
         */
        if (ya + n > kEntradasPaginasTopeDuro) {
            /* fix37: first time the page path meets its hard tier: the memory story up to here. */
            static bool hito_duro = false;
            if (!hito_duro) { hito_duro = true; Hito("primer tope duro de paginas"); }
            static uint32_t aviso_duro = 0;
            if (aviso_duro < kPaginasAvisos) {
                ++aviso_duro;
                fprintf(stderr, "[fix32] por paginas: tope duro (%zu + %zu > %zu) off=0x%zX\n", ya, n,
                        kEntradasPaginasTopeDuro, c.offset);
            }
            g_presion_mapeo.store(true, std::memory_order_relaxed);
            return false;  /* sin tocar c.fallos: el bloque vuelve por este camino */
        }
/*
         * fix33: the total is what the kernel really metered, and the window path over-counted it by
         * seven to one: 801 window calls were charged 5894 entries because a window is not a 2 MB
         * square, while the batch that the kernel really refused had 6572 and the one that went
         * through had 5622. With the window charged one entry per call the ceiling below sits between
         * the two, and TopeTotal() can only come down when the kernel says so (AnotarRechazoKernel).
         */
        const size_t totales = EntradasTotales();
        if (totales + n > TopeTotal()) {
            static uint32_t aviso_techo = 0;
            if (aviso_techo < kPaginasAvisos) {
                ++aviso_techo;
                fprintf(stderr, "[fix33] por paginas: techo de entradas (%zu + %zu > %zu) off=0x%zX\n",
                        totales, n, TopeTotal(), c.offset);
            }
            g_presion_mapeo.store(true, std::memory_order_relaxed);
            return false;  /* sin tocar c.fallos: el bloque vuelve por este camino */
        }
        if (!LiberarPresupuestoParaCarga(s)) {
            static uint32_t aviso_racion = 0;
            if (aviso_racion < kPaginasAvisos) {
                ++aviso_racion;
                fprintf(stderr,
                        "[fix32] por paginas: racionada tras reciclar (%zu + %zu > %zu) off=0x%zX\n", ya,
                        n, kEntradasPaginasTope, c.offset);
            }
            g_presion_mapeo.store(true, std::memory_order_relaxed);
            return false;  /* sin tocar c.fallos: el bloque vuelve por este camino */
        }
        static uint32_t aviso_blanda = 0;
        if (aviso_blanda < kPaginasAvisos) {
            ++aviso_blanda;
            fprintf(stderr,
                    "[fix32] por paginas: racionada pero reciclada (%zu + %zu > %zu) off=0x%zX, sigue\n",
                    ya, n, kEntradasPaginasTope, c.offset);
        }
        ya = g_entradas_paginas.load(std::memory_order_relaxed);
    }
    void** paginas = static_cast<void**>(malloc(n * sizeof(void*)));
    if (!paginas) {
        ++c.fallos;
        return false;
    }
    size_t tomadas = 0;
    for (; tomadas < n; ++tomadas) {
        void* p = PaginaTomar();
        if (!p) break;
        paginas[tomadas] = p;
    }
    if (tomadas < n) {
        ++c.fallos;
        g_presion_mapeo.store(true, std::memory_order_relaxed);  /* fix30 */
        const size_t aviso = g_paginas_sin_sitio.fetch_add(1, std::memory_order_relaxed);
        if (aviso < kPaginasAvisos) {
            const struct mallinfo mi = mallinfo();
            fprintf(stderr,
                    "[fix29] por paginas: sin sitio off=0x%zX (%zu de %zu paginas, arena libre %zu MB "
                    "trozos %zu)\n",
                    c.offset, tomadas, n, static_cast<size_t>(mi.fordblks) >> 20,
                    static_cast<size_t>(mi.ordblks));
        }
        for (size_t i = 0; i < tomadas; ++i) PaginaCeder(paginas[i]);
        free(paginas);
        return false;
    }
    /* One memset per page: they are not one buffer, and this is what the block gets on first touch. */
    for (size_t i = 0; i < n; ++i) memset(paginas[i], 0, kPageSize);

    virtmemLock();
    void* shadow = virtmemFindCodeMemory(block + alineacion, alineacion);
    if (shadow) {
        shadow = reinterpret_cast<void*>(AlignUp(reinterpret_cast<size_t>(shadow), alineacion));
    }
    VirtmemReservation* shadow_res = shadow ? virtmemAddReservation(shadow, block) : nullptr;
    virtmemUnlock();

    size_t puestas = 0;
    size_t n_runs = 0;
    size_t run_max = 0;
    Result rc = 0xFFFFFFFFu;  /* no code hole: nothing went in */
    if (shadow && shadow_res) {
        while (puestas < n) {
            const size_t fin = FinDeRun(paginas, n, puestas);
            const size_t np = fin - puestas + 1;
            const u64 dst = reinterpret_cast<u64>(shadow) + puestas * kPageSize;
            rc = svcMapProcessCodeMemory(Proc(), dst, reinterpret_cast<u64>(paginas[puestas]),
                                         np * kPageSize);
            if (R_SUCCEEDED(rc)) {
                rc = svcSetProcessMemoryPermission(Proc(), dst, np * kPageSize, Perm_Rw);
                if (R_SUCCEEDED(rc)) AnotarMapeoCodigo(dst, np * kPageSize);
            }
            if (R_FAILED(rc)) {
                svcUnmapProcessCodeMemory(Proc(), dst, reinterpret_cast<u64>(paginas[puestas]),
                                          np * kPageSize);
                break;
            }
            ++n_runs;
            if (np > run_max) run_max = np;
            puestas = fin + 1;
        }
    }

    if (puestas < n) {
        /* Unwind what went in: the runs are recomputed from the same list, so they match. */
        for (size_t k = 0; k < puestas;) {
            const size_t fin = FinDeRun(paginas, puestas, k);
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow) + k * kPageSize,
                                      reinterpret_cast<u64>(paginas[k]), (fin - k + 1) * kPageSize);
            k = fin + 1;
        }
        if (shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(shadow_res);
            virtmemUnlock();
        }
        for (size_t i = 0; i < n; ++i) PaginaCeder(paginas[i]);
        free(paginas);
/* fix30: 0xCE01 is the mapping budget talking: from here on no new raw alias
         * is created while the pressure lasts. */
        if (rc == 0x0000CE01u) g_presion_mapeo.store(true, std::memory_order_relaxed);
        /*
         * fix33: and it is the only voice that may lower the ceiling. The kernel refused with this
         * many entries outstanding, so the ceiling is set to that count on the spot, instead of
         * leaving the next batch to find the same wall by timing out.
         */
        AnotarRechazoKernel(rc);
        ++c.fallos;
        static uint32_t avisos_rechazo = 0;
        if (avisos_rechazo < kPaginasAvisos) {
            ++avisos_rechazo;
            fprintf(stderr,
                    "[fix29] por paginas RECHAZADO off=0x%zX len=0x%zX rc=0x%08X (mayor run %zu "
                    "paginas)\n",
                    c.offset, block, (unsigned)rc, run_max);
        }
        return false;
    }

    c.backing = nullptr;
    c.shadow = static_cast<uint8_t*>(shadow);
    c.shadow_res = shadow_res;
    c.paginas = paginas;
    c.n_paginas = n;
    s.committed += block;
    g_bloques_materializados.fetch_add(1);
    g_bloques_con_paginas.fetch_add(1, std::memory_order_relaxed);
    g_entradas_paginas.fetch_add(n, std::memory_order_relaxed);  /* fix31: 512 entries bought */
    /* fix30: a block went in and the budget is working again, so the raw aliases may come back. */
    g_presion_mapeo.store(false, std::memory_order_relaxed);
    const size_t hechos = g_bloques_con_paginas.load(std::memory_order_relaxed);
    if (hechos <= kPaginasAvisos) {
        const struct mallinfo mi = mallinfo();
        const u64 ms = (svcGetSystemTick() - desde) * 1000ULL / 19200000ULL;
        fprintf(stderr,
                "[fix29] por paginas: bloque %zu off=0x%zX len=0x%zX n=%zu runs=%zu (mayor %zu "
                "paginas = %zu MB) %llu ms | arena libre %zu MB trozos %zu\n",
                hechos, c.offset, block, n, n_runs, run_max, (run_max * kPageSize) >> 20,
                (unsigned long long)ms, static_cast<size_t>(mi.fordblks) >> 20,
                static_cast<size_t>(mi.ordblks));
    }
    return true;
}

/* fix30: defined with the raw-view machinery further down; the fault path holds the state mutex. */
bool LiberarPresupuestoParaCarga(State& s);

/* fix38: likewise, defined with the raw-view machinery further down; the state mutex is held. */
size_t SoltarFisicoSinUso(State& s, const Chunk* excluir);

bool MaterializarBloque(State& s, Chunk& c) {
    if (c.shadow) return true;  /* already materialised */

    /* Slice of the pre-allocated arena: its backing is the arena, released as a whole. */
    if (g_prealloc_size > 0 && c.offset < g_prealloc_size) {
        c.shadow = static_cast<uint8_t*>(g_prealloc_shadow) + c.offset;
        s.committed += c.length;
        return true;
    }

    const u64 desde = svcGetSystemTick();
    size_t block = c.length;
    if (block < kPageSize) block = kPageSize;

    /*
     * fix27: this block already gave up: do not pay for the search again on every fault.
     * fix37: and not for ever either. The block is retried once in a while (kRetiradaEsperaMs),
     * because the memory it needs can come back - a slab, a freed view, a chunk that unloads - and
     * asking on every fault is what turned this run into a storm: cortes went from 38 to 18568 while
     * the game sat at 0.0 fps, every one of those requests a ladder of memalign calls taken with the
     * mapping lock held. Between windows the answer here is the cheap one.
     */
    if (c.fallos >= kIntentosBackingMax) {
        const u64 ahora = svcGetSystemTick();
        if (c.ultimo_intento != 0 && (ahora - c.ultimo_intento) < kRetiradaEsperaMs * kTicksPorMs) {
            return false;
        }
        c.ultimo_intento = ahora;
    }

    /*
     * fix23: a whole block asks for its backing on 2 MB, the same square its window view sits on
     * (see the note on kWindowAlign); the kernel then maps it with one block descriptor instead of
     * 512 page table entries. A piece that is not a whole block (the tail of a commit) keeps 4 KB,
     * as every one of them had before.
     *
     * fix27: 2 MB while the heap grants it. The first refusal switches the process to 4 KB for
     * good (see the note on g_alineacion_baja) and the block is materialised from there, which is
     * where every block was before fix23.
     */
    /*
     * fix28: the pool is asked first and it is the pool, not libc, that decides whether a whole
     * block can be materialised: a slice out of a slab is always 2 MB aligned and always
     * contiguous, so the fragmentation of the arena stops being the reason a block cannot be had.
     * libc is still asked while the pool cannot serve (slabs refused, partial pieces), and a block
     * whose slab could not be granted is retried on the next fault, as every block was.
     */
    const bool quiere_bloque = block >= kBlockSize && g_paginas_grandes != 0;
    /*
     * fix31: 2 MB is asked for even after a refusal. The latch kept the process on the 4 KB path for
     * the rest of the run (see fix27): after the first denial of this batch (neg2m=1) every later
     * block skipped the only request that buys a block with a single entry, and the ones the pool
     * could not serve fell through to the 512-entry page path. The flag stays, as the report reads
     * it, but it no longer decides the request.
     */
    const bool pide_2m = quiere_bloque;
    void* backing = nullptr;
    bool uso_2m = false;
    if (quiere_bloque) {
        backing = PiscinaTomar();
        uso_2m = (backing != nullptr);
    }
    if (!backing && pide_2m) {
        backing = memalign(kBlockSize, block);
        uso_2m = (backing != nullptr);
        if (!uso_2m) {
            if (g_denegaciones_2m.fetch_add(1, std::memory_order_relaxed) < 4) {
                fprintf(stderr, "[fix27] 2 MB denegado off=0x%zX len=0x%zX, se pasa a 4 KB\n", c.offset,
                        block);
            }
            /* fix37: the first 2 MB refusal is where the arena stops being able to form a square. */
            static bool hito_2m = false;
            if (!hito_2m) { hito_2m = true; Hito("primer rechazo 2 MB"); }
            g_alineacion_baja.store(1, std::memory_order_relaxed);
            /*
             * fix31: the arena cannot give a 2 MB square right now, so the pool is asked for a slab
             * straight away: a slice out of a slab is 2 MB aligned for ever, whatever the arena looks
             * like afterwards, and it is the only backing that costs one entry instead of 512.
             */
            /*
             * fix38: before the pool is asked for another slab, take back the guest physical
             * granules its raw cache is still holding with nobody reading them (see
             * SoltarFisicoSinUso). What comes back is a slice, and the retry right below is the
             * one that puts it behind this block: the batch this comes from had 126 raw views
             * created and dropped, 145 blocks (290 MB) queued, the pool at 1256/1256 MB and the
             * arena at 62 MB free in 15554 pieces, with the process at its kernel limit.
             */
            SoltarFisicoSinUso(s, &c);
            PiscinaReservar();
            /*
             * fix36: whatever the pool took just now is for this block. Without the retry, the block
             * libc refused went to the page path (512 entries) while the fresh slice stayed for the
             * next one, which is the trade the pool exists to avoid.
             */
            if (!backing) {
                backing = PiscinaTomar();
                uso_2m = (backing != nullptr);
            }
        }
    }
    if (!backing) {
        backing = memalign(kPageSize, block);
    }
    if (!backing && !g_f_mode_enabled) {
        /*
         * fix29: the pool, the 2 MB request and the 4 KB-aligned 2 MB request have all refused. A
         * block comes back on every fault, so this is the path that has to stop the spin: rebuilt
         * page by page, out of whatever the arena still has, which is exactly what the arena said it
         * had (364 MB in 10490 pieces) when the guest stopped here.
         */
        if (MaterializarPorPaginas(s, c, block, kPageSize)) return true;
        /*
         * fix30: the pool, libc and the pages have all refused, so the block is
         * short of mapping budget. Before it is written off, hand a slice of the
         * raw-view cache back and leave c.fallos alone: the block comes back on
         * the next fault and usually goes in then, which is the difference
         * between the guest going round the same page for ever and the load
         * going on.
         */
        if (LiberarPresupuestoParaCarga(s)) return false;
        /*
         * fix37: the page path refuses without touching fallos (fix30/fix32/fix33), so a block that
         * kept being asked of it never reached the cap of fix27 and was searched for ever: this is
         * the last branch of the search, so this is where the attempt is counted. Reaching the cap
         * does not write the block off (see the note on Chunk::ultimo_intento); it only slows the
         * retries down to a window, which is what gives the process back the mapping lock.
         */
        if (c.fallos < kIntentosBackingMax) ++c.fallos;
        static bool hito_fin = false;
        if (!hito_fin) { hito_fin = true; Hito("primer bloque sin backing"); }
        DiagnosticoSinBacking(s, c, block, pide_2m);
        return false;
    }
    if (!backing) {
        if (LiberarPresupuestoParaCarga(s)) return false;  /* fix30 */
        ++c.fallos;
        DiagnosticoSinBacking(s, c, block, pide_2m);
        return false;
    }
    /*
     * fix27: the alignment the kernel is asked for is the one the backing really has, not the one
     * that was requested. If the 2 MB request was refused and the 4 KB one granted, the shadow goes
     * on 4 KB as well, which is what every block had before fix23: asking for a 2 MB alias of a 4 KB
     * granular backing is not what the code-memory path of this port has ever been given.
     */
    const size_t alineacion = uso_2m ? kBlockSize : kPageSize;
    memset(backing, 0, block);

    if (g_f_mode_enabled) {
        /* Direction F: heap page with MapProcessAllowed, mapped straight into the window. */
        Result rc = svcSetMemoryAttribute(backing, block, 0x8, 0);
        if (R_FAILED(rc)) {
            CederBacking(backing);
            /* fix27: a refused alias counts as an attempt too, or the block comes back for ever. */
            ++c.fallos;
            fprintf(stderr, "RexGmCommit: svcSetMemoryAttribute failed rc=0x%08X at offset %zu\n",
                    (unsigned)rc, c.offset);
            return false;
        }
        c.backing = backing;
        c.shadow = backing;
        c.shadow_res = nullptr;
        s.committed += block;
    } else {
        /* Direction B2: fresh code-memory shadow alias. */
        virtmemLock();
        /*
         * fix23: an aligned alias needs slack: virtmemFindCodeMemory returns the first byte of the
         * hole it finds, so the hole is asked for with room for the alignment and the alias is
         * rounded up inside it. The reservation is made on the rounded address, and the slack goes
         * back to the pool with the next search.
         */
        void* shadow = virtmemFindCodeMemory(block + alineacion, alineacion);
        if (shadow) {
            shadow = reinterpret_cast<void*>(AlignUp(reinterpret_cast<size_t>(shadow), alineacion));
        }
        VirtmemReservation* shadow_res = shadow ? virtmemAddReservation(shadow, block) : nullptr;
        virtmemUnlock();

        Result rc = 0;
        if (shadow && shadow_res) {
            rc = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                         reinterpret_cast<u64>(backing), block);
            if (R_SUCCEEDED(rc)) {
                rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow), block,
                                                   Perm_Rw);
                if (R_SUCCEEDED(rc)) AnotarMapeoCodigo(reinterpret_cast<u64>(shadow), block);
            }
            if (R_FAILED(rc)) {
                svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                          reinterpret_cast<u64>(backing), block);
            }
        }
        if (!shadow || !shadow_res || R_FAILED(rc)) {
            if (shadow_res) {
                virtmemLock();
                virtmemRemoveReservation(shadow_res);
                virtmemUnlock();
            }
            CederBacking(backing);
/* fix27: a refused alias counts as an attempt too, or the block comes back for ever. */
            ++c.fallos;
            /* fix33: the same voice as in the page path: this refusal is the ceiling talking. */
            AnotarRechazoKernel(rc);
            /* Bounded: one line says with what code the alias was refused, not one per block. */
            static uint32_t avisos = 0;
            if (avisos < 32) {
                ++avisos;
                fprintf(stderr, "[fix22] materializar RECHAZADO rc=0x%08X off=0x%zX len=0x%zX\n",
                        (unsigned)rc, c.offset, block);
            }
            return false;
        }
        c.backing = backing;
        c.shadow = shadow;
        c.shadow_res = shadow_res;
        s.committed += block;
        /* fix23: this alias went in on a 2 MB boundary, which is what lets the kernel map it with a
         * block descriptor instead of 512 page table entries. */
        if (alineacion > kPageSize) g_aliases_grandes.fetch_add(1);
    }

    /* fix30: a block went in and the budget is working again, so the raw aliases may come back. */
    g_presion_mapeo.store(false, std::memory_order_relaxed);
    g_bloques_materializados.fetch_add(1);
    const u64 ms = (svcGetSystemTick() - desde) * 1000ULL / 19200000ULL;
    if (ms >= 20) {
        static uint32_t avisos = 0;
        if (avisos < 64) {
            ++avisos;
            fprintf(stderr, "[fix22] lento materializar off=0x%zX len=0x%zX %llu ms\n", c.offset,
                    block, (unsigned long long)ms);
        }
    }
    return true;
}

/*
 * fix41: gives back every loose page inside [inicio, inicio + largo).
 *
 * The window calls go first, with the same (dst, src, len) triple that made them, then the code
 * alias, then the reservation and the page, which joins the free list. Both the materialisation of
 * the block (its backing now holds what the page held) and the release of the chunk (RexGmDecommit,
 * RexGmShutdown, SoltarFisicoSinUso) come through here.
 */
/*
 * fix42: la pagina de un bloque ya materializado, mapeada ella sola en la vista que fallo.
 *
 * Esta es la tanda del primer LOGO: 2,2 millones de fallos sobre la misma ventana (0xBF9A0260
 * al principio y 0xFF4C0000-0xFF500000 despues), 0.0 fps, un nucleo al 95% y ninguna linea
 * fatal. El bloque de esas paginas esta materializado (tiene alias), su pagina en la ventana no
 * lo esta, y el paso (2) de la escalera no la trae: RexGmClearProtect suelta la entrada logica,
 * pero RexGmFaultIn sigue contestando que no. Sus dos salidas tempranas son la entrada de
 * proteccion (que el paso 2 acaba de borrar) y el registro de view_mapped, que dice que el
 * bloque entero ya entro en esa vista y que no vuelve a entrar. Con el bloque entero fuera de
 * presupuesto, la escalera solo sabia emular cada acceso por la sombra, y a 8.000 accesos por
 * segundo la partida se queda quieta sin caerse.
 *
 * Lo que falta es el mapeo de una pagina sola: el alias del bloque ya existe, asi que la pagina
 * no cuesta un alias nuevo, solo una llamada de ventana (una entrada, frente a las 512 de la via
 * por paginas y a las que pide el bloque entero). El presupuesto no crece: cuando la lista esta
 * llena, o cuando la contabilidad dice que el proceso esta en su techo, se suelta la pagina
 * servida de offset mas bajo - el recorrido del invitado sube, asi que es la que menos
 * probablemente vuelva - y el sitio que deja es el que ocupa la nueva.
 *
 * Una pagina con entrada de proteccion no se toca nunca: o es una pagina vigilada o una pagina
 * trampa del propio invitado, y mapearla romperia lo que esa entrada significa. Las paginas de
 * esta lista son solo suyas.
 */
constexpr size_t kPaginasVistasMax = 512;  /* fix42: 2 MB, 512 entradas de ventana como mucho */

static std::atomic<size_t> g_paginas42{0};
static std::atomic<size_t> g_paginas42_neg{0};
static std::atomic<size_t> g_paginas42_soltadas{0};

/*
 * fix42: suelta una de las paginas servidas de una en una, desmapeandola. La clave es (vista,
 * offset de mapeo de la pagina), como la de las paginas sueltas de fix41.
 */
bool SoltarUnaPaginaVista(State& s) {
    if (s.paginas_vistas.empty()) return false;
    auto elegida = s.paginas_vistas.begin();
    for (auto it = s.paginas_vistas.begin(); it != s.paginas_vistas.end(); ++it) {
        if (it->second < elegida->second) elegida = it;
    }
    const size_t vi = elegida->first;
    const size_t pagina = elegida->second;
    const View* v = ViewOf(s, vi);
    Chunk* c = ChunkAt(s, pagina);
    if (v && c && c->shadow) {
        const size_t win_off = static_cast<size_t>(v->base - s.base) + (pagina - c->offset);
        UnmapWindowRange(s, win_off, static_cast<uint8_t*>(c->shadow) + (pagina - c->offset),
                         kPageSize);
    }
    s.paginas_vistas.erase(elegida);
    g_paginas42_soltadas.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/*
 * fix42: el registro de las paginas servidas de un rango de offsets de mapeo. No desmapea: los
 * dos sitios que lo llaman con la direccion aun viva son los que van a desmapear o a mapear el
 * bloque entero justo despues (MapChunkIntoView, UnmapChunkFromViews), y desmapear aqui dejaria
 * un agujero que hunde la llamada de rango que viene detras.
 */
void SoltarPaginasDeRango(State& s, size_t inicio, size_t largo) {
    if (s.paginas_vistas.empty()) return;
    const size_t fin = inicio + largo;
    for (auto it = s.paginas_vistas.begin(); it != s.paginas_vistas.end();) {
        if (it->second < inicio || it->second >= fin) {
            ++it;
            continue;
        }
        it = s.paginas_vistas.erase(it);
    }
}

bool ServirPaginaDeBloque(uint64_t window_address) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;
    const View* vp = ViewOf(s, vi);
    if (!vp) return false;
    const View& v = *vp;

    /* Coordenadas: la proteccion va por offset de ventana, los bloques por offset de mapeo. */
    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    const size_t off =
        v.offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v.base));
    const size_t pagina_ventana = AlignDown(win_off, kPageSize);
    const size_t pagina_mapeo = AlignDown(off, kPageSize);

    /* Una pagina con entrada de proteccion no se toca: es una vigilancia o una pagina trampa. */
    if (s.protection.find(pagina_ventana) != s.protection.end()) return false;

    Chunk* c = ChunkAt(s, pagina_mapeo);
    if (!c || !c->shadow) return false;
    if (pagina_mapeo < c->offset) return false;
    uint8_t* src = static_cast<uint8_t*>(c->shadow) + (pagina_mapeo - c->offset);
    if ((reinterpret_cast<uintptr_t>(src) & (kPageSize - 1)) != 0) return false;
    const std::pair<size_t, size_t> clave(vi, pagina_mapeo);
    if (s.paginas_vistas.count(clave) != 0) return false;  /* ya estaba: no se mapea dos veces */

    if (s.paginas_vistas.size() >= kPaginasVistasMax || EntradasTotales() + 1 > TopeTotal()) {
        /* fix42: el presupuesto no crece: primero se hace sitio. */
        if (!SoltarUnaPaginaVista(s)) return false;
    }

    const size_t win_pagina = static_cast<size_t>(v.base - s.base) + (pagina_mapeo - v.offset);
    if (!MapWindowRange(s, win_pagina, src, kPageSize)) {
        /*
         * El kernel no ha dejado mapear la pagina (0xCE01 o el limite de memoria del proceso).
         * Su alias sigue siendo valido, asi que la sombra puede servir el acceso; el registro no
         * se toca y el siguiente acceso a la pagina lo vuelve a intentar.
         */
        const size_t neg = g_paginas42_neg.fetch_add(1, std::memory_order_relaxed) + 1;
        if (neg <= 32 || (neg % 4096) == 0) {
            fprintf(stderr,
                    "[fix42] pagina de bloque RECHAZADA rc=0x%08X off=0x%zX vista=%zu negadas=%zu "
                    "entradas=%zu/%zu\n",
                    static_cast<unsigned>(s.last_result), pagina_mapeo, vi, neg,
                    EntradasTotales(), TopeTotal());
        }
        return false;
    }

    s.paginas_vistas.insert(clave);
    const size_t n = g_paginas42.fetch_add(1, std::memory_order_relaxed) + 1;
    static bool hito42 = false;
    if (!hito42) {
        hito42 = true;
        Hito("primera pagina de bloque servida");
    }
    static uint32_t aviso42 = 0;
    if (aviso42 < 32 || (n % 4096) == 0) {
        ++aviso42;
        fprintf(stderr,
                "[fix42] pagina de bloque servida off=0x%zX vista=%zu n=%zu entradas=%zu/%zu "
                "soltadas=%zu\n",
                pagina_mapeo, vi, n, EntradasTotales(), TopeTotal(),
                g_paginas42_soltadas.load(std::memory_order_relaxed));
    }
    return true;
}

void SoltarSueltasDeRango(State& s, size_t inicio, size_t largo) {
    /* fix42: las paginas servidas de una en una salen del registro por el mismo camino. */
    SoltarPaginasDeRango(s, inicio, largo);
    if (s.sueltas.empty()) return;
    const size_t fin = inicio + largo;
    auto it = s.sueltas.lower_bound(AlignDown(inicio, kPageSize));
    while (it != s.sueltas.end() && it->first < fin) {
        const size_t pagina = it->first;
        const PaginaSuelta sp = it->second;
        for (auto v = s.sueltas_vistas.begin(); v != s.sueltas_vistas.end();) {
            if (v->second != pagina) {
                ++v;
                continue;
            }
            const View* vp = ViewOf(s, v->first);
            if (vp) {
                size_t lo = 0, hi = 0;
                if (Intersect(pagina, kPageSize, vp->offset, vp->length, &lo, &hi)) {
                    const size_t win_off =
                        static_cast<size_t>(vp->base - s.base) + (lo - vp->offset);
                    UnmapWindowRange(s, win_off, static_cast<uint8_t*>(sp.alias) + (lo - pagina),
                                     hi - lo);
                }
            }
            v = s.sueltas_vistas.erase(v);
        }
        if (sp.alias) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(sp.alias),
                                      reinterpret_cast<u64>(sp.pagina), kPageSize);
        }
        if (sp.res) {
            virtmemLock();
            virtmemRemoveReservation(sp.res);
            virtmemUnlock();
        }
        PaginaCeder(sp.pagina);
        it = s.sueltas.erase(it);
    }
}

/*
 * fix41: the block is backed now, so what had been served loose inside it moves into its backing and
 * the loose page is given back. Called from MapChunkIntoView before the block is mapped, so that no
 * address of the window ends up mapped twice.
 */
void AbsorberSueltas(State& s, Chunk& c) {
    if (s.sueltas.empty() || !c.shadow) return;
    const size_t fin = c.offset + c.length;
    for (auto it = s.sueltas.lower_bound(AlignDown(c.offset, kPageSize));
         it != s.sueltas.end() && it->first < fin;) {
        const size_t pagina = it->first;
        memcpy(static_cast<uint8_t*>(c.shadow) + (pagina - c.offset), it->second.alias, kPageSize);
        SoltarSueltasDeRango(s, pagina, kPageSize);
        it = s.sueltas.lower_bound(pagina);
    }
}

/* Maps a chunk into one view: the view of the faulting access. */
bool MapChunkIntoView(State& s, size_t view_index, Chunk& c) {
    /*
     * fix22: the first touch of the block is what pays for it. Until here the chunk may be no more
     * than a descriptor written by RexGmCommit, with no backing and no shadow to map.
     */
    if (!c.shadow && !MaterializarBloque(s, c)) return false;
    /* fix41: the pages this block had served loose move into its backing now that it has one. */
    AbsorberSueltas(s, c);
    /* fix19: the index can belong to a raw view too, so it goes through ViewOf. */
    const View* vp = ViewOf(s, view_index);
    if (!vp) return false;
    const View& v = *vp;
    size_t lo, hi;
    if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) return false;
    const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
    uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
    /* fix42: el bloque entero cubre ahora las paginas que se sirvieron de una en una. */
    SoltarPaginasDeRango(s, c.offset, c.length);
    if (!MapWindowRange(s, win_off, src, hi - lo)) return false;
    s.view_mapped.insert({view_index, c.offset});
    return true;
}

void UnmapChunkFromViews(State& s, const Chunk& c) {
    /* fix42: el bloque sale de la vista: el registro de sus paginas sale con el. */
    SoltarPaginasDeRango(s, c.offset, c.length);
    /* fix19: it covers both index spaces, so a chunk also leaves the raw views. */
    const auto quitar = [&](size_t vi, const View& v) {
        auto it = s.view_mapped.find({vi, c.offset});
        if (it == s.view_mapped.end()) return;  /* never entered this view */
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) return;
        const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
        uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
        UnmapWindowRange(s, win_off, src, hi - lo);
        s.view_mapped.erase(it);
    };
    for (size_t i = 0; i < s.views.size(); i++) quitar(i, s.views[i]);
    for (size_t i = 0; i < s.raw_views.size(); i++) {
        if (s.raw_views[i].viva) quitar(kRawViewBase + i, s.raw_views[i].v);
    }
}

void ReleaseChunk(Chunk& c) {
    /* fix41: whatever this chunk had served loose goes back before its own backing does. */
    SoltarSueltasDeRango(S(), c.offset, c.length);
    /* Pre-allocated arena: released as a whole in RexGmShutdown. */
    if (g_prealloc_size > 0 && c.offset < g_prealloc_size) return;

    /*
     * fix29: a block rebuilt from single pages goes back the same way. The runs are unmapped one
     * after the other, the reservation is removed and the pages join the free list, which is what
     * makes the next block of this kind free and keeps a session that loads and unloads from
     * growing.
     */
    if (c.paginas) {
        if (c.shadow) {
            for (size_t k = 0; k < c.n_paginas;) {
                const size_t fin = FinDeRun(c.paginas, c.n_paginas, k);
                svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(c.shadow) + k * kPageSize,
                                          reinterpret_cast<u64>(c.paginas[k]),
                                          (fin - k + 1) * kPageSize);
                k = fin + 1;
            }
        }
        if (c.shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(c.shadow_res);
            virtmemUnlock();
        }
        for (size_t k = 0; k < c.n_paginas; ++k) PaginaCeder(c.paginas[k]);
        /*
         * fix31: the ration counts the entries that are alive, so a block released the way fix29
         * released it gives its entries back and a load/unload session does not spend the table.
         */
        const size_t devueltas = c.n_paginas;
        if (g_entradas_paginas.load(std::memory_order_relaxed) >= devueltas) {
            g_entradas_paginas.fetch_sub(devueltas, std::memory_order_relaxed);
        } else {
            g_entradas_paginas.store(0, std::memory_order_relaxed);
        }
        free(c.paginas);
        c.paginas = nullptr;
        c.n_paginas = 0;
        c.shadow = nullptr;
        c.shadow_res = nullptr;
        return;
    }

    if (c.backing) {
        /* Direction B: undo the code alias before freeing the heap backing. */
        if (c.shadow && c.shadow != c.backing) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(c.shadow),
                                      reinterpret_cast<u64>(c.backing), c.length);
        }
        if (c.shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(c.shadow_res);
            virtmemUnlock();
        }
        CederBacking(c.backing);
    }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory),
 * and hbloader pre-inflates the heap with ~3.2 GB, leaving almost nothing of
 * that limit for the shadow code mappings RexGmCommit creates: even committing
 * 8 KB failed with 0xCE01 (KERNELERROR_RESOURCE_EXHAUSTED) at ~500 MB mapped.
 *
 * Ask for 1 GB more of that limit before the first commit. Best effort only:
 * some launch configurations hand out a resource limit handle without
 * SetLimit permission (0xDC01). That failure is harmless (the original limit
 * stays) but is printed so `rex_perfil`'s "limite de mapeo" can be checked
 * against the real value.
 */
void RaiseResourceLimit() {
    u64 v = 0;
    Result rc = svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc)) {
        fprintf(stderr, "RexGmInit: svcGetInfo(InfoType_ResourceLimit) failed rc=0x%08X\n",
                (unsigned)rc);
        return;
    }
    Handle reslimit = static_cast<Handle>(v);
    s64 tope = 0;
    rc = svcGetResourceLimitLimitValue(&tope, reslimit, LimitableResource_Memory);
    if (R_FAILED(rc)) {
        fprintf(stderr, "RexGmInit: svcGetResourceLimitLimitValue(Memory) failed rc=0x%08X\n",
                (unsigned)rc);
        svcCloseHandle(reslimit);
        return;
    }
    rc = svcSetResourceLimitLimitValue(reslimit, LimitableResource_Memory, tope + 0x40000000LL);
    if (R_FAILED(rc)) {
        fprintf(stderr,
                "RexGmInit: svcSetResourceLimitLimitValue(+1 GB) failed rc=0x%08X "
                "(limit was 0x%llX)\n",
                (unsigned)rc, (unsigned long long)tope);
    } else {
        fprintf(stderr,
                "RexGmInit: raised LimitableResource_Memory 0x%llX -> 0x%llX\n",
                (unsigned long long)tope, (unsigned long long)(tope + 0x40000000LL));
    }
    svcCloseHandle(reslimit);
}

/* NOTE: hbloader's pre-inflated heap (~3.2 GB) is shrunk down to 1 GB at the
 * process entry point (windowed_app_main_switch.cpp, main()), BEFORE the
 * libc malloc arena is set up via svcSetHeapSize. Doing it here (or anywhere
 * after malloc has initialized) would leave libc's internal arena pointers
 * stale and cause a Data Abort (EC 0x24) inside _malloc_r. Do not re-add a
 * heap shrink call in this translation unit.
 */

}  // namespace

extern "C" {

uint8_t* RexGmInit(size_t size) {
    {
        setvbuf(stderr, nullptr, _IONBF, 0);
        FILE* f = fopen("/sdmc/rex_diag_rexgminit.txt", "w");
        if (f) { fprintf(f, "RexGmInit reached, size=0x%zX\n", size); fflush(f); fclose(f); }
        fflush(nullptr);
    }
    fprintf(stderr, "[DIAG] RexGmInit entered, size=0x%zX\n", size);
    State& s = S();
    GmLock lock(s.mutex);
    if (s.base) return s.base;

    /* hbloader pre-inflates the heap: raise the mapping limit before the first
     * commit, see RaiseResourceLimit() for why and for the 0xDC01 fallback. */
    RaiseResourceLimit();
    /* ShrinkHeap() moved to main() entry point to avoid unmapping pages already
     * allocated by the C runtime (Data Abort at host heap addresses). */

    /*
     * Direction B: pre-allocate backing in the heap and alias it in the code
     * region. This must happen BEFORE NVK/Vulkan init: the driver consumes most
     * of the code region during setup, and once that happens there is no room
     * for a contiguous code alias of the arena (that was the failure of the
     * earlier all-code-memory design). With NVK out of the way first, a code
     * alias of the whole prealloc is still findable.
     *
     * The alias makes the backing acceptable to svcMapProcessMemory (heap
     * memory alone is refused with 0xD401 InvalidCurrentMemory).
     *
     * Retry with smaller sizes: both the heap allocation of the backing and the
     * code region reservation can run out. Whichever size succeeds, slices of
     * [0, g_prealloc_size) are carved from the arena in RexGmCommit and the
     * whole thing is released as one piece in RexGmShutdown.
     */
#if !defined(__SWITCH__)
    {
        static const size_t kFallbackSizes[] = {
            128ull * 1024 * 1024,       /* 128 MB */
            64ull  * 1024 * 1024,       /* 64  MB */
            32ull  * 1024 * 1024,       /* 32  MB */
            16ull  * 1024 * 1024,       /* 16  MB */
        };
        constexpr size_t kFallbackCount =
            sizeof(kFallbackSizes) / sizeof(kFallbackSizes[0]);
        size_t stage = 0;  /* stage 0 tries the full size, then the fallbacks in order */
        for (;;) {
            const size_t try_size = (stage == 0) ? size : kFallbackSizes[stage - 1];
            g_prealloc_backing = memalign(kPageSize, try_size);
            if (!g_prealloc_backing) {
                fprintf(stderr, "RexGmInit: memalign(%zu) failed\n", try_size);
            } else {
                memset(g_prealloc_backing, 0, try_size);
                virtmemLock();
                g_prealloc_shadow = virtmemFindCodeMemory(try_size, kPageSize);
                if (g_prealloc_shadow) {
                    g_prealloc_shadow_res = virtmemAddReservation(g_prealloc_shadow, try_size);
                }
                virtmemUnlock();

                Result rc = 0;
                if (g_prealloc_shadow && g_prealloc_shadow_res) {
                    rc = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                                 reinterpret_cast<u64>(g_prealloc_backing), try_size);
                    if (R_SUCCEEDED(rc)) {
                        rc = svcSetProcessMemoryPermission(Proc(),
                                                           reinterpret_cast<u64>(g_prealloc_shadow),
                                                           try_size, Perm_Rw);
                    }
                    if (R_FAILED(rc)) {
                        svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                                  reinterpret_cast<u64>(g_prealloc_backing), try_size);
                    }
                }
                if (g_prealloc_shadow && g_prealloc_shadow_res && R_SUCCEEDED(rc)) {
                    g_prealloc_size = try_size;
                    fprintf(stderr, "RexGmInit: prealloc %zu bytes shadow=%p (code VA)\n",
                            try_size, g_prealloc_shadow);
                    break;
                }
                fprintf(stderr, "RexGmInit: prealloc %zu failed (code_va=%d) rc=0x%08X\n",
                        try_size, g_prealloc_shadow != nullptr, (unsigned)rc);
                if (g_prealloc_shadow_res) {
                    virtmemLock();
                    virtmemRemoveReservation(g_prealloc_shadow_res);
                    virtmemUnlock();
                    g_prealloc_shadow_res = nullptr;
                }
                g_prealloc_shadow = nullptr;
                free(g_prealloc_backing);
                g_prealloc_backing = nullptr;
            }
            ++stage;
            if (stage > kFallbackCount) {
                fprintf(stderr, "RexGmInit: prealloc failed for all sizes -- falling back to on-demand (B2)\n");
                g_prealloc_size = 0;
                break;
            }
        }
    }
#else
    g_prealloc_size = 0;
    fprintf(stderr, "RexGmInit: prealloc disabled on Switch (on-demand fault-in only)\n");
#endif

    /*
     * fix28: one slab is taken here, while the arena is still a handful of big runs, so that the
     * first blocks of the first commit do not depend on how the heap looks after the driver has
     * taken its own. The rest is asked for on demand, from materialisation itself.
     */
    PiscinaReservar();

    /* Direction F: try converting heap pages to MapProcessAllowed
     * and mapping them directly with svcMapProcessMemory, bypassing the code-memory
     * shadow that NVK exhausts.
     *
     * MOD v6.27: probe unconditionally.  Even a small prealloc (e.g. 128 MB) is
     * quickly exhausted for a 4.5 GB arena and B2 fails with CE01 because NVK has
     * already consumed the code-memory mapping quota.  If F works we drop the B
     * prealloc entirely and let every commit go through heap+MapProcessAllowed. */
    {
        void* test_backing = memalign(kPageSize, kPageSize);
        if (test_backing) {
            memset(test_backing, 0, kPageSize);
            /* MemoryAttribute_MapProcessAllowed = 0x8 */
            Result rc = svcSetMemoryAttribute(test_backing, kPageSize, 0x8, 0);
            fprintf(stderr,
                    "RexGmInit: svcSetMemoryAttribute(MapProcessAllowed) rc=0x%08X\n",
                    (unsigned)rc);
            if (R_SUCCEEDED(rc)) {
                virtmemLock();
                void* test_dst = virtmemFindAslr(kPageSize, kPageSize);
                VirtmemReservation* test_res =
                    test_dst ? virtmemAddReservation(test_dst, kPageSize) : nullptr;
                virtmemUnlock();
                if (test_dst && test_res) {
                    rc = svcMapProcessMemory(test_dst, Proc(),
                                             reinterpret_cast<u64>(test_backing), kPageSize);
                    fprintf(stderr,
                            "RexGmInit: svcMapProcessMemory(test_dst=%p, proc=%lu, backing=%p) rc=0x%08X\n",
                            test_dst, (unsigned long)Proc(), test_backing, (unsigned)rc);
                    if (R_SUCCEEDED(rc)) {
                        g_f_mode_enabled = true;
                        fprintf(stderr,
                                "RexGmInit: Direction F enabled (heap+MapProcessAllowed)\n");
                        /* Release any Direction B prealloc -- F covers the whole arena. */
                        if (g_prealloc_size > 0) {
                            fprintf(stderr,
                                    "RexGmInit: releasing Direction B prealloc (%zu bytes)\n",
                                    g_prealloc_size);
                            svcUnmapProcessCodeMemory(Proc(),
                                                      reinterpret_cast<u64>(g_prealloc_shadow),
                                                      reinterpret_cast<u64>(g_prealloc_backing),
                                                      g_prealloc_size);
                            if (g_prealloc_shadow_res) {
                                virtmemLock();
                                virtmemRemoveReservation(g_prealloc_shadow_res);
                                virtmemUnlock();
                                g_prealloc_shadow_res = nullptr;
                            }
                            g_prealloc_shadow = nullptr;
                            free(g_prealloc_backing);
                            g_prealloc_backing = nullptr;
                            g_prealloc_size = 0;
                        }
                        svcUnmapProcessMemory(test_dst, Proc(),
                                              reinterpret_cast<u64>(test_backing), kPageSize);
                    } else {
                        fprintf(stderr,
                                "RexGmInit: Direction F map FAILED (test_dst=%p, proc=%lu, rc=0x%08X)\n",
                                test_dst, (unsigned long)Proc(), (unsigned)rc);
                    }
                    virtmemLock();
                    virtmemRemoveReservation(test_res);
                    virtmemUnlock();
                }
            }
            free(test_backing);
        }
        if (!g_f_mode_enabled) {
            fprintf(stderr,
                    "RexGmInit: Direction F probe failed, code-memory path unavailable\n");
            /* Fall through to window init; RexGmCommit will fail on first on-demand chunk. */
        }
    }

    /*
     * fix23: the mode has to be settled before the window is reserved, because it decides how the
     * base is aligned; it is read here from its flag, so it does not matter who calls RexGmInit.
     */
    {
        const int32_t modo = REXCVAR_GET(guest_memory_large_pages);
        if (modo != g_paginas_grandes) RexGmConfigure(modo);
    }

    virtmemLock();
    /* Reserve the guest window in regular virtual address space (not code memory).
     * The window itself only needs to be a reserved VA range; actual mappings are
     * injected via svcMapProcessMemory from the shadow (code memory) on demand.
     * Using virtmemFindAslr instead of virtmemFindCodeMemory frees ~4.5 GB of
     * code memory virtual space for the shadow allocations, which is critical on
     * native 4 GB Switch where code memory VA is tight after Mesa/NVK init.
     *
     * fix23: virtmemFindAslr takes a *guard size* and not an alignment, so the base it returns is
     * only 4 KB aligned and no view over it can ever be congruent with its backing (see the note
     * on kWindowAlign): the kernel then maps everything with 4 KB page tables and runs out of
     * blocks with RAM to spare. In mode 1/2 the hole is asked for with room for the alignment and
     * the base is rounded up inside it; the reservation is made on the rounded address, so the
     * slack of the hole goes back to the pool. */
    if (g_paginas_grandes != 0) {
        uint8_t* hueco = static_cast<uint8_t*>(virtmemFindAslr(size + kWindowAlign, kBlockSize));
        s.base = hueco ? reinterpret_cast<uint8_t*>(
                             AlignUp(reinterpret_cast<size_t>(hueco), kWindowAlign))
                       : nullptr;
        if (!s.base) {
            fprintf(stderr,
                    "[fix23] base alineada a 0x%zX RECHAZADA (sin hueco de 0x%zX); sigo con la "
                    "base de 4 KB\n",
                    kWindowAlign, size + kWindowAlign);
        }
    }
    if (!s.base) s.base = static_cast<uint8_t*>(virtmemFindAslr(size, kPageSize));
    if (s.base) s.reservation = virtmemAddReservation(s.base, size);
    virtmemUnlock();

    if (!s.base || !s.reservation) {
        s.base = nullptr;
        /* Undo the code alias before the backing can be freed again. */
        if (g_prealloc_backing && g_prealloc_shadow && g_prealloc_size > 0) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                      reinterpret_cast<u64>(g_prealloc_backing), g_prealloc_size);
        }
        if (g_prealloc_shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(g_prealloc_shadow_res);
            virtmemUnlock();
            g_prealloc_shadow_res = nullptr;
        }
        g_prealloc_shadow = nullptr;
        free(g_prealloc_backing);
        g_prealloc_backing = nullptr;
        g_prealloc_size = 0;
        fprintf(stderr, "[DIAG] RexGmInit: return nullptr (window alloc failed)\n");
        return nullptr;
    }

    s.size = size;
    g_size_rapido.store(size, std::memory_order_release);
    g_base_rapido.store(s.base, std::memory_order_release);
    /*
     * fix23: these are the numbers that decide the page size of every mapping: the offset of the
     * base inside a 2 MB square (0 is the one that makes a view congruent with its backing) and
     * the grid the chunks are laid out on. Printed at every startup, so each run says which of the
     * three modes it is in.
     */
    fprintf(stderr,
            "[fix23] paginas grandes modo=%d ventana base=%p (desfase 2 MB=0x%zX) bloques=0x%zX "
            "fase fisica=0x%zX\n",
            g_paginas_grandes, (void*)s.base, reinterpret_cast<size_t>(s.base) & (kBlockSize - 1),
            kBlockSize, g_fase_fisica);
    fprintf(stderr, "[DIAG] RexGmInit: return base=%p size=0x%zX\n", (void*)s.base, s.size);
    /* fix37: what the heap holds once the port is up, before the game has loaded anything. */
    {
        static bool hito_init = false;
        if (!hito_init) { hito_init = true; Hito("fin de RexGmInit"); }
    }
    return s.base;
}

void RexGmShutdown(void) {
    State& s = S();
    GmLock lock(s.mutex);

    for (auto& kv : s.chunks) {
        UnmapChunkFromViews(s, kv.second);
        ReleaseChunk(kv.second);
    }
    s.chunks.clear();
    s.views.clear();
    s.raw_views.clear();  /* fix19 */
    s.raw_bytes = 0;
    s.protection.clear();
    s.committed = 0;
    s.mapped = 0;

    if (s.reservation) {
        virtmemLock();
        virtmemRemoveReservation(s.reservation);
        virtmemUnlock();
        s.reservation = nullptr;
    }
    s.base = nullptr;
    s.size = 0;
    /*
     * Reverse order from startup: the base is cleared first, so nobody gets in with
     * a size that is no longer valid.
     */
    g_base_rapido.store(nullptr, std::memory_order_release);
    g_size_rapido.store(0, std::memory_order_release);

    /* Release the pre-allocated arena (Direction B): unmap its code alias first
     * (the backing becomes usable again), drop the virtmem reservation, then
     * free the heap backing. */
    if (g_prealloc_backing && g_prealloc_shadow && g_prealloc_size > 0) {
        svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                  reinterpret_cast<u64>(g_prealloc_backing), g_prealloc_size);
    }
    if (g_prealloc_shadow_res) {
        virtmemLock();
        virtmemRemoveReservation(g_prealloc_shadow_res);
        virtmemUnlock();
        g_prealloc_shadow_res = nullptr;
    }
    g_prealloc_shadow = nullptr;
    if (g_prealloc_backing) {
        free(g_prealloc_backing);
        g_prealloc_backing = nullptr;
    }
    g_prealloc_size = 0;
}

uint8_t* RexGmBase(void) { return g_base_rapido.load(std::memory_order_acquire); }

size_t RexGmSize(void) { return g_size_rapido.load(std::memory_order_acquire); }

bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;
    const View* v = ViewFor(s, window_address);
    if (!v) return false;
    if (offset_out) {
        *offset_out =
            v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    }
    return true;
}

size_t RexGmCommittedBytes(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.committed;
}

size_t RexGmMappedBytes(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.mapped;
}

/*
 * fix28: the pool, as the report has to see it: how much it took from the arena, how much of that is
 * backing a chunk right now, and how many slab requests were refused (the number that tells a
 * fragmented arena from an exhausted one when the next freeze has to be read).
 */
size_t RexGmPiscinaBytes(void) {
    return g_piscina_bytes.load(std::memory_order_relaxed);
}
size_t RexGmPiscinaEnUso(void) {
    return g_piscina_en_uso.load(std::memory_order_relaxed);
}
size_t RexGmPiscinaCortes(void) {
    return g_piscina_cortes.load(std::memory_order_relaxed);
}

/*
 * fix29: the page-by-page fallback, as the report has to see it. How many blocks had to be rebuilt
 * that way, how many pages are backing a chunk right now, how many wait in the free list and how
 * many times not even this path could back a block. The last number is the one that separates a
 * fragmented arena (this path carries the load) from an exhausted one (nothing does).
 */
size_t RexGmPaginasEnUso(void) {
    return g_paginas_en_uso.load(std::memory_order_relaxed);
}
size_t RexGmBloquesPorPaginas(void) {
    return g_bloques_con_paginas.load(std::memory_order_relaxed);
}
size_t RexGmPaginasReuso(void) {
    return g_paginas_reuso_n.load(std::memory_order_relaxed);
}
size_t RexGmPaginasSinSitio(void) {
    return g_paginas_sin_sitio.load(std::memory_order_relaxed);
}
/* fix31: the entries the page path has alive, out of the ration it is given. */
size_t RexGmEntradasPaginas(void) {
    return g_entradas_paginas.load(std::memory_order_relaxed);
}

/*
 * fix30: the mapping budget as the report has to see it: how many calls of each kind the port has
 * made and how many kernel entries they cost, how many raw views the emergency gave back and whether
 * the port is under pressure right now.
 */
size_t RexGmMapeosCodigo(void) { return g_mapeos_codigo_n.load(std::memory_order_relaxed); }
size_t RexGmMapeosCodigoMb(void) { return g_mapeos_codigo_bytes.load(std::memory_order_relaxed) >> 20; }
size_t RexGmEntradasCodigo(void) { return g_entradas_codigo.load(std::memory_order_relaxed); }
size_t RexGmMapeosVentana(void) { return g_mapeos_ventana_n.load(std::memory_order_relaxed); }
size_t RexGmMapeosVentanaMb(void) { return g_mapeos_ventana_bytes.load(std::memory_order_relaxed) >> 20; }
size_t RexGmEntradasVentana(void) { return g_entradas_ventana.load(std::memory_order_relaxed); }
size_t RexGmLiberacionesVistas(void) { return g_lib_vistas.load(std::memory_order_relaxed); }
size_t RexGmLiberacionesEventos(void) { return g_lib_eventos.load(std::memory_order_relaxed); }
int RexGmPresionMapeo(void) { return g_presion_mapeo.load(std::memory_order_relaxed) ? 1 : 0; }

size_t RexGmChunkCount(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.chunks.size();
}

size_t RexGmProtectedPages(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.protection.size();
}

int RexGmModoProteccion(void) {
    switch (g_solo_lectura) {
        case SoloLectura::kSi: return 1;
        case SoloLectura::kNo: return 2;
        default: return 0;
    }
}

uint32_t RexGmLastResult(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.last_result;
}

bool RexGmAddView(uint8_t* base, size_t offset, size_t length) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;
    if (base < s.base || static_cast<size_t>(base - s.base) + length > s.size)
        return false;  /* outside the reserved window */

    const View v{base, offset, length};

    /*
     * Nothing is mapped here: what is already committed enters this view the first
     * time it is touched.
     */
    s.views.push_back(v);
    return true;
}

/*
 * fix19: drops a raw physical view, unmapping everything that had entered it.
 *
 * It runs when the guest virtual heap takes one of its pages: from then on that page is a virtual
 * allocation in use, and the raw view must stop answering for it, or the game would keep reading
 * physical bytes where it has just asked for its own memory.
 *
 * The entry stays in the list (viva = false) so that the indexes of the views created after it do
 * not move.
 */
void SoltarVistaCruda(State& s, size_t i) {
    VistaCruda& r = s.raw_views[i];
    if (!r.viva) return;
    const size_t vi = kRawViewBase + i;

    /* Every chunk that had entered this view, by its (view, chunk) key. */
    std::vector<size_t> trozos;
    for (const auto& par : s.view_mapped) {
        if (par.first == vi) trozos.push_back(par.second);
    }
    for (size_t off : trozos) {
        auto it = s.chunks.find(off);
        if (it != s.chunks.end()) UnmapChunkFromViews(s, it->second);
    }

    const size_t invitado = static_cast<size_t>(r.v.base - s.base);
    r.viva = false;
    s.raw_bytes -= r.v.length;
    fprintf(stderr, "[fix19] alias liberado invitado=0x%08zX len=0x%zX\n", invitado, r.v.length);
}

/*
 * fix30: hands back up to `max` live raw views. They are a cache of the guest physical window and
 * not a requirement: the chunks behind them stay, and what really goes back to the kernel is the
 * mapping budget of the entries that had put those chunks into the raw views - out of the same
 * budget the block that could not be materialised is asking for.
 */
namespace {
extern "C++" {
/*
 * fix30: the unnamed namespace is reopened here on purpose. LiberarPresupuestoParaCarga is
 * declared in the first one, next to the fault path, and the calls in MaterializarBloque bind
 * to that declaration: its definition has to land in the unnamed namespace too. A reopened
 * unnamed namespace is the same namespace, so the two are the same entity, and the global-scope
 * users below still reach these two helpers through the implicit using-directive.
 */
size_t LiberarVistasCrudas(size_t max) {
    State& s = S();
    size_t hechas = 0;
    for (size_t i = 0; i < s.raw_views.size() && hechas < max; i++) {
        if (!s.raw_views[i].viva) continue;
        SoltarVistaCruda(s, i);
        ++hechas;
    }
    return hechas;
}

/*
 * fix30: called with the state mutex held when a block has been refused by the pool, by libc and by
 * the page-by-page path. It releases a slice of the raw-view cache and returns true when it handed
 * something back: the caller then leaves c.fallos alone and the block is asked again on the next
 * fault. Bounded: kLibVistasPorEvento views per event and kLibEventosMax events per run, so the raw
 * window cannot be eaten up; when nothing could be handed back the caller writes the block off as it
 * did before.
 */
/*
 * fix38: hands the pool back the guest physical granules that no view holds any more. See the
 * note on g_fisicos_lib for the batch this comes from and on why handing them back is sound.
 *
 * Runs with the state mutex held; `excluir` is the chunk the caller is materialising, which must
 * never be touched here.
 */
size_t SoltarFisicoSinUso(State& s, const Chunk* excluir) {
    if (!s.base) return 0;
    if (g_fisicos_lib.load(std::memory_order_relaxed) >= kFisicosMax) return 0;
    size_t hechas = 0;
    for (auto it = s.chunks.begin(); it != s.chunks.end() && hechas < kFisicosPorEvento;) {
        Chunk& c = it->second;
        /* Only the raw physical range, only whole pages, and only what was materialised. */
        const bool en_rango =
            c.offset >= kRawFisicoBase && c.offset < kRawFisicoBase + kRawFisicoTam;
        if (!en_rango || c.offset % kPageSize != 0 || c.length == 0 ||
            c.length % kPageSize != 0 || !c.shadow || &c == excluir) {
            ++it;
            continue;
        }
        /* Somebody is still reading it: a live view of either index space has it mapped. */
        bool referenciado = false;
        for (const auto& par : s.view_mapped) {
            if (par.second != c.offset || ViewOf(s, par.first) == nullptr) continue;
            referenciado = true;
            break;
        }
        if (referenciado) {
            ++it;
            continue;
        }
        /* And the guest page table says those pages are not an allocation of the guest's own. */
        const uint32_t invitado = static_cast<uint32_t>(c.offset - kRawFisicoBase);
        bool libre = true;
        for (size_t p = 0; p < c.length; p += kPageSize) {
            if (RexSwitchGuestPageState(invitado + static_cast<uint32_t>(p)) != 1) {
                libre = false;
                break;
            }
        }
        if (!libre) {
            ++it;
            continue;
        }

        const size_t largo = c.length;
        UnmapChunkFromViews(s, c);
        ReleaseChunk(c);
        if (c.shadow && s.committed >= largo) s.committed -= largo;
        it = s.chunks.erase(it);
        ++hechas;
        const size_t total = g_fisicos_lib.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 16 || total % 64 == 0) {
            fprintf(stderr,
                    "[fix38] fisico liberado invitado=0x%08zX len=0x%zX total=%zu (%zu MB) | "
                    "piscina en uso %zu MB de %zu MB tomados\n",
                    static_cast<size_t>(invitado), largo, total, (total * largo) >> 20,
                    g_piscina_en_uso.load(std::memory_order_relaxed) >> 20,
                    g_piscina_bytes.load(std::memory_order_relaxed) >> 20);
        }
    }
    return hechas;
}

bool LiberarPresupuestoParaCarga(State& s) {
    /*
     * fix38: the guest physical cache gives back first, and it does so even when the raw views
     * are all gone: the chunks they committed are what is still holding memory here, and the
     * pool is the one that can serve the block that could not be materialised.
     */
    if (SoltarFisicoSinUso(s, nullptr) != 0) return true;
    const size_t eventos = g_lib_eventos.load(std::memory_order_relaxed);
    if (eventos >= kLibEventosMax) return false;
    const size_t vivas = VistasCrudasVivas(s);
    if (vivas == 0) return false;  /* nothing left to hand back */
    g_lib_eventos.fetch_add(1, std::memory_order_relaxed);
    g_presion_mapeo.store(true, std::memory_order_relaxed);
    const size_t hechas = LiberarVistasCrudas(kLibVistasPorEvento);
    g_lib_vistas.fetch_add(hechas, std::memory_order_relaxed);
    fprintf(stderr,
            "[fix30] presion de mapeo: liberadas %zu vistas crudas (%zu -> %zu vivas) en el evento "
            "%zu/%zu | mapeos codigo %zu (~%zu entradas, %zu MB), ventana %zu (~%zu entradas, %zu MB)\n",
            hechas, vivas, VistasCrudasVivas(s), eventos + 1, kLibEventosMax,
            g_mapeos_codigo_n.load(std::memory_order_relaxed),
            g_entradas_codigo.load(std::memory_order_relaxed),
            g_mapeos_codigo_bytes.load(std::memory_order_relaxed) >> 20,
            g_mapeos_ventana_n.load(std::memory_order_relaxed),
            g_entradas_ventana.load(std::memory_order_relaxed),
            g_mapeos_ventana_bytes.load(std::memory_order_relaxed) >> 20);
    return hechas != 0;
}

}  // extern "C++"
}  // namespace

/*
 * fix39: the fault path's own entry to the reclaim that LiberarPresupuestoParaCarga does.
 *
 * The load path and the fault path are after different things, and LiberarPresupuestoParaCarga
 * is written for the load path: a block that cannot be materialised is asking for a board, so it
 * hands the guest physical cache back first (fix38) and only then the raw views (fix30). A fault
 * that cannot be mapped in its view is asking for mapping entries, and the only thing that hands
 * entries back is the raw views (see the note over LiberarVistasCrudas: what really goes back to
 * the kernel is the mapping budget of the entries those views had taken). SoltarFisicoSinUso keeps
 * finding granules to give - the batch this comes from had 64 - so that first branch would succeed
 * on every call and the views would never be reached.
 *
 * So the views come first here and the physical cache after, both bounded by their own ceilings
 * (kLibVistasPorEvento views per event and kLibEventosMax events per run for the views,
 * kFisicosMax granules for the cache). The caller is the last step of the handler's ladder, which
 * is already the last chance before the fatal report.
 */
int RexGmLiberarPresupuestoParaCarga(void) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return 0;
    size_t vistas = 0;
    const size_t eventos = g_lib_eventos.load(std::memory_order_relaxed);
    const size_t vivas = VistasCrudasVivas(s);
    if (eventos < kLibEventosMax && vivas != 0) {
        g_lib_eventos.fetch_add(1, std::memory_order_relaxed);
        g_presion_mapeo.store(true, std::memory_order_relaxed);
        vistas = LiberarVistasCrudas(kLibVistasPorEvento);
        g_lib_vistas.fetch_add(vistas, std::memory_order_relaxed);
        fprintf(stderr,
                "[fix39] presupuesto para el fault: liberadas %zu vistas crudas (%zu -> %zu vivas) "
                "en el evento %zu/%zu\n",
                vistas, vivas, VistasCrudasVivas(s), eventos + 1, kLibEventosMax);
    }
    const size_t fisicos = SoltarFisicoSinUso(s, nullptr);
    if (fisicos != 0) PiscinaReservar();
    return (vistas != 0 || fisicos != 0) ? 1 : 0;
}

/*
 * fix41: the last rung of the ladder of fix40. Called by the exception handler with the address of
 * the fault, and only when nothing else served it: the page must belong to a committed block that is
 * not materialised and that no rung above could back.
 *
 * It returns true when the access can be resumed: either the page is mapped in the view of the
 * fault, or the page is watched and the emulation serves the instruction through its alias, which is
 * where RexGmShadowFor looks for it from here on. It returns false when the kernel refused the
 * mapping: the caller then goes on to SEH and, if that fails too, to the fatal report, exactly as it
 * did before this fix.
 */
bool ServirPaginaSuelta(uint64_t window_address) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;
    const View* v = ViewOf(s, vi);
    if (!v) return false;
    const size_t off =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    const size_t pagina = AlignDown(off, kPageSize);

    /*
     * Only a committed block that is not materialised. A page that was never handed out belongs to
     * the zero-load path (fix18) and a materialised block to the fault-in of the step above: neither
     * wants a page of its own, and serving them here would only spend the budget twice.
     */
    Chunk* c = ChunkAt(s, pagina);
    if (!c) return false;
    if (c->shadow) return false;

    auto it = s.sueltas.find(pagina);
    if (it == s.sueltas.end()) {
        if (s.sueltas.size() >= kSueltasMax) {
            ++g_sueltas_neg;
            static uint32_t aviso_tope = 0;
            if (aviso_tope < 4) {
                ++aviso_tope;
                fprintf(stderr, "[fix41] pagina suelta: el tope de %zu esta alcanzado off=0x%zX\n",
                        kSueltasMax, pagina);
            }
            return false;
        }
        if (EntradasTotales() + 2 > TopeTotal()) {
            /*
             * The port's own arithmetic is conservative here - the total never comes down, see fix33 -
             * and the alternative to trying is the abort, so this is a warning and not a refusal. What
             * the kernel says with its own 0xCE01 is the last word.
             */
            static uint32_t aviso_techo = 0;
            if (aviso_techo < 16) {
                ++aviso_techo;
                fprintf(stderr,
                        "[fix41] pagina suelta sobre el techo (%zu + 2 > %zu) off=0x%zX: se intenta\n",
                        EntradasTotales(), TopeTotal(), pagina);
            }
            LiberarPresupuestoParaCarga(s);
        }
        void* p = PaginaTomar();
        if (!p) {
            ++g_sueltas_neg;
            return false;
        }
        memset(p, 0, kPageSize);
        virtmemLock();
        void* alias = virtmemFindCodeMemory(kPageSize + kPageSize, kPageSize);
        if (alias) {
            alias = reinterpret_cast<void*>(AlignUp(reinterpret_cast<size_t>(alias), kPageSize));
        }
        VirtmemReservation* res = alias ? virtmemAddReservation(alias, kPageSize) : nullptr;
        virtmemUnlock();
        Result rc = 0;
        if (alias && res) {
            rc = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(alias),
                                        reinterpret_cast<u64>(p), kPageSize);
            if (R_SUCCEEDED(rc)) {
                rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(alias), kPageSize,
                                                   Perm_Rw);
                if (R_FAILED(rc)) {
                    svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(alias),
                                              reinterpret_cast<u64>(p), kPageSize);
                }
            }
        }
        if (!alias || !res || R_FAILED(rc)) {
            if (res) {
                virtmemLock();
                virtmemRemoveReservation(res);
                virtmemUnlock();
            }
            PaginaCeder(p);
            ++g_sueltas_neg;
            /* fix33: a 0xCE01 here lowers the ceiling this port goes by for the rest of the run. */
            AnotarRechazoKernel(rc);
            static uint32_t aviso_neg = 0;
            if (aviso_neg < 32) {
                ++aviso_neg;
                fprintf(stderr, "[fix41] pagina suelta RECHAZADA rc=0x%08X off=0x%zX negadas=%zu\n",
                        (unsigned)rc, pagina, g_sueltas_neg.load(std::memory_order_relaxed));
            }
            return false;
        }
        AnotarMapeoCodigo(reinterpret_cast<u64>(alias), kPageSize);
        it = s.sueltas.emplace(pagina, PaginaSuelta{p, alias, res}).first;
        const size_t n = g_sueltas_n.fetch_add(1, std::memory_order_relaxed) + 1;
        static bool hito_suelta = false;
        if (!hito_suelta) {
            hito_suelta = true;
            Hito("primera pagina suelta");
        }
        static uint32_t aviso_ok = 0;
        if (aviso_ok < 32 || (n % 4096) == 0) {
            ++aviso_ok;
            fprintf(stderr,
                    "[fix41] pagina suelta off=0x%zX vista=%zu n=%zu entradas=%zu/%zu sueltas=%zu\n",
                    pagina, vi, n, EntradasTotales(), TopeTotal(), s.sueltas.size());
        }
    }

    const PaginaSuelta& sp = it->second;
    /*
     * A watched page is not mapped (that is what the entry means): the emulation serves it through
     * the alias, which is enough for this rung to say the access can be resumed.
     */
    if (s.protection.find(pagina) != s.protection.end()) return true;
    if (s.sueltas_vistas.count(std::make_pair(vi, pagina)) != 0) return true;

    size_t lo = 0, hi = 0;
    if (!Intersect(pagina, kPageSize, v->offset, v->length, &lo, &hi)) return false;
    const size_t win_off = static_cast<size_t>(v->base - s.base) + (lo - v->offset);
    if (MapWindowRange(s, win_off, static_cast<uint8_t*>(sp.alias) + (lo - pagina), hi - lo)) {
        s.sueltas_vistas.insert(std::make_pair(vi, pagina));
        g_sueltas_vistas.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    /*
     * The page is there but the window call did not go in. Its alias is what the emulation needs, so
     * this is not a refusal: the handler tries the shadow once more after this rung.
     */
    return false;
}

extern "C" int RexGmServirPaginaSuelta(void* address) {
    return ServirPaginaSuelta(reinterpret_cast<uint64_t>(address)) ? 1 : 0;
}

extern "C" int RexGmServirPaginaDeBloque(void* address) {
    return ServirPaginaDeBloque(reinterpret_cast<uint64_t>(address)) ? 1 : 0;
}

/*
 * fix19: gives [V, V+granule) of the guest address space a view of its own over physical memory,
 * mirrored from 0x100000000 + V (the arena behind the 360 mirrors).
 *
 * It is called by the exception handler, and only after checking that every page of the granule is
 * free in the guest page table (RexSwitchGuestPageInfo == 1), so it cannot cover a virtual
 * allocation that is in use.
 *
 * If the physical backing at that offset is not committed yet, it is committed here in whole 2 MB,
 * to match kCommitGranule. Nothing is invented: the bytes are the ones the console would return for
 * that physical address, instead of the zeros fix18 had to emulate.
 *
 * It returns true when the address has a raw view after the call (it already had one, or it has
 * just got it). It returns false if the granule falls outside the raw range, if the budget of views
 * is used up, or if the backing cannot be committed; the caller then falls back to what it did
 * before.
 */
bool RexGmRawAlias(uint64_t window_address, size_t granule) {
    if (granule == 0) return false;
    if (g_presion_mapeo.load(std::memory_order_relaxed)) {
        /*
         * fix30: under budget pressure a raw alias is the last thing that may grow: creating one
         * commits a chunk (a code mapping) and maps it into the raw view, out of the same budget
         * that has just refused a 2 MB block. The caller falls back to the emulation this port
         * had before fix19: slower, but the run stops eating its own budget.
         */
        static uint32_t aviso = 0;
        if (aviso == 0) {
            aviso = 1;
            fprintf(stderr, "[fix30] alias crudo aplazado por presion de mapeo invitado=0x%08zX\n",
                    static_cast<size_t>(window_address));
        }
        return false;
    }
    State& s = S();

    size_t gran = 0;
    size_t largo = 0;
    {
        GmLock lock(s.mutex);
        if (!s.base) return false;
        const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
        if (window_address < wbase) return false;
        const size_t win_off = static_cast<size_t>(window_address - wbase);
        if (win_off >= kRawFisicoTam) return false;

        gran = AlignDown(win_off, granule);
        largo = std::min(granule, kRawFisicoTam - gran);
        if (largo == 0) return false;

        /* A live raw view that already covers the address is enough. */
        for (const VistaCruda& r : s.raw_views) {
            if (!r.viva) continue;
            const uint64_t lo = reinterpret_cast<uint64_t>(r.v.base);
            if (window_address >= lo && window_address < lo + r.v.length) return true;
        }
        if (VistasCrudasVivas(s) >= kRawVistasMax) {
            /*
             * fix20: the budget ran out and this granule is left to the emulation. The fix19 run
             * got here with 126/128 and wrote nothing, which is why that log could not tell a
             * budget that had run out from a granule the page table had rejected.
             */
            static uint32_t aviso = 0;
            if (aviso == 0) {
                aviso = 1;
                fprintf(stderr,
                        "[fix20] alias crudo RECHAZADO por presupuesto invitado=0x%08zX vivas=%u/%u\n",
                        win_off, static_cast<unsigned>(VistasCrudasVivas(s)),
                        static_cast<unsigned>(kRawVistasMax));
            }
            return false;
        }
    }

    /*
     * The backing, committed if it was not already. RexGmCommit is idempotent and takes the lock
     * itself, so it runs outside ours: the window lock is not reentrant and the two phases must not
     * nest.
     */
    if (!RexGmCommit(kRawFisicoBase + gran, largo, REX_GM_WRITE)) return false;

    GmLock lock(s.mutex);
    if (!s.base) return false;

    /* A live raw view may have been created meanwhile for the same range. */
    for (const VistaCruda& r : s.raw_views) {
        if (!r.viva) continue;
        if (r.v.base == s.base + gran) return true;
    }
    if (VistasCrudasVivas(s) >= kRawVistasMax) {
        static uint32_t aviso2 = 0;
        if (aviso2 == 0) {
            aviso2 = 1;
            fprintf(stderr,
                    "[fix20] alias crudo RECHAZADO por presupuesto (2a) invitado=0x%08zX vivas=%u/%u\n",
                    gran, static_cast<unsigned>(VistasCrudasVivas(s)),
                    static_cast<unsigned>(kRawVistasMax));
        }
        return false;
    }
    if (ChunkAt(s, kRawFisicoBase + gran) == nullptr) return false;  /* sin respaldo fisico */

    /*
     * The granule was free in the guest page table, but a leftover logical permission from an
     * earlier allocation would still leave it unmapped (RexGmFaultIn refuses those pages). The
     * guest is reading physical memory here, not a page of its own heap, and with the page free
     * there is no allocation the entry can belong to.
     */
    for (size_t p = gran; p < gran + largo; p += kPageSize) s.protection.erase(p);

    /* A dead slot with the same base is reused, so the list does not grow without bound. */
    for (size_t i = 0; i < s.raw_views.size(); i++) {
        if (s.raw_views[i].viva || s.raw_views[i].v.base != s.base + gran) continue;
        s.raw_views[i].v    = View{s.base + gran, kRawFisicoBase + gran, largo};
        s.raw_views[i].viva = true;
        s.raw_bytes += largo;
        fprintf(stderr, "[fix19] alias crudo invitado=0x%08zX arena=0x%zX len=0x%zX total=%u/%u\n",
                gran, kRawFisicoBase + gran, largo,
                static_cast<unsigned>(VistasCrudasVivas(s)),
                static_cast<unsigned>(kRawVistasMax));
        return true;
    }

    s.raw_views.push_back(VistaCruda{View{s.base + gran, kRawFisicoBase + gran, largo}, true});
    s.raw_bytes += largo;
    fprintf(stderr, "[fix19] alias crudo invitado=0x%08zX arena=0x%zX len=0x%zX total=%u/%u\n", gran,
            kRawFisicoBase + gran, largo, static_cast<unsigned>(VistasCrudasVivas(s)),
            static_cast<unsigned>(kRawVistasMax));
    return true;
}

/* fix19: bytes of guest address space covered by live raw views. For diagnostics. */
size_t RexGmRawAliasBytes(void) {
    State& s = S();
    GmLock lock(s.mutex);
    return s.raw_bytes;
}

/*
 * fix23: the mode of the aligned window, and how many whole blocks went in on a 2 MB boundary.
 * The mode is settled before the window is reserved; the count is what tells a run of one mode
 * from another, next to the 512 page table entries per 2 MB that are no longer paid for.
 */
void RexGmConfigure(int modo) {
    if (modo < 0) modo = 0;
    if (modo > 2) modo = 2;
    g_paginas_grandes = modo;
    g_fase_fisica = (modo == 2) ? kFaseE0 : 0;
    fprintf(stderr, "[fix23] RexGmConfigure modo=%d fase fisica=0x%zX\n", g_paginas_grandes,
            g_fase_fisica);
}

int RexGmLargePagesMode(void) { return g_paginas_grandes; }

size_t RexGmLargePageAliases(void) { return g_aliases_grandes.load(); }

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {
    /*
     * Memory is always committed accessible. Restricting permissions is the job of
     * RexGmProtect, which works on window addresses and not on mapping offsets (and
     * it has to, because the same offset can be mirrored in several windows with
     * different permissions: the 360 does exactly that).
     *
     * `access` stays in the signature because the natural caller is
     * AllocFixed(kCommit, access), and removing it would invite forgetting to apply
     * it. Pages that already had logical protection keep it.
     */
    (void)access;
    if (length == 0) return true;

    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    /*
     * fix23: the grid the views stay congruent with (GridDown/GridUp), not a plain 2 MB alignment:
     * in mode 2 the physical arena is shifted by the 4 KB phase of the 0xE0000000 view. In mode 1,
     * the default, the two are the same thing.
     */
    const size_t begin = GridDown(offset);
    const size_t end   = std::min(GridUp(offset + length), s.size);

    /*
     * fix22: every commit is lazy now (see MaterializarBloque), so what this call does is only
     * bookkeeping; this measures it, and the line below records the requests that used to be the
     * expensive ones. Both go to rex_stderr.log, bounded so they cannot drown it.
     */
    const u64 desde_commit = svcGetSystemTick();
    if (length >= (32u << 20)) {
        static uint32_t avisos_diferido = 0;
        if (avisos_diferido < 16) {
            ++avisos_diferido;
            fprintf(stderr,
                    "[fix22] commit diferido offset=0x%zX largo=0x%zX (bloques de 2 MB; respaldo y "
                    "alias al primer toque)\n",
                    offset, length);
        }
    }

    size_t cur = begin;
    while (cur < end) {
        if (Chunk* existente = ChunkAt(s, cur)) {
            cur = existente->offset + existente->length;
            continue;
        }

        /* Where the gap ends: the next chunk, or the end. */
        size_t hueco_fin = end;
        auto sig = s.chunks.upper_bound(cur);
        if (sig != s.chunks.end() && sig->second.offset < hueco_fin)
            hueco_fin = sig->second.offset;

        /*
         * Grow the commit window to the next block boundary unconditionally.
         *
         * Horizon charges the process mapping limit (LimitableResource_Memory)
         * per mapping, not per byte, so a stream of 8 KB requests was paying a
         * full mapping each time and ran out of limit with 0xCE01 even though
         * plenty of physical memory remained. Committing a whole block instead
         * means one svcMapProcessMemory covers many future small commits;
         * they hit ChunkAt above and skip the syscall.
         *
         * The window only ever extends into a gap that has no committed chunk
         * and never crosses the next existing chunk (hueco_fin already stops
         * at it), so no previous mapping is duplicated and s.chunks keeps a
         * single entry for the whole block.
         */
        size_t tam = hueco_fin - cur;
        size_t hueco_ext = GridUp(hueco_fin);  /* fix23: to the boundary of the grid */
        size_t siguiente = (sig != s.chunks.end()) ? sig->second.offset
                                                   : ~size_t(0);
        if (hueco_ext > siguiente) hueco_ext = siguiente;
        if (hueco_ext > hueco_fin) {
            hueco_fin = hueco_ext;
            tam = hueco_fin - cur;
        }

        if (cur + tam <= g_prealloc_size) {
            /* Slice from the pre-allocated arena: its backing and code alias are
             * released as a whole in RexGmShutdown, so slabs carry no resources
             * of their own. */
            void* shadow = static_cast<uint8_t*>(g_prealloc_shadow) + cur;

            const Chunk c{cur, tam, nullptr, shadow, nullptr};
            s.chunks[cur] = c;
            s.committed += tam;
            cur = hueco_fin;
        } else {
            /*
             * fix22: only the descriptor is written here. The backing, its zeroing and the code
             * alias used to be made at this point for the whole request, and a single commit of
             * 0x37080000 (883 MB) during the loading screen is what pushed the process to
             * 3185/3189 MB of mapping limit and left the game at 0.0 fps (see MaterializarBloque).
             * Now they happen in MaterializarBloque, on the first touch of the block.
             *
             * The block is capped at kBlockSize (2 MB) so that "first touch" is worth something: a
             * chunk is the unit that is materialised, and materialising an 883 MB one on its first
             * byte would put the problem back. The tile stays aligned because begin is aligned down
             * to kCommitGranule (which is kBlockSize), so the granules cover the request exactly.
             */
            size_t block = tam;
            if (block < kPageSize) block = kPageSize;
            if (block > kBlockSize) block = kBlockSize;

            const Chunk c{cur, block, nullptr, nullptr, nullptr};
            s.chunks[cur] = c;
            cur += block;
        }
    }

    /*
     * fix22: the whole commit is only bookkeeping now, so 20 ms here means something else is
     * holding the window lock. One line each time, bounded.
     */
    const u64 ms = (svcGetSystemTick() - desde_commit) * 1000ULL / 19200000ULL;
    if (ms >= 20) {
        static uint32_t avisos = 0;
        if (avisos < 32) {
            ++avisos;
            fprintf(stderr, "[fix22] lento commit off=0x%zX len=0x%zX %llu ms\n", offset, length,
                    (unsigned long long)ms);
        }
    }
    return true;
}

bool RexGmFaultIn(uint64_t window_address) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;

    /*
     * Page with logical protection: it is unmapped on purpose (write watching, MMIO,
     * read-only). Mapping the chunk would not bring it in (protected pages are
     * skipped), so answering yes would leave the fault repeating forever. The next
     * steps of the handler deal with it.
     */
    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    if (s.protection.find(AlignDown(win_off, kPageSize)) != s.protection.end()) return false;

    /* fix19: the index can belong to a raw view, so it goes through ViewOf. */
    const View* vf = ViewOf(s, vi);
    if (!vf) return false;
    const size_t off = vf->offset +
                       static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(vf->base));
    Chunk* c = ChunkAt(s, off);
    if (!c) return false;                                       /* sin confirmar */
    if (s.view_mapped.count({vi, c->offset}) != 0) return false; /* ya estaba */
    return MapChunkIntoView(s, vi, *c);
}

/*
 * fix21: how many pages have been lent this run, and how many lendings the kernel refused.
 *
 * They are read by the 10 s report (RexGmBorrowedPages, RexGmBorrowRefused), so the log says what the
 * lending cost and whether it is the path that is serving the reads.
 */
static std::atomic<size_t> g_borrow_pages{0};
static std::atomic<size_t> g_borrow_refused{0};

/*
 * fix21: pages whose lending has already been refused by the kernel.
 *
 * A refused page stays watched and every read of it faults again, so without this the exception
 * handler would ask the kernel to map the same page once per read: eight million system calls
 * instead of eight million exceptions, and the freeze would be back in another form. The refusal is
 * decisive (the mapping spans the whole 4 KB, so a page that cannot be lent cannot be lent ever), so
 * it is remembered. The set is bounded: it is a diagnostic cost ceiling, not a cache.
 */
static std::mutex g_borrow_mutex;
static std::set<size_t> g_borrow_refused_pages;
constexpr size_t kBorrowRefusedMax = 8192;

bool RexGmBorrowWatchForRead(uint64_t window_address) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    const size_t pagina = AlignDown(win_off, kPageSize);

    /*
     * Only a page that is watched logically, and *read* protected: REX_GM_WRITE means the page is not
     * watched (it is mapped), and REX_GM_NONE is a guard page the guest protects itself, which this
     * must not map.
     */
    const auto it = s.protection.find(pagina);
    if (it == s.protection.end() || it->second != REX_GM_READ) return false;

    {
        std::lock_guard<std::mutex> lock_neg(g_borrow_mutex);
        if (g_borrow_refused_pages.find(pagina) != g_borrow_refused_pages.end()) return false;
    }

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;
    const View* v = ViewOf(s, vi);
    if (!v) return false;
    const size_t off =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    Chunk* c = ChunkAt(s, off);
    if (!c) return false;
    /*
     * The chunk has to be in this view already: if it is not, there is no mapping to mirror and the
     * shadow is not reachable from the window. The first access brings the chunk in (RexGmCommit
     * honors the protection), and the next read of the page comes back here.
     */
    if (s.view_mapped.count({vi, c->offset}) == 0) return false;

    /*
     * The page is unmapped (that is why this read faulted), so the mapping is the one
     * MapChunkIntoView makes: the page of the window to its page of the shadow. The offset is
     * aligned down, because the faulting access need not be the first byte of the page.
     */
    const size_t off_pagina = off - (win_off - pagina);
    /*
     * The page of the shadow has to be a whole page, aligned on both sides. If it is not - which
     * would mean a chunk that does not start on a page boundary - the mapping would point the window
     * at the wrong bytes of the shadow, and the guest would read garbage silently. Nothing is lent
     * then: the emulation below serves the read. Chunks are committed on granules of at least 4 KB,
     * so this is a guard, not a case that happens.
     */
    if (off_pagina < c->offset) return false;
    const u64 src = reinterpret_cast<u64>(
        static_cast<uint8_t*>(c->shadow) + (off_pagina - c->offset));
    if ((src & (kPageSize - 1)) != 0) return false;  /* not mappable: leave it to the shadow */

    const Result rc = svcMapProcessMemory(s.base + pagina, Proc(), src, kPageSize);
    if (R_FAILED(rc)) {
        s.last_result = rc;
        g_borrow_refused.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock_neg(g_borrow_mutex);
            if (g_borrow_refused_pages.size() < kBorrowRefusedMax) {
                g_borrow_refused_pages.insert(pagina);
            }
        }
        /*
         * One line with the reason, the first time only: if the lending is refused, this says with
         * what code, which is what tells a page that is already mapped from one the kernel will not
         * give a second mapping for.
         */
        static uint32_t aviso = 0;
        if (aviso == 0) {
            aviso = 1;
            fprintf(stderr, "[fix21] prestamo RECHAZADO rc=0x%08X invitado=0x%08zX\n",
                    (unsigned)rc, pagina);
        }
        return false;
    }

    s.mapped += kPageSize;
    s.protection.erase(it);
    g_borrow_pages.fetch_add(1);
    return true;
}

size_t RexGmBorrowedPages(void) { return g_borrow_pages.load(); }
size_t RexGmBorrowRefused(void) { return g_borrow_refused.load(); }

/*
 * fix22: what the deferred commit has already paid for, and what is still only a descriptor.
 *
 * The 10 s report prints both: "materializados" (2 MB blocks that were really allocated, zeroed and
 * aliased) and "pendientes" (the bytes a commit like the 883 MB one left as bare ranges). The
 * second figure is what says whether the laziness is doing its job.
 */
size_t RexGmMaterializedChunks(void) { return g_bloques_materializados.load(); }

size_t RexGmPendingChunks(void) {
    State& s = S();
    GmLock lock(s.mutex);
    size_t n = 0;
    for (const auto& kv : s.chunks) {
        if (!kv.second.shadow) ++n;
    }
    return n;
}

size_t RexGmPendingBytes(void) {
    State& s = S();
    GmLock lock(s.mutex);
    size_t n = 0;
    for (const auto& kv : s.chunks) {
        if (!kv.second.shadow) n += kv.second.length;
    }
    return n;
}

bool RexGmDecommit(size_t offset, size_t length) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    const size_t begin = AlignDown(offset, kPageSize);
    const size_t end   = AlignUp(offset + length, kPageSize);

    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        Chunk c = it->second;
        /*
         * Only chunks entirely contained in the range are freed: splitting a chunk
         * would also mean splitting its backing, and the backing is a single heap
         * allocation. Partial chunks stay committed, which is conservative but correct.
         */
        if (c.offset >= begin && c.offset + c.length <= end) {
            UnmapChunkFromViews(s, c);
            ReleaseChunk(c);
            /*
             * fix22: a chunk that was never materialised never added to the backing figure either
             * (see MaterializarBloque), so subtracting it here would make the number drift.
             */
            if (c.shadow) s.committed -= c.length;
            it = s.chunks.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr + length > wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);
    const size_t end   = AlignUp(static_cast<size_t>(addr - wbase) + length, kPageSize);

    if (out_old) *out_old = AccessOf(s, begin);

    /*
     * fix19: the guest virtual heap is taking a page of the raw range that had a raw physical view.
     * The virtual allocation always wins: the view is dropped, so the page stops serving physical
     * bytes and the first access to it brings in its own chunk. Without this, the page the game has
     * just committed would keep answering with the arena's bytes.
     */
    if (!s.raw_views.empty() && begin < kRawFisicoTam) {
        const size_t fin = std::min(end, kRawFisicoTam);
        for (size_t i = 0; i < s.raw_views.size(); i++) {
            if (!s.raw_views[i].viva) continue;
            const size_t lo = static_cast<size_t>(s.raw_views[i].v.base - s.base);
            if (lo >= fin || lo + s.raw_views[i].v.length <= begin) continue;
            SoltarVistaCruda(s, i);
        }
    }

    const bool debe_estar = (access == REX_GM_WRITE);

    /*
     * Fast path, and the common one: asking for full access on a range that has no
     * protected page. AllocFixed(kCommit) does it for every allocation of the game
     * heap, and without this it would walk the range page by page (131,072
     * iterations for the 512 MB of physical memory) to do nothing.
     */
    if (debe_estar) {
        auto it = s.protection.lower_bound(begin);
        if (it == s.protection.end() || it->first >= end) return true;
    }

    for (size_t p = begin; p < end; p += kPageSize) {
        const bool estaba = (AccessOf(s, p) == REX_GM_WRITE);

        if (estaba != debe_estar) {
            /*
             * The view is looked up per page: a range can cross the boundary
             * between two views with different offsets.
             */
            if (const View* v = ViewFor(s, wbase + p)) {
                const size_t off_mapeo =
                    v->offset + (p - static_cast<size_t>(v->base - s.base));
                size_t vi = 0;
                Chunk* c = ChunkAt(s, off_mapeo);
                /*
                 * If the chunk has not entered this view yet, there is nothing to map
                 * or unmap: the permission is recorded and the first access will bring
                 * the chunk in with the right permission already.
                 */
                const bool en_vista = c != nullptr && ViewIndexFor(s, wbase + p, &vi) &&
                                      s.view_mapped.count({vi, c->offset}) != 0;
                if (en_vista) {
                    const u64 src = reinterpret_cast<u64>(
                        static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset));
                    /*
                     * Permissions are tried first, which keeps the page readable. If the
                     * kernel refuses, it falls back to the usual unmap. ProtegerConPermisos
                     * remembers whether it works.
                     */
                    if (!ProtegerConPermisos(s.base + p, estaba)) {
                        const Result rc =
                            estaba ? svcUnmapProcessMemory(s.base + p, Proc(), src, kPageSize)
                                   : svcMapProcessMemory(s.base + p, Proc(), src, kPageSize);
                        if (R_FAILED(rc)) {
                            s.last_result = rc;
                            return false;
                        }
                        if (estaba) s.mapped -= kPageSize;
                        else s.mapped += kPageSize;
                    }
                }
                /*
                 * Not committed: nothing to map, but the permission is still recorded
                 * and RexGmCommit will honor it on commit.
                 */
            }
        }

        if (debe_estar) s.protection.erase(p);
        else s.protection[p] = access;
    }

    /*
     * fix20: who installs the watches.
     *
     * A watch is what turns every read of a page into an exception, and the fix19 profile could not
     * say where the 160,000 emulated reads per second came from: the paths that install one (the
     * guest's NtProtectVirtualMemory, the GPU callbacks under graphics/) wrote nothing. RexGmProtect
     * is the single place they all pass through, so the first eight distinct callers are written
     * once, with their return address: addr2line names them from the next log. One comparison per
     * call, not per page.
     */
    if (access != REX_GM_WRITE) {
        static const void* vistos[8] = {};
        static uint32_t     instaladas = 0;
        if (instaladas < 8) {
            const void* quien = __builtin_return_address(0);
            bool repetido = false;
            for (const void* v : vistos) {
                if (v == quien) {
                    repetido = true;
                    break;
                }
            }
            if (!repetido) {
                vistos[instaladas++] = quien;
                fprintf(stderr,
                        "[fix20] vigilancia instalada caller=%p ventana=0x%zX largo=0x%zX access=%d\n",
                        quien, begin, end - begin, static_cast<int>(access));
            }
        }
    }
    return true;
}

bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr >= wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);

    /*
     * An uncommitted page is reported as having no access, even if its logical
     * permission is the default one.
     *
     * This matters a lot: after a fault, the SDK's MMIO handler queries this, and if
     * it sees the page as accessible it concludes that another thread already
     * unprotected it and retries the instruction. If the page does not really exist,
     * the retry faults again... forever. A silent hang.
     */
    if (!IsCommittedWindowAddress(s, wbase + begin)) {
        if (out_access) *out_access = REX_GM_NONE;
        if (length) *length = kPageSize;
        return true;
    }

    const RexGmAccess a = AccessOf(s, begin);
    if (out_access) *out_access = a;

    /*
     * On input *length is the maximum to walk (0 = one page); on output, the size of
     * the committed range with the same permission.
     */
    size_t maximo = (length && *length) ? *length : kPageSize;
    if (begin + maximo > s.size) maximo = s.size - begin;

    size_t p = begin;
    const size_t tope = begin + maximo;
    while (p < tope && AccessOf(s, p) == a && IsCommittedWindowAddress(s, wbase + p))
        p += kPageSize;
    if (length) *length = p - begin;
    return true;
}

void* RexGmShadowFor(uint64_t window_address) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) return nullptr;

    const View* v = ViewFor(s, window_address);
    if (!v) return nullptr;

    const size_t off_mapeo =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    Chunk* c = ChunkAt(s, off_mapeo);
    if (c && c->shadow) return static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset);
    /*
     * fix22: a chunk that has not been materialised has no shadow, and returning the offset as if it
     * were a pointer would have the emulation write through an address that is not mapped. The
     * handler tests the result for NULL and falls back, so this is all it needs.
     */
    /*
     * fix41: unless the page is one of those served loose (see ServirPaginaSuelta): its code alias is
     * a real address, which the emulation can read and write. This is what lets the last rung serve
     * the access even when the window call of its page did not go in.
     */
    {
        const size_t pagina = AlignDown(off_mapeo, kPageSize);
        const auto it = s.sueltas.find(pagina);
        if (it != s.sueltas.end()) {
            return static_cast<uint8_t*>(it->second.alias) + (off_mapeo - pagina);
        }
    }
    /* fix41: ni sombra ni pagina suelta: NULL, como dice la nota de arriba. */
    return nullptr;
}

/*
 * fix14 diagnostic: why is this window address not there? Reports, without changing anything,
 * what the port's own bookkeeping knows about the page: whether a view covers it, whether a
 * chunk (committed backing) exists, whether there is a logical protection entry (a page with an
 * entry is unmapped on purpose), the chunk that starts at or before it, and whether that chunk
 * is currently mapped into that view.
 *
 * Bits of the return value: 1 = chunk, 2 = protection entry, 4 = view.
 * Called from the exception handler: takes the lock and does not allocate more than the
 * caller's buffer.
 */
uint32_t RexGmFaultDiag(uint64_t window_address, char* out, size_t out_size) {
    if (out && out_size) out[0] = 0;
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base) {
        if (out && out_size) std::snprintf(out, out_size, "base nulo");
        return 0;
    }

    uint32_t bits = 0;
    size_t vi = 0;
    const bool hay_vista = ViewIndexFor(s, window_address, &vi);
    if (hay_vista) bits |= 4;

    const size_t off =
        hay_vista ? s.views[vi].offset +
                        static_cast<size_t>(window_address -
                                            reinterpret_cast<uint64_t>(s.views[vi].base))
                  : static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    Chunk* c = hay_vista ? ChunkAt(s, off) : nullptr;
    if (c) bits |= 1;

    const bool protegida = s.protection.find(AlignDown(off, kPageSize)) != s.protection.end();
    if (protegida) bits |= 2;

    const bool confirmado = c && s.view_mapped.count({vi, c->offset}) != 0;

    const Chunk* cerca = nullptr;
    const auto it = s.chunks.upper_bound(off);
    if (it != s.chunks.begin()) {
        auto anterior = it;
        --anterior;
        cerca = &anterior->second;
    }

    if (out && out_size) {
        std::snprintf(
            out, out_size,
            "vista=%s pag=%llX off=%llX trozo=%llX+%llX prot=%s cerca=%llX+%llX mapeado=%s "
            "confirmado=%s",
            hay_vista ? "si" : "no", (unsigned long long)AlignDown(off, kPageSize),
            (unsigned long long)off, c ? (unsigned long long)c->offset : 0ull,
            c ? (unsigned long long)c->length : 0ull, protegida ? "si" : "no",
            cerca ? (unsigned long long)cerca->offset : 0ull,
            cerca ? (unsigned long long)cerca->length : 0ull, c ? "si" : "no",
            confirmado ? "si" : "no");
    }
    return bits;
}

/*
 * fix14: erase the *logical* protection entry of a page, leaving the view mapping alone.
 *
 * RexGmProtect would do it too, but it maps and unmaps around the page, which would disturb
 * the neighbouring guard pages of the same chunk. This only forgets what the port remembers
 * so that RexGmFaultIn stops refusing to map the page.
 */
void RexGmClearProtect(size_t offset, size_t length) {
    State& s = S();
    GmLock lock(s.mutex);
    if (!s.base || !length) return;
    size_t p = AlignDown(offset, kPageSize);
    const size_t tope = AlignUp(offset + length, kPageSize);
    while (p < tope) {
        s.protection.erase(p);
        p += kPageSize;
    }
}

/*
 * fix26: mapping-lock tracker, for the watchdog of switch_perf.cpp. In ticks; 0 means free.
 */
u64 RexGmLockHeldTicks(void) {
    const u64 ini = g_lock_ini.load(std::memory_order_relaxed);
    if (!ini) {
        return 0;
    }
    const u64 ahora = armGetSystemTick();
    return ahora > ini ? ahora - ini : 0;
}

void RexGmLockSnapshot(u64* max_ticks, u64* count) {
    if (max_ticks) {
        *max_ticks = g_lock_max.exchange(0, std::memory_order_relaxed);
    }
    if (count) {
        *count = g_lock_n.exchange(0, std::memory_order_relaxed);
    }
}

/*
 * fix27: 1 once the heap refused a 2 MB backing and the port fell back to 4 KB for good, plus how
 * many refusals there were and how many blocks were left without backing. Read by the watchdog line
 * of switch_perf.cpp: the 10 s report runs in a thread that can be the one that stops.
 */
int RexGmBajaAlineacion(void) {
    return g_alineacion_baja.load(std::memory_order_relaxed);
}

size_t RexGmDenegaciones2m(void) {
    return g_denegaciones_2m.load(std::memory_order_relaxed);
}

/*
 * fix37: the heap histogram as the heartbeat of switch_perf.cpp reads it: everything the wrapped
 * allocation calls hold right now, and in how many blocks. It should agree with the mallinfo
 * uordblks printed on the same line; when it does not, the difference is allocation that does not
 * come through malloc/calloc/realloc/memalign, which is an answer in itself.
 */
size_t RexGmMallocVivo(void) {
    return static_cast<size_t>(g_malloc_vivos_total.load(std::memory_order_relaxed));
}
size_t RexGmMallocBloques(void) {
    return static_cast<size_t>(g_malloc_bloques_total.load(std::memory_order_relaxed));
}

size_t RexGmSinBacking(void) {
    return g_sin_backing.load(std::memory_order_relaxed);
}

}  // extern "C"

#endif /* __SWITCH__ */
