// Universal Xbox 360 PM4 decode helpers: turn the GPU register mirror a game's ring reader keeps into the structs
// the renderer consumes. Game-independent (the Xbox 360 GPU is the same for every title); a game's ring reader
// (NFS's nfsmw_nativo_sistema.cpp, Halo's halo3_gpu.cpp) passes its register array and calls these.
#pragma once

#include <cstdint>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/x360/destinos.h>

namespace rex::x360 {

// A draw with RB_MODECONTROL in copy mode is a resolve (and/or a clear), not geometry.
inline bool EsCopia(const uint32_t* registros) {
  namespace g = rex::graphics;
  return (registros[g::XE_GPU_REG_RB_MODECONTROL] & 0x7u) == uint32_t(rex::graphics::xenos::EdramMode::kCopy);
}

// The registers a resolve/clear needs, read from the mirror (matches the emulated path, vulkan command_processor).
inline RegistrosCopia LeerRegistrosCopia(const uint32_t* r) {
  namespace g = rex::graphics;
  RegistrosCopia c;
  c.rb_surface_info = r[g::XE_GPU_REG_RB_SURFACE_INFO];
  c.rb_color_info[0] = r[g::XE_GPU_REG_RB_COLOR_INFO];
  c.rb_color_info[1] = r[g::XE_GPU_REG_RB_COLOR1_INFO];
  c.rb_color_info[2] = r[g::XE_GPU_REG_RB_COLOR2_INFO];
  c.rb_color_info[3] = r[g::XE_GPU_REG_RB_COLOR3_INFO];
  c.rb_depth_info = r[g::XE_GPU_REG_RB_DEPTH_INFO];
  c.rb_copy_control = r[g::XE_GPU_REG_RB_COPY_CONTROL];
  c.rb_copy_dest_base = r[g::XE_GPU_REG_RB_COPY_DEST_BASE];
  c.rb_copy_dest_pitch = r[g::XE_GPU_REG_RB_COPY_DEST_PITCH];
  c.rb_copy_dest_info = r[g::XE_GPU_REG_RB_COPY_DEST_INFO];
  c.rb_color_clear = r[g::XE_GPU_REG_RB_COLOR_CLEAR];
  c.rb_color_clear_lo = r[g::XE_GPU_REG_RB_COLOR_CLEAR_LO];
  c.rb_depth_clear = r[g::XE_GPU_REG_RB_DEPTH_CLEAR];
  c.pa_sc_window_offset = r[g::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  c.pa_sc_window_scissor_tl = r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  c.pa_sc_window_scissor_br = r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  c.pa_su_sc_mode_cntl = r[g::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  c.pa_su_vtx_cntl = r[g::XE_GPU_REG_PA_SU_VTX_CNTL];
  c.fetch_vertices[0] = r[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  c.fetch_vertices[1] = r[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1];
  return c;
}

}  // namespace rex::x360
