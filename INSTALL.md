# Установка ds2-nx 1.0.0 (Dead Space 2 для Nintendo Switch)

ds2-nx — нативный порт Dead Space 2 (Xbox 360) для Switch в виде homebrew-программы. В архиве релиза только
программа. **Сама игра берётся из вашей собственной копии**: два диска Xbox 360 и обновление Title Update 2.

## 1. Что понадобится

| Что | Подробности |
| --- | --- |
| Nintendo Switch | С кастомной прошивкой **Atmosphère** и Homebrew Menu |
| Карта microSD | Свободно **~15 ГБ**. Подойдёт exFAT или FAT32: самые большие файлы игры меньше 4 ГБ |
| Образы ваших дисков | Dead Space 2 для Xbox 360, **Disc 1** и **Disc 2**, файлы `.iso`, издание World (En/Fr/Es) |
| Title Update 2 | Файл **`TU_12K226V_000000C000000.00000000000G3`** (Media ID 2915C8F8) |
| Компьютер | Windows, macOS или Linux с **Python 3**, плюс ~15 ГБ свободного места |
| Архив релиза | `ds2-nx-1.0.0.zip` |

**Откуда взять файлы игры.** Образы `.iso` делаются с ваших собственных дисков. Title Update 2 Xbox 360 скачивает
сам, когда игра запускается онлайн. На накопителе консоли он лежит в
`Content/0000000000000000/454108DF/000B0000/`.

Программа принимает только эти версии. Каждый файл проверяется по контрольной сумме, а с другим изданием
перекомпилированный код просто не совпадёт. Обновления TU2 для других регионов (другой Media ID) содержат другой
исполняемый файл, поэтому не подходят.

## 2. Распаковать архив релиза

Распакуйте `ds2-nx-1.0.0.zip` в любую папку на компьютере. Внутри:

```
ds2-nx-1.0.0/
    switch/ds2-nx/ds2-nx.nro         программа
    switch/ds2-nx/ds2_shaders.dslib  шейдеры игры, переведённые для Switch
    switch/ds2-nx/shadercache_dksh.bin  те же шейдеры, заранее скомпилированные (без подтормаживаний)
    tools/stage_sd.py                скрипт, который готовит файлы игры
    INSTALL.md  CHANGELOG.md  LICENSE  THIRD_PARTY_NOTICES.md
```

## 3. Подготовить файлы игры (на компьютере)

1. **Установите Python 3** с https://www.python.org. На Windows при установке отметьте **«Add python.exe to PATH»**.
2. **Положите рядом три файла:** `Dead Space 2 (Disc 1).iso`, `Dead Space 2 (Disc 2).iso` и
   `TU_12K226V_000000C000000.00000000000G3`. Например, в папку `ds2-nx-1.0.0`.
3. **Откройте терминал в папке `ds2-nx-1.0.0`.** На Windows — правый клик по папке → «Открыть в терминале». На macOS —
   «Терминал», затем `cd` и перетащить папку в окно.
4. **Запустите** (на Windows вместо `python3` пишите `py`):

   ```
   python3 tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
   ```

   Если файлы лежат в другом месте, укажите полный путь к каждому в кавычках.

Скрипт проверит обновление и оба диска и распакует их. Это займёт 5–15 минут, в зависимости от диска компьютера.
В конце будет строка `Ready: ...`, а рядом появится папка `game` с тремя папками внутри: `disc1`, `disc2` и `tu`.

## 4. Скопировать на карту

1. Скопируйте папку **`switch`** из `ds2-nx-1.0.0` в **корень** карты. Если папка `switch` на карте уже есть,
   содержимое объединится.
2. Скопируйте **содержимое** папки `game` (`disc1`, `disc2`, `tu`) в **`switch/ds2-nx/`** на карте.

В итоге на карте должно быть:

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro
    ds2_shaders.dslib
    shadercache_dksh.bin
    tu/
        default.xex
        default.xexp
    disc1/
        default.xex  bigfile0.viv  bigfile1.viv  disc1_info.txt  ...
    disc2/
        default.xex  bigfile0.viv  bigfile1.viv  disc2_info.txt  ...
```

Сохранения игра создаст сама в `sdmc:/switch/ds2-nx/ds2/`, логи — в `sdmc:/switch/ds2-nx/logs/`.

## 5. Запуск

1. Вставьте карту и загрузите Atmosphère.
2. Запустите **любую установленную игру, удерживая кнопку R**: так откроется Homebrew Menu с полным доступом к
   памяти. Через альбом (режим апплета) памяти не хватит.
3. Выберите **Dead Space 2**.

Первый запуск дольше обычного. Счётчик FPS включается и выключается сочетанием **ZL + ZR + вверх на крестовине**.

**Производительность.** С разгоном (например, sys-clk) играется плавно, близко к 30 fps, на которых игра ограничена. На
стоковых частотах обычно 20–30 fps, в самых тяжёлых боях бывают просадки.

## Если что-то не так

| Сообщение или проблема | Что делать |
| --- | --- |
| `not the Title Update 2 this port needs` | Нужен TU2 с Media ID 2915C8F8, а не другой регион |
| `Disc 1 default.xex does not match` | Образ не того издания или повреждён: нужен World (En/Fr/Es) |
| `Not an Xbox 360 disc image` | Это не образ диска Xbox 360 (или он запакован в другой формат) |
| `--disc1: ... not found` | Неверный путь к файлу: проверьте имя и кавычки |
| Программа сразу закрывается | Проверьте, что на карте есть `tu/`, `disc1/`, `disc2/`, и что запуск идёт через игру с R |
| Другие проблемы | Приложите файлы из `sdmc:/switch/ds2-nx/logs/` к сообщению об ошибке |

---

# Installing ds2-nx 1.0.0 (Dead Space 2 for Nintendo Switch)

ds2-nx is a native Switch port of Dead Space 2 (Xbox 360) as a homebrew program. The release archive contains only
the program. **The game comes from your own copy**: the two Xbox 360 discs and Title Update 2.

## 1. You need

- A Switch with **Atmosphère** and the Homebrew Menu, and a microSD card with **~15 GB** free (exFAT or FAT32).
- Images of **your** Dead Space 2 Xbox 360 discs: **Disc 1** and **Disc 2** as `.iso`, World (En/Fr/Es) release.
- **Title Update 2**: `TU_12K226V_000000C000000.00000000000G3` (Media ID 2915C8F8). On an Xbox 360 it is in
  `Content/0000000000000000/454108DF/000B0000/`. Other regional TU2 packages carry another executable and are refused.
- A computer (Windows, macOS or Linux) with **Python 3** and ~15 GB free.

## 2. Prepare the game files

1. Install Python 3 from https://www.python.org. On Windows, tick **"Add python.exe to PATH"**.
2. Unzip `ds2-nx-1.0.0.zip`, open a terminal in `ds2-nx-1.0.0`, and run (on Windows use `py` instead of `python3`):

   ```
   python3 tools/stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" --disc2 "Dead Space 2 (Disc 2).iso" --tu "TU_12K226V_000000C000000.00000000000G3" -o game
   ```

   It checks every file against its hash, extracts both discs (5–15 minutes) and ends with `Ready: ...`. The `game`
   folder then holds `disc1/`, `disc2/` and `tu/`.

## 3. Copy to the SD card

Copy the archive's **`switch`** folder to the root of the card, then the **contents** of `game` into
**`switch/ds2-nx/`**:

```
sdmc:/switch/ds2-nx/
    ds2-nx.nro   ds2_shaders.dslib   shadercache_dksh.bin
    tu/      default.xex  default.xexp
    disc1/   default.xex  bigfile0.viv  bigfile1.viv  ...
    disc2/   default.xex  bigfile0.viv  bigfile1.viv  ...
```

Saves go to `sdmc:/switch/ds2-nx/ds2/`, logs to `sdmc:/switch/ds2-nx/logs/`.

## 4. Run

Start any installed game **while holding R** (title takeover: full memory; the album/applet mode does not have
enough), and choose **Dead Space 2** in the Homebrew Menu. The first start takes longer. **ZL + ZR + D-pad Up**
shows or hides the FPS counter.

Overclocked (e.g. sys-clk) it runs close to the game's 30 fps cap; at stock clocks it is mostly 20–30 fps, with drops
in the heaviest fights. If something goes wrong, include the files from `sdmc:/switch/ds2-nx/logs/` in your report.

---

Unofficial fan project, not affiliated with or endorsed by Electronic Arts, Visceral Games, Microsoft or Nintendo.
"Dead Space" is a trademark of Electronic Arts. You must own the game. Source code: https://github.com/VasilkovND/ds2-nx
