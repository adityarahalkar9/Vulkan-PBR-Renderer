#include "render/uploadqueue.hpp"

#include "core/log.hpp"
#include "render/vulkancontext.hpp"
#include "render/vulkanutils.hpp"

#include <chrono>
#include <cmath>

namespace {

    struct StagingBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };

    StagingBuffer createStaging(VmaAllocator allocator, const void* data, size_t size) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo aci{};
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.usage = VMA_MEMORY_USAGE_AUTO;

        StagingBuffer s;
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(allocator, &ci, &aci, &s.buffer, &s.allocation, &info));
        std::memcpy(info.pMappedData, data, size); // HOST_VISIBLE | HOST_COHERENT via VMA_AUTO
        return s;
    }

    VkBuffer createDeviceBuffer(VmaAllocator allocator, VkBufferUsageFlags usage, size_t size,
                                VmaAllocation& allocation) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VkBuffer buffer = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(allocator, &ci, &aci, &buffer, &allocation, nullptr));
        return buffer;
    }

    uint32_t computeMipLevels(uint32_t width, uint32_t height) {
        uint32_t largest = std::max(width, height);
        return static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(largest)))) + 1;
    }

    VkSampler createSampler(VulkanContext& ctx, const SamplerParams& p, uint32_t mipLevels) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = p.mag;
        ci.minFilter = p.min;
        ci.mipmapMode = p.mip;
        ci.addressModeU = p.wrapS;
        ci.addressModeV = p.wrapT;
        ci.addressModeW = p.wrapS;
        ci.maxLod = static_cast<float>(mipLevels);
        if(ctx.anisotropy) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(ctx.props.limits.maxSamplerAnisotropy, 8.0f);
        }
        VkSampler sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(ctx.device, &ci, nullptr, &sampler));
        ctx.name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(sampler), "asset sampler");
        return sampler;
    }

} // namespace

void UploadQueue::init(VulkanContext& context) {
    ctx_ = &context;

    VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCi.queueFamilyIndex = ctx_->graphicsFamily;
    VK_CHECK(vkCreateCommandPool(ctx_->device, &poolCi, nullptr, &pool_));

    VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(ctx_->device, &fenceCi, nullptr, &fence_));
}

void UploadQueue::start() {
    running_ = true;
    worker_ = std::thread([this] { run(); });
    LOG_INFO("uploader thread started (queue family {})", ctx_->graphicsFamily);
}

void UploadQueue::shutdown() {
    if(!running_) return;
    running_ = false;
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    cvWork_.notify_all();
    if(worker_.joinable()) worker_.join();
    vkDestroyFence(ctx_->device, fence_, nullptr);
    vkDestroyCommandPool(ctx_->device, pool_, nullptr);
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    LOG_INFO("uploader stopped ({} uploads, {:.1f} MB total)", completed_.load(),
             bytesUploaded_.load() / (1024.0 * 1024.0));
}

void UploadQueue::enqueue(UploadTask&& task) {
    {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(task));
        pendingTasks_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

UploadResult UploadQueue::processSync(UploadTask&& task) {
    UploadResult result;
    result.kind = task.kind;
    result.id = task.kind == UploadTask::Kind::Mesh ? task.mesh.modelId : task.texture.textureId;
    result.request = task.request;
    doProcess(task, result);
    return result;
}

void UploadQueue::run() {
    for(;;) {
        std::unique_lock lock(mutex_);
        cvWork_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if(queue_.empty()) break; // stopping and fully drained
        UploadTask task = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();

        UploadResult result;
        result.kind = task.kind;
        result.id = task.kind == UploadTask::Kind::Mesh ? task.mesh.modelId : task.texture.textureId;
        result.request = task.request;

        pendingTasks_.fetch_sub(1, std::memory_order_relaxed);
        auto t0 = std::chrono::high_resolution_clock::now();
        doProcess(task, result);
        auto ms = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();

        completed_.fetch_add(1, std::memory_order_relaxed);
        if(result.ok)
            LOG_TRACE("uploaded '{}' in {:.2f} ms",
                      result.kind == UploadResult::Kind::Mesh ? "mesh" : "texture", ms);
        results_.push(std::move(result));
    }
}

void UploadQueue::doProcess(UploadTask& task, UploadResult& result) {
    std::vector<StagingBuffer> staging;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t mipLevels = 1;

    try {
        VK_CHECK(vkResetCommandPool(ctx_->device, pool_, 0));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx_->device, &ai, &cb));

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &begin));

        if(task.kind == UploadTask::Kind::Mesh) {
            const auto& mesh = task.mesh;
            size_t vertexBytes = mesh.vertices.size() * sizeof(Vertex);
            size_t indexBytes = mesh.indices.size() * sizeof(uint32_t);

            result.vertexBuffer = createDeviceBuffer(ctx_->allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                     vertexBytes, result.vertexAllocation);
            result.indexBuffer = createDeviceBuffer(ctx_->allocator, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                    indexBytes, result.indexAllocation);
            staging.push_back(createStaging(ctx_->allocator, mesh.vertices.data(), vertexBytes));
            staging.push_back(createStaging(ctx_->allocator, mesh.indices.data(), indexBytes));

            VkBufferCopy c0{0, 0, vertexBytes};
            vkCmdCopyBuffer(cb, staging[0].buffer, result.vertexBuffer, 1, &c0);
            VkBufferCopy c1{0, 0, indexBytes};
            vkCmdCopyBuffer(cb, staging[1].buffer, result.indexBuffer, 1, &c1);

            // Make vertex/index data visible to subsequent draws on this queue.
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cb, &di);

            bytesUploaded_.fetch_add(vertexBytes + indexBytes, std::memory_order_relaxed);
            ctx_->name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(result.vertexBuffer),
                       task.mesh.modelId == 0 ? "mesh vb" : "mesh vb");
        }
        else {
            const auto& tex = task.texture;
            mipLevels = computeMipLevels(tex.width, tex.height);

            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = tex.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent = {tex.width, tex.height, 1};
            ici.mipLevels = mipLevels;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo iaci{};
            iaci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VK_CHECK(vmaCreateImage(ctx_->allocator, &ici, &iaci, &image, &imageAllocation, nullptr));

            staging.push_back(createStaging(ctx_->allocator, tex.pixels.data(), tex.pixels.size()));

            imageBarrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, VK_REMAINING_MIP_LEVELS);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {tex.width, tex.height, 1};
            vkCmdCopyBufferToImage(cb, staging[0].buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            // Mipmap chain via blits between levels of the same image.
            imageBarrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);

            for(uint32_t i = 1; i < mipLevels; ++i) {
                int32_t sw = static_cast<int32_t>(tex.width >> (i - 1));
                int32_t sh = static_cast<int32_t>(tex.height >> (i - 1));
                int32_t dw = static_cast<int32_t>(tex.width >> i);
                int32_t dh = static_cast<int32_t>(tex.height >> i);
                sw = std::max(sw, 1); sh = std::max(sh, 1);
                dw = std::max(dw, 1); dh = std::max(dh, 1);

                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
                blit.srcOffsets[1] = {sw, sh, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
                blit.dstOffsets[1] = {dw, dh, 1};
                vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                imageBarrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, i, 1);
            }

            imageBarrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         0, VK_REMAINING_MIP_LEVELS);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = ici.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
            VK_CHECK(vkCreateImageView(ctx_->device, &vci, nullptr, &view));

            sampler = createSampler(*ctx_, tex.sampler, mipLevels);

            result.image = image;
            result.view = view;
            result.imageAllocation = imageAllocation;
            result.sampler = sampler;
            result.mipLevels = mipLevels;
            bytesUploaded_.fetch_add(tex.pixels.size(), std::memory_order_relaxed);
        }

        VK_CHECK(vkEndCommandBuffer(cb));

        {
            // The graphics queue is shared with the render thread.
            std::scoped_lock lock(ctx_->submitMutex);
            VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbsi.commandBuffer = cb;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.commandBufferInfoCount = 1;
            si.pCommandBufferSubmitInfos = &cbsi;
            VK_CHECK(vkResetFences(ctx_->device, 1, &fence_));
            VK_CHECK(vkQueueSubmit2(ctx_->graphicsQueue, 1, &si, fence_));
        }
        VK_CHECK(vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX));
        result.ok = true;
    }
    catch(const std::exception& e) {
        result.ok = false;
        result.error = e.what();
        LOG_ERROR("upload failed: {}", e.what());
        if(image) vmaDestroyImage(ctx_->allocator, image, imageAllocation);
        if(view) vkDestroyImageView(ctx_->device, view, nullptr);
        if(sampler) vkDestroySampler(ctx_->device, sampler, nullptr);
        result.image = VK_NULL_HANDLE;
        result.view = VK_NULL_HANDLE;
        result.imageAllocation = VK_NULL_HANDLE;
        result.sampler = VK_NULL_HANDLE;
    }

    for(auto& s : staging)
        vmaDestroyBuffer(ctx_->allocator, s.buffer, s.allocation);
}