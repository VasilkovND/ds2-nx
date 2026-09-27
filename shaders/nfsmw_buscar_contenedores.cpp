// Searches for original containers without modifying the files of the game copy.
// A signature and dimension match is only a candidate, not a validation of the
// microcode. The output keeps the source and the exact offset.
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>

static uint32_t LeerBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "uso: nfsmw_buscar_contenedores <entrada> <salida nueva>\n");
    return 1;
  }
  const std::filesystem::path salida(argv[2]);
  if (std::filesystem::exists(salida)) throw std::runtime_error("la salida ya existe");
  std::filesystem::create_directories(salida);
  std::ofstream indice(salida / "procedencia.tsv");
  indice << "archivo\torigen\tdesplazamiento\tvirtual\tfisico\n";
  size_t total = 0;
  for (const auto& entrada : std::filesystem::recursive_directory_iterator(argv[1])) {
    if (!entrada.is_regular_file()) continue;
    const auto ext = entrada.path().extension().string();
    if (ext != ".BIN" && ext != ".bin" && ext != ".xex" && ext != ".exe") continue;
    const size_t n = entrada.file_size();
    if (n < 24 || n > 1024ULL * 1024 * 1024) continue;
    std::vector<uint8_t> datos(n);
    std::ifstream archivo(entrada.path(), std::ios::binary);
    if (!archivo.read(reinterpret_cast<char*>(datos.data()), n)) throw std::runtime_error("fallo de lectura");
    size_t encontrados = 0;
    for (size_t i = 0; i + 24 <= n; ++i) {
      const auto* p = datos.data() + i;
      if (p[0] != 0x10 || p[1] != 0x2A || p[2] != 0x0E || p[3] > 1) continue;
      const uint32_t v = LeerBE(p + 4), f = LeerBE(p + 8), c = LeerBE(p + 16), s = LeerBE(p + 20);
      if (v < 24 || uint64_t(v) + f > 65536 || uint64_t(v) + f > n - i ||
          c < 24 || c >= v || s < 24 || uint64_t(s) + 24 > v || !f) continue;
      char nombre[48];
      std::snprintf(nombre, sizeof(nombre), "%c_%06zu.bin", p[3] ? 'v' : 'p', total);
      std::ofstream copia(salida / nombre, std::ios::binary);
      copia.write(reinterpret_cast<const char*>(p), size_t(v) + f);
      if (!copia) throw std::runtime_error("fallo de escritura");
      indice << nombre << '\t' << entrada.path().string() << '\t' << i << '\t' << v << '\t' << f << '\n';
      ++total;
      ++encontrados;
    }
    std::printf("%s: %zu candidatos\n", entrada.path().filename().string().c_str(), encontrados);
    std::fflush(stdout);
  }
  std::printf("Total: %zu candidatos; comprobar antes de traducir.\n", total);
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 2;
}
