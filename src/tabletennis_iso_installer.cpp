// Xbox 360 disc image reader based on skate3recomp's installer.

#include "tabletennis_iso_installer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ui/overlay/install_wizard_overlay.h>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

namespace tabletennis {

#if defined(__APPLE__)
std::filesystem::path PickIsoFileMacOS();
#endif

namespace {

constexpr uint32_t kTableTennisTitleId = 0x545407DF;

constexpr uint64_t kSectorSize = 2048;
// Where the game partition starts: plain XDVDFS dumps, then XGD3, XGD2 and
// XGD1 discs.
constexpr std::array<uint64_t, 6> kPossibleGameOffsets = {
    0x00000000ull, 0x0000FB20ull, 0x00020600ull, 0x02080000ull, 0x0FD90000ull, 0x18300000ull};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::string_view kDefaultXex = "default.xex";
// Console system update files, never used by the game.
constexpr std::string_view kSystemUpdateDirectory = "$systemupdate/";

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsUnsafeIsoPath(std::string_view path) {
  if (path.empty() || path.starts_with('/') || path.starts_with('\\')) {
    return true;
  }
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    auto component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == ".." ||
        component.find('\\') != std::string_view::npos) {
      return true;
    }
    if (end == path.size()) {
      break;
    }
    start = end + 1;
  }
  return false;
}

// Title ID from the XEX2 execution info optional header.
std::optional<uint32_t> ParseXexTitleId(const std::vector<uint8_t>& xex) {
  constexpr uint32_t kExecutionInfoKey = 0x00040006;
  if (xex.size() < 0x18 || std::string_view(reinterpret_cast<const char*>(xex.data()), 4) != "XEX2") {
    return std::nullopt;
  }
  const uint32_t header_count = ReadBe32(xex.data() + 0x14);
  for (uint32_t i = 0; i < header_count; ++i) {
    const size_t header_offset = 0x18 + size_t(8) * i;
    if (header_offset + 8 > xex.size()) {
      return std::nullopt;
    }
    if (ReadBe32(xex.data() + header_offset) != kExecutionInfoKey) {
      continue;
    }
    const uint32_t info_offset = ReadBe32(xex.data() + header_offset + 4);
    if (size_t(info_offset) + 16 > xex.size()) {
      return std::nullopt;
    }
    return ReadBe32(xex.data() + info_offset + 12);
  }
  return std::nullopt;
}

struct IsoEntry {
  std::string path;
  uint64_t offset = 0;
  uint64_t size = 0;
};

class XboxIsoReader {
 public:
  bool Open(const std::filesystem::path& path, std::string& error) {
    file_.open(path, std::ios::binary);
    if (!file_) {
      error = "Unable to open the selected ISO.";
      return false;
    }
    file_.seekg(0, std::ios::end);
    file_size_ = static_cast<uint64_t>(file_.tellg());

    std::optional<uint64_t> game_offset;
    std::array<char, kXdvdfsMagic.size()> magic{};
    for (uint64_t candidate : kPossibleGameOffsets) {
      if (ReadAt(candidate + 32 * kSectorSize, magic.data(), magic.size()) &&
          std::string_view(magic.data(), magic.size()) == kXdvdfsMagic) {
        game_offset = candidate;
        break;
      }
    }
    if (!game_offset) {
      error = "The selected file is not a recognized Xbox 360 game ISO.";
      return false;
    }

    std::array<uint8_t, 8> root_info{};
    if (!ReadAt(*game_offset + 32 * kSectorSize + 20, root_info.data(), root_info.size())) {
      error = "The ISO root directory could not be read.";
      return false;
    }
    const uint32_t root_sector = ReadLe32(root_info.data());
    const uint32_t root_size = ReadLe32(root_info.data() + 4);
    if (root_size < 13 || root_size > 32 * 1024 * 1024) {
      error = "The ISO root directory is invalid.";
      return false;
    }

    entries_.clear();
    return ParseDirectory(*game_offset, *game_offset + uint64_t(root_sector) * kSectorSize,
                          error);
  }

  const IsoEntry* FindFile(std::string_view path) const {
    const auto wanted = ToLower(std::string(path));
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const IsoEntry& entry) { return ToLower(entry.path) == wanted; });
    return it != entries_.end() ? &*it : nullptr;
  }

  bool ReadFilePrefix(const IsoEntry& entry, size_t max_size, std::vector<uint8_t>& data) {
    data.resize(size_t(std::min<uint64_t>(entry.size, max_size)));
    return ReadAt(entry.offset, data.data(), data.size());
  }

  uint64_t TotalSize() const {
    uint64_t total = 0;
    for (const auto& entry : entries_) {
      total += entry.size;
    }
    return total;
  }

  bool ExtractAll(const std::filesystem::path& target_root, std::atomic<uint64_t>& copied_bytes,
                  std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(target_root, ec);
    if (ec) {
      error = "Unable to create the game directory.";
      return false;
    }

    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    for (const auto& entry : entries_) {
      if (IsUnsafeIsoPath(entry.path)) {
        error = "The ISO contains an unsafe file path.";
        return false;
      }
      const auto target = target_root / std::filesystem::path(entry.path);
      std::filesystem::create_directories(target.parent_path(), ec);
      if (ec) {
        error = "Unable to create an install subdirectory.";
        return false;
      }
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Unable to create " + entry.path + ".";
        return false;
      }
      uint64_t remaining = entry.size;
      uint64_t read_offset = entry.offset;
      while (remaining > 0) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
        if (!ReadAt(read_offset, buffer.data(), chunk)) {
          error = "Failed to read " + entry.path + " from the ISO.";
          return false;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk));
        if (!out) {
          error = "Failed to write " + entry.path + ". Is the disk full?";
          return false;
        }
        remaining -= chunk;
        read_offset += chunk;
        copied_bytes.fetch_add(chunk, std::memory_order_relaxed);
      }
    }
    return true;
  }

 private:
  bool ReadAt(uint64_t offset, void* data, size_t size) {
    if (offset + size > file_size_) {
      return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return file_.good();
  }

  // Directories are binary trees of entries addressed in 4-byte units.
  bool ParseDirectory(uint64_t game_offset, uint64_t root_directory_offset, std::string& error) {
    struct PendingNode {
      uint64_t directory_offset = 0;
      uint32_t node_offset = 0;
      std::string prefix;
    };

    std::vector<PendingNode> pending;
    pending.push_back({root_directory_offset, 0, ""});
    std::array<uint8_t, 14> header{};
    size_t visited = 0;

    while (!pending.empty()) {
      auto node = std::move(pending.back());
      pending.pop_back();
      if (++visited > 500000) {
        error = "The ISO directory tree is unexpectedly large.";
        return false;
      }

      const uint64_t entry_offset = node.directory_offset + node.node_offset;
      if (!ReadAt(entry_offset, header.data(), header.size())) {
        error = "Failed to read an ISO directory entry.";
        return false;
      }
      const uint16_t left = ReadLe16(header.data());
      const uint16_t right = ReadLe16(header.data() + 2);
      // An empty directory's only "entry" is 0xFF padding.
      if (left == 0xFFFF && right == 0xFFFF) {
        continue;
      }
      const uint32_t sector = ReadLe32(header.data() + 4);
      const uint32_t length = ReadLe32(header.data() + 8);
      const uint8_t attributes = header[12];
      const uint8_t name_length = header[13];
      if (name_length == 0 || name_length > 240) {
        error = "The ISO contains an invalid directory entry name.";
        return false;
      }
      std::string name(name_length, '\0');
      if (!ReadAt(entry_offset + header.size(), name.data(), name.size())) {
        error = "Failed to read an ISO directory entry name.";
        return false;
      }

      if (left) {
        pending.push_back({node.directory_offset, uint32_t(left) * 4u, node.prefix});
      }
      if (right) {
        pending.push_back({node.directory_offset, uint32_t(right) * 4u, node.prefix});
      }

      const std::string full_path = node.prefix + name;
      if (attributes & 0x10) {
        if (length != 0) {
          pending.push_back({game_offset + uint64_t(sector) * kSectorSize, 0, full_path + "/"});
        }
      } else if (!ToLower(full_path).starts_with(kSystemUpdateDirectory)) {
        entries_.push_back({full_path, game_offset + uint64_t(sector) * kSectorSize, length});
      }
    }
    return true;
  }

  std::ifstream file_;
  uint64_t file_size_ = 0;
  std::vector<IsoEntry> entries_;
};

std::filesystem::path PickIsoFile() {
#if defined(__APPLE__)
  return PickIsoFileMacOS();
#elif defined(_WIN32)
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Xbox 360 ISO (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select your Table Tennis Xbox 360 ISO";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) {
    return {};
  }
  return filename;
#else
  REXLOG_ERROR(
      "The ISO picker is only implemented on macOS and Windows. Set TABLETENNIS_INSTALL_ISO to "
      "the ISO path, or game_data_root to an extracted copy of the game.");
  return {};
#endif
}

// Extracts into a sibling staging directory and renames it into place at the
// end, so an interrupted install never looks like a complete one.
bool InstallFromIso(const std::filesystem::path& iso_path, const std::filesystem::path& game_root,
                    std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                    std::string& error) {
  XboxIsoReader iso;
  if (!iso.Open(iso_path, error)) {
    return false;
  }
  const IsoEntry* xex = iso.FindFile(kDefaultXex);
  if (!xex) {
    error = "The ISO does not contain default.xex.";
    return false;
  }
  std::vector<uint8_t> xex_header;
  if (!iso.ReadFilePrefix(*xex, 64 * 1024, xex_header)) {
    error = "Failed to read default.xex from the ISO.";
    return false;
  }
  const std::optional<uint32_t> title_id = ParseXexTitleId(xex_header);
  if (title_id != kTableTennisTitleId) {
    error = title_id ? fmt::format(
                           "This ISO is a different game (title ID {:08X}), not Rockstar Games "
                           "presents Table Tennis ({:08X}).",
                           *title_id, kTableTennisTitleId)
                     : "default.xex in the ISO is not a valid Xbox 360 executable.";
    return false;
  }

  std::error_code ec;
  if (std::filesystem::exists(game_root, ec)) {
    if (!std::filesystem::is_empty(game_root, ec)) {
      error = fmt::format(
          "{} already exists but has no default.xex. Remove or rename it, then try again.",
          game_root.string());
      return false;
    }
    std::filesystem::remove(game_root, ec);
  }
  auto staging_root = game_root;
  staging_root += ".partial";
  std::filesystem::remove_all(staging_root, ec);

  total_bytes = iso.TotalSize();
  if (!iso.ExtractAll(staging_root, copied_bytes, error)) {
    return false;
  }
  std::filesystem::rename(staging_root, game_root, ec);
  if (ec) {
    error = fmt::format("Failed to move the installed files into {}: {}", game_root.string(),
                        ec.message());
    return false;
  }
  return true;
}

std::filesystem::path ResolveGameDataRoot(const rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return paths.game_data_root;
  }
  return paths.config_path.parent_path() / "game";
}

}  // namespace

bool IsGameInstalled(const std::filesystem::path& game_root) {
  return std::filesystem::is_regular_file(game_root / std::string(kDefaultXex));
}

std::optional<rex::PathConfig> FinalizeGamePaths(const rex::PathConfig& defaults,
                                                 rex::ui::ImGuiDrawer* drawer,
                                                 std::function<void(rex::PathConfig)> resume) {
  rex::PathConfig paths = defaults;
  paths.game_data_root = ResolveGameDataRoot(defaults);
  if (IsGameInstalled(paths.game_data_root)) {
    return paths;
  }

  if (const char* iso_path = std::getenv("TABLETENNIS_INSTALL_ISO"); iso_path && *iso_path) {
    REXLOG_INFO("Installing game files from TABLETENNIS_INSTALL_ISO={}", iso_path);
    std::atomic<uint64_t> copied_bytes{0};
    std::atomic<uint64_t> total_bytes{0};
    std::string error;
    if (InstallFromIso(iso_path, paths.game_data_root, copied_bytes, total_bytes, error)) {
      REXLOG_INFO("Installed {} bytes of game files into {}", copied_bytes.load(),
                  paths.game_data_root.string());
      return paths;
    }
    REXLOG_ERROR("ISO installation failed, falling back to the installer: {}", error);
  }

  REXLOG_INFO("Game files not found at {}; showing the ISO installer",
              paths.game_data_root.string());
  const auto game_root = paths.game_data_root;
  // macOS runs a quarantined app that was never moved from a randomized
  // read-only mount, so nothing could be installed next to it.
  const bool translocated = game_root.string().find("/AppTranslocation/") != std::string::npos;
  const char* intro =
      translocated
          ? "macOS is running this app from a temporary read-only location, so the game files "
            "can't be installed next to it. Quit, move the app into another folder (dragging "
            "it in Finder is enough), and open it again."
          : "Table Tennis game files were not found. Select your own Xbox 360 ISO of Rockstar "
            "Games presents Table Tennis to install them.";
  new rex::ui::InstallWizardDialog(
      drawer, "Setup", "Game Files", intro, game_root.string(), [] { return PickIsoFile(); },
      [game_root, translocated](const std::filesystem::path& source,
                                std::atomic<uint64_t>& copied_bytes,
                                std::atomic<uint64_t>& total_bytes, std::string& error) {
        if (translocated) {
          error = "Move the app out of its temporary location first, then open it again.";
          return false;
        }
        return InstallFromIso(source, game_root, copied_bytes, total_bytes, error);
      },
      [paths, resume = std::move(resume)]() mutable { resume(std::move(paths)); });
  return std::nullopt;
}

}  // namespace tabletennis
