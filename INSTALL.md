# Установка ds2-nx 1.3.0 (Dead Space 2 для Nintendo Switch)

ds2-nx — нативный порт Dead Space 2 (Xbox 360) для Switch в виде homebrew-программы. Версия 1.3.0 стабильная и
играбельная: игру можно пройти целиком. На стоке игра идёт в 540p с 30 fps, при небольшом разгоне — в 720p с 30 fps.
Готовых файлов не
распространяется: программа и шейдеры собираются у вас на компьютере, а игра берётся **из вашей собственной копии**
(два диска Xbox 360 и Title Update 2).

Шаги: собрать программу → подготовить файлы игры → сделать шейдеры из вашей копии → скопировать на карту.

## Что понадобится

| Что | Подробности |
| --- | --- |
| Nintendo Switch | С кастомной прошивкой **Atmosphère** и Homebrew Menu |
| Карта microSD | Свободно **~15 ГБ**, exFAT или FAT32 (самые большие файлы игры меньше 4 ГБ) |
| Образы ваших дисков | Dead Space 2 для Xbox 360, **Disc 1** и **Disc 2**, `.iso`, издание **World** (En/Fr/Es) или **Russia** (En/Ru) |
| Title Update 2 | Для вашего издания (таблица ниже). На накопителе Xbox 360 лежит в `Content/0000000000000000/454108DF/000B0000/` |
| Компьютер | **macOS** (проверено) или Linux (не проверялось), ~25 ГБ свободного места, интернет |

| Издание | Media ID | Файл Title Update 2 |
| --- | --- | --- |
| World (En/Fr/Es) | 2915C8F8 | `TU_12K226V_000000C000000.00000000000G3` |
| Russia (En/Ru) | 2C199AEB | `TU_12K226V_000000K000000.00000000000G5` |

Обновление должно быть от того же издания, что и диски. Принимаются только эти версии: каждый файл проверяется по
контрольной сумме, TU2 других регионов не подходят. Программа одна и та же для обоих изданий: после TU2 их код
совпадает.

## Шаг 1. Собрать программу (ds2-nx.nro)

Один раз ~20–30 минут (на Apple M1 Pro) и ~3 ГБ места.

1. **Инструменты Xcode:** `xcode-select --install` (только macOS).
2. **Homebrew** (https://brew.sh), затем `brew install cmake ninja python git`. На Linux — те же пакеты из
   дистрибутива.
3. **devkitPro:** установщик `devkitpro-pacman-installer.pkg` со страницы https://github.com/devkitPro/pacman/releases
   (на Linux — по https://devkitpro.org/wiki/Getting_Started), затем:

   ```
   sudo dkp-pacman -S switch-dev deko3d
   ```

4. **Исходники и сборка:**

   ```
   git clone --branch v1.3.0 https://github.com/VasilkovND/ds2-nx
   cd ds2-nx
   deadspace2/tools/build_mac.sh
   ```

   Скрипт сам скачает перекомпилированный код игры (проект Dead-Space-2-Recomp, ровно ту версию, с которой проверен
   этот релиз), библиотеки и SDK и соберёт **`deadspace2/out/sw/deadspace2.nro`**.

## Шаг 2. Подготовить файлы игры

Из папки `ds2-nx` (на Windows вместо `python3` пишите `py`):

```
python3 deadspace2/tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
```

Для русского издания укажите его образы и `TU_12K226V_000000K000000.00000000000G5`. Укажите полные пути к своим
файлам в кавычках. Скрипт проверит обновление и оба диска и распакует их (5–15 минут).
В конце будет `Ready: ...`, а в папке `game` — `disc1`, `disc2` и `tu`.

## Шаг 3. Шейдеры из вашей копии игры

Шейдеры игры переводятся для Switch заранее, на компьютере. Сначала их нужно получить с консоли: игра создаёт все
шейдеры при запуске, и программа сама их сохранит.

1. **Первый запуск без шейдеров.** Положите на карту в `sdmc:/switch/ds2-nx/` программу (`deadspace2.nro` под
   именем `ds2-nx.nro`) и содержимое папки `game` (`disc1`, `disc2`, `tu`). Запустите: любую игру с зажатой **R**
   → Homebrew Menu → **Dead Space 2**. Экран будет чёрным — это нормально, шейдеров ещё нет. Подождите
   **2–3 минуты** (пауза ~40 секунд на загрузке — это запись файлов), затем закройте программу кнопкой HOME.
2. **Перенесите на компьютер** папку `sdmc:/switch/ds2-nx/shaders_dump/` (~1300 файлов).
3. **Установите Vulkan SDK** (https://vulkan.lunarg.com, нужны `dxc` и `spirv-val`; на macOS он ставится в
   `~/VulkanSDK`).
4. **Соберите библиотеку шейдеров** из папки `ds2-nx` (~10 минут):

   ```
   DKSH=1 shaders/build_library.sh <путь к shaders_dump> shaders_out
   ```

   На Linux укажите папку с `dxc` и `spirv-val`: `VULKAN_BIN=/путь/к/bin DKSH=1 shaders/build_library.sh ...`.
   Результат: **`shaders_out/ds2_shaders.dslib`** и **`shaders_out/shadercache_dksh.bin`**.

## Шаг 4. Скопировать на карту

Положите оба файла шейдеров в `sdmc:/switch/ds2-nx/`; папку `shaders_dump` на карте можно удалить. В итоге:

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro             шаг 1 (deadspace2.nro, переименован)
    ds2_shaders.dslib      шаг 3
    shadercache_dksh.bin   шаг 3
    tu/        default.xex  default.xexp                    шаг 2
    disc1/     default.xex  bigfile0.viv  bigfile1.viv  ... шаг 2
    disc2/     default.xex  bigfile0.viv  bigfile1.viv  ... шаг 2
```

**Русское издание:** игра берёт язык из настроек консоли, а программа по умолчанию сообщает английский. Чтобы
выбрать русский, положите рядом с программой файл `ds2.toml` с одной строкой `user_language = 12`.

Сохранения игра создаст сама в `sdmc:/switch/ds2-nx/ds2/`, логи пишутся в `sdmc:/switch/ds2-nx/logs/`.

## Запуск

1. Вставьте карту и загрузите Atmosphère.
2. Запустите **любую установленную игру, удерживая кнопку R**: так откроется Homebrew Menu с полным доступом к
   памяти. Через альбом (режим апплета) памяти не хватит.
3. Выберите **Dead Space 2**.

Счётчик FPS включается и выключается сочетанием **ZL + ZR + вверх на крестовине**.

**Меню настроек: ZL + ZR + Минус** (во время игры). Пока меню открыто, игра не реагирует на кнопки:

- **вверх / вниз** — выбрать строку, **влево / вправо** — изменить значение;
- **RESOLUTION**: **720P** (1280×720, для консоли с разгоном) или **540P** (960×540, для стоковых частот); сохраняется в
  `ds2_resolucion.txt` рядом с программой и включается при **следующем запуске** игры;
- **VIBRATION**: сила вибрации 0 / 25 / 50 / 75 / 100 % (по умолчанию 50 %), меняется сразу; сохраняется в
  `ds2_vibracion.txt`;
- **A** — сохранить, **B** — закрыть меню.

Без сохранённого выбора программа сама берёт 540p на стоковых частотах и 720p с разгоном (процессор от 1,5 ГГц).
В портативном режиме программа просит у системы официальный профиль с GPU 460,8 МГц (как коммерческие игры; это не
разгон). Версия 1.3.0 стабильная и играбельная: игру можно пройти целиком. На стоковых частотах игра идёт в 540p со
стабильными 30 fps (на них игра ограничена). При небольшом разгоне (например, через sys-clk) игра идёт в 720p, тоже
с 30 fps.

## Если что-то не так

| Сообщение или проблема | Что делать |
| --- | --- |
| `not a Title Update 2 this port supports` | Нужен TU2 с Media ID 2915C8F8 (World) или 2C199AEB (Russia), а не другой регион |
| `Disc 1 default.xex is from the ... edition` | Диск и TU2 от разных изданий: возьмите TU2 с указанным Media ID |
| `Disc 1 default.xex does not match` | Образ не того издания или повреждён: нужен World (En/Fr/Es) или Russia (En/Ru) |
| `Not an Xbox 360 disc image` | Это не образ диска Xbox 360 (или он в другом формате) |
| `--disc1: ... not found` | Неверный путь к файлу: проверьте имя и кавычки |
| `devkitA64 not found` / `deko3d not installed` | Не выполнен пункт 3 шага 1 (devkitPro и `dkp-pacman -S switch-dev deko3d`) |
| `missing .../dxc (set VULKAN_BIN)` | Не установлен Vulkan SDK, или укажите `VULKAN_BIN` |
| Чёрный экран в игре | Нет файлов шейдеров рядом с программой (шаг 3) |
| Программа сразу закрывается | Проверьте `tu/`, `disc1/`, `disc2/` и запуск через игру с R |
| Тормозит на стоковых частотах (выбрано 720P) | Откройте меню ZL + ZR + Минус, выберите 540P, нажмите A и перезапустите игру |
| Другие проблемы | Приложите к сообщению об ошибке всю папку `sdmc:/switch/ds2-nx/logs/` (вместе с `logs/rex/`) и опишите, где и что произошло. В первой строке лога записаны версия сборки, прошивка и режим запуска |

---

# Installing ds2-nx 1.3.0 (Dead Space 2 for Nintendo Switch)

ds2-nx is a native Switch port of Dead Space 2 (Xbox 360) as a homebrew program. 1.3.0 is stable and playable: the
whole game can be played through, at 540p and 30 fps at stock clocks, and at 720p and 30 fps with a mild overclock.
**No prebuilt files are
distributed**: you build the program and the shaders on your computer, from **your own copy** of the game (the two
Xbox 360 discs and Title Update 2).

## You need

- A Switch with **Atmosphère** and the Homebrew Menu, and a microSD card with **~15 GB** free (exFAT or FAT32).
- Images of **your** Dead Space 2 Xbox 360 discs: **Disc 1** and **Disc 2** as `.iso`, the **World** (En/Fr/Es) or
  **Russia** (En/Ru) release.
- **Title Update 2** for the same release: World `TU_12K226V_000000C000000.00000000000G3` (Media ID 2915C8F8), Russia
  `TU_12K226V_000000K000000.00000000000G5` (Media ID 2C199AEB). On an Xbox 360 it is in
  `Content/0000000000000000/454108DF/000B0000/`. Other regional TU2 packages are refused. Both releases run on the same
  program: after TU2 their code is identical.
- A computer with **macOS** (tested) or Linux (not tested), ~25 GB free and an internet connection.

## Step 1. Build ds2-nx.nro

About 20–30 minutes once (on an Apple M1 Pro) and ~3 GB of disk.

```
xcode-select --install                        # macOS only
brew install cmake ninja python git           # macOS (Homebrew); on Linux, your distribution's packages
# devkitPro: https://github.com/devkitPro/pacman/releases (macOS pkg) or https://devkitpro.org/wiki/Getting_Started
sudo dkp-pacman -S switch-dev deko3d
git clone --branch v1.3.0 https://github.com/VasilkovND/ds2-nx
cd ds2-nx
deadspace2/tools/build_mac.sh                 # -> deadspace2/out/sw/deadspace2.nro
```

## Step 2. Prepare the game files

```
python3 deadspace2/tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
```

(For the Russian release, pass its images and `TU_12K226V_000000K000000.00000000000G5`.) It checks every file
against its hash and extracts both discs (5–15 minutes) into `game/disc1`, `game/disc2` and
`game/tu`.

## Step 3. Shaders from your copy

1. Put `deadspace2.nro` (renamed `ds2-nx.nro`) and the contents of `game` into `sdmc:/switch/ds2-nx/` and run it once
   (any game holding **R** → Homebrew Menu → **Dead Space 2**). The screen stays black, which is expected: there are no
   shaders yet. Wait **2–3 minutes** (a ~40 s pause while loading is the files being written), then close it with
   HOME. The game creates all its shaders at startup and the program writes them to
   `sdmc:/switch/ds2-nx/shaders_dump/` (~1,300 files).
2. Copy `shaders_dump` to the computer, install the **Vulkan SDK** (https://vulkan.lunarg.com: `dxc`, `spirv-val`)
   and, from `ds2-nx`, run (~10 minutes):

   ```
   DKSH=1 shaders/build_library.sh <path to shaders_dump> shaders_out
   ```

   (On Linux add `VULKAN_BIN=<folder with dxc and spirv-val>`.) This writes `shaders_out/ds2_shaders.dslib` and
   `shaders_out/shadercache_dksh.bin`.

## Step 4. Copy to the SD card

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro             step 1
    ds2_shaders.dslib      step 3
    shadercache_dksh.bin   step 3
    tu/      default.xex  default.xexp                     step 2
    disc1/   default.xex  bigfile0.viv  bigfile1.viv  ...   step 2
    disc2/   default.xex  bigfile0.viv  bigfile1.viv  ...   step 2
```

`shaders_dump/` can be deleted. Russian release: the game takes its language from the console's, and the program
reports English by default; for Russian, put a `ds2.toml` with the line `user_language = 12` next to the NRO. Saves go to `sdmc:/switch/ds2-nx/ds2/`, logs to `sdmc:/switch/ds2-nx/logs/`.

## Run

Start any installed game **while holding R** (title takeover: full memory; the album/applet mode does not have
enough) and choose **Dead Space 2** in the Homebrew Menu. **ZL + ZR + D-pad Up** shows or hides the FPS counter.

**Options menu: ZL + ZR + Minus** while playing (the game ignores the controller while it is open). **Up / Down**
choose the row, **Left / Right** change it, **A** saves, **B** closes. **RESOLUTION**: **720P** (1280x720, overclocked
consoles) or **540P** (960x540, stock clocks), kept in `ds2_resolucion.txt` next to the NRO and applied at the **next
start**; without one, 540p below a 1.5 GHz CPU and 720p above. **VIBRATION**: 0 / 25 / 50 / 75 / 100 % (50 % by
default), felt at once, kept in `ds2_vibracion.txt`. In handheld mode the program asks for Horizon's own 460.8 MHz GPU profile (as commercial
games do; not an overclock). 1.3.0 is stable and playable: the whole game can be played through. At stock clocks it
runs at 540p and a stable 30 fps (the game's cap). With a mild overclock (e.g. sys-clk) it runs at 720p, also at 30 fps.
If something goes wrong, send the whole `sdmc:/switch/ds2-nx/logs/` folder (with
`logs/rex/`) and say where and what happened: the first log lines record the build, the firmware and how it was started.

---

Unofficial fan project, not affiliated with or endorsed by Electronic Arts, Visceral Games, Microsoft or Nintendo.
"Dead Space" is a trademark of Electronic Arts. You must own the game. Source code: https://github.com/VasilkovND/ds2-nx
