// Vulkan GPU frame trace viewer for Table Tennis.

#include <memory>

#include <rex/graphics/trace_viewer.h>
#include <rex/graphics/vulkan/graphics_system.h>

namespace {

class TabletennisTraceViewer final : public rex::graphics::TraceViewer {
 public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<TabletennisTraceViewer>(
        new TabletennisTraceViewer(app_context));
  }

 protected:
  std::unique_ptr<rex::graphics::GraphicsSystem> CreateGraphicsSystem() override {
    return std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  }

  uintptr_t GetColorRenderTarget(
      uint32_t pitch, rex::graphics::xenos::MsaaSamples samples, uint32_t base,
      rex::graphics::xenos::ColorRenderTargetFormat format) override {
    (void)pitch;
    (void)samples;
    (void)base;
    (void)format;
    return 0;
  }

  uintptr_t GetDepthRenderTarget(
      uint32_t pitch, rex::graphics::xenos::MsaaSamples samples, uint32_t base,
      rex::graphics::xenos::DepthRenderTargetFormat format) override {
    (void)pitch;
    (void)samples;
    (void)base;
    (void)format;
    return 0;
  }

  uintptr_t GetTextureEntry(
      const rex::graphics::TextureInfo& texture_info,
      const rex::graphics::SamplerInfo& sampler_info) override {
    (void)texture_info;
    (void)sampler_info;
    return 0;
  }

 private:
  explicit TabletennisTraceViewer(rex::ui::WindowedAppContext& app_context)
      : TraceViewer(app_context, "tabletennis-gpu-trace-viewer") {}
};

}  // namespace

REX_DEFINE_APP(tabletennis_trace_viewer, TabletennisTraceViewer::Create)
