// SPDX-License-Identifier: GPL-3.0-or-later
// Native title host for the existing Table Tennis recompilation and 0.8 runtime.
#include "generated/default/tabletennis_init.h"
#include "audio.h"
#include "capture.h"
#include "pad_input.h"
#include "log.h"
#include "tabletennis_defaults.h"
#include "native/tabletennis_native_renderer.h"
#include "test/tabletennis_frontend_launch_test.h"

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/perf/thread_cpu.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceKernelUsleep(uint32_t);
REXCVAR_DECLARE(uint32_t, rexcrt_heap_size_mb);
// The console offers 3840x2160, 2560x1440 and 1920x1080 display modes; the
// presenter scales the guest output (FSR 1 with present_effect = "fsr").
REXCVAR_DEFINE_INT32(ps5_display_width, 3840, "PS5", "Display mode width");
REXCVAR_DEFINE_INT32(ps5_display_height, 2160, "PS5", "Display mode height");
REXCVAR_DEFINE_INT32(ps5_pad_vibration_mode, 2, "PS5",
                     "scePadSetVibrationMode value at pad open: 2 = classic rumble, "
                     "1 = audio haptics (the default, which ignores rumble), -1 = leave it");
REXCVAR_DEFINE_INT32(ps5_capture_start_s, 0, "PS5",
                     "Seconds after launch to start capturing the guest output into "
                     "/app0/cap (0: never)");
REXCVAR_DEFINE_INT32(ps5_capture_count, 30, "PS5", "Guest output frames to capture");
REXCVAR_DEFINE_INT32(ps5_capture_interval_ms, 100, "PS5", "Time between captured frames");

namespace {
using tabletennis::ps5::Line;
using tabletennis::ps5::Print;
int crash_log = -1;

void crash_hex(const char* label, uint64_t value) {
  char message[128];
  size_t n = 0;
  while (*label && n < sizeof(message) - 20) message[n++] = *label++;
  message[n++] = '0'; message[n++] = 'x';
  constexpr char digits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) message[n++] = digits[(value >> shift) & 15];
  message[n++] = '\n';
  if (crash_log >= 0) (void)write(crash_log, message, n);
}

void fatal_signal(int signal, siginfo_t* info, void* context) {
  crash_hex("CRASH signal ", uint64_t(signal));
  if (info) crash_hex("fault address ", reinterpret_cast<uintptr_t>(info->si_addr));
  if (context) {
    // Measured scalar layout from this console's installed-title runs.
    const auto* machine = reinterpret_cast<const mcontext_t*>(static_cast<const uint8_t*>(context) + 0x40);
    crash_hex("instruction pointer ", uint64_t(machine->mc_rip));
  }
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(signal, &action, nullptr);
  raise(signal);
}

__attribute__((constructor(101))) void early_log() {
  crash_log = open("/app0/tt-game-crash.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  struct sigaction action{};
  action.sa_sigaction = fatal_signal;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGSYS}) sigaction(signal, &action, nullptr);
  tabletennis::ps5::log_fd = open("/app0/tt-game.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  // Libraries report on stderr, which a title otherwise loses (RADV's
  // per-10 s submission timing among them).
  if (tabletennis::ps5::log_fd >= 0) dup2(tabletennis::ps5::log_fd, STDERR_FILENO);
  Line("Table Tennis PS5: early constructor reached");
}

class Ps5AudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit Ps5AudioSystem(rex::runtime::FunctionDispatcher* dispatcher) : AudioSystem(dispatcher) {}
 protected:
  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                            rex::audio::AudioDriver** out) override {
    auto driver = std::make_unique<Ps5AudioDriver>(memory_, semaphore);
    if (!driver->Initialize()) return X_STATUS_UNSUCCESSFUL;
    *out = driver.release();
    return X_STATUS_SUCCESS;
  }
  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

int run_game() {
  Line("Table Tennis PS5: starting native game host");
  rex::perf::thread_cpu::RegisterCurrentThread();
  rex::perf::thread_cpu::SetThreadName(uintptr_t(pthread_self()), "host main");
  // A title launched from the home screen is sandboxed without /data; /app0 is
  // the title's own /data/homebrew/PPSA99782 and is always visible.
  std::filesystem::path root;
  for (const char* candidate : {"/app0/ttrecomp", "/data/ttrecomp"}) {
    if (std::filesystem::is_regular_file(std::filesystem::path(candidate) / "game/default.xex")) {
      root = candidate;
      break;
    }
  }
  if (root.empty()) {
    Line("FAIL: game/default.xex is missing from /app0/ttrecomp and /data/ttrecomp"); return 1;
  }
  Print("Game root: %s\n", root.c_str());
  std::filesystem::create_directories(root / "user");
  std::filesystem::create_directories(root / "cache");
  char name[] = "tabletennis";
  char* arguments[] = {name, nullptr};
  rex::cvar::Init(1, arguments);
  tabletennis::ApplyAppDefaults();
  rex::cvar::SetFlagByName("mnk_mode", "false");
  rex::cvar::SetFlagByName("hid_mappings_file", "");
  rex::cvar::SetFlagByName("menu_chord", "");
  // A PS5 mprotect costs ~33 us however many pages it covers, so trade some
  // extra uploading for far fewer write-watch faults and re-protections.
  // 256 KB measured best (docs/ps5_performance.md).
  rex::cvar::SetFlagByName("shared_memory_cpu_invalidation_widen_kb", "256");
  // Keeping the upload shadow means reading ~24 MB a frame back out of
  // write-combined upload memory; on the console that costs more than upload
  // hoisting saves (~3 ms per match frame, docs/ps5_performance.md).
  rex::cvar::SetFlagByName("shared_memory_upload_shadow_max_mb", "0");
  // CPU and GPU share memory on the console: the GPU reads guest memory
  // directly instead of copies kept in sync with write-watching. Match frames
  // went from ~20 ms to under 16.7 ms on the GPU thread. The two settings
  // above only apply if the import fails and copying takes over again.
  rex::cvar::SetFlagByName("vulkan_zero_copy_shared_memory", "true");
  // 2560x1440 rendering, FSR 1 to the 4K display mode: a locked 60 in rallies
  // (8.1 ms of GPU work per frame, docs/ps5_performance.md).
  rex::cvar::SetFlagByName("resolution_scale", "2");
  rex::cvar::SetFlagByName("draw_resolution_scale_x", "2");
  rex::cvar::SetFlagByName("draw_resolution_scale_y", "2");
  rex::cvar::SetFlagByName("present_effect", "fsr");
  // At 2x the fork's host-pixel half-pixel offset left the right and bottom
  // edge of every resolve uncovered: garbage strips and a cyan fringe under
  // blurs. The standard guest-pixel offset with edge fill renders them clean.
  rex::cvar::SetFlagByName("draw_resolution_scaled_half_pixel_offset", "false");
  rex::InitLoggingEarly();
  rex::LogConfig logging;
  logging.log_to_console = false;
  logging.extra_sinks.push_back(std::make_shared<tabletennis::ps5::FdSink>(tabletennis::ps5::log_fd));
  logging.flush_level = spdlog::level::info;
  rex::InitLogging(logging);
  // Optional cvar overrides (flat `name = value` TOML), editable over FTP so
  // diagnostics can be toggled without a rebuild.
  rex::cvar::LoadConfig(root / "ps5.toml");
  Line("NEXT: SDL offscreen context and native Vulkan presentation");
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
  if (!SDL_Init(SDL_INIT_VIDEO)) { Print("FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
  // Title lifetime belongs to the system shell, including failure paths.
  // Keep runtime/window/context alive until it terminates this process.
  auto* context = new rex::ui::SDLWindowedAppContext();
  auto graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  auto status = graphics->SetupPresentation(context);
  if (XFAILED(status)) { Print("FAIL: presentation setup: %08x\n", unsigned(status)); return 1; }
  auto* window = rex::ui::Window::Create(*context, "Table Tennis",
                                         uint32_t(REXCVAR_GET(ps5_display_width)),
                                         uint32_t(REXCVAR_GET(ps5_display_height)))
                     .release();
  if (!window || !window->Open()) { Line("FAIL: display window"); return 1; }
  window->SetPresenter(graphics->presenter());
  if (REXCVAR_GET(ps5_capture_start_s) > 0) {
    tabletennis::ps5::StartGuestOutputCapture(graphics->presenter(), "/app0/cap",
                                              REXCVAR_GET(ps5_capture_start_s),
                                              REXCVAR_GET(ps5_capture_count),
                                              REXCVAR_GET(ps5_capture_interval_ms));
  }
  auto* runtime = new rex::Runtime(root / "game", root / "user", {}, root / "cache");
  runtime->set_app_context(context);
  runtime->set_display_window(window);
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  config.kernel_init = rex::kernel::InitializeKernel;
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<rex::input::InputSystem>(nullptr);
    auto pad = std::make_unique<Ps5PadInputDriver>(REXCVAR_GET(ps5_pad_vibration_mode));
    if (XFAILED(pad->Setup())) return nullptr;
    input->AddDriver(std::move(pad));
    return input;
  };
  config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
      -> std::unique_ptr<rex::system::IAudioSystem> {
    return std::make_unique<Ps5AudioSystem>(dispatcher);
  };
  Line("NEXT: initialize game runtime and guest memory");
  status = runtime->Setup(tabletennis_PPCImageConfig, std::move(config));
  if (XFAILED(status)) { Print("FAIL: runtime setup: %08x\n", unsigned(status)); return 1; }
  if (tabletennis_PPCImageConfig.register_modules)
    tabletennis_PPCImageConfig.register_modules(runtime->kernel_state());
  if (!runtime->input_system()) { Line("FAIL: native controller setup"); return 1; }
  static_cast<rex::input::InputSystem*>(runtime->input_system())->AttachWindow(window);
#ifdef TABLETENNIS_PS5_NO_NATIVE_HOOKS
  Line("Native gameplay hooks disabled (perf A/B build)");
#else
  tabletennis::native::Install();
  // Passive unless tabletennis_test_path is set (ps5.toml): then the title
  // drives itself into an Exhibition match for unattended perf runs.
  tabletennis::test::InstallFrontendLaunchTest();
#endif
  Line("NEXT: load Table Tennis XEX");
  status = runtime->LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) { Print("FAIL: XEX load: %08x\n", unsigned(status)); return 1; }
  if (runtime->kernel_state()->title_id() != 0x545407df) { Line("FAIL: unexpected Xbox title ID"); return 1; }
  if (tabletennis_PPCImageConfig.rexcrt_heap &&
      !rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory())) {
    Line("FAIL: guest CRT heap"); return 1;
  }
  runtime->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  Line("NEXT: prepare guest main thread and shader storage");
  auto main = runtime->PrepareModuleLaunch();
  if (!main) { Line("FAIL: guest main thread creation"); return 1; }
  static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system())->InitializeShaderStorage(
      root / "cache", runtime->kernel_state()->title_id(), true);
  const int splash = sceSystemServiceHideSplashScreen();
  Print("Launch splash dismissal: 0x%x\n", unsigned(splash));
  Line("NEXT: run Table Tennis guest code");
  main->Resume();
  return context->RunMainLoop();
}
}  // namespace

extern "C" [[noreturn]] void catchReturnFromMain(int status) {
  Print("Table Tennis PS5: host returned %d; waiting for system close\n", status);
  rex::FlushLogging();
  for (;;) sceKernelUsleep(1000000);
}

int main() {
  try { return run_game(); }
  catch (const std::exception& error) {
    Print("FAIL: game host exception: %s\n", error.what());
    return 1;
  }
}
