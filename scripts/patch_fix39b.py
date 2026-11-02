#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix39 (segunda mitad) - patch anclado, preservando el fin de linea de cada bloque.

  A) guest_memory_switch.cpp : nueva entrada C  RexGmLiberarPresupuestoParaCarga()
  B) exception_handler_switch.cpp :
     D) declaracion extern "C" de la anterior
     A2) el escalon (3) entra por rango de arena (sin condicion)
     2a) marca si la pagina ya tenia respaldo
     2b+2c) reciclaje con un reintento + una linea acotada de evidencia

No toca el comportamiento del juego fuera del camino que falla: todo lo que hace
es idempotente (RexGmCommit sobre un chunk existente, RexGmFaultIn sobre uno ya
mapeado) y el reciclaje ya existe, solo cambia el orden con que se pide.
"""

import hashlib
import os
import sys

BS = chr(92)
NL = chr(10)
CR = chr(13)
CRLF = CR + NL

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEM = os.path.join(ROOT, "repos", "nfsmw-nx", "sdk", "src", "core", "guest_memory_switch.cpp")
HND = os.path.join(ROOT, "repos", "nfsmw-nx", "sdk", "src", "core", "exception_handler_switch.cpp")

MEM_SHA = "b0766d14ed46cd97b429cbe6d9a9595f43ec5e1a311cf72cfad87818df830c1b"
HND_SHA = "08ce6d11dc5f265c80e3a45dc121bbf2d0481447434cce216450999be7c0556f"


def read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def write_bytes(p, b):
    with open(p, "wb") as f:
        f.write(b)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def split_keep(text):
    """[(linea sin EOL, EOL)]"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        j = text.find(NL, i)
        if j < 0:
            out.append((text[i:], ""))
            break
        if j > i and text[j - 1] == CR:
            out.append((text[i:j - 1], CRLF))
        else:
            out.append((text[i:j], NL))
        i = j + 1
    return out


def join_keep(parts):
    return "".join(ln + e for ln, e in parts)


def sub_block(parts, old_lines, new_lines, tag):
    hits = []
    span = len(old_lines)
    for s in range(len(parts) - span + 1):
        ok = True
        for i in range(span):
            if parts[s + i][0] != old_lines[i]:
                ok = False
                break
        if ok:
            hits.append(s)
    if len(hits) != 1:
        print("[ABORT] %s: %d coincidencias (se esperaba 1)" % (tag, len(hits)))
        sys.exit(1)
    s = hits[0]
    eol_fin = parts[s + span - 1][1] or NL
    repl = []
    for i, ln in enumerate(new_lines):
        eol = parts[s + i][1] if s + i < s + span else ""
        if not eol:
            eol = parts[s + i - 1][1] if i else eol_fin
        repl.append((ln, eol))
    parts[s:s + span] = repl
    return parts


def no_bs(lines):
    """el texto C++ lleva @B@ donde debe ir una barra invertida (\\n de printf)"""
    return [ln.replace("@B@", BS) for ln in lines]


# ---------------------------------------------------------------- linea nueva memoria
FUNC = no_bs([
    "/*",
    " * fix39: the fault path's own entry to the reclaim that LiberarPresupuestoParaCarga does.",
    " *",
    " * The load path and the fault path are after different things, and LiberarPresupuestoParaCarga",
    " * is written for the load path: a block that cannot be materialised is asking for a board, so it",
    " * hands the guest physical cache back first (fix38) and only then the raw views (fix30). A fault",
    " * that cannot be mapped in its view is asking for mapping entries, and the only thing that hands",
    " * entries back is the raw views (see the note over LiberarVistasCrudas: what really goes back to",
    " * the kernel is the mapping budget of the entries those views had taken). SoltarFisicoSinUso keeps",
    " * finding granules to give - the batch this comes from had 64 - so that first branch would succeed",
    " * on every call and the views would never be reached.",
    " *",
    " * So the views come first here and the physical cache after, both bounded by their own ceilings",
    " * (kLibVistasPorEvento views per event and kLibEventosMax events per run for the views,",
    " * kFisicosMax granules for the cache). The caller is the last step of the handler's ladder, which",
    " * is already the last chance before the fatal report.",
    " */",
    "int RexGmLiberarPresupuestoParaCarga(void) {",
    "    State& s = S();",
    "    GmLock lock(s.mutex);",
    "    if (!s.base) return 0;",
    "    size_t vistas = 0;",
    "    const size_t eventos = g_lib_eventos.load(std::memory_order_relaxed);",
    "    const size_t vivas = VistasCrudasVivas(s);",
    "    if (eventos < kLibEventosMax && vivas != 0) {",
    "        g_lib_eventos.fetch_add(1, std::memory_order_relaxed);",
    "        g_presion_mapeo.store(true, std::memory_order_relaxed);",
    "        vistas = LiberarVistasCrudas(kLibVistasPorEvento);",
    "        g_lib_vistas.fetch_add(vistas, std::memory_order_relaxed);",
    "        fprintf(stderr,",
    '                "[fix39] presupuesto para el fault: liberadas %zu vistas crudas '
    '(%zu -> %zu vivas) "',
    '                "en el evento %zu/%zu@B@n",',
    "                vistas, vivas, VistasCrudasVivas(s), eventos + 1, kLibEventosMax);",
    "    }",
    "    const size_t fisicos = SoltarFisicoSinUso(s, nullptr);",
    "    if (fisicos != 0) PiscinaReservar();",
    "    return (vistas != 0 || fisicos != 0) ? 1 : 0;",
    "}",
])

# ---------------------------------------------------------------- handler
OLD_DECL = ['extern "C" void RexGmClearProtect(size_t offset, size_t length);']
NEW_DECL = OLD_DECL + ['extern "C" int RexGmLiberarPresupuestoParaCarga(void);']

OLD_GATE = [
    "     * when even the shadow cannot serve the load.",
    "     */",
    "    if (!hay_permiso || RexGmShadowFor(far) == nullptr) {",
]
NEW_GATE = [
    "     * when even the shadow cannot serve the load.",
    "     *",
    "     * fix39 (second part): the gate is not a condition any more. The batch this step was written",
    "     * for came back with the step still skipped: the page that crashed had a chunk behind it (its",
    "     * granule had never been handed back, fix19 had only unmapped it from the raw view it had",
    "     * entered), so hay_permiso was true and the step never ran. Every mapping offset of the arena",
    "     * behind the mirrors is guest physical memory: it exists, it is not a free guest virtual page,",
    "     * and committing or mapping it can only help. What keeps this step off the other faults is the",
    "     * range test below, and everything it does is idempotent (RexGmCommit over an existing chunk,",
    "     * RexGmFaultIn over an already mapped one), so the cost for a fault it cannot serve is the same",
    "     * lookups step (2) had already paid.",
    "     */",
    "    {",
]

OLD_RANGE = [
    "      if (RexGmWindowToOffset(far, &offset) && offset >= kFisicaIni && offset < kFisicaFin) {",
]
NEW_RANGE = OLD_RANGE + [
    "        const bool respaldo_previo = (RexGmShadowFor(far) != nullptr);",
]

OLD_TAIL = [
    "        if (servido) {",
    "          RexSwitchPerfCount(17);",
    "          RexResumeFromException(ctx);",
    "        }",
]
NEW_TAIL = no_bs([
    "        if (!servido) {",
    "          /*",
    "           * fix39: the page has backing here but could not be mapped in this view. RexGmFaultIn",
    "           * refuses in six ways and the fatal report only says which one after the run has ended;",
    "           * the two a fault can still get out of are the mapping-entry budget and the physical",
    "           * cache, and the reclaim of the load path hands both back (raw views first: they are the",
    "           * only holder that returns entries). One retry: if it still refuses, the fatal report",
    "           * says why, together with the diagnostic that accompanies it.",
    "           */",
    "          if (RexGmLiberarPresupuestoParaCarga() != 0) {",
    "            servido = RexGmFaultIn(far);",
    "          }",
    "        }",
    "        if (!respaldo_previo) {",
    "          /*",
    "           * fix39: this step had never run before this build, so a line (bounded: the first 48",
    "           * pages) is worth it to see it working: a page that had no backing when it faulted and",
    "           * has one now is the rebuild SoltarFisicoSinUso relies on, served from the fault path.",
    "           */",
    "          static std::atomic<uint32_t> fisica39{0};",
    "          if (fisica39.fetch_add(1, std::memory_order_relaxed) < 48) {",
    "            fprintf(stderr,",
    '                    "[fix39] fisica a la primera invitado=0x%08llX off=0x%zX servido=%d "',
    '                    "escritura=%d@B@n",',
    "                    static_cast<unsigned long long>(far -",
    "                                                   reinterpret_cast<uint64_t>(RexGmBase())),",
    "                    offset, servido ? 1 : 0, is_write ? 1 : 0);",
    "          }",
    "        }",
    "        if (servido) {",
    "          RexSwitchPerfCount(17);",
    "          RexResumeFromException(ctx);",
    "        }",
])


def main():
    mem_raw = read_bytes(MEM)
    hnd_raw = read_bytes(HND)
    if sha256(mem_raw) != MEM_SHA:
        print("[ABORT] guest_memory_switch.cpp sha256 != esperado: %s" % sha256(mem_raw))
        sys.exit(1)
    if sha256(hnd_raw) != HND_SHA:
        print("[ABORT] exception_handler_switch.cpp sha256 != esperado: %s" % sha256(hnd_raw))
        sys.exit(1)

    write_bytes(MEM + ".fix39a.bak", mem_raw)
    write_bytes(HND + ".fix39a.bak", hnd_raw)

    mem = split_keep(mem_raw.decode("utf-8"))
    hnd = split_keep(hnd_raw.decode("utf-8"))

    # A) memoria: insertar la funcion detras del cierre del namespace anonimo
    anchor_mem = [
        "}  // namespace",
        "",
        "/*",
        " * fix19: gives [V, V+granule) of the guest address space a view of its own over physical memory,",
    ]
    new_mem = [
        "}  // namespace",
        "",
    ] + FUNC + [
        "",
        "/*",
        " * fix19: gives [V, V+granule) of the guest address space a view of its own over physical memory,",
    ]
    mem = sub_block(mem, anchor_mem, new_mem, "mem/func39")

    # B) handler
    hnd = sub_block(hnd, OLD_DECL, NEW_DECL, "hnd/decl39")
    hnd = sub_block(hnd, OLD_GATE, NEW_GATE, "hnd/gate39")
    hnd = sub_block(hnd, OLD_RANGE, NEW_RANGE, "hnd/respaldo39")
    hnd = sub_block(hnd, OLD_TAIL, NEW_TAIL, "hnd/retry39")

    mem_out = join_keep(mem).encode("utf-8")
    hnd_out = join_keep(hnd).encode("utf-8")
    write_bytes(MEM, mem_out)
    write_bytes(HND, hnd_out)

    for etiqueta, p, b in (("guest_memory_switch.cpp", MEM, mem_out),
                           ("exception_handler_switch.cpp", HND, hnd_out)):
        bom = "BOM" if b[:3] == b"\xef\xbb\xbf" else "sin BOM"
        crlf = b.count(CRLF.encode("utf-8"))
        lf = b.count(NL.encode("utf-8")) - crlf
        print("%-28s %7d B  %5d lineas  sha256 %s  %s  CRLF %d / LF %d"
              % (etiqueta, len(b), len(split_keep(b.decode("utf-8"))), sha256(b), bom, crlf, lf))

    print("\n--- regiones cambiadas ---")
    for etiqueta, path in (("handler", HND), ("memoria", MEM)):
        txt = read_bytes(path).decode("utf-8")
        print("\n[%s]" % etiqueta)
        for marca in ("fix39",):
            for i, ln in enumerate(txt.split(NL), 1):
                if marca in ln:
                    print("  %s:%d: %s" % (os.path.basename(path), i, ln.strip()[:110]))


if __name__ == "__main__":
    main()