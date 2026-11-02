/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @author      mzy1996 (T2ROTF port), based on host_path_device.cpp
 */

#include <rex/filesystem/devices/overlay_host_path_device.h>

#include <chrono>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>

namespace rex::filesystem {

OverlayHostPathDevice::OverlayHostPathDevice(const std::string_view mount_path,
                                             std::vector<std::filesystem::path> layer_paths,
                                             bool read_only, bool allow_share_delete)
    : Device(mount_path), name_("STFS"), read_only_(read_only) {
  for (auto& layer_path : layer_paths) {
    layers_.push_back(std::make_unique<HostPathDevice>(mount_path, layer_path, read_only,
                                                       allow_share_delete));
  }
}

OverlayHostPathDevice::~OverlayHostPathDevice() = default;

bool OverlayHostPathDevice::Initialize() {
  bool any_initialized = false;
  for (auto& layer : layers_) {
    auto host_path = layer->host_path();
    if (!layer->Initialize()) {
      REXFS_WARN("Overlay layer failed to initialize: {}",
                 rex::path_to_utf8(host_path));
      continue;
    }
    any_initialized = true;
    REXFS_INFO("  Overlay layer mounted: {}",
               rex::path_to_utf8(host_path));
  }
  if (!any_initialized) {
    REXFS_ERROR("OverlayHostPathDevice: no layer initialized");
    return false;
  }
  return true;
}

void OverlayHostPathDevice::Dump(string::StringBuffer* string_buffer) {
  for (auto& layer : layers_) {
    layer->Dump(string_buffer);
  }
}

Entry* OverlayHostPathDevice::ResolvePath(const std::string_view path) {
  // Each layer has its own in-memory tree and its own lazy host lookup.
  // Return the first layer that contains the path (TU over DLC over base).
  for (auto& layer : layers_) {
    auto* entry = layer->ResolvePath(path);
    if (entry) {
      return entry;
    }
  }
  return nullptr;
}

}  // namespace rex::filesystem