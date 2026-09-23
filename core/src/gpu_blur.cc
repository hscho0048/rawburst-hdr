// Vulkan compute 디스크 블러 호스트 코드. Android에서만 실제 구현 (libvulkan은 NDK 기본 제공).
// Adreno는 통합 메모리 → host-visible 버퍼를 매핑해 쓴다 (스테이징 복사 없음). 입력은 coherent,
// 출력은 CPU가 읽으므로 가능하면 HOST_CACHED(+ invalidate). 커널 시간은 타임스탬프 쿼리.
#include "burstpipe/gpu_blur.h"

#if defined(BP_ANDROID)
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstring>
#include <vector>
#include "shaders/disc_blur.spv.h"

namespace bp {
namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

class VkBlur : public GpuBlur {
 public:
  ~VkBlur() override {
    if (dev_) {
      vkDeviceWaitIdle(dev_);
      if (qp_) vkDestroyQueryPool(dev_, qp_, nullptr);
      if (fence_) vkDestroyFence(dev_, fence_, nullptr);
      if (cp_) vkDestroyCommandPool(dev_, cp_, nullptr);
      if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
      if (pipe_) vkDestroyPipeline(dev_, pipe_, nullptr);
      if (pl_) vkDestroyPipelineLayout(dev_, pl_, nullptr);
      if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
      if (sm_) vkDestroyShaderModule(dev_, sm_, nullptr);
      for (int i = 0; i < 3; ++i) {
        if (map_[i]) vkUnmapMemory(dev_, mem_[i]);
        if (buf_[i]) vkDestroyBuffer(dev_, buf_[i], nullptr);
        if (mem_[i]) vkFreeMemory(dev_, mem_[i], nullptr);
      }
      vkDestroyDevice(dev_, nullptr);
    }
    if (inst_) vkDestroyInstance(inst_, nullptr);
  }

  bool init(int qw, int qh, std::string* why) {
    qw_ = qw; qh_ = qh;
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "burstpipe"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &inst_) != VK_SUCCESS) return fail("vkCreateInstance");
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    if (!n) return fail("no vulkan device");
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst_, &n, pds.data());
    pd_ = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd_, &props);
    name_ = props.deviceName;
    ts_period_ns_ = props.limits.timestampPeriod;
    if (props.limits.maxComputeSharedMemorySize < (16 + 2 * kMaxRadius) * (16 + 2 * kMaxRadius) * 16u) return fail("shared memory too small");
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &nq, qfs.data());
    qf_ = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
      if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf_ = i; ts_ok_ = qfs[i].timestampValidBits > 0; break; }
    if (qf_ == UINT32_MAX) return fail("no compute queue");
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf_; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(pd_, &dci, nullptr, &dev_) != VK_SUCCESS) return fail("vkCreateDevice");
    vkGetDeviceQueue(dev_, qf_, 0, &q_);

    const VkDeviceSize sz[3] = {(VkDeviceSize)qw * qh * 3 * 4, (VkDeviceSize)qw * qh * 4, (VkDeviceSize)qw * qh * 3 * 4};
    for (int i = 0; i < 3; ++i)
      if (!make_buffer(sz[i], i == 2, i)) return fail("buffer alloc");

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(k_disc_blur_spv); smi.pCode = k_disc_blur_spv;
    if (vkCreateShaderModule(dev_, &smi, nullptr, &sm_) != VK_SUCCESS) return fail("shader module");
    VkDescriptorSetLayoutBinding b[3]{};
    for (int i = 0; i < 3; ++i) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 3; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &dsl_) != VK_SUCCESS) return fail("dsl");
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl_; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev_, &pli, nullptr, &pl_) != VK_SUCCESS) return fail("pipeline layout");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm_; cpi.stage.pName = "main";
    cpi.layout = pl_;
    if (vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe_) != VK_SUCCESS) return fail("compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_) != VK_SUCCESS) return fail("descriptor pool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool_; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl_;
    if (vkAllocateDescriptorSets(dev_, &dai, &ds_) != VK_SUCCESS) return fail("descriptor set");
    VkDescriptorBufferInfo bi[3];
    VkWriteDescriptorSet wr[3]{};
    for (int i = 0; i < 3; ++i) {
      bi[i] = {buf_[i], 0, VK_WHOLE_SIZE};
      wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds_; wr[i].dstBinding = i;
      wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(dev_, 3, wr, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex = qf_;
    if (vkCreateCommandPool(dev_, &cpci, nullptr, &cp_) != VK_SUCCESS) return fail("command pool");
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cp_; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_, &cbai, &cb_) != VK_SUCCESS) return fail("command buffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev_, &fci, nullptr, &fence_) != VK_SUCCESS) return fail("fence");
    if (ts_ok_) {
      VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 2;
      if (vkCreateQueryPool(dev_, &qpi, nullptr, &qp_) != VK_SUCCESS) ts_ok_ = false;
    }
    return true;
  }

  bool run(const Image<float>& rgb, const Image<float>& alpha, int radius, Image<float>& out, GpuBlurTimes* t) override {
    if (radius > kMaxRadius || rgb.w != qw_ * 3 || rgb.h != qh_) return false;
    auto t0 = std::chrono::steady_clock::now();
    for (int y = 0; y < qh_; ++y) {  // 업로드 = 매핑된 메모리로 memcpy (통합 메모리)
      std::memcpy((float*)map_[0] + (size_t)y * qw_ * 3, rgb.row(y), (size_t)qw_ * 3 * 4);
      std::memcpy((float*)map_[1] + (size_t)y * qw_, alpha.row(y), (size_t)qw_ * 4);
    }
    if (!coherent_[0] || !coherent_[1]) {
      VkMappedMemoryRange r[2] = {{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem_[0], 0, VK_WHOLE_SIZE},
                                  {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem_[1], 0, VK_WHOLE_SIZE}};
      vkFlushMappedMemoryRanges(dev_, 2, r);
    }
    const double up = ms_since(t0);

    auto t1 = std::chrono::steady_clock::now();
    vkResetCommandBuffer(cb_, 0);
    VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb_, &bgi);
    if (ts_ok_) { vkCmdResetQueryPool(cb_, qp_, 0, 2); vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp_, 0); }
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, pl_, 0, 1, &ds_, 0, nullptr);
    const int32_t pcv[3] = {qw_, qh_, radius};
    vkCmdPushConstants(cb_, pl_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb_, (qw_ + 15) / 16, (qh_ + 15) / 16, 1);
    if (ts_ok_) vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp_, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cb_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb_;
    vkResetFences(dev_, 1, &fence_);
    if (vkQueueSubmit(q_, 1, &si, fence_) != VK_SUCCESS) return false;
    if (vkWaitForFences(dev_, 1, &fence_, VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
    const double sw = ms_since(t1);
    double kern = -1;
    if (ts_ok_) {
      uint64_t ts[2];
      if (vkGetQueryPoolResults(dev_, qp_, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        kern = (double)(ts[1] - ts[0]) * ts_period_ns_ * 1e-6;
    }

    auto t2 = std::chrono::steady_clock::now();
    if (!coherent_[2]) {
      VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem_[2], 0, VK_WHOLE_SIZE};
      vkInvalidateMappedMemoryRanges(dev_, 1, &r);
    }
    for (int y = 0; y < qh_; ++y) std::memcpy(out.row(y), (const float*)map_[2] + (size_t)y * qw_ * 3, (size_t)qw_ * 3 * 4);
    const double down = ms_since(t2);
    if (t) { t->upload_ms = up; t->submit_wait_ms = sw; t->kernel_ms = kern; t->download_ms = down; }
    return true;
  }

  std::string device_name() const override { return name_; }

 private:
  bool make_buffer(VkDeviceSize size, bool readback, int i) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bci, nullptr, &buf_[i]) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, buf_[i], &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd_, &mp);
    // 읽어 오는 버퍼는 HOST_CACHED 우선 (uncached 읽기는 느리다), 쓰는 버퍼는 COHERENT 우선
    const VkMemoryPropertyFlags want[2][2] = {
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT}};
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags f : want[readback ? 1 : 0]) {
      for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & f) == f) type = t;
      if (type != UINT32_MAX) break;
    }
    if (type == UINT32_MAX) return false;
    coherent_[i] = (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev_, &mai, nullptr, &mem_[i]) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, buf_[i], mem_[i], 0);
    return vkMapMemory(dev_, mem_[i], 0, VK_WHOLE_SIZE, 0, &map_[i]) == VK_SUCCESS;
  }

  int qw_ = 0, qh_ = 0;
  std::string name_;
  float ts_period_ns_ = 1.f;
  bool ts_ok_ = false;
  VkInstance inst_ = VK_NULL_HANDLE;
  VkPhysicalDevice pd_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue q_ = VK_NULL_HANDLE;
  uint32_t qf_ = 0;
  VkBuffer buf_[3] = {};
  VkDeviceMemory mem_[3] = {};
  void* map_[3] = {};
  bool coherent_[3] = {true, true, true};
  VkShaderModule sm_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
  VkPipelineLayout pl_ = VK_NULL_HANDLE;
  VkPipeline pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;
  VkDescriptorSet ds_ = VK_NULL_HANDLE;
  VkCommandPool cp_ = VK_NULL_HANDLE;
  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkQueryPool qp_ = VK_NULL_HANDLE;
};

}  // namespace

std::unique_ptr<GpuBlur> GpuBlur::create(int qw, int qh, std::string* why) {
  auto g = std::make_unique<VkBlur>();
  if (!g->init(qw, qh, why)) return nullptr;
  return g;
}

}  // namespace bp

#else  // PC: Vulkan 경로 없음

namespace bp {
std::unique_ptr<GpuBlur> GpuBlur::create(int, int, std::string* why) {
  if (why) *why = "not built for Android";
  return nullptr;
}
}  // namespace bp

#endif
