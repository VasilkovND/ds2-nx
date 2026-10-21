# Dead Space 2 → Nintendo Switch: передача контекста (20.10.2026)

## Что это
Нативный порт Dead Space 2 (Xbox 360, TU2) на Switch (homebrew NRO, Atmosphère).
- Репозиторий: https://github.com/VasilkovND/ds2-nx (вырос из nx-mod/xbox360-nx), игра в `deadspace2/`.
- Код игры не генерируем: берём готовый `src_tu2/` из https://github.com/bifftannen1/Dead-Space-2-Recomp
  (47 739 функций с именами из PDB, ReXGlue v0.10.0). На Mac лежит в `~/Downloads/DeadSpace2_Switch/Dead-Space-2-Recomp`.
- SDK: подмодуль `sdk` → nx-mod/rexglue-sdk-nx, ветка switch, коммит f961303 (libnx + deko3d, без Vulkan на Switch).
  Наши правки SDK — патчами в `deadspace2/sdk-patches/`, `build_mac.sh` применяет их сам.
- Подробности и история запусков: `deadspace2/PORTING.md`. Общий обзор: `README.md`, шейдеры: `shaders/README.md`.

## Сборка и установка (Mac, devkitPro из официального pkg)
```
cd ~/Downloads/DeadSpace2_Switch/ds2-nx
export PATH="/opt/homebrew/bin:$PATH" CMAKE_POLICY_VERSION_MINIMUM=3.5
deadspace2/tools/build_mac.sh            # -> deadspace2/out/sw/deadspace2.nro
cd .. && ./install_to_sd.sh [/Volumes/КАРТА]   # кладёт как sdmc:/switch/ds2-nx/ds2-nx.nro
```
Данные на карте: `switch/ds2-nx/{tu,disc1,disc2}` + `ds2_shaders.dslib` и `shadercache_dksh.bin` рядом с NRO (готовит `prepare_sd.sh` / `deadspace2/tools/stage_sd.py`).
TU2: `~/Downloads/DeadSpace2_TU2/Title Update 2 - Media ID 2915C8F8/TU_12K226V_000000C000000.00000000000G3` (хэш xexp проверен).
Запуск: R + любая игра → Homebrew Menu → ds2-nx. Логи: `switch/ds2-nx/logs/` (+ `logs/rex/rex_perfil.log`).

## Текущее состояние
- Репозиторий вычищен: код NFS и Halo 3, старая документация, Mesa и прочие
  инструменты удалены, всё переименовано на `ds`: `shaders/ds_hlsl.cpp`, `ds_empaquetar.cpp`, `ds_shader_library.{h,cpp}` (namespace `ds`),
  `dksh/ds_dksh.cpp`, макросы `DS_COMPARTIDA_*`. HLSL теперь только под deko3d.
- Библиотека шейдеров теперь `ds2_shaders.dslib` (магия `DS2SPV`), собирается `shaders/build_library.sh
  <shaders_dump> <out>` (`DKSH=1` — ещё и `shadercache_dksh.bin`). Пересобрана и проверена: 1302 шейдера,
  SPIR-V идентичен прежнему, кроме отладочных имён; 10642 варианта DKSH, все скомпилированы.
- `telemetria.flag` рядом с NRO: скриншоты каждые 5 с, дампы кадров, сэмплер стеков 1 мс (дорого, только для отладки).
- Имена структур UBO в `shader_common.h` (`NfsmwBloque*`) не трогать: они в отладочных именах SPIR-V, ключ кэша DKSH —
  хеш всего SPIR-V, и переименование обнуляет накопленный на консоли `cache/nfsmw_dksh.bin` (фризы от компиляции на лету).
- Выборочные проверки геометрии/текстур рендерера — по умолчанию SDK: полные хеши каждый кадр утраивали CPU потока кольца.
- `sdk-patches/0004-fixed-flush-mode.patch`: на Switch FPCR.FZ всегда включён, переключения flush-to-zero
  перекомпилированного кода (45 тыс. мест) — пустые. Цель: CPU аудио (компрессор rwaudiocore) и всех потоков игры.
- Отладочные сочетания — задние курки (L+R+крестовина мешали системному оверлею): ZL+ZR+вверх — счётчик FPS,
  вправо — режимы пропуска вызовов (1 = безопасный), вниз — скриншот в `capturas/`, влево — дамп резолвов в
  `volcado_NN/` и трасса всех вызовов кадра в логе (с базой глубины и резолвами), ZL+ZR+L — A/B-круг GPU, ZL+ZR+R — сэмплер стеков CPU вкл/выкл (вместо perfil_pilas.flag).
- Сцена разгерметизации: отложенная очистка цели, уже привязанной в текущем проходе, ждала резолва и стирала весь
  G-буфер (частицы привязывали его до очистки). Теперь отложенная очистка закрывает проход (`destinos.cpp`). Там же
  одновыборочные очистки D3D (rect list из 3 вершин, маска FFFF, цель RGBA8, цвет 0..1) эмулируются — буфер частиц
  в четверть экрана больше не копит дым кадр за кадром.
- `ds2_audio_tiempos` (по умолчанию выкл., для профилирования): каждые 10 с в логе `[audio] tiempo de CPU por etapa` — доля ядра каждой стадии rwaudiocore
  (реверберации, ресемплинг, панорама, фильтры). По этим числам решать, что облегчать дальше.
- Резолвы: прямоугольник не от (0,0) пишется в текстуру по базе копии на свою позицию (раньше база сдвигалась на
  тайл и получалась отдельная текстура — мусор в бою с боссом); новые резолв-текстуры очищаются один раз; выборка
  меньше резолва (320x180 из текстуры шириной 640) идёт из копии угла (`ResueltaRecortada`) — вертикальные полосы.
- Аудио: `MixWithGain`/`CopyWithGain` нативно на NEON (`ds2_audio_nativo`). Замеры на стоке в бою: Mixer::Execute
  50–95% ядра, из них SndPlayer1 25–36% (декодеры — теперь тоже в замерах), MixWithGain 14%, Send 12%, Resample 12%.
- GPU на стоке (307 МГц): сцена 28–40 мс, копии 4–7 мс на кадр — потолок около 25 fps без снижения разрешения.
- Профиль стеков (ZL+ZR+R, 12:22): главный поток до ~23% времени ждал мьютекс — кодоген превращает каждый
  `mtmsrd` (прерывания выкл. вокруг lwarx/stwcx.) в один глобальный рекурсивный мьютекс процесса; на нём сидели
  планировщик задач EARS (скиннинг, отсечение) и 4 потока EARSJob. `tools/make_nx_pch.py` (п. 3) делает это
  состояние потоковым; атомики и так CAS. Старое поведение: `ds2_msr_cerrojo_global=true` (`src/ds2_cpu.cpp`).
- Тени: игра сама задаёт depth bias в проходе теней 960 (scale 0.32, offset 1e-5), рендерер его применяет.
- Пока сэмплер стеков включён, счётчик пишет PRF вместо FPS.
- Профиль после снятия замка mtmsrd (сток): главный поток ~97% CPU, почти без ожиданий — дальше упирается в сам
  код игры (скиннинг SkinVerts* ~9%, остальное размазано). Миксер звука стоял на мьютексе контекста XMA, который
  декодер держит весь декод (`sdk-patches/0006-xma-locks.patch`: Enable без замка, будить декодер только когда он
  спит). `EA::Thread::SetThreadPriority` при ignore_thread_priorities — пустой (`src/ds2_cpu.cpp`).
- Открытые проблемы:
  - **Стоковые частоты**: CPU занят на ~280% из 300%. Аудио больше половины: Dac 50–80%, XMA 20–45%, EARSJob 4×~15%.
    Поток кольца GPU 20–45%.
  - **Видео на стоке** дёргается: VP6 декодируется программно в потоке Job Manager (99% ядра на 1020 МГц).
  - **Частицы n873** в начале игры: до 74 мс GPU за ~50 вызовов — похоже на слишком крупные спрайты.

## История запусков
- Запуск 1: падение из-за лимита блоков памяти Horizon (2001-0103) → исправлено `sdk-patches/0001-commit-granule.patch`.
- Запуск 2: игра работает — меню, музыка, геймплей по звуку, 28–30 swaps/s, до ~820 draw/кадр, ошибок нет.
  Экран чёрный/белый: рендерер (заглушка `src/ds2_gpu.cpp`, из Halo 3) получает только resolve/clear,
  геометрию не рисует.
- Профиль CPU (3 ядра = 300%): Job Manager ~97–99% (вероятно busy-wait), RwAudioCore Dac 38–62%,
  Main 50–61%, EARSJob×4 ~8%, XMA 8%. Звук без недогрузов.
- Добавлен `src/ds2_shader_dump.cpp`: хук D3DDevice_CreateVertexShader (0x82220568) / CreatePixelShader (0x822201C0),
  пишет контейнеры в `switch/ds2-nx/shaders_dump/{vs,ps}_<xxh3>.bin`. Сборка готова, на консоли ещё не проверена.

## Следующие шаги
1. Получить с консоли `shaders_dump/` и `logs/` (со стеками, perfil_pilas.flag).
2. Job Manager: найти цикл ожидания по стекам → `[[midasm_hook]]`/хук со sleep ~100 us (как в порте Halo 3).
3. Библиотека шейдеров: готова (`ds2_shaders.dslib` + кэш DKSH, см. `shaders/README.md`).
4. Декодер кольца PM4 (`src/ds2_gpu.cpp`, сборка deko3d) → `PeticionDibujo` → `rex::x360::RenderDk`: в работе.
5. Неподдержанный формат render target (лог: «formato de destino de render todavia no soportado», causa 114).

## Ограничения облачной сессии
- `pkg.devkitpro.org` закрыт прокси; тулчейн в облаке собирали из исходников (долго). Основная сборка — на Mac.
- Чтобы сессия могла пушить, подключите репозиторий VasilkovND/ds2-nx к сессии.
