#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix26: instrumentation hardening. Byte-level, CRLF-preserving, anchors asserted."""
import sys

SDK = "J:/XBOX360 port/Transformers Dark of the Moon/repos/nfsmw-nx/sdk"

def leer(path):
    with open(path, "rb") as fh:
        crudo = fh.read()
    txt = crudo.decode("utf-8").replace("\r\n", "\n")
    assert "\r" not in txt, path + ": leftover lone CR"
    return txt

def escribir(path, txt):
    with open(path, "wb") as fh:
        fh.write(txt.replace("\n", "\r\n").encode("utf-8"))

def aplicar(path, cambios):
    txt = leer(path)
    for nombre, viejo, nuevo, veces in cambios:
        n = txt.count(viejo)
        if n != veces:
            print("FALLO %s / %s: esperaba %d coincidencias, hay %d" % (path, nombre, veces, n))
            print("  ancla: %r" % (viejo[:120],))
            return False
        if nuevo and nuevo in txt:
            print("FALLO %s / %s: el texto nuevo ya estaba" % (path, nombre))
            return False
        txt = txt.replace(viejo, nuevo)
        print("ok    %s / %s (%d)" % (path.rsplit("/", 1)[-1], nombre, veces))
    escribir(path, txt)
    return True

# --------------------------------------------------------------------------- guest_memory_switch.cpp
GMS = SDK + "/src/core/guest_memory_switch.cpp"

G_TRACKER = r'''State& S() {
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
};'''

G_ACCESORES = r'''/*
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

}  // extern "C"

#endif /* __SWITCH__ */'''

gms_cambios = [
    ("tracker", r'''State& S() {
    static State s;
    return s;
}''', G_TRACKER, 1),
    ("lock_guard->GmLock", "std::lock_guard<std::mutex> lock(s.mutex);", "GmLock lock(s.mutex);", 23),
    ("accesores", r'''}  // extern "C"

#endif /* __SWITCH__ */''', G_ACCESORES, 1),
]

# --------------------------------------------------------------------------- logging.cpp
LOG = SDK + "/src/core/logging.cpp"

LOG_GLOBAL = r'''std::mutex g_mutex;
LogConfig g_config;
/*
 * fix26: path of the file sink as resolved at startup, for the watchdog of switch_perf.cpp. It watches
 * how much it grows to tell "the game stopped logging" (the guest is stuck) from "the card stopped
 * taking writes" (nothing grows, for anybody).
 */
std::string g_log_file_path;'''

LOG_ACCESOR = r'''/*
 * fix26: the log file's path, as resolved at startup, for the watchdog of switch_perf.cpp.
 */
extern "C" const char* RexLogFilePath(void) { return g_log_file_path.c_str(); }

}  // namespace rex'''

log_cambios = [
    ("global", "std::mutex g_mutex;\nLogConfig g_config;", LOG_GLOBAL, 1),
    ("set", r'''        static_cast<size_t>(REXCVAR_GET(log_max_files)), false);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
  }''', r'''        static_cast<size_t>(REXCVAR_GET(log_max_files)), false);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
  }
  g_log_file_path = resolved_path;''', 1),
    ("accesor", "}  // namespace rex", LOG_ACCESOR, 1),
]

# --------------------------------------------------------------------------- switch_perf.cpp
SP = SDK + "/src/ui/switch_perf.cpp"

SP_DECLS = r'''size_t RexGmLargePageAliases(void);
/*
 * fix26: from the memory core and from the log (guest_memory_switch.cpp, logging.cpp), for the watchdog.
 */
u64 RexGmLockHeldTicks(void);
void RexGmLockSnapshot(u64* max_ticks, u64* count);
const char* RexLogFilePath(void);'''

SP_CONST = r'''std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_perfil.log";
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
std::atomic<u64> g_perf_loop{0};
std::atomic<u32> g_perf_stage{0};
std::atomic<u64> g_report_ok{0};
std::atomic<u64> g_report_fail{0};'''

SP_STAGE_REPORT = r'''void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  /*
   * fix26: stage 4 of the watchdog. This is the first thing the report does and it is the phase that
   * can block (one svcGetInfo per live thread).
   */
  g_perf_stage.store(4, std::memory_order_relaxed);
  const double seconds = double(elapsed_ticks) / double(tick_freq);'''

SP_FOPEN = r'''  /*
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
  g_perf_stage.store(6, std::memory_order_relaxed);'''

SP_REPORT_FIN = r'''      std::fputc('\n', f);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
  g_perf_stage.store(1, std::memory_order_relaxed);
}'''

SP_DETECT = r'''    g_perf_stage.store(7, std::memory_order_relaxed);
    {
      static u64 tic_lento = 0;'''

SP_CRIT = r'''        /*
         * fix26: the criterion used to be "not a single frame presented in the whole second". A screen
         * that presents one or two frames and spends the rest of the time stuck is the same problem and
         * was not being registered; the runs that stop do it after falling to 1.0 and then 0.0 fps, and
         * the loading screen itself sits at 7-10 fps. kVigiaFpsLento covers both, and it is the same
         * criterion the watchdog counts with.
         */
        if (pres - pres_lento <= kVigiaFpsLento) {'''

SP_SAMPLING = r'''    g_perf_stage.store(2, std::memory_order_relaxed);
    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {'''

SP_OVERLAY = r'''    g_perf_stage.store(3, std::memory_order_relaxed);
    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).'''

SP_LOOPFIN = r'''      report_start = armGetSystemTick();
    }
    /*
     * fix26: the loop counter is the watchdog's proof that this thread is still running, and 1 is the
     * stage it should be in at the top of the loop (sleeping its 100 ms or about to).
     */
    g_perf_stage.store(1, std::memory_order_relaxed);
    g_perf_loop.fetch_add(1, std::memory_order_relaxed);
  }
}'''

SP_VIGIA = r'''/*
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
  s64 offset = 0;
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
  if (!v.Abrir()) {
    std::fprintf(stderr, "[fix26] vigia: no se pudo abrir %srex_latido.log\n", RexSwitchLogDir());
    avisado = true;
  }
  u64 ult_pres = g_counters[0].load(std::memory_order_relaxed);
  u64 ult_tic = armGetSystemTick();
  u64 ult_loop = g_perf_loop.load(std::memory_order_relaxed);
  u32 seg_lento = 0;
  u32 episodios = 0;
  u32 latidos = 0;
  bool ya_rendio = false;      // do not judge before the game has presented once
  bool stderr_episodio = false;
  s64 log_bytes = 0;
  u32 log_rc = 0;
  for (;;) {
    __real_svcSleepThread(kVigiaPeriodoNs);
    if (!v.abierto) {
      if (!avisado) {
        std::fprintf(stderr, "[fix26] vigia: sigue sin abrir %srex_latido.log\n", RexSwitchLogDir());
        avisado = true;
      }
      if (!v.Abrir()) {
        continue;
      }
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

    /* The game log, every kVigiaLogCadaLatidos lines. It is opened read-only: if the card is wedged,
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

    char linea[1024];
    int n = std::snprintf(
        linea, sizeof(linea),
        "[fix26] t=%6.1fs fps=%5.1f pres=%llu lento=%us perf_loop=%llu(%+lld) perf_st=%u "
        "rep=%llu repfail=%llu lock=%llums(max %llums/%llu) log=%lldKB(rc 0x%X) "
        "sd=%lluus(rc 0x%X) hilos=%u",
        t, fps, static_cast<unsigned long long>(pres), static_cast<unsigned>(seg_lento),
        static_cast<unsigned long long>(loop), static_cast<long long>(loop - ult_loop), st,
        static_cast<unsigned long long>(rep_ok), static_cast<unsigned long long>(rep_fail),
        static_cast<unsigned long long>(lock_ahora * 1000ULL / freq),
        static_cast<unsigned long long>(lock_max * 1000ULL / freq),
        static_cast<unsigned long long>(lock_n), static_cast<long long>(log_bytes >> 10),
        static_cast<unsigned>(log_rc), static_cast<unsigned long long>(v.lat_us),
        static_cast<unsigned>(v.ultimo_rc), VigiaHilos());
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
            "-- [fix26] episodio %u a los %.1f s: %u s seguidos con %llu fotogramas o menos por "
            "segundo\n"
            "   perf_loop=%llu perf_st=%u informes=%llu fallidos=%llu hilos=%u\n"
            "   cerrojo de mapeo: %llums ahora, maximo %llums en %llu adquisiciones\n"
            "   log del juego: %lld KB (rc 0x%X) | ultimo latido: %lluus (rc 0x%X)\n"
            "   cpu:",
            episodios, t, static_cast<unsigned>(seg_lento), static_cast<unsigned long long>(datos),
            static_cast<unsigned long long>(loop), st, static_cast<unsigned long long>(rep_ok),
            static_cast<unsigned long long>(rep_fail), VigiaHilos(),
            static_cast<unsigned long long>(lock_ahora * 1000ULL / freq),
            static_cast<unsigned long long>(lock_max * 1000ULL / freq),
            static_cast<unsigned long long>(lock_n), static_cast<long long>(log_bytes >> 10),
            static_cast<unsigned>(log_rc), static_cast<unsigned long long>(v.lat_us),
            static_cast<unsigned>(v.ultimo_rc));
        for (size_t k = 0; k < n_cpu && m > 0 && m < static_cast<int>(sizeof(bloque)) - 64; ++k) {
          m += std::snprintf(bloque + m, sizeof(bloque) - static_cast<size_t>(m), " %s %.1f%%",
                             cpu[k].nombre, cpu[k].pct);
        }
        if (m > 0 && m < static_cast<int>(sizeof(bloque)) - 2) {
          bloque[m++] = '\n';
          bloque[m] = 0;
          v.Escribir(bloque, static_cast<size_t>(m));
        }
        if (!stderr_episodio) {
          stderr_episodio = true;
          std::fprintf(stderr,
                       "[fix26] episodio %u: fps<=%llu durante %u s (t=%.1f), perf_st=%u, "
                       "lock=%llums\n",
                       episodios, static_cast<unsigned long long>(kVigiaFpsLento),
                       static_cast<unsigned>(seg_lento), t, st,
                       static_cast<unsigned long long>(lock_ahora * 1000ULL / freq));
        }
      }
    } else {
      if (seg_lento >= kVigiaSegundosLento) {
        char fin[256];
        const int m = std::snprintf(fin, sizeof(fin), "-- [fix26] fin del episodio %u: %.1f s, %u s "
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

/* Priority 102: after switch_crash_hooks.c, before the SDK. */'''

SP_START = r'''  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
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
}'''

sp_cambios = [
    ("include cerrno", "#include <cinttypes>", "#include <cerrno>\n#include <cinttypes>", 1),
    ("decls", "size_t RexGmLargePageAliases(void);", SP_DECLS, 1),
    ("constantes", r'''std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_perfil.log";
}''', SP_CONST, 1),
    ("g_vigia", "Thread g_thread;", "Thread g_thread;\nThread g_vigia;", 1),
    ("stage informe", r'''void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  const double seconds = double(elapsed_ticks) / double(tick_freq);''', SP_STAGE_REPORT, 1),
    ("fopen endurecido", r'''  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    g_sample_count = 0;
    return;
  }''', SP_FOPEN, 1),
    ("fin informe", r'''      std::fputc('\n', f);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}''', SP_REPORT_FIN, 1),
    ("detector stage", r'''    {
      static u64 tic_lento = 0;''', SP_DETECT, 1),
    ("criterio paron", "        if (pres == pres_lento) {", SP_CRIT, 1),
    ("stage muestreo", "    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {",
     SP_SAMPLING, 1),
    ("stage overlay", r'''    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).''',
     SP_OVERLAY, 1),
    ("fin de bucle", r'''      report_start = armGetSystemTick();
    }
  }
}''', SP_LOOPFIN, 1),
    ("vigia", "/* Priority 102: after switch_crash_hooks.c, before the SDK. */", SP_VIGIA, 1),
    ("start vigia", r'''  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
}''', SP_START, 1),
]

ok = True
for path, cambios in ((GMS, gms_cambios), (LOG, log_cambios), (SP, sp_cambios)):
    if not aplicar(path, cambios):
        ok = False
print("RESULTADO:", "OK" if ok else "FALLO")
sys.exit(0 if ok else 1)