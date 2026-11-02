/*
 * Guest memory on Horizon: reservation, commit, aliases and protection.
 *
 * This is the core of the port. Everything here was measured on the console, not
 * assumed; see docs/platform-notes.md.
 *
 * The problem
 *
 * ReXGlue inherits from Xenia a memory map with three requirements:
 *   1. a contiguous reservation of 0x120000000 (4.5 GB) of virtual space
 *   2. aliases: several guest virtual windows pointing to the same
 *      physical memory (0xA0000000, 0xC0000000 and 0xE0000000 are mirrors)
 *   3. page faults to emulate MMIO and to watch writes
 *
 * And Horizon imposes three restrictions that clash head-on:
 *   1. there is no sparse memory: you cannot reserve 4.5 GB and then commit
 *      page by page. There is only 3.2 GB of RAM
 *   2. page permissions cannot be changed on what is mapped in the window:
 *      it is left in the ProcessMem state, which lacks PermChangeAllowed
 *   3. the exception handler cannot make system calls
 *
 * The design
 *
 * - The window is virtual space reserved with virtmemAddReservation. Empty.
 * - Memory is committed in chunks, and each chunk carries its shadow alias: the
 *   codeAlias that is needed anyway in order to map (see the dance below). That
 *   alias is always accessible, at a private address.
 * - A chunk committed at a mapping offset is mapped into every view that covers
 *   that offset, each view on its first access (RexGmFaultIn). That is where the
 *   360 mirrors come from, for free.
 * - "Protecting" a page means unmapping it from the window. Logical permissions
 *   are kept in a table of our own, because the kernel does not keep them for us.
 * - The exception handler emulates the access by writing or reading through the
 *   shadow alias. It never makes system calls.
 *
 * The commit dance, which is not obvious
 *
 *   memalign()                     backing, from the newlib heap
 *   virtmemFindCodeMemory()        free range in the code region
 *   svcMapProcessCodeMemory()      turns the backing into a code alias
 *   svcSetProcessMemoryPermission  RW permissions on that alias
 *   svcMapProcessMemory()          mirrors the alias into each view
 *
 * svcMapProcessMemory rejects heap memory as the source: it returns 0xD401
 * (Kernel/106, InvalidCurrentMemory). The detour through code memory is needed.
 * And beware: after svcMapProcessCodeMemory the original heap pointer is left
 * with permissions 0, marked as borrowed. It must not be touched.
 */
#pragma once

#if defined(__SWITCH__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Logical permissions, with the same values as rex::memory::PageAccess so that
 * the conversion is the identity.
 */
typedef enum {
    REX_GM_NONE  = 0,
    REX_GM_READ  = 1 << 0,
    REX_GM_WRITE = (1 << 0) | (1 << 1),  /* lectura + escritura */
} RexGmAccess;

/* Reserves the window. size is usually 0x120000000. Returns the base or NULL. */
uint8_t* RexGmInit(size_t size);

/* * Releases everything. */
void RexGmShutdown(void);

/* Base of the window, or NULL if not initialized. */
uint8_t* RexGmBase(void);

/* Size of the reserved window, or 0 if not initialized. */
size_t RexGmSize(void);

/**
 * Translates a window address into the mapping offset it reflects, according to
 * the view that contains it. Returns false if it falls outside every view.
 *
 * A range [addr, addr+len) is translated by its first address, so the caller
 * must guarantee that it does not cross from one view into another. The SDK heaps
 * satisfy this by construction: each heap lives inside a single view.
 */
bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out);

/**
 * Registers a view: the part of the window that starts at `base` and is
 * `length` long reflects the mapping from offset `offset` onwards.
 *
 * Several views can share the same `offset`: that is exactly what creates the
 * 360 mirrors. Committing once fills them all.
 *
 * It does not map anything by itself; what is already committed is mapped on
 * the fly.
 */
bool RexGmAddView(uint8_t* base, size_t offset, size_t length);

/**
 * Commits real memory in [offset, offset+length) of the mapping. Each view that
 * covers it maps it on first access (RexGmFaultIn).
 *
 * Idempotent: what is already committed is left as it is.
 */
bool RexGmCommit(size_t offset, size_t length, RexGmAccess access);

/* The reverse. Frees the backing. */
bool RexGmDecommit(size_t offset, size_t length);

/*
 * Maps into the window the chunk that covers window_address, and only in the
 * view that address belongs to. Chunks are not mapped when they are committed:
 * the Xbox 360 sees the same memory from up to five addresses, and mapping every
 * chunk into all of them exhausts the process limit on mappable memory
 * (2001-0103, resource exhausted), which is the real ceiling on Horizon.
 *
 * Returns true if it mapped something, that is, if the faulting access can now
 * be retried. Returns false if there is no committed chunk there or it was
 * already mapped: then the fault is something else. Called by the exception
 * handler.
 */
bool RexGmFaultIn(uint64_t window_address);

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

/**
 * fix23: alignment of the guest window, and the price of every mapping.
 *
 * Horizon's kernel maps with a 2 MB block descriptor only where the window address and the backing
 * address are congruent modulo 2 MB; everywhere else it pages with 4 KB entries, and the pool of
 * page table blocks is what runs out first (0xCE01, OutOfResource) with RAM to spare.
 * virtmemFindAslr takes a guard size and not an alignment, so the base of this window came out
 * 4 KB aligned and every mapping of this port paid 512 entries for 2 MB. This decides how the
 * window is aligned; it must be called before RexGmInit (RexGmInit reads the flag itself, so
 * calling it is optional):
 *   0 = 4 KB base: what this port did up to fix22, 4 KB pages everywhere.
 *   1 = 32 MB aligned base: every view congruent except 0xE0000000 (default).
 *   2 = as 1 plus the 4 KB phase of 0xE0000000 on the grid of the physical arena.
 */
void RexGmConfigure(int modo);

/* fix23: mode in force, and whole blocks whose alias went in on a 2 MB boundary. For diagnostics. */
int RexGmLargePagesMode(void);
size_t RexGmLargePageAliases(void);

/**
 * fix21: serves *one read* of a watched page natively, by lending the page its real backing.
 *
 * A page with logical read protection is unmapped (see RexGmProtect), so every access to it is an
 * exception. That is what watching needs for *writes*: the write has to be seen. For *reads* it is
 * pure cost: the same instruction faults again and again while the same bytes are fetched from the
 * shadow, one exception per access. Measured in a race: a byte by byte copy over 3 MB of a watched
 * buffer, 8.32 million read faults, the game down to 0.1 fps.
 *
 * This maps the faulting 4 KB page back in, taking its own chunk's shadow as backing (the same
 * mapping RexGmProtect uses when it restores a page), and drops that page's logical protection, so
 * from then on the hardware serves the reads of that page: one mapping per page instead of one
 * exception per access.
 *
 * What is given up: while the page has no logical protection, a *write* to it no longer faults, so
 * the SDK's write callback for that page is not called until the watch is installed again. It is not
 * coherence by separation that is lost (the backing is the same memory the guest writes and the GPU
 * reads: nothing diverges), only the notification. The watch is installed again by the SDK's own
 * RexGmProtect the next time it resyncs access to the page, and this call can be repeated on every
 * fault, so the state is stable in both directions.
 *
 * Returns true only when the read has been served, that is, when the page is mapped and the caller
 * can resume the faulting instruction. It returns false when there is nothing to lend (no logical
 * read protection, no chunk mapped in this view, or the kernel refuses the mapping), and then the
 * caller falls back to the shadow emulation, which is where it was before.
 */
bool RexGmBorrowWatchForRead(uint64_t window_address);

/* fix21: pages lent this run, and how many lendings the kernel refused. For diagnostics. */
size_t RexGmBorrowedPages(void);
size_t RexGmBorrowRefused(void);

/**
 * Changes the logical permissions of a range of the window.
 *
 * On Horizon there are no page permissions on the window, so this is done by
 * mapping and unmapping:
 *   REX_GM_WRITE  -> mapped and accessible
 *   REX_GM_READ   -> unmapped. Beware: reads fault too, and have to be
 *                    emulated. That is the price of not having real permissions
 *   REX_GM_NONE   -> unmapped
 *
 * If the kernel accepts svcSetProcessMemoryPermission on the views, REX_GM_READ
 * pages are made read-only instead (see RexGmModoProteccion).
 *
 * If out_old is not NULL, it receives the previous permissions.
 */
bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old);

/* Queries the logical permissions. length receives the size of the uniform range. */
bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access);

/**
 * Equivalent address in the shadow alias, always accessible even where the
 * window is unmapped. It is what the exception handler emulates through.
 *
 * Returns NULL if that address is not committed.
 */
void* RexGmShadowFor(uint64_t window_address);

/* Bytes of backing committed right now. For diagnostics. */
size_t RexGmCommittedBytes(void);

/*
 * fix22: chunks materialised on their first touch, and chunks that are still only a descriptor
 * written by RexGmCommit (no backing, no shadow). For diagnostics.
 */
size_t RexGmMaterializedChunks(void);
size_t RexGmPendingChunks(void);
size_t RexGmPendingBytes(void);

/**
 * Bytes mapped in the window, counting each mirror separately.
 *
 * This is the figure that hits Horizon's mapping ceiling (MarathonRecomp-NX puts
 * it at ~2.25 GB). With the ReXGlue memory map, physical memory alone appears in
 * five views, so this figure grows much faster than the backing one.
 */
size_t RexGmMappedBytes(void);

/* Number of committed chunks and of pages with logical protection (unmapped for
 * write watching). Each one costs kernel memory blocks. For diagnostics. */
size_t RexGmChunkCount(void);
size_t RexGmProtectedPages(void);

/*
 * Tells whether watched pages are protected with permissions (readable) or by
 * unmapping (not readable). It is printed in the profile header to show which
 * of the two paths Horizon took. 0 = not tried yet, 1 = permissions,
 * 2 = unmapping.
 */
int RexGmModoProteccion(void);

/**
 * Result of the last system call that failed, or one of the REX_GM_ERR_* codes.
 * 0 if nothing has failed.
 *
 * Failure semantics of RexGmCommit: the failing range is undone entirely, so
 * nothing is left half committed. What earlier ranges of the same call
 * committed stays, because committing is idempotent and a retry resumes where
 * it stopped.
 */
uint32_t RexGmLastResult(void);

#define REX_GM_ERR_NOMEM    0xFFFF0001u  /* memalign of the backing store failed */
#define REX_GM_ERR_NOCODEVA 0xFFFF0002u  /* no free space left in the code region */

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ */
