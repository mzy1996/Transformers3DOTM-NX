# -*- coding: utf-8 -*-
"""
fix19 patch: raw physical alias views.

Applies, with an assertion of exactly one match per pattern, the whole fix19 change on top of
the fix18 tree:

  repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp   (11 patterns, the 1st one is already in)
  repos/nfsmw-nx/sdk/src/core/guest_memory_switch.h     (1 pattern)
  repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp (3 patterns)
  repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp             (2 patterns)

Any pattern that does not match exactly once aborts the script before writing anything.
"""
import os
import sys

ROOT = "J:/XBOX360 port/Transformers Dark of the Moon/repos/nfsmw-nx/sdk"
SRC = os.path.join(ROOT, "src")


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "surrogateescape")


def write(path, text):
    with open(path, "wb") as f:
        f.write(text.encode("utf-8", "surrogateescape"))


def count_of(text, pat):
    n = text.count(pat)
    if n == 0:
        n = text.count(pat.replace("\n", "\r\n"))
    return n


def normalize(text, pat):
    return pat.replace("\n", "\r\n") if text.count(pat) == 0 else pat


def patch(path, pairs, label):
    text = read(path)
    for idx, (old, new) in enumerate(pairs):
        n = count_of(text, old)
        if n != 1:
            print("ABORT %s patron %d: %d coincidencias" % (label, idx, n))
            print("--- patron ---")
            print(old[:400])
            sys.exit(1)
        text = text.replace(normalize(text, old), normalize(text, new))
    write(path, text)
    print("OK %s: %d cambios (%d bytes)" % (label, len(pairs), len(text)))


# -------------------------------------------------------------------------------------------------
# 1. guest_memory_switch.cpp
# -------------------------------------------------------------------------------------------------
CPP = os.path.join(SRC, "core", "guest_memory_switch.cpp")

STATE_OLD = """    std::vector<View> views;
    /* chunks sorted by offset: allows finding the one that covers an offset */"""
STATE_NEW = """    std::vector<View> views;
    /* fix19: raw physical views, created on demand. See VistaCruda. */
    std::vector<VistaCruda> raw_views;
    size_t   raw_bytes = 0;

    /* chunks sorted by offset: allows finding the one that covers an offset */"""

SOLD = """State& S() {
    static State s;
    return s;
}
"""
SNEW = """State& S() {
    static State s;
    return s;
}

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
"""

VIEWFOR_OLD = """/* The view that contains a window address. */
const View* ViewFor(State& s, uint64_t addr) {
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}"""
VIEWFOR_NEW = """/*
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
}"""

VIDX_OLD = """bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
    for (size_t i = 0; i < s.views.size(); i++) {
        const uint64_t lo = reinterpret_cast<uint64_t>(s.views[i].base);
        if (addr >= lo && addr < lo + s.views[i].length) { *out = i; return true; }
    }
    return false;
}"""
VIDX_NEW = """bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
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
}"""

MAP_OLD = """bool MapChunkIntoView(State& s, size_t view_index, const Chunk& c) {
    const View& v = s.views[view_index];"""
MAP_NEW = """bool MapChunkIntoView(State& s, size_t view_index, const Chunk& c) {
    /* fix19: the index can belong to a raw view too, so it goes through ViewOf. */
    const View* vp = ViewOf(s, view_index);
    if (!vp) return false;
    const View& v = *vp;"""

UNMAP_OLD = """void UnmapChunkFromViews(State& s, const Chunk& c) {
    for (size_t i = 0; i < s.views.size(); i++) {
        auto it = s.view_mapped.find({i, c.offset});
        if (it == s.view_mapped.end()) continue;  /* never entered this view */
        const View& v = s.views[i];
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) continue;
        const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
        uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
        UnmapWindowRange(s, win_off, src, hi - lo);
        s.view_mapped.erase(it);
    }
}"""
UNMAP_NEW = """void UnmapChunkFromViews(State& s, const Chunk& c) {
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
}"""

ADDVIEW_OLD = """    s.views.push_back(v);
    return true;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {"""
ADDVIEW_NEW = r"""    s.views.push_back(v);
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
    State& s = S();

    size_t gran = 0;
    size_t largo = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
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
        if (VistasCrudasVivas(s) >= kRawVistasMax) return false;
    }

    /*
     * The backing, committed if it was not already. RexGmCommit is idempotent and takes the lock
     * itself, so it runs outside ours: the window lock is not reentrant and the two phases must not
     * nest.
     */
    if (!RexGmCommit(kRawFisicoBase + gran, largo, REX_GM_WRITE)) return false;

    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    /* A live raw view may have been created meanwhile for the same range. */
    for (const VistaCruda& r : s.raw_views) {
        if (!r.viva) continue;
        if (r.v.base == s.base + gran) return true;
    }
    if (VistasCrudasVivas(s) >= kRawVistasMax) return false;
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
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.raw_bytes;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {"""

FAULTIN_OLD = """    const size_t off = s.views[vi].offset +
                       static_cast<size_t>(window_address -
                                           reinterpret_cast<uint64_t>(s.views[vi].base));"""
FAULTIN_NEW = """    /* fix19: the index can belong to a raw view, so it goes through ViewOf. */
    const View* vf = ViewOf(s, vi);
    if (!vf) return false;
    const size_t off = vf->offset +
                       static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(vf->base));"""

PROTECT_OLD = """    if (out_old) *out_old = AccessOf(s, begin);

    const bool debe_estar = (access == REX_GM_WRITE);"""
PROTECT_NEW = """    if (out_old) *out_old = AccessOf(s, begin);

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

    const bool debe_estar = (access == REX_GM_WRITE);"""

SHUTDOWN_OLD = """    s.chunks.clear();
    s.views.clear();"""
SHUTDOWN_NEW = """    s.chunks.clear();
    s.views.clear();
    s.raw_views.clear();  /* fix19 */
    s.raw_bytes = 0;"""

patch(CPP,
      [(STATE_OLD, STATE_NEW), (SOLD, SNEW), (VIEWFOR_OLD, VIEWFOR_NEW), (VIDX_OLD, VIDX_NEW),
       (MAP_OLD, MAP_NEW), (UNMAP_OLD, UNMAP_NEW), (ADDVIEW_OLD, ADDVIEW_NEW),
       (FAULTIN_OLD, FAULTIN_NEW), (PROTECT_OLD, PROTECT_NEW), (SHUTDOWN_OLD, SHUTDOWN_NEW)],
      "guest_memory_switch.cpp")

# -------------------------------------------------------------------------------------------------
# 2. guest_memory_switch.h
# -------------------------------------------------------------------------------------------------
HDR = os.path.join(SRC, "core", "guest_memory_switch.h")

HDR_OLD = "bool RexGmFaultIn(uint64_t window_address);\n"
HDR_NEW = """bool RexGmFaultIn(uint64_t window_address);

/**
 * fix19: gives the raw physical range a view of its own, on demand.
 *
 * On the console the 512 MB of physical memory is also readable at its own address: the "raw
 * physical" view, the fifth one, next to 0x7F000000, 0xA0000000, 0xC0000000 and 0xE0000000. This
 * port never registered it, so a physical address handed to the guest - the number
 * MmGetPhysicalAddress returns, the video front buffer, the XMA context array - had no view, and
 * every access to it faulted.
 *
 * `granule` may be 2 MB or 64 KB. The caller must have checked that all of its pages are free in
 * the guest page table (RexSwitchGuestPageInfo == 1); this refuses to cover a virtual allocation.
 * If the physical backing behind that offset is not committed yet, it is committed here, in whole
 * 2 MB, so that the view can show real bytes.
 *
 * Returns true if the address has a raw view after the call (it already had one, or it has just got
 * it). Returns false if it falls outside 0x00000000-0x1FFFFFFF, if the budget of raw views is used
 * up, or if the backing cannot be committed.
 */
bool RexGmRawAlias(uint64_t window_address, size_t granule);

/* fix19: bytes of guest address space covered by live raw views. For diagnostics. */
size_t RexGmRawAliasBytes(void);
"""

patch(HDR, [(HDR_OLD, HDR_NEW)], "guest_memory_switch.h")

# -------------------------------------------------------------------------------------------------
# 3. exception_handler_switch.cpp
# -------------------------------------------------------------------------------------------------
EXC = os.path.join(SRC, "core", "exception_handler_switch.cpp")

HELPER_OLD = """  ctx->pc.x += 4;
  return true;
}

void FillContext(HostThreadContext& tc, const ThreadExceptionDump* ctx) {"""
HELPER_NEW = r"""  ctx->pc.x += 4;
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
      char info[256];
      if (RexSwitchGuestPageInfo(p, info, sizeof(info)) != 1) {
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

void FillContext(HostThreadContext& tc, const ThreadExceptionDump* ctx) {"""

SITE_OLD = """        if (codigo_zl == 1) {
          /*
           * fix18: fix17 worked but was silent, so a run could not tell a cap that had been"""
SITE_NEW = """        if (codigo_zl == 1) {
          /*
           * fix19: before emulating the read as zero, the granule is given a raw physical view.
           * When that works the load is served by the hardware on the retry, and the whole copy
           * stops faulting instead of being walked 16 bytes at a time.
           */
          if (TryRawPhysicalAlias(far, guest_zl)) {
            RexResumeFromException(ctx);
            return;  /* served: the fix18 emulation below must not run as well */
          }
          /*
           * fix18: fix17 worked but was silent, so a run could not tell a cap that had been"""

COUNTER_OLD = """            if (EmulateZeroLoad(ctx)) {
              cero_simulados.store(hechos + 1);
              RexSwitchPerfCount(19);"""
COUNTER_NEW = """            if (EmulateZeroLoad(ctx)) {
              cero_simulados.store(hechos + 1);
              /*
               * fix19: id 29 for the zero-load emulations. It was 19, which the profile line
               * prints as "parones/s" and which command_processor.cpp also counts, so a run
               * could not tell emulated loads from a stalled GPU pipeline.
               */
              RexSwitchPerfCount(29);"""

patch(EXC, [(HELPER_OLD, HELPER_NEW), (SITE_OLD, SITE_NEW), (COUNTER_OLD, COUNTER_NEW)],
      "exception_handler_switch.cpp")

# -------------------------------------------------------------------------------------------------
# 4. switch_perf.cpp
# -------------------------------------------------------------------------------------------------
PERF = os.path.join(SRC, "ui", "switch_perf.cpp")

PERF_FMT_OLD = """               "muestras %zu | vigilancia: %s | samplers: %.0f nuevos/s, %.0f parones/s\\n","""
PERF_FMT_NEW = """               "muestras %zu | vigilancia: %s | samplers: %.0f nuevos/s, %.0f parones/s | "
               "zero-load %.0f/s, alias fisico %.0f/s\\n","""

PERF_ARG_OLD = """                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds);
  // Work the game sends to the GPU, per presented frame. One screen is"""
PERF_ARG_NEW = """                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds,
                double(counters_now[29] - counters_last[29]) / seconds,
                double(counters_now[30] - counters_last[30]) / seconds);
  // Work the game sends to the GPU, per presented frame. One screen is"""

patch(PERF, [(PERF_FMT_OLD, PERF_FMT_NEW), (PERF_ARG_OLD, PERF_ARG_NEW)], "switch_perf.cpp")

# -------------------------------------------------------------------------------------------------
# 5. Safety check: the counters fix19 uses (29, 30) must be free in the whole SDK.
# -------------------------------------------------------------------------------------------------
used = []
for dirpath, _dirs, files in os.walk(SRC):
    for name in files:
        if not name.endswith((".cpp", ".h", ".c", ".inl", ".inja")):
            continue
        path = os.path.join(dirpath, name)
        text = read(path)
        for cid in (29, 30):
            for macro in ("RexSwitchPerfCount", "RexSwitchPerfAdd", "RexSwitchPerfMax"):
                if "%s(%d" % (macro, cid) in text:
                    used.append((macro, cid, os.path.relpath(path, ROOT)))

if used:
    print("ABORT: contadores 29/30 ya en uso:")
    for u in used:
        print("   ", u)
    sys.exit(1)
print("OK contadores 29 y 30 libres en todo el SDK")

for path in (CPP, HDR, EXC, PERF):
    text = read(path)
    kc = [l for l in text.splitlines() if "kCounterCount" in l and "=" in l]
    for l in kc:
        print("   %s: %s" % (os.path.basename(path), l.strip()))

print("PATCH COMPLETO")