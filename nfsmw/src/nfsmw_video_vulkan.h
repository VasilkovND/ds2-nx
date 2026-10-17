#pragma once
#include "nfsmw_shader_vulkan.h"
#include <array>

namespace nfsmw::native {
struct VerticeVideo { float x, y, z, u, v; };
static_assert(sizeof(VerticeVideo) == 20);

// Layout shared with shader_common.h and the original SPIR-V.
struct alignas(16) ConstantesVideo {
  uint32_t texturas2D[16]{0, 1, 2};
  uint32_t texturas3D[16]{};
  uint32_t texturasCube[16]{};
  uint32_t samplers[16]{};
  uint32_t booleanos = 0;
  uint32_t intercambiarUV = 0;
  float medioPixel[2]{};
  float umbralAlfa = 0;
  uint32_t relleno[3]{};
};
static_assert(offsetof(ConstantesVideo, booleanos) == 256);
static_assert(offsetof(ConstantesVideo, intercambiarUV) == 260);
static_assert(offsetof(ConstantesVideo, medioPixel) == 264);
static_assert(sizeof(ConstantesVideo) == 288);

#define NFSMW_FUNCIONES_VIDEO(X) \
  X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) \
  X(CreateRenderPass) X(DestroyRenderPass) \
  X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(CreateDescriptorPool) X(DestroyDescriptorPool) \
  X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
  X(CreateSampler) X(DestroySampler) \
  X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) \
  X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) \
  X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdDraw) X(CmdClearAttachments)

// Video draw path; it creates no device, queues or waits. The caller enables
// shaderInt64, shaderSampledImageArrayDynamicIndexing, bufferDeviceAddress,
// runtimeDescriptorArray and scalarBlockLayout when creating the VkDevice.
// It must check the features actually enabled before using this path and
// keep textures, constants, vertices and framebuffer alive until their fence.
// Use from a single thread. ConfigurarTexturas and destruction require every
// earlier submission that uses this object to have finished.
class VideoVulkan {
 public:
  VideoVulkan(VkDevice dispositivo, PFN_vkGetDeviceProcAddr proc,
              ModulosShaders& modulos, const Shader& vs,
              const Shader& ps0, const Shader& ps1, VkFormat destino);
  ~VideoVulkan();
  VideoVulkan(const VideoVulkan&) = delete;
  VideoVulkan& operator=(const VideoVulkan&) = delete;
  VkRenderPass render_pass() const { return renderPass_; }
  void ConfigurarTexturas(const std::array<VkImageView, 3>& planos);
  // The target image arrives in COLOR_ATTACHMENT_OPTIMAL. Contents outside the
  // triangles are preserved. The textures arrive in SHADER_READ_ONLY_OPTIMAL.
  void Dibujar(VkCommandBuffer cmd, VkFramebuffer destino, uint32_t ancho, uint32_t alto,
               VkBuffer vertices, VkDeviceSize offset, VkDeviceAddress constantes,
               uint32_t variante, bool fondoNegro = false);

 private:
  void Liberar();
  VkDevice dispositivo_;
#define NFSMW_DECLARAR_VIDEO(n) PFN_vk##n n = nullptr;
  NFSMW_FUNCIONES_VIDEO(NFSMW_DECLARAR_VIDEO)
#undef NFSMW_DECLARAR_VIDEO
  std::array<VkDescriptorSetLayout, 4> layouts_{};
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 2> pipelines_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  bool texturasListas_ = false;
};
}  // namespace nfsmw::native
