#pragma once
#include "render/vulkancontext.hpp"

#include <cstdint>

// GPU pass timings via timestamp queries. One query pool per frame slot,
// reset from the host (hostQueryReset, core 1.2) at frame begin; results are
// read two frames later, which the frame fence already guarantees.
class GpuProfiler {
public:
    enum class Stage { Shadow, Geometry, Ui, Count };

    void init(VulkanContext& ctx, uint32_t frameSlots);
    void shutdown();
    void beginFrame(uint32_t slot);                 // resolve + reset this slot's pool
    void mark(VkCommandBuffer cb, Stage stage, bool isEnd);

    float stageMs(int stage) const { return stageMs_[stage]; }
    float totalMs() const { return totalMs_; }

private:
    static constexpr uint32_t kQueriesPerFrame = 8;

    VulkanContext* ctx_ = nullptr;
    uint32_t slots_ = 0;
    uint32_t slot_ = 0;
    VkQueryPool pools_[8] = {};
    uint32_t writtenMask_[8] = {};
    float stageMs_[static_cast<int>(Stage::Count)] = {};
    float totalMs_ = 0.0f;
};