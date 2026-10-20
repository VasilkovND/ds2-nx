/**
 * @file    ds_hlsl.cpp
 * @brief   Dead Space 2's Xbox 360 shaders (format 2008 containers) to HLSL for deko3d.
 *
 * XenosRecomp's translator only: DXC (from the Vulkan SDK) compiles the HLSL to SPIR-V, ds_empaquetar packs
 * it into the library and dksh/ds_dksh compiles it to DKSH. build_library.sh runs the whole chain.
 *
 * Input: the containers dumped on the console (shaders_dump/, ds2_shader_dump.cpp), one .bin each.
 *
 * Usage
 *   ds_hlsl <input folder> <output folder> <shader_common.h> [--saltos-lineales]
 *
 * --saltos-lineales: forward-only jumps as `if (pc <= N)` blocks instead of `while (true) switch (pc)`, which
 * uam cannot compile in reasonable time (ShaderRecompiler::saltosLineales).
 */

#include "XenosRecomp/shader_recompiler.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

namespace {

std::vector<uint8_t> LeerTodo(const std::filesystem::path& ruta) {
  std::vector<uint8_t> datos;
  FILE* f = std::fopen(ruta.string().c_str(), "rb");
  if (!f) {
    return datos;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    datos.resize(static_cast<size_t>(n));
    if (std::fread(datos.data(), 1, datos.size(), f) != datos.size()) {
      datos.clear();
    }
  }
  std::fclose(f);
  return datos;
}

// Format 2008, compared without the low byte (shader type and compiler revision).
bool FirmaValida(uint32_t flags_be) {
  return (__builtin_bswap32(flags_be) & 0xFFFFFF00u) == 0x102A1100u;
}


}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4 || (argc == 5 && std::strcmp(argv[4], "--saltos-lineales") != 0) || argc > 5) {
    std::printf("uso: ds_hlsl <entrada> <salida> <shader_common.h> [--saltos-lineales]\n");
    return 1;
  }
  const std::filesystem::path entrada = argv[1];
  const std::filesystem::path salida = argv[2];
  // ShaderRecompiler::saltosLineales: forward-only jumps without the switch loop.
  const bool saltos_lineales = argc == 5;

  const std::vector<uint8_t> comun = LeerTodo(argv[3]);
  if (comun.empty()) {
    std::printf("no pude leer %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(comun.data()), comun.size());

  std::error_code ec;
  if (std::filesystem::exists(salida) && !std::filesystem::is_empty(salida)) {
    std::fprintf(stderr, "la salida debe estar vacia para no mezclar resultados anteriores\n");
    return 1;
  }
  std::filesystem::create_directories(salida, ec);
  if (ec) throw std::runtime_error("no se pudo crear la carpeta de salida");

  size_t total = 0, ok = 0, saltados = 0;
  for (const auto& e : std::filesystem::directory_iterator(entrada)) {
    if (!e.is_regular_file() || e.path().extension() != ".bin") {
      continue;
    }
    const std::vector<uint8_t> datos = LeerTodo(e.path());
    ++total;
    if (datos.size() < 24) {
      std::printf("  %s: archivo ilegible o cabecera truncada\n", e.path().filename().string().c_str());
      ++saltados;
      continue;
    }

    uint32_t flags_be = 0;
    std::memcpy(&flags_be, datos.data(), 4);
    if (!FirmaValida(flags_be)) {
      std::printf("  %-20s firma desconocida 0x%08X\n", e.path().filename().string().c_str(),
                  __builtin_bswap32(flags_be));
      ++saltados;
      continue;
    }

    std::fflush(stdout);
    ShaderRecompiler recompilador;
    recompilador.saltosLineales = saltos_lineales;
    try {
      recompilador.recompile(datos.data(), include);
    } catch (const std::exception& error) {
      std::printf("  %s: traduccion rechazada: %s\n", e.path().filename().string().c_str(), error.what());
      ++saltados;
      continue;
    }
    if (recompilador.out.empty()) {
      std::printf("  %-20s no produjo nada\n", e.path().filename().string().c_str());
      ++saltados;
      continue;
    }

    auto destino = salida / e.path().filename();
    destino.replace_extension(".hlsl");
    FILE* f = std::fopen(destino.string().c_str(), "wb");
    if (f) {
      const bool completo = std::fwrite(recompilador.out.data(), 1, recompilador.out.size(), f) == recompilador.out.size();
      const bool cerrado = std::fclose(f) == 0;
      if (completo && cerrado) ++ok;
      else ++saltados;
    } else {
      ++saltados;
    }
  }

  std::printf("\n%zu shaders: %zu traducidos, %zu saltados\n", total, ok, saltados);
  return total > 0 && ok == total ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
