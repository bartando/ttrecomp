#pragma once

namespace tabletennis::native {

bool ObserverOverlayEnabled();
bool TableMeshObserverOverlayEnabled();
void RequestObserverOverlayForFrame(bool gameplay_active, bool camera_valid);
void InstallObserverOverlay();
void ShutdownObserverOverlay();

}  // namespace tabletennis::native
