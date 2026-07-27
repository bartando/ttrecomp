#pragma once

#include <cstddef>
#include <span>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

struct DrawReplacementRoute {
  rex::graphics::NativeGuestDrawMatcher matcher = nullptr;
  rex::graphics::NativeGuestDrawRenderer renderer = nullptr;
  void *user_data = nullptr;
};

// Routes one matched guest draw to the same replacement renderer. Routes are
// checked in declaration order, making overlap resolution explicit and
// deterministic as more trace-verified families are added.
class DrawReplacerDispatcher {
public:
  explicit DrawReplacerDispatcher(std::span<const DrawReplacementRoute> routes);

  bool Match(const rex::graphics::NativeGuestDrawContext &context);
  bool Render(const rex::graphics::NativeGuestDrawContext &context);
  void Reset();

  bool has_pending_route() const;

private:
  static constexpr size_t kNoRoute = static_cast<size_t>(-1);

  std::span<const DrawReplacementRoute> routes_;
  size_t pending_route_ = kNoRoute;
};

// SDK callback pair backed by the title's ordered route table.
bool MatchDispatchedDrawReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);
bool RenderDispatchedDrawReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);
void ResetDrawReplacerDispatcher();

} // namespace tabletennis::native
