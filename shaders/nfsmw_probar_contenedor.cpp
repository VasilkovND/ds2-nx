// Regression test for the format and the local corpus. It contains no game data.
#include "nfsmw_contenedor.h"
#include "XenosRecomp/shader_code.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

static std::vector<uint8_t> Leer(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void Exigir(bool valor, const char* mensaje) {
  if (!valor) throw std::runtime_error(mensaje);
}
static void Rechazar(const std::vector<uint8_t>& datos) {
  try { nfsmw::Flujo f; nfsmw::Convertir2005(datos, f); }
  catch (const std::runtime_error&) { return; }
  throw std::runtime_error("se acepto una entrada invalida");
}
static void Poner(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = v >> 24; d[o+1] = v >> 16; d[o+2] = v >> 8; d[o+3] = v;
}

int main(int argc, char** argv) try {
  if (argc != 3) throw std::runtime_error("uso: nfsmw_probar_contenedor <disco> <volcados anteriores>");
  std::vector<std::vector<uint8_t>> corpus;
  std::set<uint32_t> operaciones;
  size_t pixeles = 0, vertices = 0, descarte = 0;
  for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
    if (e.path().extension() != ".bin") continue;
    auto datos = Leer(e.path());
    const nfsmw::Lector l{datos};
    nfsmw::Flujo flujo;
    auto salida = nfsmw::Convertir2005(datos, flujo);
    const nfsmw::Lector normal{salida};
    Exigir(std::equal(datos.begin()+l.u32(4), datos.end(), salida.begin()+normal.u32(4)), "microcodigo alterado al normalizar");
    uint32_t exportaciones = 0;
    bool mata = false;
    for (uint32_t i = 0; i < flujo.bytes; i += 12) {
      const uint32_t o = l.u32(4) + i;
      const uint64_t cf[] = {uint64_t(l.u32(o)) | uint64_t(l.u32(o+4) & 65535) << 32,
                             uint64_t(l.u32(o+4) >> 16) | uint64_t(l.u32(o+8)) << 16};
      for (uint64_t c : cf) {
        const uint32_t op = (c >> 44) & 15;
        operaciones.insert(op);
        if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) continue;
        uint32_t secuencia = (c >> 16) & 4095;
        for (uint32_t j = 0; j < ((c >> 12) & 7); ++j, secuencia >>= 2) {
          if (secuencia & 1) continue;
          const uint32_t a = l.u32(4) + ((c & 4095) + j)*12;
          const uint32_t palabras[] = {l.u32(a), l.u32(a+4), l.u32(a+8)};
          AluInstruction alu;
          static_assert(sizeof(alu) == sizeof(palabras));
          std::memcpy(&alu, palabras, sizeof(alu));
          mata |= (alu.vectorOpcode >= AluVectorOpcode::KillEq && alu.vectorOpcode <= AluVectorOpcode::KillNe);
          mata |= (alu.scalarOpcode >= AluScalarOpcode::KillsEq && alu.scalarOpcode <= AluScalarOpcode::KillsOne);
          if (alu.exportData) {
            if (alu.vectorDest < 4) exportaciones |= 1u << alu.vectorDest;
            else if (alu.vectorDest == 61) exportaciones |= 16;
          }
        }
      }
    }
    if (!(l.u32(0) & 1)) {
      ++pixeles;
      const uint32_t mascara = l.u32(l.u32(20) + 28);
      Exigir((mascara & 15) == (exportaciones & 15), "salidas de color distintas del microcodigo");
      Exigir(!(exportaciones & 16), "el corpus ahora contiene profundidad: revisar su formato");
      Exigir(bool(mascara & 32) == mata, "el bit 5 no coincide con instrucciones de descarte");
      descarte += mata;
    } else ++vertices;
    // Truncated headers, overflowing limits and nonexistent CF.
    for (size_t n = 0; n < 24; ++n) Rechazar({datos.begin(), datos.begin()+n});
    auto corrupto = datos;
    Poner(corrupto, 20, 0xFFFFFFF0); Rechazar(corrupto);
    corrupto = datos; Poner(corrupto, 16, 0xFFFFFFF0); Rechazar(corrupto);
    corrupto = datos; Poner(corrupto, 4, 0xFFFFFFF0); Rechazar(corrupto);
    corrupto = datos; std::fill(corrupto.begin()+l.u32(4), corrupto.end(), 0); Rechazar(corrupto);
    corpus.push_back(std::move(datos));
  }
  size_t rechazados = 0, correspondencias = 0;
  for (const auto& e : std::filesystem::directory_iterator(argv[2])) {
    if (e.path().extension() != ".bin") continue;
    const auto datos = Leer(e.path());
    Rechazar(datos); ++rechazados;
    const nfsmw::Lector l{datos};
    const uint32_t ct = l.u32(16), sh = l.u32(20);
    bool existe = false;
    for (const auto& original : corpus) {
      const nfsmw::Lector r{original};
      if (l.u32(0) != r.u32(0) || l.u32(4) != r.u32(4) || l.u32(8) != r.u32(8) || ct != r.u32(16) || sh != r.u32(20)) continue;
      // CTAB is immutable; the live object also contains working fields.
      existe |= std::equal(datos.begin()+ct, datos.begin()+sh, original.begin()+ct);
    }
    if (!existe) std::cout << "Sin CTAB equivalente en disco: " << e.path().filename().string() << '\n';
    correspondencias += existe;
  }
  Exigir(!corpus.empty() && rechazados != 0, "corpus vacio");
  std::cout << corpus.size() << " contenedores: " << vertices << " vertices, " << pixeles << " pixeles; "
            << descarte << " con descarte; " << rechazados << " volcados incorrectos rechazados; "
            << correspondencias << " CTAB recuperadas.\nCF presentes:";
  for (auto op : operaciones) std::cout << ' ' << op;
  std::cout << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
