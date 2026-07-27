#include "native/tabletennis_guest_memory.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>

#include <csetjmp>
#include <mutex>

#include <rex/exception_handler.h>
#endif

namespace tabletennis::native {

#if defined(_WIN32)
namespace {

using GuestMemcpy = void* (*)(void*, const void*, size_t);
volatile GuestMemcpy g_guest_memcpy = std::memcpy;

}  // namespace

__declspec(noinline) bool GuestTryCopy(void* destination, const void* source,
                                       size_t size) {
  __try {
    g_guest_memcpy(destination, source, size);
    return true;
  } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
               GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}
#else
namespace {

thread_local volatile bool g_copy_active = false;
thread_local sigjmp_buf g_copy_recovery;

bool GuestCopyFaultHandler(rex::arch::Exception* exception, void*) {
  if (!g_copy_active ||
      exception->code() !=
          rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  g_copy_active = false;
  siglongjmp(g_copy_recovery, 1);
}

void EnsureGuestCopyHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::arch::ExceptionHandler::Install(GuestCopyFaultHandler, nullptr);
  });
}

}  // namespace

bool GuestTryCopy(void* destination, const void* source, size_t size) {
  EnsureGuestCopyHandler();
  if (sigsetjmp(g_copy_recovery, 0) != 0) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGSEGV);
    sigaddset(&signals, SIGBUS);
    pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
    return false;
  }
  g_copy_active = true;
  std::memcpy(destination, source, size);
  g_copy_active = false;
  return true;
}
#endif

}  // namespace tabletennis::native
