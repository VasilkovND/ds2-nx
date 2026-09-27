#include "nfsmw_shader_hooks.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>

namespace nfsmw::native {
namespace {
BibliotecaShaders g_biblioteca;
std::atomic<bool> g_lista{false};
std::mutex g_mutex;
std::unordered_map<uint32_t, const Shader*> g_objetos;

uint32_t LeerBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

const Shader* Identificar(const uint8_t* base, uint32_t direccion, bool vertices) {
  if (!g_lista.load(std::memory_order_acquire) || !direccion || direccion > UINT32_MAX - 24)
    return nullptr;
  const uint8_t* p = base + direccion;
  if (LeerBE(p) != (vertices ? 0x102A0E01u : 0x102A0E00u)) return nullptr;
  const uint32_t virtuales = LeerBE(p + 4), fisicos = LeerBE(p + 8);
  const uint64_t total = uint64_t(virtuales) + fisicos;
  if (virtuales < 24 || !fisicos || total > 65536 ||
      uint64_t(direccion) + total > (uint64_t(1) << 32)) return nullptr;
  return g_biblioteca.Buscar(std::span<const uint8_t>(p, size_t(total)));
}

void Recordar(uint32_t objeto, const Shader* shader) {
  if (!objeto || !g_lista.load(std::memory_order_acquire)) return;
  try {
    std::lock_guard lock(g_mutex);
    // The address can be reused after Release. An unknown shader invalidates any
    // previous association with that same address.
    if (shader) g_objetos.insert_or_assign(objeto, shader);
    else g_objetos.erase(objeto);
  } catch (const std::exception& e) {
    // Never let an exception from the experimental registry reach the guest.
    // Disabling all lookups avoids using stale associations.
    g_lista.store(false, std::memory_order_release);
    REXLOG_WARN("Registro de shaders nativos desactivado: {}", e.what());
  }
}
}  // namespace

void IniciarBibliotecaShaders() {
  static std::once_flag inicio;
  std::call_once(inicio, [] {
    try {
      g_biblioteca.Cargar(rex::filesystem::GetExecutableFolder() / "nfsmw_shaders.nfsp");
      g_lista.store(true, std::memory_order_release);
      REXLOG_INFO("Biblioteca experimental: {} shaders nativos; video disponible, resto de dibujados Xenos",
                  g_biblioteca.shaders().size());
    } catch (const std::exception& e) {
      REXLOG_WARN("Biblioteca de shaders nativos no disponible: {}", e.what());
    }
  });
}

const Shader* ShaderDeObjeto(uint32_t objeto) {
  if (!g_lista.load(std::memory_order_acquire) || !objeto) return nullptr;
  std::lock_guard lock(g_mutex);
  auto it = g_objetos.find(objeto);
  return it != g_objetos.end() ? it->second : nullptr;
}
const Shader* ShaderOriginal(std::span<const uint8_t> contenedor) {
  if (!g_lista.load(std::memory_order_acquire)) return nullptr;
  return g_biblioteca.Buscar(contenedor);
}
}  // namespace nfsmw::native

// These constructors receive the original contiguous container in r3 and return
// the object in r3 (or zero). The hash is computed before the driver's copies.
// The original function always runs and no PPC register is modified.
REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {
  const auto* shader = nfsmw::native::Identificar(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  nfsmw::native::Recordar(ctx.r3.u32, shader);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {
  const auto* shader = nfsmw::native::Identificar(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  nfsmw::native::Recordar(ctx.r3.u32, shader);
}
