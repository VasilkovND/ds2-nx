#include "nfsmw_video_session.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace nfsmw::native {
namespace {
void Comprobar(VkResult r, const char* m) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(m)+": "+std::to_string(r));
}
VkDeviceSize Alinear(VkDeviceSize n) { return (n+4095)&~VkDeviceSize(4095); }
}
struct SesionVideo::Recursos {
  VkDevice device;
  PFN_vkGetDeviceProcAddr proc;
  const VkPhysicalDeviceMemoryProperties& memoria;
  uint32_t ancho, alto;
  const Shader* shaderVS; const Shader* shaderPS[2];
  std::unique_ptr<VideoVulkan> video;
  VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memoriaBuffer = VK_NULL_HANDLE;
  void* mapeado = nullptr; bool coherente = false;
  VkDeviceAddress direccion = 0;
  std::array<VkDeviceSize,3> offsets{};
  VkDeviceSize offsetConstantes = 0, offsetVertices = 0;
  std::array<VkImage,3> imagenes{};
  std::array<VkImageView,3> vistas{};
  std::array<VkDeviceMemory,3> memorias{};
  VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE; bool enviada = false, texturasUsadas = false;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  uint64_t version = 0; uint32_t anchoDestino = 0, altoDestino = 0;
  template<class T> T D(const char* n) const {
    auto f = reinterpret_cast<T>(proc(device,n));
    if (!f) throw std::runtime_error(std::string("Falta ")+n);
    return f;
  }
#define N(n) D<PFN_vk##n>("vk" #n)
  uint32_t Tipo(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i=0;i<memoria.memoryTypeCount;++i)
      if ((bits&(1u<<i)) && (memoria.memoryTypes[i].propertyFlags&flags)==flags) return i;
    return UINT32_MAX;
  }
  Recursos(VkDevice d, PFN_vkGetDeviceProcAddr p, const VkPhysicalDeviceMemoryProperties& m,
            uint32_t familia, ModulosShaders& modulos, const FotogramaVideo& f)
      : device(d),proc(p),memoria(m),ancho(f.ancho),alto(f.alto),shaderVS(f.vs),shaderPS{f.ps[0],f.ps[1]} {
    try {
      video = std::make_unique<VideoVulkan>(device,proc,modulos,*f.vs,*f.ps[0],*f.ps[1],VK_FORMAT_A2B10G10R10_UNORM_PACK32);
      VkDeviceSize bytes = 0;
      for (unsigned i=0;i<3;++i) { offsets[i]=bytes; bytes=Alinear(bytes+f.planos[i].size()); }
      offsetConstantes=bytes; offsetVertices=bytes+sizeof(ConstantesVideo);
      bytes=Alinear(offsetVertices+sizeof(f.vertices));
      VkBufferCreateInfo bc{}; bc.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bc.size=bytes;
      bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      Comprobar(N(CreateBuffer)(device,&bc,nullptr,&buffer),"Buffer de video");
      VkMemoryRequirements mr{}; N(GetBufferMemoryRequirements)(device,buffer,&mr);
      uint32_t tipo=Tipo(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      coherente=tipo!=UINT32_MAX;
      if (!coherente) tipo=Tipo(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      if (tipo==UINT32_MAX) throw std::runtime_error("Memoria visible de video no disponible");
      VkMemoryAllocateFlagsInfo flags{}; flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
      flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
      VkMemoryAllocateInfo ma{}; ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ma.pNext=&flags; ma.allocationSize=mr.size; ma.memoryTypeIndex=tipo;
      Comprobar(N(AllocateMemory)(device,&ma,nullptr,&memoriaBuffer),"Memoria de video");
      Comprobar(N(BindBufferMemory)(device,buffer,memoriaBuffer,0),"Enlace de video");
      Comprobar(N(MapMemory)(device,memoriaBuffer,0,VK_WHOLE_SIZE,0,&mapeado),"Mapeo de video");
      VkBufferDeviceAddressInfo bd{}; bd.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO; bd.buffer=buffer;
      direccion=N(GetBufferDeviceAddress)(device,&bd);
      for (unsigned i=0;i<3;++i) {
        VkImageCreateInfo ic{}; ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R8_UNORM;
        ic.extent={i?ancho/2:ancho,i?alto/2:alto,1}; ic.mipLevels=ic.arrayLayers=1;
        ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        Comprobar(N(CreateImage)(device,&ic,nullptr,&imagenes[i]),"Plano de video");
        N(GetImageMemoryRequirements)(device,imagenes[i],&mr);
        tipo=Tipo(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (tipo==UINT32_MAX) throw std::runtime_error("Memoria local de video no disponible");
        ma.pNext=nullptr; ma.memoryTypeIndex=tipo; ma.allocationSize=mr.size;
        Comprobar(N(AllocateMemory)(device,&ma,nullptr,&memorias[i]),"Memoria de plano");
        Comprobar(N(BindImageMemory)(device,imagenes[i],memorias[i],0),"Enlace de plano");
        VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image=imagenes[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8_UNORM;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        Comprobar(N(CreateImageView)(device,&vi,nullptr,&vistas[i]),"Vista de plano");
      }
      video->ConfigurarTexturas(vistas);
      VkCommandPoolCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex=familia;
      Comprobar(N(CreateCommandPool)(device,&cp,nullptr,&pool),"Pool de video");
      VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
      Comprobar(N(AllocateCommandBuffers)(device,&ca,&cmd),"Comandos de video");
      VkFenceCreateInfo fc{}; fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
      Comprobar(N(CreateFence)(device,&fc,nullptr,&fence),"Fence de video");
    } catch (...) { Liberar(); throw; }
  }
  ~Recursos() { Liberar(); }
  void Liberar() {
    if (enviada) N(WaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX);
    if (framebuffer) N(DestroyFramebuffer)(device,framebuffer,nullptr);
    if (fence) N(DestroyFence)(device,fence,nullptr);
    if (pool) N(DestroyCommandPool)(device,pool,nullptr);
    video.reset();
    for (unsigned i=0;i<3;++i) {
      if (vistas[i]) N(DestroyImageView)(device,vistas[i],nullptr);
      if (imagenes[i]) N(DestroyImage)(device,imagenes[i],nullptr);
      if (memorias[i]) N(FreeMemory)(device,memorias[i],nullptr);
    }
    if (mapeado) N(UnmapMemory)(device,memoriaBuffer);
    if (buffer) N(DestroyBuffer)(device,buffer,nullptr);
    if (memoriaBuffer) N(FreeMemory)(device,memoriaBuffer,nullptr);
  }
  bool Libre() {
    const VkResult r=N(GetFenceStatus)(device,fence);
    if (r==VK_NOT_READY) return false;
    Comprobar(r,"Estado de fence de video"); enviada=false; return true;
  }
  void Barrera(VkImage i,VkImageLayout antes,VkImageLayout despues,VkPipelineStageFlags origen,VkPipelineStageFlags destino,VkAccessFlags a,VkAccessFlags b) {
    VkImageMemoryBarrier m{}; m.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    m.srcAccessMask=a; m.dstAccessMask=b; m.oldLayout=antes; m.newLayout=despues;
    m.srcQueueFamilyIndex=m.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    m.image=i; m.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    N(CmdPipelineBarrier)(cmd,origen,destino,0,0,nullptr,0,nullptr,1,&m);
  }
  void Grabar(const FotogramaVideo& f,VkImage imagen,VkImageView vista,uint64_t nuevaVersion,bool escrita,uint32_t w,uint32_t h) {
    if (!framebuffer || version!=nuevaVersion || anchoDestino!=w || altoDestino!=h) {
      if (framebuffer) N(DestroyFramebuffer)(device,framebuffer,nullptr);
      framebuffer=VK_NULL_HANDLE;
      VkFramebufferCreateInfo fb{}; fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb.renderPass=video->render_pass(); fb.attachmentCount=1; fb.pAttachments=&vista; fb.width=w; fb.height=h; fb.layers=1;
      Comprobar(N(CreateFramebuffer)(device,&fb,nullptr,&framebuffer),"Framebuffer de presentacion nativa");
      version=nuevaVersion; anchoDestino=w; altoDestino=h;
    }
    for (unsigned i=0;i<3;++i) std::memcpy(static_cast<uint8_t*>(mapeado)+offsets[i],f.planos[i].data(),f.planos[i].size());
    ConstantesVideo c;
    // XenosRecomp convention also used by Marathon: D3D half-pixel offset.
    c.medioPixel[0]=1.f/w; c.medioPixel[1]=-1.f/h;
    std::memcpy(static_cast<uint8_t*>(mapeado)+offsetConstantes,&c,sizeof(c));
    std::memcpy(static_cast<uint8_t*>(mapeado)+offsetVertices,f.vertices.data(),sizeof(f.vertices));
    if (!coherente) {
      VkMappedMemoryRange rango{}; rango.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      rango.memory=memoriaBuffer; rango.size=VK_WHOLE_SIZE;
      Comprobar(N(FlushMappedMemoryRanges)(device,1,&rango),"Publicacion de planos en memoria no coherente");
    }
    Comprobar(N(ResetCommandBuffer)(cmd,0),"Reinicio de comandos nativos");
    VkCommandBufferBeginInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Comprobar(N(BeginCommandBuffer)(cmd,&ci),"Inicio de video nativo");
    for (unsigned i=0;i<3;++i) {
      Barrera(imagenes[i],texturasUsadas?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          texturasUsadas?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
          texturasUsadas?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy copia{}; copia.bufferOffset=offsets[i]; copia.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copia.imageExtent={i?ancho/2:ancho,i?alto/2:alto,1};
      N(CmdCopyBufferToImage)(cmd,buffer,imagenes[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copia);
      Barrera(imagenes[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    Barrera(imagen,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        escrita?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        escrita?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    video->Dibujar(cmd,framebuffer,w,h,buffer,offsetVertices,direccion+offsetConstantes,f.variante,true);
    Barrera(imagen,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    Comprobar(N(EndCommandBuffer)(cmd),"Fin de video nativo");
  }
  void Enviar(VkQueue cola) {
    Comprobar(N(ResetFences)(device,1,&fence),"Reinicio de fence nativa");
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    Comprobar(N(QueueSubmit)(cola,1,&si,fence),"Envio de video nativo");
    enviada=texturasUsadas=true;
  }
#undef N
};
SesionVideo::SesionVideo(VkDevice d,PFN_vkGetDeviceProcAddr p,const VkPhysicalDeviceMemoryProperties& m,uint32_t familia)
    :device_(d),proc_(p),memoria_(m),familia_(familia) {
  modulos_=std::make_unique<ModulosShaders>(d,reinterpret_cast<PFN_vkCreateShaderModule>(p(d,"vkCreateShaderModule")),
      reinterpret_cast<PFN_vkDestroyShaderModule>(p(d,"vkDestroyShaderModule")));
}
SesionVideo::~SesionVideo() = default;
bool SesionVideo::Preparar(const FotogramaVideo& f,VkImage i,VkImageView v,uint64_t version,bool escrita,uint32_t w,uint32_t h) {
  preparado_=nullptr;
  if (!f.vs || !f.ps[0] || !f.ps[1] || !f.ancho || !f.alto || ((f.ancho|f.alto)&1) || f.variante>1 || !w || !h) return false;
  for (unsigned n=0;n<3;++n) if (f.planos[n].size()!=size_t(n?f.ancho/2:f.ancho)*(n?f.alto/2:f.alto)) return false;
  for (unsigned n=0;n<3;++n) {
    auto& r=recursos_[(siguiente_+n)%3];
    if (r && !r->Libre()) continue;
    if (r && (r->ancho!=f.ancho || r->alto!=f.alto || r->shaderVS!=f.vs || r->shaderPS[0]!=f.ps[0] || r->shaderPS[1]!=f.ps[1])) r.reset();
    if (!r) r=std::make_unique<Recursos>(device_,proc_,memoria_,familia_,*modulos_,f);
    r->Grabar(f,i,v,version,escrita,w,h);
    preparado_=r.get(); siguiente_=(siguiente_+n+1)%3; return true;
  }
  return false;
}
void SesionVideo::Enviar(VkQueue cola) {
  if (!preparado_) throw std::runtime_error("Video no preparado");
  preparado_->Enviar(cola); preparado_=nullptr;
}
}
