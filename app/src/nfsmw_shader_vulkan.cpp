#include "nfsmw_shader_vulkan.h"
#include <stdexcept>

namespace nfsmw::native {
ModulosShaders::ModulosShaders(VkDevice dispositivo, PFN_vkCreateShaderModule crear,
                               PFN_vkDestroyShaderModule destruir)
    : dispositivo_(dispositivo), crear_(crear), destruir_(destruir) {
  if (!dispositivo || !crear || !destruir)
    throw std::invalid_argument("Dispositivo o funciones Vulkan ausentes");
}

ModulosShaders::~ModulosShaders() {
  for (auto [shader, modulo] : modulos_) destruir_(dispositivo_, modulo, nullptr);
}

VkResult ModulosShaders::Obtener(const Shader& shader, VkShaderModule& modulo) {
  modulo = VK_NULL_HANDLE;
  // Reserving before creating the resource avoids leaking it if the reservation fails.
  auto [it, nuevo] = modulos_.try_emplace(&shader, VK_NULL_HANDLE);
  if (nuevo) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = shader.spirv.size() * sizeof(uint32_t);
    info.pCode = shader.spirv.data();
    const VkResult resultado = crear_(dispositivo_, &info, nullptr, &it->second);
    if (resultado != VK_SUCCESS) {
      modulos_.erase(it);
      return resultado;
    }
  }
  modulo = it->second;
  return VK_SUCCESS;
}
}  // namespace nfsmw::native
