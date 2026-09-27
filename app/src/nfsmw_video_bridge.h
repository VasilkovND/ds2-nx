#pragma once
#include "nfsmw_video_vulkan.h"
#include <deque>
#include <memory>
#include <mutex>

namespace nfsmw::native {
struct FotogramaVideo {
  uint32_t objeto = 0;
  uint32_t ancho = 0, alto = 0, variante = 0;
  std::array<std::vector<uint8_t>, 3> planos;
  std::array<VerticeVideo, 6> vertices{};
  const Shader* vs = nullptr;
  const Shader* ps[2]{};
};

// One marker per guest Swap, even if that frame uses Xenos.
// That way a race never consumes a cutscene's last frame by mistake.
class ColaVideo {
 public:
  bool Encolar(std::shared_ptr<const FotogramaVideo> fotograma) {
    std::lock_guard lock(mutex_);
    if (cola_.size() >= 8) { cola_.clear(); return false; }
    cola_.push_back(std::move(fotograma)); return true;
  }
  std::shared_ptr<const FotogramaVideo> Consumir() {
    std::lock_guard lock(mutex_);
    if (cola_.empty()) return {};
    auto f = std::move(cola_.front()); cola_.pop_front(); return f;
  }
  void Vaciar() { std::lock_guard lock(mutex_); cola_.clear(); }
 private:
  std::mutex mutex_;
  std::deque<std::shared_ptr<const FotogramaVideo>> cola_;
};

void CapturarPlanosVideo(const uint8_t* base, uint32_t objeto, uint32_t datos);
void AnotarDibujoVideo(const uint8_t* base, bool esVideo, uint32_t objeto);
void InvalidarVideo();
void AnotarSwapVideo();
void DesactivarVideo(const char* motivo);
std::shared_ptr<const FotogramaVideo> ConsumirVideo();
}  // namespace nfsmw::native
