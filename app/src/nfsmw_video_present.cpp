#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/instance.h>
// The SDK defines the beta extensions and the Horizon surface first.
#include "nfsmw_video_session.h"

namespace {
std::shared_ptr<const nfsmw::native::FotogramaVideo> g_fotograma;
std::unique_ptr<nfsmw::native::SesionVideo> g_sesion;
uint64_t g_presentados=0;
}

// Only the GPU command thread calls these. The guest capture arrives through
// a marker queue, and the resources are never destroyed from the UI thread.
extern "C" bool RexNativeVideoBeginSwap() {
  g_fotograma=nfsmw::native::ConsumirVideo();
  return bool(g_fotograma);
}
extern "C" bool RexNativeVideoPresent(const rex::ui::vulkan::VulkanDevice* dispositivo,
                                      rex::ui::Presenter* presentador,uint32_t ancho,uint32_t alto) {
  if (!g_fotograma) return false;
  auto f=std::move(g_fotograma);
  try {
    const auto& p=dispositivo->properties();
    if (!p.shaderInt64 || !p.shaderSampledImageArrayDynamicIndexing || !p.bufferDeviceAddress ||
        !p.runtimeDescriptorArray || !p.scalarBlockLayout) {
      nfsmw::native::DesactivarVideo("faltan capacidades Vulkan habilitadas"); return false;
    }
    if (!ancho || !alto || ancho>1920 || alto>1080) return false;
    if (!g_sesion) {
      VkPhysicalDeviceMemoryProperties memoria{};
      const auto& ifn=dispositivo->vulkan_instance()->functions();
      ifn.vkGetPhysicalDeviceMemoryProperties(dispositivo->physical_device(),&memoria);
      g_sesion=std::make_unique<nfsmw::native::SesionVideo>(dispositivo->device(),ifn.vkGetDeviceProcAddr,
          memoria,dispositivo->queue_family_graphics_compute());
    }
    const bool ok=presentador->RefreshGuestOutput(ancho,alto,ancho,alto,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& base) {
          auto& c=static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
          if (!g_sesion->Preparar(*f,c.image(),c.image_view(),c.image_version(),c.image_ever_written_previously(),ancho,alto)) return false;
          auto cola=dispositivo->AcquireQueue(dispositivo->queue_family_graphics_compute(),0);
          g_sesion->Enviar(cola.queue());
          c.SetIs8bpc(true);
          return true;
        });
    if (ok) {
      ++g_presentados;
      if (g_presentados==1 || g_presentados%300==0)
        REXLOG_INFO("[video nativo] presentado {}: YUV {}x{}, salida {}x{}, variante {}",g_presentados,f->ancho,f->alto,ancho,alto,f->variante);
    }
    return ok;
  } catch (const std::exception& e) {
    nfsmw::native::DesactivarVideo(e.what()); return false;
  }
}
extern "C" void RexNativeVideoShutdown() {
  nfsmw::native::DesactivarVideo("cierre del dispositivo");
  g_fotograma.reset(); g_sesion.reset();
}
