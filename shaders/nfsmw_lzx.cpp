// LZX decompression with the same library and parameters as the ReXGlue runtime.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mspack.h>
#include <lzx.h>

extern "C" struct mspack_system* mspack_default_system;

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "uso: nfsmw_lzx <entrada> <salida nueva> <bits ventana> <bytes salida>\n");
    return 1;
  }
  const int bits = std::atoi(argv[3]);
  const long bytes = std::atol(argv[4]);
  if (bits < 15 || bits > 21 || bytes <= 0 || bytes > 512*1024*1024 || std::filesystem::exists(argv[2])) return 1;
  auto* sistema = mspack_default_system;
  auto* entrada = sistema->open(sistema, argv[1], MSPACK_SYS_OPEN_READ);
  if (!entrada) return 1;
  auto* salida = sistema->open(sistema, argv[2], MSPACK_SYS_OPEN_WRITE);
  if (!salida) { sistema->close(entrada); return 1; }
  auto* flujo = lzxd_init(sistema, entrada, salida, bits, 0, 0x8000, bytes, 0);
  const int estado = flujo ? lzxd_decompress(flujo, bytes) : -1;
  if (flujo) lzxd_free(flujo);
  sistema->close(entrada);
  sistema->close(salida);
  if (estado || std::filesystem::file_size(argv[2]) != size_t(bytes)) {
    std::fprintf(stderr, "LZX fallo: %d; la salida no es valida\n", estado);
    return 2;
  }
  std::printf("LZX: %ld bytes descomprimidos\n", bytes);
  return 0;
}
