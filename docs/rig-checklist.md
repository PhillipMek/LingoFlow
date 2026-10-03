# Run-sheet площадки — первый живой SoundGrid-сервер (2026-10-03)

Русский документ для человека на площадке. Цель: закрыть открытые device-чеки задач
**004 (маршрутизация), 005 (loopback), 006 (gain на слух)** и собрать реальные измерения
для **012/018** (размер jitter-буфера, бюджет задержки) и **023** (материал обрывов).

Правило на всю площадку: **ASIO-устройство эксклюзивно**. Перед каждым замером —
закрыть SoundGrid QRec и Driver Control Panel (на этой PC именно они держали драйвер,
и тот отказывал: «the driver reported none at all»). Control Panel нужен только один раз,
чтобы указать сервер; затем закрыть.

Все прогоны сохранять в файл и привезти обратно: `... *> loopback_005.txt`
(PowerShell перенаправляет весь поток вывода).

## 0. Подготовка

1. Драйвер Waves SoundGrid ASIO установлен (на dev-PC: 16.5.197.301; версия не важна
   критично, важен сам факт).
2. В Driver Control Panel выбрать сервер и частоту дискретизации сервера — **48 kHz**
   (приложение просит 48 000; драйвер предложит то, что реально доступно).
3. Control Panel закрыть. Открыть PowerShell в папке с `lingoflow_asio_probe.exe`
   (Release: `D:\work\LingoFlow\build\src\Release\`).
4. С пульта подать сигнал (микрофон/синус/розовый шум) на назначенный канал SoundGrid
   «в звуковую карту» — дальше подберём канал экспериментально (шаг 4).

## 1. Перечисление (без загрузки драйвера)

```powershell
.\lingoflow_asio_probe.exe --list
.\lingoflow_asio_probe.exe --verify
```

Ожидание: устройство `Waves SoundGrid ASIO`, `VERIFY OK: 1 registration(s)...` —
1:1 с `HKLM\SOFTWARE\ASIO`. Записать: список устройств (на площадке их может быть больше).

## 2. Реальные возможности устройства — записать ВСЁ

```powershell
.\lingoflow_asio_probe.exe --probe "Waves SoundGrid ASIO" --start *> probe.txt
```

Заполнить таблицу (сравнить с безсерверными числами из `docs/device-defaults.md`:
32x32, rates 44.1/48/88.2/96 k, buffer **256 единственный**, latency in 384 / out 256):

| Параметр | На площадке |
|---|---|
| каналов in / out | ____ / ____ |
| имена каналов | ____ |
| доступные частоты | ____ |
| доступные размеры блока | ____ |
| latency in / out, сэмплы | ____ / ____ |
| xrun-счётчик | reported / not reported |
| активный конфиг после open (rate/block) | ____ / ____ |

## 3. Стабильность жизненного цикла на живом сервере

```powershell
.\lingoflow_asio_probe.exe --lifecycle "Waves SoundGrid ASIO" --cycles 20 *> lifecycle.txt
```

Ожидание: `LIFECYCLE OK`, exit 0, без зависаний и отказов (без сервера было 10 циклов —
тут 20).

## 4. Найти канал с консольным сигналом (закрывает 004 «audio routing»)

Итерировать candidate-входы, глядя на `in=` (пост gain, т.е. это уровень, который
ушёл бы в переводчик):

```powershell
.\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 10 --input 1 --output 3 --jitter 120
# in= ноль -> --input 2, и так до ненулевого. Оба нуля -> exit 1 LOOPBACK INCONCLUSIVE — это честно про маршрутизацию, не про код.
```

Имя канала у драйвера generic (`SoundGrid N`) — это поток драйвера, не патч пульта,
поэтому только эксперимент. Записать: **вход = ____**, **выход = ____** (выход — тот,
который слышен в мониторной цепи). Дальше везде подставить эти номера вместо `<IN>/<OUT>`.

## 5. Задача 005: loopback-чек

```powershell
.\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 15 --input <IN> --output <OUT> --jitter 120 *> loopback_005.txt
```

Отметить PASS только если всё подтверждено:
- [ ] аудио с пульта реально приходит (`in=` двигается);
- [ ] оно выходит на `<OUT>` и слышно в мониторе;
- [ ] `out=` повторяет `in=` с задержкой ≈120 мс (preroll);
- [ ] `underruns` и `overruns` = 0 при непрерывном сигнале;
- [ ] при паузе источника видно, как буфер доигрывает и re-primed'ится (не рассыпается).

## 6. Задача 006: ухи — gain, клип, отсутствие щелчков

```powershell
.\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 20 --input <IN> --output <OUT> --jitter 120 --gain-out -6 *> loopback_006a.txt
```

- [ ] `out=` примерно на 6 дБ тише `in=`, `appliedGain=` показывает `0.0/-6.0`;
- [ ] переход уровня плавный, без щелчка/tick.

Позитивная проверка клип-индикации (поднимать, пока не появится):

```powershell
.\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 20 --input <IN> --output <OUT> --jitter 120 --gain-in 18 *> loopback_006b.txt
```

- [ ] при большом `--gain-in` загорается `clip=`, а после закрытия печатаются счётчики
  `clipping after close` (toTranslator/toAudience > 0);
- записать, с какого усиления появился клип: ____.

Mute по CLI не проверяется (живое состояние движка, не флаг инструмента); кнопки — 014.

## 7. Измерения для 012/018: минимальный preroll под живой нагрузкой

Максимальная нагрузка на сервер/сеть (все каналы активны) и свип preroll'а:

```powershell
foreach ($j in 40,80,120,200,300) { .\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 30 --input <IN> --output <OUT> --jitter $j *>"jitter_$j.txt" }
```

Записать для каждой точки: underruns / overruns. **Минимальный preroll без dropouts = ____ мс.**
Это вход для подтверждения дефолта `translation.jitterBufferMs`: с 012 в коде стоит 250 мс —
инженерное число из живых фактов OpenAI (всплески дельт до ~2x realtime, дренаж после close
~4.7x realtime), на реальном железе оно ещё не проверялось; свип ниже решает, оставить его или
двигать — по цифрам, не по ощущениям.
Субъективно замерить «рот → слышно в мониторе» на 0 дБ gain — ориентир для бюджета 018.

## 8. Приложение: конфигурация и старт

С 012 приложение монтирует настоящую цепочку: OpenAI-бэкенд за reconnect-supervisor, а
`--smoke` остаётся офлайн (Null-бэкенд, сеть не трогает). Проверяем связку
settings → реальное устройство → живой перевод:

1. В `%APPDATA%\LingoFlow\config.json` задать: `audio.inputDeviceId` и
   `audio.outputDeviceId` = `Waves SoundGrid ASIO`, `audio.inputChannel`/`audio.outputChannel`
   = найденные номера, `diagnostics.writeLogFile: true`.
2. ```powershell
   .\LingoFlow.exe --smoke
   ```
   Ожидание: **exit 0**; в `%APPDATA%\LingoFlow\logs\lingoflow.log`:
   `opening the audio device selected in settings`, реальная частота/блок
   (если запрошенного 480 нет — залогированный fallback; записать, что предложили),
   `audio gains in effect`.
3. Основной чек 012 (он и есть REQUIRED checkpoint этого задачника): GUI без `--smoke`,
   подать живую речь на выбранный вход и держать 1–2 минуты. Ожидание:
   - лог: `translation: OpenAI backend mounted behind the reconnect supervisor`,
     `translation session: connected`, `session open: model=gpt-realtime-translate ...`,
     `translation streaming started`;
   - на выбранном выходе через ~секунд слышен перевод вместо/поверх речи (пока без NDI —
     только аудио; текст придёт в 013);
   - окно живёт, краха нет; закрыть окно — в логе `translation streaming stopped: N frames
     submitted...` (N > 0) и `translation session: closed` (после реального дренажа).
   Записать: время `connecting`→`connected`, когда перевод стал слышен относительно речи,
   были ли провалы/артефакты. Если сеть площадки не пускает WebSocket — это тоже ответ
   чина (в логе будет `translation session: reconnecting` и категории ошибок).

Отрицательная проверка на месте: выбрать в settings несуществующее ASIO-устройство →
приложение должно отказать по имени и выйти с кодом 2, не подменив устройство молча.

## 9. Если на площадке есть интернет (дополняет шаг 8 фактами прова)

Сетевой факт «глазами площадки»: WebSocket-соединение к `api.openai.com` может блокироваться
корпоративной/ивент-сетью. Ключ — в `HKCU\Environment\OPENAI_API_KEY` или переменной
`OPENAI_API_KEY` этого процесса; в файлы на ноуте ключ не класть.

```powershell
# сначала со sample-речью (mono PCM16 wav, лежит в temp-папке dev-PC: test_en_24k.wav)
.\lingoflow_openai_probe.exe test_en_24k.wav venue_out_ru.wav ru 24000 *> openai_venue.txt
# затем, если успели записать реальную речь пульта (моно, PCM16, 24/48 кГц):
.\lingoflow_openai_probe.exe real_venue.wav venue_out_ru2.wav ru 24000 *> openai_venue2.txt
```

Ожидание: exit 0, перевод слушается. Записать: время открытия сессии, первый
переведённый дельта-чанк, всего доставлено; если отказ — дословную линию ошибки
(для `docs/openai-realtime-protocol.md` / 012).

С 013 probe печатает и сам перевод одной строкой (`translated text (N chars): ...`) —
скопируйте её в захват. Отдельный пункт наблюдения для policy-числа settle (2500 мс):
на живом материале площадки видно, на каких паузах переводчик замолкает и успевает ли
settle разрезать линии там, где человек ждёт новую реплику. То же в приложении:
`diagnostics.logLevel: "debug"` в config.json добавит в лог линии `subtitle line
settled` — по ним видно, как часто и с какими паузами закрываются реплики.

## 10. Опционально, для 023 (материал, не чек): обрыв сервера в середине

Во время `--loopback ... --seconds 60` выдернуть кабель сервера. Записать: что печатает
инструмент, код выхода, есть ли зависание (если завис — Ctrl+C / taskkill и пометка,
как именно отпустило). Поведение приложения при обрыве сервера будет реализовано в 023 —
здесь только сырые факты.

## Что привезти обратно

- все `.txt`-захваты прогонов;
- `%APPDATA%\LingoFlow\logs\lingoflow.log`;
- `venue_out_ru*.wav`, если делали 9;
- заполненные таблицы из §2 и §7.

Числа в код не попадают (AGENTS.md 19) — только в `docs/device-defaults.md` как
датированные измерения.

## Файлы на ноут площадки

- `D:\work\LingoFlow\build\src\Release\lingoflow_asio_probe.exe` (шаги 1–7, 10);
- `D:\work\LingoFlow\build\src\LingoFlow_artefacts\Release\LingoFlow.exe` (шаг 8; с 012 это
  уже приложение с реальным OpenAI-бэкендом за supervisor'ом — при выбранном устройстве и
  наличии API-ключа оно переводит, окно и `--smoke` различаются: smoke не трогает сеть);
- `D:\work\LingoFlow\build\src\Release\lingoflow_openai_probe.exe` + `test_en_24k.wav` (шаг 9, опция);
- установщик **VC++ 2015-2022 x64 redistributable** — бинари линкуют динамический CRT;
- этот файл;
- для воспроизводимости: коммит, с которого собрано (см. git log, `task 011`).
  Если ноут площадки = эта dev-PC — ничего копировать не надо, всё уже на месте,
  кроме записей о закрытии QRec/Control Panel.
