#pragma once

#include <functional>

namespace tabletennis::test {

// Enables the compile-time frontend Update hook. The hook itself remains
// passive unless tabletennis_test_path is enabled.
void InstallFrontendLaunchTest();
void ShutdownFrontendLaunchTest();

// Installed by the host app so the title-side gameplay marker can request a
// trace without relying on a synthetic macOS function-key event.
void SetGameplayTraceRequester(std::function<void()> requester);

// Stops the loading-screen Continue automation as soon as the title's render
// path proves that both match players are live.
void NotifyGameplayReached();

}  // namespace tabletennis::test
