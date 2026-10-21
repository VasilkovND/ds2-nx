# Установка ds2-nx 1.0.2 (Dead Space 2 для Nintendo Switch)

ds2-nx — нативный порт Dead Space 2 (Xbox 360) для Switch в виде homebrew-программы. **Готовый `.nro` не
распространяется.** Программа собирается у вас на компьютере из исходников и перекомпилированного кода игры, а
сама игра берётся **из вашей собственной копии**: два диска Xbox 360 и Title Update 2. Архив релиза содержит только
подготовленные шейдеры и скрипт для файлов игры.

Всего три шага: подготовить файлы игры, собрать программу, скопировать всё на карту.

## Что понадобится

| Что | Подробности |
| --- | --- |
| Nintendo Switch | С кастомной прошивкой **Atmosphère** и Homebrew Menu |
| Карта microSD | Свободно **~15 ГБ**, exFAT или FAT32 (самые большие файлы игры меньше 4 ГБ) |
| Образы ваших дисков | Dead Space 2 для Xbox 360, **Disc 1** и **Disc 2**, `.iso`, издание World (En/Fr/Es) |
| Title Update 2 | **`TU_12K226V_000000C000000.00000000000G3`** (Media ID 2915C8F8). На накопителе Xbox 360 лежит в `Content/0000000000000000/454108DF/000B0000/` |
| Компьютер | **macOS** (проверено) или Linux (не проверялось), ~20 ГБ свободного места, интернет для загрузки инструментов |
| Архив релиза | `ds2-nx-1.0.2.zip`: шейдеры, кэш шейдеров, `tools/stage_sd.py` |

Принимаются только эти версии: каждый файл проверяется по контрольной сумме. Перекомпилированный код сделан из
именно этого издания, с другим он не совпадёт. TU2 других регионов (другой Media ID) несут другой исполняемый файл.

## Шаг 1. Подготовить файлы игры

1. Установите **Python 3**: на macOS он ставится вместе с инструментами шага 2 (`brew install python`); на Linux —
   из пакетов дистрибутива.
2. Распакуйте `ds2-nx-1.0.2.zip`, положите рядом образы дисков и файл TU2 и откройте терминал в папке
   `ds2-nx-1.0.2`.
3. Запустите:

   ```
   python3 tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
   ```

   Если файлы лежат в другом месте, укажите полный путь к каждому в кавычках. Скрипт проверит обновление и оба
   диска и распакует их; это 5–15 минут. В конце он напишет `Ready: ...`, а в папке `game` появятся `disc1`,
   `disc2` и `tu`.

`disc1` и `disc2` — это просто всё содержимое образов, распакованное как есть. В `tu` скрипт кладёт `default.xex`
с первого диска и `default.xexp`, извлечённый из пакета обновления.

## Шаг 2. Собрать программу (ds2-nx.nro)

Сборка один раз занимает ~20–30 минут (на Apple M1 Pro) и ~3 ГБ места.

### macOS

1. **Инструменты Xcode:** `xcode-select --install`.
2. **Homebrew** (https://brew.sh), затем:

   ```
   brew install cmake ninja python git
   ```

3. **devkitPro:** установщик `devkitpro-pacman-installer.pkg` со страницы
   https://github.com/devkitPro/pacman/releases, затем:

   ```
   sudo dkp-pacman -S switch-dev deko3d
   ```

4. **Исходники и сборка:**

   ```
   git clone --branch v1.0.2 https://github.com/VasilkovND/ds2-nx
   cd ds2-nx
   deadspace2/tools/build_mac.sh
   ```

   Скрипт сам скачает перекомпилированный код игры (проект Dead-Space-2-Recomp, ровно ту версию, с которой собран
   этот релиз), библиотеки и SDK, применит наши правки и соберёт программу. Готовый файл:
   **`ds2-nx/deadspace2/out/sw/deadspace2.nro`**.

### Linux (не проверялось)

Те же шаги: devkitPro по инструкции https://devkitpro.org/wiki/Getting_Started, затем
`sudo dkp-pacman -S switch-dev deko3d`; `cmake`, `ninja`, `python3`, `git` — из пакетов дистрибутива; дальше
`git clone ...` и `deadspace2/tools/build_mac.sh`, как выше.

## Шаг 3. Скопировать на карту

Создайте на карте папку `switch/ds2-nx/` и положите в неё:

| Откуда | Что |
| --- | --- |
| Шаг 2 | `deadspace2.nro`, **переименовав его в `ds2-nx.nro`** |
| Архив релиза | `switch/ds2-nx/ds2_shaders.dslib` и `switch/ds2-nx/shadercache_dksh.bin` |
| Шаг 1 | Содержимое папки `game`: `disc1`, `disc2`, `tu` |

В итоге:

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro             собран на шаге 2
    ds2_shaders.dslib      из архива релиза
    shadercache_dksh.bin   из архива релиза
    tu/
        default.xex
        default.xexp
    disc1/
        default.xex  bigfile0.viv  bigfile1.viv  disc1_info.txt  ...
    disc2/
        default.xex  bigfile0.viv  bigfile1.viv  disc2_info.txt  ...
```

Сохранения игра создаст сама в `sdmc:/switch/ds2-nx/ds2/`, логи пишутся в `sdmc:/switch/ds2-nx/logs/`.

## Запуск

1. Вставьте карту и загрузите Atmosphère.
2. Запустите **любую установленную игру, удерживая кнопку R**: так откроется Homebrew Menu с полным доступом к
   памяти. Через альбом (режим апплета) памяти не хватит.
3. Выберите **Dead Space 2**.

Первый запуск дольше обычного. Счётчик FPS включается и выключается сочетанием **ZL + ZR + вверх на крестовине**.
С разгоном (например, sys-clk) игра идёт близко к 30 fps, на которых она ограничена; на стоковых частотах обычно
20–30 fps, в самых тяжёлых боях бывают просадки.

## Если что-то не так

| Сообщение или проблема | Что делать |
| --- | --- |
| `not the Title Update 2 this port needs` | Нужен TU2 с Media ID 2915C8F8, а не другой регион |
| `Disc 1 default.xex does not match` | Образ не того издания или повреждён: нужен World (En/Fr/Es) |
| `Not an Xbox 360 disc image` | Это не образ диска Xbox 360 (или он в другом формате) |
| `--disc1: ... not found` | Неверный путь к файлу: проверьте имя и кавычки |
| `devkitA64 not found` / `deko3d not installed` | Не выполнен пункт 3 сборки (devkitPro и `dkp-pacman -S switch-dev deko3d`) |
| Программа сразу закрывается | Проверьте, что на карте есть `tu/`, `disc1/`, `disc2/` и оба файла шейдеров, и что запуск идёт через игру с R |
| Стартовое видео (и другие ролики) дёргается | Известная проблема на стоковых частотах: игра декодирует видео программно на одном ядре, и ему не хватает частоты. Изображение при этом обновляется 30 раз в секунду. С разгоном видео идёт почти плавно |
| Другие проблемы | Приложите к сообщению об ошибке всю папку `sdmc:/switch/ds2-nx/logs/` (вместе с `logs/rex/`) и опишите, где и что произошло. В первой строке лога записаны версия сборки, прошивка и режим запуска |

---

# Installing ds2-nx 1.0.2 (Dead Space 2 for Nintendo Switch)

ds2-nx is a native Switch port of Dead Space 2 (Xbox 360) as a homebrew program. **No ready-made `.nro` is
distributed**: you build it on your computer from the sources and the recompiled game code, and the game itself comes
from **your own copy** (the two Xbox 360 discs and Title Update 2). The release archive holds only the prepared
shaders and the script for the game files.

## You need

- A Switch with **Atmosphère** and the Homebrew Menu, and a microSD card with **~15 GB** free (exFAT or FAT32).
- Images of **your** Dead Space 2 Xbox 360 discs: **Disc 1** and **Disc 2** as `.iso`, World (En/Fr/Es) release.
- **Title Update 2**: `TU_12K226V_000000C000000.00000000000G3` (Media ID 2915C8F8); on an Xbox 360 it is in
  `Content/0000000000000000/454108DF/000B0000/`. Other regional TU2 packages carry another executable and are refused.
- A computer with **macOS** (tested) or Linux (not tested), ~20 GB free and an internet connection.

## Step 1. Prepare the game files

Unzip `ds2-nx-1.0.2.zip`, put your disc images and the TU2 file next to it, open a terminal in `ds2-nx-1.0.2` and run:

```
python3 tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
```

It checks every file against its hash and extracts both discs (5–15 minutes). The `game` folder then holds `disc1/`,
`disc2/` and `tu/`.

## Step 2. Build ds2-nx.nro

About 20–30 minutes once (on an Apple M1 Pro) and ~3 GB of disk.

```
xcode-select --install                        # macOS only
brew install cmake ninja python git           # macOS (Homebrew); on Linux, your distribution's packages
# devkitPro: https://github.com/devkitPro/pacman/releases (macOS pkg) or https://devkitpro.org/wiki/Getting_Started
sudo dkp-pacman -S switch-dev deko3d
git clone --branch v1.0.2 https://github.com/VasilkovND/ds2-nx
cd ds2-nx
deadspace2/tools/build_mac.sh
```

The script fetches the recompiled game code (Dead-Space-2-Recomp, at the exact commit this release was built
against), the libraries and the SDK, applies our patches and builds `deadspace2/out/sw/deadspace2.nro`.

## Step 3. Copy to the SD card

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro             step 2's deadspace2.nro, renamed
    ds2_shaders.dslib      from the release archive
    shadercache_dksh.bin   from the release archive
    tu/      default.xex  default.xexp                 (step 1)
    disc1/   default.xex  bigfile0.viv  bigfile1.viv  ... (step 1)
    disc2/   default.xex  bigfile0.viv  bigfile1.viv  ... (step 1)
```

Saves go to `sdmc:/switch/ds2-nx/ds2/`, logs to `sdmc:/switch/ds2-nx/logs/`.

## Run

Start any installed game **while holding R** (title takeover: full memory; the album/applet mode does not have
enough) and choose **Dead Space 2** in the Homebrew Menu. **ZL + ZR + D-pad Up** shows or hides the FPS counter.
Overclocked (e.g. sys-clk) it runs close to the game's 30 fps cap; at stock clocks mostly 20–30 fps, with drops in the
heaviest fights. Known issue: at stock clocks the intro video and other full-motion videos stutter (the game decodes
them in software on one core); overclocked they play nearly smoothly. If something goes wrong, send the whole `sdmc:/switch/ds2-nx/logs/` folder (with `logs/rex/`) and
say where and what happened: the first log lines record the build, the firmware and how it was started.

---

Unofficial fan project, not affiliated with or endorsed by Electronic Arts, Visceral Games, Microsoft or Nintendo.
"Dead Space" is a trademark of Electronic Arts. You must own the game. Source code: https://github.com/VasilkovND/ds2-nx
