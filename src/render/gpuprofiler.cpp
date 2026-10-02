#include "render/gpuprofiler.hpp"
#include "core/log.hpp"
#include "render/vulkanutils.hpp"

void GpuProfiler::init(VulkanContext& ctx, uint32_t frameSlots) {
    ctx_ = &ctx;
    slots_ = std::min<uint32_t>(frameSlots, 8);

    if(!ctx.props.limits.timestampComputeAndGraphics)
        LOG_WARN("timestamp queries not supported - GPU timings will read zero");

    for(uint32_t i = 0; i < slots_; ++i) {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kQueriesPerFrame;
        VK_CHECK(vkCreateQueryPool(ctx.device, &ci, nullptr, &pools_[i]));
    }
}

void GpuProfiler::shutdown() {
    if(!ctx_) return;
    for(uint32_t i = 0; i < slots_; ++i)
        if(pools_[i]) { vkDestroyQueryPool(ctx_->device, pools_[i], nullptr); pools_[i] = VK_NULL_HANDLE; }
}

void GpuProfiler::beginFrame(uint32_t slot) {
    slot_ = slot % slots_;
    uint32_t mask = writtenMask_[slot_];
    writtenMask_[slot_] = 0;

    if(mask != 0) {
        uint64_t data[kQueriesPerFrame] = {};
        VkResult r = vkGetQueryPoolResults(ctx_->device, pools_[slot_], 0, kQueriesPerFrame,
                                           sizeof(data), data, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if(r == VK_SUCCESS) {
            const float toMs = ctx_->props.timestampPeriod / 1.0e6f;
            for(int s = 0; s < static_cast<int>(Stage::Count); ++s) {
                uint32_t bit = 1u << s;
                stageMs_[s] = (mask & bit) ? static_cast<float>(data[s * 2 + 1] - data[s * 2]) * toMs : 0.0f;
            }
            int first = 0;
            while(first < static_cast<int>(Stage::Count) && !(mask & (1u << first))) ++first;
            int last = static_cast<int>(Stage::Count) - 1;
            while(last >= 0 && !(mask & (1u << last))) --last;
            if(first <= last)
                totalMs_ = static_cast<float>(data[last * 2 + 1] - data[first * 2]) * toMs;
        }
    }
    vkResetQueryPool(ctx_->device, pools_[slot_], 0, kQueriesPerFrame); // host query reset
}

void GpuProfiler::mark(VkCommandBuffer cb, Stage stage, bool isEnd) {
    uint32_t index = slot_ * kQueriesPerFrame + static_cast<uint32_t>(stage) * 2 + (isEnd ? 1 : 0);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pools_[slot_], index);
    writtenMask_[slot_] |= 1u << static_cast<uint32_t>(stage);
}