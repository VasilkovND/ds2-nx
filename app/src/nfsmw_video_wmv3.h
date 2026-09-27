// nfsmw - WMV movies decoded natively with FFmpeg
//
// The game's videos (Movies/*.wmv) are WMV3 main profile, 1280x720 at 30 fps and without B frames. On
// the Switch the XDK's recompiled decoder runs at ~98 % of a core and produces ~22 frames per second:
// the picture falls behind and the audio ends first.
//  - DescodificadorWmv3: decodes with FFmpeg's WMV3 the compressed frames as stored in the container.
//    nfsmw_video_nativo.cpp passes it the same bytes the game hands to its own decoder.
//  - LeerInfoWmv: reads the size and the 4 WMV3 sequence bytes from the movie's ASF header.
//  - PeliculaWmv: full ASF reader (diagnostic: decodes the file on its own).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nfsmw::video_wmv3 {

// YUV 4:2:0 planes of the last decoded frame; valid until the next call.
struct Fotograma {
  const uint8_t* planos[3] = {nullptr, nullptr, nullptr};
  int pasos[3] = {0, 0, 0};
  int ancho = 0;
  int alto = 0;
  bool clave = false;
  uint32_t bytes = 0;  // compressed frame size
};

struct InfoWmv {
  int ancho = 0;
  int alto = 0;
  std::vector<uint8_t> secuencia;  // extra data of the WMV3 stream (STRUCT_C, 4 bytes)
};

// Reads the movie's ASF header using the guest path (the same one NtCreateFile uses).
bool LeerInfoWmv(const std::string& ruta, InfoWmv& info);

class DescodificadorWmv3 {
 public:
  DescodificadorWmv3();
  ~DescodificadorWmv3();
  DescodificadorWmv3(const DescodificadorWmv3&) = delete;
  DescodificadorWmv3& operator=(const DescodificadorWmv3&) = delete;

  bool Abrir(const InfoWmv& info);
  // Decodes one complete compressed frame. false if FFmpeg fails or returns no picture.
  bool Descodificar(const uint8_t* datos, size_t bytes, bool clave, Fotograma& salida);

  int ancho() const { return ancho_; }
  int alto() const { return alto_; }
  int64_t fotogramas() const { return fotogramas_; }

 private:
  struct Estado;
  std::unique_ptr<Estado> e_;
  int ancho_ = 0;
  int alto_ = 0;
  int64_t fotogramas_ = 0;
};

class PeliculaWmv {
 public:
  PeliculaWmv();
  ~PeliculaWmv();
  PeliculaWmv(const PeliculaWmv&) = delete;
  PeliculaWmv& operator=(const PeliculaWmv&) = delete;

  // Opens the movie with the guest path and prepares the decoder.
  bool Abrir(const std::string& ruta);
  // Reads the next video frame from the container and decodes it. false at the end or on failure.
  bool Siguiente(Fotograma& salida);

  const std::string& ruta() const { return ruta_; }
  int ancho() const { return info_.ancho; }
  int alto() const { return info_.alto; }
  int64_t fotogramas() const { return fotogramas_; }

 private:
  struct Estado;
  std::unique_ptr<Estado> e_;
  std::string ruta_;
  InfoWmv info_;
  int64_t fotogramas_ = 0;
};

}  // namespace nfsmw::video_wmv3
