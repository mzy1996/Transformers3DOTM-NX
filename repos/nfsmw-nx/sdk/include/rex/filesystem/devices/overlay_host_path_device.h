/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @author      mzy1996 (T2ROTF port), based on host_path_device.h
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

class HostPathDevice;

/*
 * Layered read-only host device.
 *
 * Exposes a single guest mount (e.g. \Device\Harddisk0\Partition1) backed by
 * several host directories. On the Xbox 360 a title update overlays the base
 * game partition and DLC adds new assets without modifying the base files; a
 * recompilation port keeps those layers separate on disk (game/ + update/ +
 * DLC/) and only merges them at lookup time.
 *
 * Resolution is top-down: the first layer that contains the requested path
 * wins (TU over DLC over base game). Directories are per-layer trees, so
 * enumerating a directory shows the contents of the layer the path resolved
 * in; overlay files are always reachable by their exact path, which is how
 * the game and the TU patch loader (default.xexp beside default.xex) access
 * them.
 */
class OverlayHostPathDevice : public Device {
 public:
  /* layer_paths are checked in order; the first hit wins. */
  OverlayHostPathDevice(const std::string_view mount_path,
                        std::vector<std::filesystem::path> layer_paths, bool read_only,
                        bool allow_share_delete = false);
  ~OverlayHostPathDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_;
  std::vector<std::unique_ptr<HostPathDevice>> layers_;
  bool read_only_;
};

}  // namespace rex::filesystem