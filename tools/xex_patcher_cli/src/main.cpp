/**
 * @file        main.cpp
 * @brief       XexPatcher CLI: merge a title update delta patch into a base XEX.
 *
 * Usage:
 *   xex_patcher <base.xex> <patch.xexp> <output.xex>
 *
 * Produces the merged executable (patched/default.xex in the T2ROTF layout)
 * exactly like the UnleashedRecomp installer does at install time. The
 * executable is a standalone Windows binary with no MSYS runtime dependency.
 */

#include <cstdio>
#include <cstring>

#include "xex_patcher.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr,
                 "Usage: xex_patcher <base.xex> <patch.xexp> <output.xex>\n"
                 "Merges an Xbox 360 title update delta patch (default.xexp)\n"
                 "into the base executable (default.xex) and writes the result.\n");
    return 2;
  }

  const char* base_path = argv[1];
  const char* patch_path = argv[2];
  const char* output_path = argv[3];

  XexPatcher::Result result = XexPatcher::apply(base_path, patch_path, output_path);
  switch (result) {
    case XexPatcher::Result::Success:
      std::printf("Patched %s -> %s\n", base_path, output_path);
      return 0;
    case XexPatcher::Result::FileOpenFailed:
      std::fprintf(stderr, "Error: failed to open input/output file\n");
      break;
    case XexPatcher::Result::FileWriteFailed:
      std::fprintf(stderr, "Error: failed to write output file\n");
      break;
    case XexPatcher::Result::XexFileUnsupported:
      std::fprintf(stderr, "Error: base XEX is unsupported\n");
      break;
    case XexPatcher::Result::XexFileInvalid:
      std::fprintf(stderr, "Error: base XEX is invalid\n");
      break;
    case XexPatcher::Result::PatchFileInvalid:
      std::fprintf(stderr, "Error: patch file is invalid\n");
      break;
    case XexPatcher::Result::PatchIncompatible:
      std::fprintf(stderr, "Error: patch is incompatible with the base XEX\n");
      break;
    case XexPatcher::Result::PatchFailed:
      std::fprintf(stderr, "Error: patching failed\n");
      break;
    case XexPatcher::Result::PatchUnsupported:
      std::fprintf(stderr, "Error: patch type is unsupported\n");
      break;
  }
  return 1;
}