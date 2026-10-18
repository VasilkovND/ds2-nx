/**
 * @file    halo3_hlsl.cpp
 * @brief   Halo 3's shader containers to HLSL with XenosRecomp (shaders/XenosRecomp, NFS's version).
 *
 * The same driver as shaders/nfsmw_hlsl.cpp, for the containers halo3_shader_containers.py finds in the maps.
 * Those are in the 2008 format XenosRecomp reads, with one difference: Halo 3 ships them without a constant table
 * (its offset is 0; Bungie keeps the names in its own tags). XenosRecomp needs the table to name the constants, the
 * booleans and the samplers, so this gives each container a synthetic one, appended after it:
 *
 *  - g_c: every float register (256 for vertex shaders, 224 for pixel shaders, XenosRecomp's limits), so each
 *    constant is read by register number, relative addressing included;
 *  - g_b0..g_b15: the boolean registers of the shader's stage;
 *  - g_s0..g_s15: the texture fetch slots (deko3d bindings 0-15). The dimension of each fetch comes from the
 *    instruction itself, not from the table.
 *
 * The renderer fills the constants by register, as the Xbox 360 GPU sees them, so no names are needed at run time.
 *
 * Usage
 *   halo3_hlsl <input folder> <output folder> <shader_common.h>
 */
#include "XenosRecomp/shader_recompiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  data.resize(n > 0 ? size_t(n) : 0);
  if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size()) {
    data.clear();
  }
  std::fclose(f);
  return data;
}

void PutBE32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
  v[o] = uint8_t(x >> 24);
  v[o + 1] = uint8_t(x >> 16);
  v[o + 2] = uint8_t(x >> 8);
  v[o + 3] = uint8_t(x);
}

void PutBE16(std::vector<uint8_t>& v, size_t o, uint16_t x) {
  v[o] = uint8_t(x >> 8);
  v[o + 1] = uint8_t(x);
}

uint32_t GetBE32(const std::vector<uint8_t>& v, size_t o) {
  return uint32_t(v[o]) << 24 | uint32_t(v[o + 1]) << 16 | uint32_t(v[o + 2]) << 8 | v[o + 3];
}

// Appends a D3DXSHADER_CONSTANTTABLE (inside XenosRecomp's ConstantTableContainer) and points the header at it.
std::vector<uint8_t> WithSyntheticConstantTable(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out = in;
  out.resize((out.size() + 15) & ~size_t(15), 0);
  const bool pixel = (GetBE32(in, 0) & 1) == 0;

  struct Entry {
    std::string name;
    uint16_t set;  // RegisterSet: 0 bool, 1 int4, 2 float4, 3 sampler
    uint16_t index;
    uint16_t count;
    uint16_t cls;   // ParameterClass
    uint16_t type;  // ParameterType
    uint16_t rows, columns, elements;
  };
  std::vector<Entry> entries;
  entries.push_back({"g_c", 2, 0, uint16_t(pixel ? 224 : 256), 1, 3, 1, 4, uint16_t(pixel ? 224 : 256)});
  for (uint16_t i = 0; i < 16; ++i) {
    entries.push_back({"g_b" + std::to_string(i), 0, i, 1, 0, 1, 1, 1, 1});
  }
  for (uint16_t i = 0; i < 16; ++i) {
    entries.push_back({"g_s" + std::to_string(i), 3, i, 1, 4, 12, 1, 1, 1});  // Object, Sampler2D
  }

  // Layout, relative to the ConstantTable (constantTableData): the table (28 bytes), the ConstantInfo array
  // (20 bytes each), the TypeInfo array (16 bytes each), then the names.
  const size_t container = out.size();  // ConstantTableContainer: size, then the ConstantTable
  const size_t table = container + 4;
  const size_t infos = 28;
  const size_t types = infos + entries.size() * 20;
  size_t names = types + entries.size() * 16;
  size_t end = names;
  for (const Entry& e : entries) {
    end += e.name.size() + 1;
  }
  end = (end + 3) & ~size_t(3);
  out.resize(table + end, 0);

  PutBE32(out, container, uint32_t(end));
  PutBE32(out, table + 0, 28);                  // size
  PutBE32(out, table + 8, pixel ? 0xFFFF0300u : 0xFFFE0300u);  // version (ps_3_0 / vs_3_0)
  PutBE32(out, table + 12, uint32_t(entries.size()));
  PutBE32(out, table + 16, uint32_t(infos));
  PutBE32(out, table + 24, 0);                  // target
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    const size_t info = table + infos + i * 20;
    const size_t type = table + types + i * 16;
    PutBE32(out, info + 0, uint32_t(names));
    PutBE16(out, info + 4, e.set);
    PutBE16(out, info + 6, e.index);
    PutBE16(out, info + 8, e.count);
    PutBE32(out, info + 12, uint32_t(types + i * 16));
    PutBE16(out, type + 0, e.cls);
    PutBE16(out, type + 2, e.type);
    PutBE16(out, type + 4, e.rows);
    PutBE16(out, type + 6, e.columns);
    PutBE16(out, type + 8, e.elements);
    std::memcpy(out.data() + table + names, e.name.c_str(), e.name.size() + 1);
    names += e.name.size() + 1;
  }
  PutBE32(out, 16, uint32_t(container));  // ShaderContainer::constantTableOffset
  return out;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4) {
    std::printf("usage: halo3_hlsl <input> <output> <shader_common.h>\n");
    return 1;
  }
  const std::filesystem::path input = argv[1];
  const std::filesystem::path output = argv[2];
  const std::vector<uint8_t> common = ReadAll(argv[3]);
  if (common.empty()) {
    std::printf("cannot read %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(common.data()), common.size());
  if (std::filesystem::exists(output) && !std::filesystem::is_empty(output)) {
    std::fprintf(stderr, "the output folder must be empty\n");
    return 1;
  }
  std::filesystem::create_directories(output);

  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(input)) {
    if (e.is_regular_file() && e.path().extension() == ".bin") {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());

  size_t ok = 0, skipped = 0;
  for (const auto& path : files) {
    const std::vector<uint8_t> data = ReadAll(path);
    const std::string name = path.filename().string();
    if (data.size() < 36 || (GetBE32(data, 0) & 0xFFFFFF00u) != 0x102A1100u) {
      std::printf("  %s: not a 2008 container\n", name.c_str());
      ++skipped;
      continue;
    }
    const std::vector<uint8_t> container = GetBE32(data, 16) ? data : WithSyntheticConstantTable(data);
    ShaderRecompiler recompiler;
    try {
      recompiler.recompile(container.data(), include);
    } catch (const std::exception& error) {
      std::printf("  %s: rejected: %s\n", name.c_str(), error.what());
      ++skipped;
      continue;
    }
    if (recompiler.out.empty()) {
      std::printf("  %s: produced nothing\n", name.c_str());
      ++skipped;
      continue;
    }
    auto target = output / path.filename();
    target.replace_extension(".hlsl");
    FILE* f = std::fopen(target.string().c_str(), "wb");
    const bool written = f && std::fwrite(recompiler.out.data(), 1, recompiler.out.size(), f) == recompiler.out.size();
    if (f && std::fclose(f) != 0) {
      ++skipped;
      continue;
    }
    written ? ++ok : ++skipped;
  }
  std::printf("\n%zu shaders: %zu translated, %zu skipped\n", files.size(), ok, skipped);
  return !files.empty() && ok == files.size() ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
