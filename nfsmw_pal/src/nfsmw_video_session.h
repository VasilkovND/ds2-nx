#pragma once
#include "nfsmw_video_bridge.h"

namespace nfsmw::native {
// Three resource sets, without waiting to reuse a busy one. The graphics
// thread decides whether to present natively or keep the Xenos frame.
class SesionVideo {
 public:
  SesionVideo(VkDevice device, PFN_vkGetDeviceProcAddr proc,
               const VkPhysicalDeviceMemoryProperties& memoria, uint32_t familia);
  ~SesionVideo();
  SesionVideo(const SesionVideo&) = delete;
  SesionVideo& operator=(const SesionVideo&) = delete;
  bool Preparar(const FotogramaVideo& f, VkImage imagen, VkImageView vista, uint64_t version,
                bool escrita, uint32_t ancho, uint32_t alto);
  void Enviar(VkQueue cola);
 private:
  struct Recursos;
  VkDevice device_;
  PFN_vkGetDeviceProcAddr proc_;
  VkPhysicalDeviceMemoryProperties memoria_;
  uint32_t familia_;
  std::unique_ptr<ModulosShaders> modulos_;
  std::array<std::unique_ptr<Recursos>, 3> recursos_;
  Recursos* preparado_ = nullptr;
  unsigned siguiente_ = 0;
};
}
