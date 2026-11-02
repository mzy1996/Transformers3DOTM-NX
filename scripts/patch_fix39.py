#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix39: make the physical on-demand commit step of the ladder reachable.

Why the patch:

  In HandleSwitchException the ladder's step (3), "Uncommitted physical memory: it is
  committed and mapped", is gated on `if (!hay_permiso)`, and RexGmQueryProtect answers
  *true* with REX_GM_NONE for a page with no committed backing on purpose (the SDK's MMIO
  handler must not be told that an address that does not exist is accessible, or it would
  retry the instruction forever, see its comment at guest_memory_switch.cpp:3307). So
  `!hay_permiso` was false for exactly the faults the step exists for and the path had
  never run: a read of a guest physical page that no chunk backed - the mirrors at
  0xA0000000 / 0xC0000000 / 0xE0000000, the same bytes the console always has - could not
  be served by any step and went straight to the fatal report.

  The logs of the fix38 batch show exactly that: two runs, same guest address 0xBA842718
  (RtlEnterCriticalSection over the mirror of physical 0x1A842718, i.e. arena offset
  0x11A842718), esr 0x92000006, and fix38's own log lines hand that granule back to the
  pool ("fisico liberado invitado=0x1A800000 ... total=64 (128 MB)"). The comment of
  SoltarFisicoSinUso justifies the hand-back with "the port rebuilds it the moment
  RexGmRawAlias asks for that granule again", but RexGmRawAlias (a) only serves window
  offsets below 0x20000000 and (b) refuses while g_presion_mapeo is set (fix30), so the
  rebuild never happened for a mirror read. Resurrecting step (3) is the documented,
  designed way to make those hand-backs reversible.

What it does:

  (A) step (3) is entered whenever the page has no backing to be served from
      (`!hay_permiso || RexGmShadowFor(far) == nullptr`) instead of `!hay_permiso` alone,
      and when the page is unmapped only because it is watched (acceso == REX_GM_READ,
      which is fix21's lending case) and even the shadow could not serve the load, the
      watch of that one page is given up so that the backing can be mapped: the same
      trade fix21 already accepts for a read.
  (B) the fatal path writes one bounded [fix39] line before the dump, with what the
      ladder knew about the address that ended the run, so that a fault this fix does not
      cover says which link refused it instead of needing another guess.
  (C) RepairMissingGuestVirtualPage passed a *mapping* offset to RexGmClearProtect, which
      erases by *window* offset (the key RexGmProtect writes and RexGmFaultIn reads), so
      for any view that does not sit at offset 0 it erased an unrelated page's entry or
      nothing at all. It now passes the window offset, which its own comment describes.

The file has mixed line endings, so the replacement keeps, line by line, the endings of
the region it replaces, and the patch is anchored on exact text: it aborts unless the
baseline fingerprint matches and every anchor is found exactly once.
"""
import hashlib
import os
import re
import shutil
import sys

REL = "repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp"
BASELINE_SHA256 = "c9b73d5523e0f27324f854edb89d634ee1159a666648e780869aed81d14b0dd4"
BASELINE_SIZE = 58942

# ---------------------------------------------------------------- (A) step (3)
A_OLD = """    /*
     * (3) Uncommitted physical memory: it is committed and mapped.
     *
     * Guest physical memory is no longer committed in full at startup (see xmemory.cpp): it is
     * committed the first time it is touched. One 4 MB chunk per fault means at most 128 chunks, which
     * with the 360's five views are about 640 mappings (1 MB chunks gave about 2,500); with 64 KB pages
     * it would be 40,000 and the kernel runs out of blocks. The same instruction is retried, without touching the PC.
     */
if (!hay_permiso) {
      constexpr size_t kFisicaIni = 0x100000000ull;
      constexpr size_t kFisicaFin = 0x120000000ull;
      constexpr size_t kTrozo = 0x400000;  // 4 MB: fewer chunks and fewer mappings
      size_t offset = 0;
      if (RexGmWindowToOffset(far, &offset) && offset >= kFisicaIni && offset < kFisicaFin) {
        if (RexGmCommit(offset & ~(kTrozo - 1), kTrozo, REX_GM_WRITE) && RexGmFaultIn(far)) {
          RexSwitchPerfCount(17);
          RexResumeFromException(ctx);
        }
      }
    }"""

A_NEW = """    /*
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
     */
    if (!hay_permiso || RexGmShadowFor(far) == nullptr) {
      constexpr size_t kFisicaIni = 0x100000000ull;
      constexpr size_t kFisicaFin = 0x120000000ull;
      constexpr size_t kTrozo = 0x400000;  // 4 MB: fewer chunks and fewer mappings
      size_t offset = 0;
      if (RexGmWindowToOffset(far, &offset) && offset >= kFisicaIni && offset < kFisicaFin) {
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
        if (servido) {
          RexSwitchPerfCount(17);
          RexResumeFromException(ctx);
        }
      }
    }"""

# ---------------------------------------------------------------- (B) fatal path
B_OLD = """  (void)far;
  (void)esr;
  RexSwitchCrashLog(data_abort ? "fallo de acceso a memoria" : "excepcion de CPU", ctx,
                    ctx->sp.x, pc);"""

B_NEW = """  (void)far;
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
                    "permiso=%d acceso=%d respaldo=%d | %s\\n",
                    in_window ? 1 : 0, hay_off ? 1 : 0, off_fatal, is_write ? 1 : 0,
                    permiso_fatal ? 1 : 0, static_cast<int>(acceso_fatal),
                    RexGmShadowFor(far) != nullptr ? 1 : 0, info_fatal);
      RexSwitchDiagLine(linea_fatal);
    }
  }
  RexSwitchCrashLog(data_abort ? "fallo de acceso a memoria" : "excepcion de CPU", ctx,
                    ctx->sp.x, pc);"""

# ---------------------------------------------------------------- (C) clear key
C_OLD = """  constexpr size_t kTrozo = 0x100000;  // 1 MB: RexGmCommit rounds it up to its 2 MB granule
  RexGmClearProtect(offset & ~size_t(0xFFF), 0x1000);"""

C_NEW = """  constexpr size_t kTrozo = 0x100000;  // 1 MB: RexGmCommit rounds it up to its 2 MB granule
  /*
   * fix39: RexGmClearProtect erases by *window* offset, the key RexGmProtect writes and
   * RexGmFaultIn reads (the comment of RexGmCommit says why: the same mapping offset can be
   * mirrored in several views with different permissions). This call passed the mapping offset,
   * so for any view that does not sit at offset 0 it erased a page that had nothing to do with
   * the fault instead of the protection that was making RexGmFaultIn refuse the mapping.
   */
  RexGmClearProtect(static_cast<size_t>(far - reinterpret_cast<uint64_t>(RexGmBase())) &
                        ~size_t(0xFFF),
                    0x1000);"""

PATCHES = [("A", A_OLD, A_NEW), ("B", B_OLD, B_NEW), ("C", C_OLD, C_NEW)]


def anchor_regex(anchor):
    """The anchor with any mixture of CRLF/LF line endings tolerated."""
    return "\\r?\\n".join(re.escape(line) for line in anchor.split("\n"))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, REL)
    data = open(path, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != BASELINE_SHA256 or len(data) != BASELINE_SIZE:
        print("ABORT: baseline mismatch: %d bytes, sha256 %s" % (len(data), sha))
        return 2
    print("baseline ok: %d bytes, sha256 %s" % (len(data), sha))

    if not os.path.exists(path + ".fix38.bak"):
        shutil.copyfile(path, path + ".fix38.bak")
        print("backup written: %s.fix38.bak" % REL)

    text = data.decode("utf-8")
    for tag, old, new in PATCHES:
        pat = re.compile(anchor_regex(old), re.M)
        hits = pat.findall(text)
        if len(hits) != 1:
            print("ABORT: anchor %s found %d times (expected 1)" % (tag, len(hits)))
            return 3
        m = pat.search(text)
        matched = m.group(0)
        crlf = matched.count("\r\n")
        lf = matched.count("\n") - crlf
        eol = "\r\n" if crlf >= lf else "\n"
        repl = eol.join(new.split("\n"))
        text = text[:m.start()] + repl + text[m.end():]
        print("anchor %s applied: %d lines -> %d lines, %s endings"
              % (tag, len(matched.split("\n")), len(new.split("\n")),
                 "CRLF" if eol == "\r\n" else "LF"))

    out = text.encode("utf-8")
    open(path, "wb").write(out)
    print("written: %d bytes -> %d bytes" % (len(data), len(out)))
    print("sha256 now: %s" % hashlib.sha256(out).hexdigest())
    crlf = out.count(b"\r\n")
    print("line endings now: %d LF total, %d CRLF, %d lone LF"
          % (out.count(b"\n"), crlf, out.count(b"\n") - crlf))
    return 0


if __name__ == "__main__":
    sys.exit(main())