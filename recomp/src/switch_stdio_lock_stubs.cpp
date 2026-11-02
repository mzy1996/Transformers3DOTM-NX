/*
 * switch_stdio_lock_stubs.cpp
 *
 * newlib for Switch declares flockfile()/funlockfile()/ftrylockfile() in
 * <stdio.h> but ships no implementation.  The only reference in this build
 * comes from mesa's libvulkan.a (fossilize_db.c.o), which uses them to guard
 * its temporary cache FILE* against concurrent access.  libnx's stdio already
 * serializes the underlying console/file I/O, so no-op implementations are
 * safe here and keep the Vulkan driver linkable.
 */
#include <stdio.h>

extern "C" {

void flockfile(FILE *file __attribute__((unused)))
{
}

int ftrylockfile(FILE *file __attribute__((unused)))
{
    return 0;
}

void funlockfile(FILE *file __attribute__((unused)))
{
}

} // extern "C"