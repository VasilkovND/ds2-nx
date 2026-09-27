#pragma once

#include "nfsmw_shader_library.h"
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace nfsmw::native {

// Owned by the render thread. The library must stay immutable and the device
// alive until this cache is destroyed. It does not wait for the GPU or create
// pipelines: those steps belong to the renderer and its synchronization.
class ModulosShaders {
 public:
  ModulosShaders(VkDevice dispositivo, PFN_vkCreateShaderModule crear,
                 PFN_vkDestroyShaderModule destruir);
  ~ModulosShaders();
  ModulosShaders(const ModulosShaders&) = delete;
  ModulosShaders& operator=(const ModulosShaders&) = delete;
  VkResult Obtener(const Shader& shader, VkShaderModule& modulo);

 private:
  VkDevice dispositivo_;
  PFN_vkCreateShaderModule crear_;
  PFN_vkDestroyShaderModule destruir_;
  std::unordered_map<const Shader*, VkShaderModule> modulos_;
};
}  // namespace nfsmw::native
