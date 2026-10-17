#include "nfsmw_video_vulkan.h"
#include <stdexcept>
#include <string>

namespace nfsmw::native {
namespace {
void Comprobar(VkResult r, const char* paso) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(paso) + ": " + std::to_string(r));
}
}
VideoVulkan::VideoVulkan(VkDevice dispositivo, PFN_vkGetDeviceProcAddr proc,
                         ModulosShaders& modulos, const Shader& vs,
                         const Shader& ps0, const Shader& ps1, VkFormat destino)
    : dispositivo_(dispositivo) {
  if (!dispositivo || !proc || !vs.vertices || ps0.vertices || ps1.vertices)
    throw std::invalid_argument("Dispositivo o etapas incorrectos para video");
#define NFSMW_CARGAR_VIDEO(n) \
  n = reinterpret_cast<PFN_vk##n>(proc(dispositivo, "vk" #n)); \
  if (!n) throw std::runtime_error("Falta vk" #n);
  NFSMW_FUNCIONES_VIDEO(NFSMW_CARGAR_VIDEO)
#undef NFSMW_CARGAR_VIDEO
  try {
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      binding.descriptorCount = i == 3 ? 1 : 3;
      binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.bindingCount = i == 0 || i == 3 ? 1 : 0;
      info.pBindings = &binding;
      Comprobar(CreateDescriptorSetLayout(dispositivo, &info, nullptr, &layouts_[i]), "Layout de descriptores de video");
    }
    const VkPushConstantRange rango{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 4; pl.pSetLayouts = layouts_.data();
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &rango;
    Comprobar(CreatePipelineLayout(dispositivo, &pl, nullptr, &pipelineLayout_), "Layout de pipeline de video");
    VkAttachmentDescription color{};
    color.format = destino; color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference referencia{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &referencia;
    VkRenderPassCreateInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &color; rp.subpassCount = 1; rp.pSubpasses = &subpass;
    Comprobar(CreateRenderPass(dispositivo, &rp, nullptr, &renderPass_), "Render pass de video");
    VkShaderModule moduloVS, moduloPS[2];
    Comprobar(modulos.Obtener(vs, moduloVS), "VS de video");
    Comprobar(modulos.Obtener(ps0, moduloPS[0]), "PS de video 0");
    Comprobar(modulos.Obtener(ps1, moduloPS[1]), "PS de video 1");
    const VkVertexInputBindingDescription vb{0, sizeof(VerticeVideo), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription atributos[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {4, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = atributos;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState mezcla{}; mezcla.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &mezcla;
    const VkDynamicState dinamicos[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dinamicos;
    VkPipelineShaderStageCreateInfo etapas[2]{};
    for (auto& etapa : etapas) { etapa.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; etapa.pName = "main"; }
    etapas[0].stage = VK_SHADER_STAGE_VERTEX_BIT; etapas[0].module = moduloVS;
    etapas[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkGraphicsPipelineCreateInfo gp{}; gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = etapas; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = pipelineLayout_; gp.renderPass = renderPass_;
    for (size_t i = 0; i < 2; ++i) {
      etapas[1].module = moduloPS[i];
      Comprobar(CreateGraphicsPipelines(dispositivo, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelines_[i]), "Pipeline de video");
    }
    const VkDescriptorPoolSize tamanos[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 4; dp.poolSizeCount = 2; dp.pPoolSizes = tamanos;
    Comprobar(CreateDescriptorPool(dispositivo, &dp, nullptr, &pool_), "Pool de video");
    VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = pool_; da.descriptorSetCount = 4; da.pSetLayouts = layouts_.data();
    Comprobar(AllocateDescriptorSets(dispositivo, &da, sets_.data()), "Descriptores de video");
    VkSamplerCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sm.magFilter = sm.minFilter = VK_FILTER_LINEAR; sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sm.maxLod = 0;
    Comprobar(CreateSampler(dispositivo, &sm, nullptr, &sampler_), "Sampler de video");
  } catch (...) { Liberar(); throw; }
}
VideoVulkan::~VideoVulkan() { Liberar(); }
void VideoVulkan::Liberar() {
  if (pool_) DestroyDescriptorPool(dispositivo_, pool_, nullptr);
  if (sampler_) DestroySampler(dispositivo_, sampler_, nullptr);
  for (auto p : pipelines_) if (p) DestroyPipeline(dispositivo_, p, nullptr);
  if (renderPass_) DestroyRenderPass(dispositivo_, renderPass_, nullptr);
  if (pipelineLayout_) DestroyPipelineLayout(dispositivo_, pipelineLayout_, nullptr);
  for (auto l : layouts_) if (l) DestroyDescriptorSetLayout(dispositivo_, l, nullptr);
}
void VideoVulkan::ConfigurarTexturas(const std::array<VkImageView, 3>& planos) {
  std::array<VkDescriptorImageInfo, 3> imagenes{};
  for (size_t i = 0; i < 3; ++i) {
    if (!planos[i]) throw std::invalid_argument("Plano de video ausente");
    imagenes[i].imageView = planos[i]; imagenes[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  VkDescriptorImageInfo sampler{}; sampler.sampler = sampler_;
  VkWriteDescriptorSet escrituras[2]{};
  for (auto& e : escrituras) e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  escrituras[0].dstSet = sets_[0]; escrituras[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  escrituras[0].descriptorCount = 3; escrituras[0].pImageInfo = imagenes.data();
  escrituras[1].dstSet = sets_[3]; escrituras[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  escrituras[1].descriptorCount = 1; escrituras[1].pImageInfo = &sampler;
  UpdateDescriptorSets(dispositivo_, 2, escrituras, 0, nullptr);
  texturasListas_ = true;
}
void VideoVulkan::Dibujar(VkCommandBuffer cmd, VkFramebuffer destino, uint32_t ancho, uint32_t alto,
                          VkBuffer vertices, VkDeviceSize offset, VkDeviceAddress constantes, uint32_t variante, bool fondoNegro) {
  if (!texturasListas_ || variante >= 2 || !cmd || !destino || !ancho || !alto || !vertices || !constantes || constantes % 16)
    throw std::invalid_argument("Dibujado de video incompleto o constantes desalineadas");
  VkRenderPassBeginInfo inicio{}; inicio.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  inicio.renderPass = renderPass_; inicio.framebuffer = destino; inicio.renderArea.extent = {ancho, alto};
  CmdBeginRenderPass(cmd, &inicio, VK_SUBPASS_CONTENTS_INLINE);
  if (fondoNegro) {
    VkClearAttachment negro{}; negro.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    negro.clearValue.color.float32[3] = 1;
    const VkClearRect zona{{{0,0},{ancho,alto}},0,1};
    CmdClearAttachments(cmd,1,&negro,1,&zona);
  }
  CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[variante]);
  CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 4, sets_.data(), 0, nullptr);
  const uint64_t push[] = {0, 0, constantes};
  CmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24, push);
  const VkViewport viewport{0, 0, float(ancho), float(alto), 0, 1};
  const VkRect2D tijera{{0, 0}, {ancho, alto}};
  CmdSetViewport(cmd, 0, 1, &viewport); CmdSetScissor(cmd, 0, 1, &tijera);
  CmdBindVertexBuffers(cmd, 0, 1, &vertices, &offset);
  CmdDraw(cmd, 6, 1, 0, 0);
  CmdEndRenderPass(cmd);
}
}  // namespace nfsmw::native
