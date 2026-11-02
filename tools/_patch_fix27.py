#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix27: byte level patch over the T3DOTM port sources. Every replacement is exact (the script fails
if an anchor does not appear exactly once) and the line endings of each file are preserved (both
files are pure CRLF).
"""
import io
import sys

ROOT = "J:/XBOX360 port/Transformers Dark of the Moon/repos/nfsmw-nx/sdk/src"

GUEST = ROOT + "/core/guest_memory_switch.cpp"
PERF = ROOT + "/ui/switch_perf.cpp"

# ---------------------------------------------------------------------------
# guest_memory_switch.cpp
# ---------------------------------------------------------------------------

G1_OLD = """std::atomic<size_t> g_aliases_grandes{0};
"""

G1_NEW = """std::atomic<size_t> g_aliases_grandes{0};

/*
 * fix27: the 2 MB alignment of the backing is a wish, not a requirement: it only saves the kernel
 * 512 page table entries per block. Under fragmentation memalign(2 MB, 2 MB) fails while the arena
 * still has free space, and from then on every fault on that block pays the failing search again:
 * 2045 refusals on the same offset in a 30 s run of this game, with the screen at 0.0 fps.
 *
 * The port used to retry on every fault. Now the first refusal drops the whole process to the 4 KB
 * alignment every block had before fix23, and 2 MB is not asked for again in this run: what costs
 * is the refusal, because the search for an aligned hole walks the whole free list. A block that
 * cannot be had either way is retried kIntentosBackingMax times and then left alone.
 */
std::atomic<int> g_alineacion_baja{0};
std::atomic<size_t> g_denegaciones_2m{0};
std::atomic<size_t> g_sin_backing{0};
constexpr unsigned kIntentosBackingMax = 4;
"""

G2_OLD = """    VirtmemReservation* shadow_res; /* virtmem reservation of the shadow (nullptr for prealloc slices) */
};
"""

G2_NEW = """    VirtmemReservation* shadow_res; /* virtmem reservation of the shadow (nullptr for prealloc slices) */
    unsigned fallos = 0;            /* fix27: failed materialisations; capped by kIntentosBackingMax */
};
"""

G3_OLD = """bool MaterializarBloque(State& s, Chunk& c) {
"""

G3_NEW = """/*
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
            "[fix27] sin backing: off=0x%zX len=0x%zX fallo 2MB=%d intentos=%u previos=%zu\\n"
            "[fix27] arena: arena=%llu fordblks=%llu keepcost=%llu uordblks=%llu ordblks=%llu\\n"
            "[fix27] invitado: comprometido=%llu MB mapeado=%llu MB bloques=%zu alias 2MB=%llu "
            "denegaciones 2MB=%llu\\n",
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

bool MaterializarBloque(State& s, Chunk& c) {
"""

G4_OLD = """    const u64 desde = svcGetSystemTick();
    size_t block = c.length;
    if (block < kPageSize) block = kPageSize;
"""

G4_NEW = """    const u64 desde = svcGetSystemTick();
    size_t block = c.length;
    if (block < kPageSize) block = kPageSize;

    /* fix27: this block already gave up: do not pay for the search again on every fault. */
    if (c.fallos >= kIntentosBackingMax) return false;
"""

G5_OLD = """     */
    const size_t alineacion =
        (block >= kBlockSize && g_paginas_grandes != 0) ? kBlockSize : kPageSize;
    void* backing = memalign(alineacion, block);
    if (!backing) {
        fprintf(stderr, "RexGmCommit: memalign(%zu) failed at offset %zu\\n", block, c.offset);
        return false;
    }
    memset(backing, 0, block);
"""

G5_NEW = """     *
     * fix27: 2 MB while the heap grants it. The first refusal switches the process to 4 KB for
     * good (see the note on g_alineacion_baja) and the block is materialised from there, which is
     * where every block was before fix23.
     */
    const bool pide_2m = block >= kBlockSize && g_paginas_grandes != 0 &&
                         !g_alineacion_baja.load(std::memory_order_relaxed);
    void* backing = pide_2m ? memalign(kBlockSize, block) : nullptr;
    if (pide_2m && !backing) {
        if (g_denegaciones_2m.fetch_add(1, std::memory_order_relaxed) < 4) {
            fprintf(stderr, "[fix27] 2 MB denegado off=0x%zX len=0x%zX, se pasa a 4 KB\\n", c.offset,
                    block);
        }
        g_alineacion_baja.store(1, std::memory_order_relaxed);
    }
    if (!backing) {
        backing = memalign(kPageSize, block);
    }
    if (!backing) {
        ++c.fallos;
        DiagnosticoSinBacking(s, c, block, pide_2m);
        return false;
    }
    const size_t alineacion = pide_2m ? kBlockSize : kPageSize;
    memset(backing, 0, block);
"""

G6_OLD = """void RexGmLockSnapshot(u64* max_ticks, u64* count) {
    if (max_ticks) {
        *max_ticks = g_lock_max.exchange(0, std::memory_order_relaxed);
    }
    if (count) {
        *count = g_lock_n.exchange(0, std::memory_order_relaxed);
    }
}

}  // extern "C"
"""

G6_NEW = """void RexGmLockSnapshot(u64* max_ticks, u64* count) {
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

size_t RexGmSinBacking(void) {
    return g_sin_backing.load(std::memory_order_relaxed);
}

}  // extern "C"
"""

# ---------------------------------------------------------------------------
# switch_perf.cpp
# ---------------------------------------------------------------------------

P1_OLD = """const char* RexLogFilePath(void);
"""

P1_NEW = """const char* RexLogFilePath(void);
/* fix27: whether the heap refused a 2 MB backing (1 = the port is on 4 KB from then on), how many
 * refusals there were and how many blocks were left without backing (guest_memory_switch.cpp). */
int RexGmBajaAlineacion(void);
size_t RexGmDenegaciones2m(void);
size_t RexGmSinBacking(void);
"""

P2_OLD = """constexpr u32 kVigiaLogCadaLatidos = 5;
"""

P2_NEW = """constexpr u32 kVigiaLogCadaLatidos = 5;
/*
 * fix27: how many watchdog lines are mirrored to stderr. In fix26 the latido file went through the
 * raw fsdev interface and stayed at zero bytes for the whole run: the watchdog wrote and nothing
 * arrived - and that file is the channel that has to arrive. stderr never failed in this port, so
 * every line goes out through it as well while this budget lasts.
 */
constexpr u32 kLatidoEspejoMax = 900;
"""

P3_OLD = """class Vigia {
 public:
  bool Abrir() {
    char ruta[FS_MAX_PATH];
    std::snprintf(ruta, sizeof(ruta), "%srex_latido.log", RexSwitchLogDir());
    FsFileSystem* fs = nullptr;
    char path[FS_MAX_PATH];
    if (fsdevTranslatePath(ruta, &fs, path) != 0 || !fs) {
      return false;
    }
    fsFsCreateFile(fs, path, 0, 0);  // it fails if it already exists, which is not a problem
    if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write, &file))) {
      if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &file))) {
        return false;
      }
    }
    abierto = true;
    return true;
  }

  /* One line per second. Measured so that the line after it can say what the card took. */
  void Escribir(const char* datos, size_t n) {
    if (!abierto || !n) {
      return;
    }
    const u64 t0 = armGetSystemTick();
    const Result rc = fsFileWrite(&file, offset, datos, n, FsWriteOption_Flush);
    const u64 t1 = armGetSystemTick();
    offset += n;
    lat_us = freq ? (t1 - t0) * 1000000ULL / freq : 0;
    ultimo_rc = static_cast<u32>(rc);
  }

  void MarcarFreq(u64 f) { freq = f; }
  bool abierto = false;
  u64 lat_us = 0;
  u32 ultimo_rc = 0;
  FsFile file{};
"""

P3_NEW = """class Vigia {
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
"""

P4_OLD = """ private:
  s64 offset = 0;
  u64 freq = 0;
"""

P4_NEW = """ private:
  u64 freq = 0;
"""

P5_OLD = """  bool avisado = false;
  if (!v.Abrir()) {
    std::fprintf(stderr, "[fix26] vigia: no se pudo abrir %srex_latido.log\\n", RexSwitchLogDir());
    avisado = true;
  }
"""

P5_NEW = """  bool avisado = false;
  if (v.Abrir()) {
    std::fprintf(stderr, "[fix27] vigia: latido por stdio, espejo en stderr hasta %u lineas\\n",
                 kLatidoEspejoMax);
  } else {
    std::fprintf(stderr, "[fix27] vigia: no se pudo abrir %srex_latido.log, el latido va a stderr\\n",
                 RexSwitchLogDir());
    avisado = true;
  }
"""

P6_OLD = """    if (!v.abierto) {
      if (!avisado) {
        std::fprintf(stderr, "[fix26] vigia: sigue sin abrir %srex_latido.log\\n", RexSwitchLogDir());
        avisado = true;
      }
      if (!v.Abrir()) {
        continue;
      }
    }
"""

P6_NEW = """    if (!v.abierto) {
      if (!avisado) {
        std::fprintf(stderr, "[fix27] vigia: sin %srex_latido.log, el latido sigue por stderr\\n",
                     RexSwitchLogDir());
        avisado = true;
      }
      /* fix27: no continue: without the file the line has to go out through the mirror anyway. */
      v.Abrir();
    }
"""

P7_OLD = """    /* The game log, every kVigiaLogCadaLatidos lines. It is opened read-only: if the card is wedged,
     * it shows here, and it is not worth an open per second. */
    if (latidos % kVigiaLogCadaLatidos == 0) {
      FsFileSystem* fs = nullptr;
      char path[FS_MAX_PATH];
      FsFile f{};
      const char* ruta = RexLogFilePath();
      log_rc = 0xFFFFFFFFu;
      if (ruta && ruta[0] && fsdevTranslatePath(ruta, &fs, path) == 0 && fs &&
          R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Read, &f))) {
        s64 tam = 0;
        const Result rc = fsFileGetSize(&f, &tam);
        log_rc = static_cast<u32>(rc);
        if (R_SUCCEEDED(rc)) {
          log_bytes = tam;
        }
        fsFileClose(&f);
      }
    }
"""

P7_NEW = """    /* The game log, every kVigiaLogCadaLatidos lines, through stdio like everything else: the size
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
"""

P8_OLD = """        "[fix26] t=%6.1fs fps=%5.1f pres=%llu lento=%us perf_loop=%llu(%+lld) perf_st=%u "
        "rep=%llu repfail=%llu lock=%llums(max %llums/%llu) log=%lldKB(rc 0x%X) "
        "sd=%lluus(rc 0x%X) hilos=%u","""

P8_NEW = """        "[fix27] t=%6.1fs fps=%5.1f pres=%llu lento=%us perf_loop=%llu(%+lld) perf_st=%u "
        "rep=%llu repfail=%llu lock=%llums(max %llums/%llu) log=%lldKB(rc 0x%X) "
        "sd=%lluus(rc 0x%X) al4k=%d neg2m=%zu sin_back=%zu hilos=%u","""

P9_OLD = """        static_cast<unsigned>(v.ultimo_rc), VigiaHilos());
"""

P9_NEW = """        static_cast<unsigned>(v.ultimo_rc), RexGmBajaAlineacion(), RexGmDenegaciones2m(),
        RexGmSinBacking(), VigiaHilos());
"""

P10_OLD = """  bool ya_rendio = false;      // do not judge before the game has presented once
  bool stderr_episodio = false;
  s64 log_bytes = 0;
"""

P10_NEW = """  bool ya_rendio = false;      // do not judge before the game has presented once
  s64 log_bytes = 0;
"""

P11_OLD = """            "-- [fix26] episodio %u a los %.1f s: %u s seguidos con %llu fotogramas o menos por "
"""

P11_NEW = """            "-- [fix27] episodio %u a los %.1f s: %u s seguidos con %llu fotogramas o menos por "
"""

P12_OLD = """        if (!stderr_episodio) {
          stderr_episodio = true;
          std::fprintf(stderr,
                       "[fix26] episodio %u: fps<=%llu durante %u s (t=%.1f), perf_st=%u, "
                       "lock=%llums\\n",
                       episodios, static_cast<unsigned long long>(kVigiaFpsLento),
                       static_cast<unsigned>(seg_lento), t, st,
                       static_cast<unsigned long long>(lock_ahora * 1000ULL / freq));
        }
"""

P12_NEW = """        /*
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
                     "hilos=%u\\n",
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
                     RexGmDenegaciones2m(), RexGmSinBacking(), VigiaHilos());
"""

P13_OLD = """        const int m = std::snprintf(fin, sizeof(fin), "-- [fix26] fin del episodio %u: %.1f s, %u s "
"""

P13_NEW = """        const int m = std::snprintf(fin, sizeof(fin), "-- [fix27] fin del episodio %u: %.1f s, %u s "
"""

PATCHES = [
    (GUEST, "G1 globals", G1_OLD, G1_NEW),
    (GUEST, "G2 Chunk.fallos", G2_OLD, G2_NEW),
    (GUEST, "G3 diagnostico", G3_OLD, G3_NEW),
    (GUEST, "G4 tope de intentos", G4_OLD, G4_NEW),
    (GUEST, "G5 fallback 4 KB", G5_OLD, G5_NEW),
    (GUEST, "G6 accesores", G6_OLD, G6_NEW),
    (PERF, "P1 externs", P1_OLD, P1_NEW),
    (PERF, "P2 kLatidoEspejoMax", P2_OLD, P2_NEW),
    (PERF, "P3 Vigia stdio", P3_OLD, P3_NEW),
    (PERF, "P4 offset", P4_OLD, P4_NEW),
    (PERF, "P5 apertura", P5_OLD, P5_NEW),
    (PERF, "P6 reintento", P6_OLD, P6_NEW),
    (PERF, "P7 tamano del log", P7_OLD, P7_NEW),
    (PERF, "P8 linea de latido", P8_OLD, P8_NEW),
    (PERF, "P9 argumentos", P9_OLD, P9_NEW),
    (PERF, "P10 sin stderr_episodio", P10_OLD, P10_NEW),
    (PERF, "P11 bloque episodio", P11_OLD, P11_NEW),
    (PERF, "P12 episodio a stderr", P12_OLD, P12_NEW),
    (PERF, "P13 fin de episodio", P13_OLD, P13_NEW),
]


def load(path):
    with io.open(path, "rb") as fh:
        data = fh.read()
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    text = data.decode("utf-8")
    return text, crlf, lf


def main():
    files = {}
    for path in (GUEST, PERF):
        text, crlf, lf = load(path)
        files[path] = [text, crlf, lf]
        print("load %-24s crlf=%d lf=%d bytes=%d" % (path.split("/")[-1], crlf, lf,
                                                     len(text.encode("utf-8"))))

    changed = {}
    for path, label, old, new in PATCHES:
        text = files[path][0]
        crlf = files[path][1] > 0
        o = old.replace("\n", "\r\n") if crlf else old
        n = new.replace("\n", "\r\n") if crlf else new
        count = text.count(o)
        if count != 1:
            print("FAIL %-28s anclas encontradas=%d" % (label, count))
            return 2
        text = text.replace(o, n, 1)
        files[path][0] = text
        changed[path] = changed.get(path, 0) + 1
        print("ok   %-28s" % label)

    for path in (GUEST, PERF):
        text, crlf, lf = files[path]
        with io.open(path, "wb") as fh:
            fh.write(text.encode("utf-8"))
        nuevo_crlf = text.count("\r\n")
        nuevo_lf = text.count("\n") - nuevo_crlf
        print("write %-22s parches=%d crlf=%d lf=%d bytes=%d" % (path.split("/")[-1],
              changed.get(path, 0), nuevo_crlf, nuevo_lf, len(text.encode("utf-8"))))
        if nuevo_lf != lf:
            print("FAIL: se introdujeron saltos LF sueltos")
            return 3
    print("PATCH OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())