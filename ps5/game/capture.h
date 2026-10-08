// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <rex/ui/presenter.h>

#include "log.h"

// Captures of the guest output for unattended visual checks: nothing else
// lets a title screenshot itself, and the system's captures need someone at
// the controller.
namespace tabletennis::ps5 {

// Half size, as binary PPM: enough to spot rendering faults, a quarter of the
// bytes to transfer.
inline bool WriteHalfSizePpm(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  const uint32_t width = image.width / 2, height = image.height / 2;
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  std::string row(size_t(width) * 3, '\0');
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = image.data.data() + size_t(y) * 2 * image.stride;
    for (uint32_t x = 0; x < width; ++x) {
      row[size_t(x) * 3 + 0] = char(source[size_t(x) * 8 + 0]);
      row[size_t(x) * 3 + 1] = char(source[size_t(x) * 8 + 1]);
      row[size_t(x) * 3 + 2] = char(source[size_t(x) * 8 + 2]);
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  return true;
}

// From start_s after launch, captures count frames interval_ms apart into
// dir/NNN.ppm: the game's output, or with final_output the presented frame
// including overlays.
inline void StartGuestOutputCapture(rex::ui::Presenter* presenter, std::filesystem::path dir,
                                    int start_s, int count, int interval_ms,
                                    bool final_output) {
  std::thread([=] {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::this_thread::sleep_for(std::chrono::seconds(start_s));
    int written = 0;
    for (int i = 0; i < count; ++i) {
      rex::ui::RawImage image;
      char name[16];
      std::snprintf(name, sizeof(name), "%03d.ppm", i);
      const bool captured = final_output ? presenter->CaptureFinalOutput(image)
                                         : presenter->CaptureGuestOutput(image);
      if (captured && WriteHalfSizePpm(dir / name, image)) ++written;
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    Print("Guest output capture: %d of %d frames in %s\n", written, count, dir.c_str());
  }).detach();
}

}  // namespace tabletennis::ps5
