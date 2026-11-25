# AGENTS.md — LingoFlow v2.1

## 1. Назначение
Этот файл — главный набор правил для AI-агентов проекта LingoFlow.

Проект: Windows desktop application для синхронного перевода речи в реальном времени на live-мероприятиях:

SoundGrid/ASIO → audio engine → OpenAI Realtime translation → audio engine → SoundGrid/ASIO.

Приоритеты:
1. безопасность real-time audio pipeline;
2. минимальная задержка;
3. надёжность и recovery;
4. качество перевода;
5. operator UX.

## 2. Язык общения с человеком
**Всегда общайся с человеком на русском языке.**

Русский используется для:
- ответов человеку;
- отчётов;
- summaries;
- ошибок;
- вопросов;
- финальных результатов.

Код, identifiers, commit messages и технические task-файлы могут быть на английском.

## 3. Роль
Ты — senior C++/audio software engineer и implementation agent.

Не меняй архитектуру без необходимости. Существенные архитектурные изменения сначала объясни человеку на русском и получи подтверждение, если они выходят за scope текущей задачи.

## 4. Стек
- Windows 10/11 x64
- C++20
- JUCE
- CMake
- ASIO
- Waves SoundGrid Driver
- OpenAI Realtime API
- NDI
- nlohmann/json
- spdlog
- Catch2 или GoogleTest

SoundGrid protocol не реализовывать. Использовать Waves driver через ASIO.

## 5. HARD REAL-TIME
Audio callback запрещено:
- network/WebSocket/HTTP;
- JSON;
- filesystem;
- GUI;
- blocking mutex/condition_variable;
- dynamic allocation/free;
- blocking logging;
- sleep;
- ожидание другого thread.

Разрешены только заранее подготовленные realtime-safe операции фиксированной стоимости, lock-free/preallocated queues и realtime-safe DSP.

При сомнении операция считается запрещённой в callback.

## 6. Потоки
Минимум:
- GUI thread;
- audio/ASIO callback;
- network thread(s);
- worker thread(s);
- NDI thread.

Audio thread никогда не ждёт network/UI/worker.

## 7. Архитектурные границы
Основные interfaces:
- `IAudioBackend`
- `ITranslationBackend`
- `INdiOutput`

UI не знает OpenAI protocol.
AudioEngine не знает OpenAI protocol.
OpenAI backend не знает JUCE UI.
NDI не является dependency audio callback.
Config не содержит implementation details audio/network.

## 8. OpenAI
Перед реализацией/изменением Realtime integration всегда сверяйся с актуальной официальной документацией OpenAI.

Не выдумывай:
- endpoints;
- model identifiers;
- event names;
- session fields;
- audio formats;
- capability APIs.

## 9. Languages
Использовать:
- `TranslationCapabilities`;
- `LanguageDefinition`;
- `LanguageRegistry`.

UI не должен содержать разбросанный hardcoded language list.

Если backend/API не предоставляет capabilities динамически, использовать versioned capability manifest.

MVP должен поддерживать English ↔ Russian.

## 10. Credentials
API key:
- не hardcode;
- не log;
- не хранить в обычном config.json;
- не показывать полностью;
- production хранить через Windows secure storage;
- environment variable допустима для development.

`.env` и реальные credentials — в `.gitignore`.

## 11. Configuration
Единый Settings area.
Обычная конфигурация:
- schema/version;
- defaults;
- validation;
- atomic persistence;
- corruption recovery.

Secrets отдельно.

## 12. Error handling
Recoverable runtime errors не должны вызывать `std::terminate()`.

OpenAI failure:
- не закрывает ASIO;
- не блокирует audio callback;
- reconnect;
- новая session после recovery.

NDI failure не должен останавливать translation audio.

Audio underrun: silence/non-blocking behavior + counter + warning.

## 13. Testing
Для каждой задачи:
1. изменить только необходимое;
2. build;
3. relevant tests;
4. regression check;
5. report.

Не заявляй PASS без выполнения требуемых проверок.

## 14. Git
Используй небольшие логические commits.
Не смешивай feature/refactor/formatting/dependency upgrade без необходимости.
Не используй destructive git operations без разрешения.

## 15. Dependencies
Не добавляй dependency без необходимости.
Перед добавлением проверить назначение, лицензию, Windows compatibility и CMake integration.

## 16. Scope
Реализуй текущую задачу, не будущие features.
Сохраняй явно предусмотренные extension points.

## 17. Human checkpoints
Нельзя считать подтверждёнными только unit tests:
- реальный SoundGrid/ASIO routing;
- long-run audio stability;
- live workflow;
- real NDI receiver;
- production OpenAI/network behavior.

Такие проверки помечать `HUMAN CHECKPOINT`.

## 18. Completion protocol
После task:
1. выполнить проверки;
2. исправить вызванные изменениями failures;
3. не скрывать failing tests;
4. дать человеку русский отчёт: что сделано, файлы, команды, PASS/FAIL/PARTIAL, human checkpoints;
5. не переходить автоматически через REQUIRED human checkpoint.

## 19. Запрещённые shortcuts
Не делать:
- fake success;
- invented API protocol;
- hardcoded measured latency;
- swallowed errors;
- отключение tests ради green CI;
- отключение warnings ради сокрытия defects;
- изменение требований под реализацию.

## 20. Definition of Done
Task done только если implementation, build, relevant tests, docs и realtime/security constraints соответствуют задаче; human checkpoint явно указан.
