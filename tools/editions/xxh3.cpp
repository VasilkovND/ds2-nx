// Prints the XXH3-64 of each file, the same fingerprint the shader library uses for containers.
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (!f) continue;
    std::vector<unsigned char> d;
    unsigned char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(f);
    std::printf("%016llX %s\n", (unsigned long long)XXH3_64bits(d.data(), d.size()), argv[i]);
  }
}
