# Guitar Duels API (`/api/v1/duels`)

Серверная часть режима **Guitar Duels**: онлайн-дуэли, матчи с ботом,
доверенный расчёт HP/урона/победителя, XP, Elo-рейтинг, история и лидерборд.

Клиентский движок правил (`GuitarBattle/Source/Gameplay/DuelEngine.h`) и
серверный `app/services/duel_rules.py` реализуют одни и те же правила.
**Клиент не присылает HP, урон, счёт или победителя.** Он присылает только
пары `(note_index, quality)`, всё остальное сервер считает сам.

- Базовый URL: `https://fretfight.online/api/v1` (в разработке
  `http://localhost:8000/api/v1`).
- Авторизация REST: `Authorization: Bearer <access_token>`.
- Авторизация WebSocket: `?token=<access_token>` (браузер не умеет ставить
  заголовок на handshake). Токен не логируется.
- `rules_version = 2`; матч хранит версию, с которой он создан.
  Версия 2 подняла урон до клиентского баланса 1.2.0 (0.10 за очко, зажим
  `[1.0, 10.0]`); матчи версии 1 остаются в истории как есть.

---

## 1. Правила дуэли (rules_version = 2)

### 1.1 Качества нот

| quality | Очки |
|---|---:|
| `perfect` | 100 |
| `great` | 85 |
| `good` | 70 |
| `miss` | 0 |

### 1.2 Один обмен (exchange)

Для каждой ноты чарта сравниваются качества двух сторон:

| Ситуация | exchange | Урон |
|---|---|---|
| Оба `miss` | `both_miss` | 0 |
| Одинаковое качество, оба > 0 | `block` | 0 |
| Игрок лучше | `player_hit` | игрок наносит урон сопернику |
| Соперник лучше | `opponent_hit` | соперник наносит урон игроку |

Урон:

```text
damage = clamp(abs(points_a - points_b) * 0.10, 1.0, 10.0)
```

- `perfect` vs `good`: `30 * 0.10 = 3.0`.
- `perfect` vs `miss`: `100 * 0.10 = 10.0` (максимум).
- `great` vs `good`: `15 * 0.10 = 1.5`.
- HP стартует со `100`, после каждого обмена зажимается в `[0, 100]`.
- При HP `0` сторона сразу проигрывает. Если чарт закончился без нокаута,
  побеждает сторона с большим HP; равный HP — ничья.

### 1.3 Бот

Бот детерминирован: `seed` + `note_index` всегда дают то же качество.
Порт C++ `duelBotHash` (32-битная арифметика) находится в
`duel_bot_hash`/`duel_bot_quality`.

| level | Имя | Hit rate | Perfect | Great |
|---|---|---:|---:|---:|
| `easy` | RUSTY | 0.25 | 0.20 | 0.30 |
| `normal` | MIDNIGHT | 0.65 | 0.35 | 0.35 |
| `hard` | VOLT | 0.90 | 0.55 | 0.30 |

Бот не присылает события. Сервер генерирует его партию на лету теми же
правилами. Рейтинг в матче с ботом **не меняется**; XP начисляется.

### 1.4 XP

Формула совпадает с клиентским `DuelMatch::getRewards`:

```text
xp = 20 + perfect*5 + great*3 + good*2 + (победа ? 40 : 0)
```

- Участие даёт 20 XP даже при поражении.
- Техническое поражение без единого разрешённого хода (выход/`finish` до
  первой ноты) XP не даёт — иначе бот-матчи можно было бы фармить выходами.
- Ничья не даёт бонус победы.
- XP пишется в `users.xp_total`, уровень пересчитывается существующей
  кривой `level_for_xp`.
- Один матч начисляет XP ровно один раз: повторный `finish` идемпотентен.

### 1.5 Elo

- Только `mode = online` и **только человек против человека**.
- `K = 32`, стартовый рейтинг `1000`.
- `expected_a = 1 / (1 + 10^((rating_b - rating_a)/400))`.
- `delta = round(K * (score_a - expected_a))`, `score_a` = 1 / 0.5 / 0.
- Сторона B получает `-delta`; сумма рейтингов сохраняется (кроме зажима на 0).
- `rating_before` фиксируется при входе в очередь/комнату, `rating_after` —
  при завершении. Текущий рейтинг игрока — `rating_after` последнего
  завершённого online-матча; до первого матча — `1000`.

---

## 2. Жизненный цикл матча

```text
waiting ──(оба ready)──> playing ──(finish/нокаут/конец чарта)──> finished
   │                         │
   └──(leave/нет соперника)──┴──> cancelled
```

- `waiting`: бот-матч ждёт `ready` игрока; online-матч ждёт второго игрока
  и/или `ready`.
- `playing`: `started_at = now + 3000 ms` (отсчёт). Нота 0 чарта приходится
  на `started_at`.
- `finished`: есть результаты, XP и Elo.
- `cancelled`: очередь отменена или игрок ушёл до старта.

Очередь online:

- Поиск идёт по `song_id`, режиму `online`, статусу `waiting` и рейтинговому
  окну. Стартовое окно ±200, растёт на 20 за секунду ожидания, максимум 1000.
- Если за `30` секунд пары нет, `POST /ready` возвращает `409 no_opponent`,
  матч переходит в `cancelled`; клиент может предложить бота.
- Пара выбирается в БД через `FOR UPDATE OF duel_matches SKIP LOCKED`, поэтому
  два одновременных игрока не займут одну комнату.

---

## 3. Схемы

### 3.1 `DuelSongInfo`

```json
{
  "id": "5f0f1c1e-5e0a-4a7e-9a3c-8a1b2c3d4e5f",
  "slug": "pridumay-svetly-mir",
  "title": "Придумай светлый мир",
  "artist": "Example",
  "bpm": 120,
  "difficulty": 3,
  "duration_seconds": 90,
  "time_signature_numerator": 4,
  "time_signature_denominator": 4
}
```

### 3.2 `DuelParticipantInfo`

```json
{
  "side": "player",
  "user_id": "6a1b2c3d-4e5f-6789-abcd-ef0123456789",
  "username": "player_1",
  "display_name": "Player One",
  "is_bot": false,
  "bot_name": null,
  "bot_avatar": null,
  "ready": true,
  "connected": true,
  "left": false,
  "hp": 97.0,
  "perfect": 10,
  "great": 2,
  "good": 1,
  "miss": 3,
  "notes": 16,
  "accuracy": 86.56,
  "damage_dealt": 7.4,
  "damage_taken": 2.1,
  "xp_awarded": 76,
  "rating_before": 1000,
  "rating_after": 1016,
  "rating_delta": 16,
  "result": "win"
}
```

### 3.3 `DuelMatchState`

```json
{
  "match_id": "9b8c7d6e-5f4a-3210-9fed-cba987654321",
  "mode": "online",
  "status": "playing",
  "rules_version": 2,
  "seed": 3141592653,
  "bot_level": null,
  "song": { "...": "DuelSongInfo" },
  "note_count": 180,
  "next_note_index": 12,
  "your_side": "player",
  "starts_in_ms": -4200,
  "server_time_ms": 4200,
  "waiting_seconds": 4,
  "waiting_expired": false,
  "winner_user_id": null,
  "winner_side": null,
  "created_at": "2026-11-07T12:00:00Z",
  "started_at": "2026-11-07T12:00:03Z",
  "finished_at": null,
  "participants": [
    { "...": "DuelParticipantInfo" },
    { "...": "DuelParticipantInfo" }
  ]
}
```

- `starts_in_ms`: сколько осталось до старта (отрицательное после старта).
- `server_time_ms`: серверное время относительно `started_at`; во время
  отсчёта отрицательное.
- `waiting_seconds`, `waiting_expired`: только для `waiting`.
- В широковещательных WS-сообщениях `your_side` может быть `null`; сторону
  можно определить по `participants[].user_id`.

---

## 4. REST

### 4.1 Создать матч

`POST /duels`

С ботом:

```json
{ "song_id": "5f0f1c1e-...", "mode": "bot", "bot_level": "easy" }
```

Online (в очередь):

```json
{ "song_id": "5f0f1c1e-...", "mode": "online" }
```

Ответ `201` — `DuelMatchState`. Для online, если найден ждущий соперник,
возвращается его матч, а `your_side = "opponent"`. Если пары нет — создаётся
новая комната, `your_side = "player"`.

Ошибки: `404 song_not_found`, `422 song_not_published`,
`422 bot_level_required` (Pydantic), `429 rate_limited`.

### 4.2 Войти в матч

`POST /duels/{match_id}/join`

Добавляет игрока в `waiting` online-матч на сторону `opponent`. Повторный
вызов тем же игроком идемпотентен. Ошибки: `403 not_participant` (не
участник, но это не join), `409 not_joinable`, `409 match_not_waiting`,
`409 match_full`, `409 no_opponent`.

### 4.3 Состояние

`GET /duels/{match_id}`

Только для участника. Возвращает `DuelMatchState`. На этом же эндпоинте
клиент после переподключения получает актуальные HP и `next_note_index`.

### 4.4 Готовность

`POST /duels/{match_id}/ready`

```json
{
  "match_id": "...",
  "status": "playing",
  "started_at": "2026-11-07T12:00:03Z",
  "starts_in_ms": 3000
}
```

- Бот-матч стартует сразу.
- Online стартует, когда `ready` прислали оба.
- До старта `ready` можно повторять; после старта ответ идемпотентен.
- Если online-пара не нашлась за 30 секунд: `409 no_opponent`.

### 4.5 События нот

`POST /duels/{match_id}/events`

```json
{
  "events": [
    { "note_index": 12, "quality": "perfect", "client_time_ms": 6100 },
    { "note_index": 13, "quality": "great", "client_time_ms": 6600 },
    { "note_index": 14, "quality": "miss", "client_time_ms": 7100 }
  ]
}
```

- Максимум 100 событий в батче.
- `quality`: `perfect` / `great` / `good` / `miss`.
- `client_time_ms` необязателен и служит только диагностикой.
- `note_index` — индекс ноты в **замороженном чарте** матча.
- Повтор `(match, user, note_index)` не применяется второй раз: он попадает
  в `duplicates`.
- Ответ `200`:

```json
{
  "match": { "...": "DuelMatchState" },
  "accepted": [12, 13],
  "duplicates": [14],
  "rejected": [
    {
      "index": 0,
      "note_index": 500,
      "code": "event_out_of_range",
      "message": "note_index is outside the chart"
    }
  ],
  "exchanges": [
    {
      "note_index": 12,
      "exchange": "player_hit",
      "quality": { "player": "perfect", "opponent": "good" },
      "damage_to_player": 0.0,
      "damage_to_opponent": 0.9,
      "player_hp": 100.0,
      "opponent_hp": 99.1,
      "player_stats": { "...": "counters" },
      "opponent_stats": { "...": "counters" }
    }
  ],
  "finished": false
}
```

Коды `rejected[].code`: `event_out_of_range`, `invalid_quality`,
`invalid_note_index`, `event_too_early`, `event_too_late`.

### 4.6 Завершить

`POST /duels/{match_id}/finish`

```json
{
  "match": { "...": "DuelMatchState" },
  "already_finished": false,
  "waiting_for_opponent": true
}
```

- Бот-матч завершается сразу; оставшиеся ноты считаются `miss`.
- **`finish` без единого отправленного события — техническое поражение
  (0 XP)**, а не способ форсировать ничью/фармить участие. Это правило
  действует и для бота, и для online.
- Online: первый `finish` только фиксируется (`waiting_for_opponent: true`).
  Матч завершается, когда `finish` прислали оба, когда все ноты разрешены,
  или когда соперник ушёл.
- Повторный `finish` возвращает `already_finished: true` и **не** начисляет
  XP/рейтинг второй раз.
- Конец чарта и нокаут также завершают матч автоматически.

### 4.7 Покинуть

`POST /duels/{match_id}/leave`

- `waiting` → матч `cancelled`.
- `playing` и у игрока нет ни одного отправленного события → техническое
  поражение.
- `playing` и события были → победитель по текущему HP.
- Повторный вызов идемпотентен.

### 4.8 Результат

`GET /duels/{match_id}/result`

`200` с `DuelMatchState` только для `finished`; иначе `409 match_not_finished`.
Участник получает полный результат: HP, счётчики, урон, XP, рейтинг.

### 4.9 История

`GET /duels/history?limit=20&offset=0&mode=bot|online&result=win|loss|draw`

```json
{
  "items": [
    {
      "match_id": "...",
      "mode": "online",
      "status": "finished",
      "song": { "...": "DuelSongInfo" },
      "opponent": { "...": "DuelParticipantInfo" },
      "your_side": "player",
      "hp": 100.0,
      "opponent_hp": 88.0,
      "result": "win",
      "xp_awarded": 76,
      "rating_delta": 16,
      "created_at": "2026-11-07T12:00:00Z",
      "finished_at": "2026-11-07T12:02:10Z"
    }
  ],
  "total": 1,
  "limit": 20,
  "offset": 0
}
```

### 4.10 Лидерборд

`GET /duels/leaderboard?period=week|month|all&limit=20&offset=0`

Период фильтрует завершённые **online**-матчи по `finished_at`; боты в
лидерборд не входят. Сортировка: победы, XP, очки точности, имя.

```json
{
  "period": "week",
  "items": [
    {
      "rank": 1,
      "user_id": "...",
      "username": "player_1",
      "display_name": "Player One",
      "level": 4,
      "rating": 1016,
      "duels": 5,
      "wins": 4,
      "losses": 1,
      "draws": 0,
      "win_rate": 80.0,
      "xp_earned": 420,
      "accuracy": 87.25,
      "damage_dealt": 54.2
    }
  ],
  "total": 1,
  "limit": 20,
  "offset": 0
}
```

### 4.11 Профиль игрока

`GET /duels/players/{user_id}` (нужен любой валидный токен)

```json
{
  "user_id": "...",
  "username": "player_1",
  "display_name": "Player One",
  "level": 4,
  "rating": 1016,
  "peak_rating": 1032,
  "online": {
    "duels": 5, "wins": 4, "losses": 1, "draws": 0, "win_rate": 80.0,
    "xp_earned": 420, "accuracy": 87.25, "damage_dealt": 54.2,
    "best_win_streak": 3
  },
  "bot": {
    "duels": 2, "wins": 1, "losses": 1, "draws": 0, "win_rate": 50.0,
    "xp_earned": 140, "accuracy": 71.0, "damage_dealt": 18.4,
    "best_win_streak": 1
  },
  "recent_matches": [ { "...": "DuelHistoryItem" } ]
}
```

### 4.12 Мой профиль

`GET /duels/me` — тот же профиль плюс `active_match`
(`DuelMatchState | null`) для `waiting`/`playing`.

---

## 5. WebSocket

`ws(s)://<host>/api/v1/duels/{match_id}/ws?token=<access_token>`

Один матч — одна комната. Переподключение разрешено: повторный `join` на
`playing` возвращает актуальное состояние (HP, счёт, `next_note_index`).

### 5.1 Клиент → сервер

```json
{ "type": "join" }
{ "type": "ready" }
{ "type": "note", "note_index": 12, "quality": "perfect", "client_time_ms": 6100 }
{ "type": "events", "events": [ { "note_index": 13, "quality": "great" } ] }
{ "type": "finish" }
{ "type": "leave" }
{ "type": "ping" }
```

`note` и `events` проходят ту же валидацию, что REST. Сервер отвечает `ack`:

```json
{ "type": "ack", "accepted": [12], "duplicates": [], "rejected": [] }
```

### 5.2 Сервер → клиент

```json
{ "type": "connected", "match": { "...": "DuelMatchState" }, "your_side": "player", "reconnected": false }
{ "type": "join", "match_id": "...", "user_id": "...", "side": "opponent" }
{ "type": "ready", "match_id": "...", "side": "player" }
{ "type": "countdown", "match_id": "...", "seconds": 3, "starts_in_ms": 3000 }
{ "type": "start", "match_id": "...", "started_at": "2026-11-07T12:00:03Z", "countdown_ms": 3000, "rules_version": 2, "seed": 123, "note_count": 180 }
{ "type": "note", "match_id": "...", "note_index": 12, "exchange": "player_hit", "quality": { "player": "perfect", "opponent": "good" }, "damage_to_player": 0.0, "damage_to_opponent": 3.0, "player_hp": 100.0, "opponent_hp": 97.0, "player_stats": { "...": "..." }, "opponent_stats": { "...": "..." } }
{ "type": "block", "match_id": "...", "note_index": 13, "player_hp": 100.0, "opponent_hp": 97.0 }
{ "type": "hp", "match_id": "...", "note_index": 13, "player_hp": 100.0, "opponent_hp": 97.0, "player_stats": { "...": "..." }, "opponent_stats": { "...": "..." } }
{ "type": "state", "match": { "...": "DuelMatchState" } }
{ "type": "finish", "match_id": "...", "result": { "...": "DuelMatchState" } }
{ "type": "cancelled", "match_id": "..." }
{ "type": "ping" }
{ "type": "pong" }
{ "type": "error", "code": "rate_limited", "message": "Too many messages" }
```

- `note`, `block`, `hp` приходят на каждый разрешённый обмен. `block` —
  дополнительное событие для удобства анимации; основное состояние уже в
  `note`/`hp`.
- `state` приходит после `ready`, пачки событий, `finish`, `leave` и
  периодического тика. Это авторитетный снимок; клиент должен применять
  именно его, а не считать HP сам.
- После `finish` сервер закрывает комнату (код `1000`). Клиент может
  перечитать результат через `GET /duels/{id}/result`.
- При бездействии сервер раз в ~20 секунд шлёт `ping`; клиент отвечает
  `pong` (или любым сообщением).
- Если сокет закрылся, у стороны есть **30 секунд** на переподключение.
  После этого матч завершается как `leave` (техническое поражение или по HP).

### 5.3 Ошибки WebSocket

| code | Причина |
|---|---|
| `unauthorized` | нет/просрочен `token` |
| `match_not_found`, `not_participant` | матч не найден или чужой |
| `invalid_json`, `invalid_message` | тело не JSON-объект |
| `message_too_large` | больше 64 KiB |
| `invalid_events`, `batch_too_large` | пустой список / > 100 событий |
| `rate_limited` | превышен лимит сообщений |
| `match_finished`, `match_not_started`, `participant_left` | неверное состояние |
| `unknown_message` | неизвестный `type` |

Коды закрытия: `4401` — не авторизован, `4403` — не участник,
`4404` — матч не найден.

---

## 6. Античит

Сервер не доверяет клиенту ни в одном из следующих пунктов:

1. **HP, урон, счёт, победитель** не принимаются от клиента вообще. Их
   считает `app/services/duel_rules.py` по событиям.
2. **Индекс ноты** должен быть в `[0, note_count)`. `note_count` и времена
   нот замораживаются в `duel_matches.note_times_ms` при создании матча;
   повторный анализ/замена MIDI не двигает чарт идущего матча.
3. **Время события** проверяется по серверным часам относительно
   `started_at`:
   - реальный чарт: `[-1500, +4000]` мс от позиции ноты;
   - песня без MIDI-анализа: синтетические 500 мс/нота и окно
     `[-3000, +15000]` мс.
   Слишком раннее/позднее событие отклоняется; пропущенная нота при
   разрешении становится `miss`.
4. **Идемпотентность**: `UNIQUE(match_id, user_id, note_index)` в
   `duel_events`. Повтор и переподключение не применяют ноту дважды.
5. **Строгий порядок разрешения**: обмены применяются по возрастанию
   `note_index` (`next_note_index` в матче). События на будущие ноты лежат
   в БД и ждут своей очереди; обойти порядок нельзя.
6. **Rate limit** (в памяти процесса):
   - создание матчей: burst 5, далее ~0.2/с на пользователя;
   - события: 40/с, burst 120, батч ≤ 100, стоимость батча
     `max(1, len/10)`;
   - WebSocket: 20 сообщений/с, burst 60 на `(match, user)`.
7. **Блокировки**: все мутации матча берут `SELECT ... FOR UPDATE` на
   `duel_matches`; два `finish` не начислят XP/рейтинг дважды.
8. **Очередь**: `FOR UPDATE OF duel_matches SKIP LOCKED`; окно рейтинга
   расширяется по времени ожидания.
9. **`finish`/`leave` без единого отправленного события** — техническое
   поражение и 0 XP, чтобы нельзя было форсировать ничью или фармить участие.
10. **Приватность**: `GET /duels/{id}` и WS доступны только участнику;
    чужой матч отдаёт `not_participant`.
11. **Токены** не логируются приложением; в WS токен передаётся
    query-параметром, а фильтр `uvicorn.access` в `app/main.py` заменяет его
    значение на `***` в access-логах.

Ограничения текущей реализации:

- Комнаты и rate limit живут в памяти процесса. `docker/entrypoint.sh`
  запускает один uvicorn worker; при переходе на несколько worker'ов нужен
  Redis/pub-sub.
- Для песен без проанализированного MIDI используется синтетический чарт
  (500 мс/нота, широкое окно). В продакшене у песни должен быть активный
  MIDI с анализом.
- `note_index` соответствует серверному порядку нот
  (`start_tick, track_index, channel, midi_note, id`). Клиент должен
  использовать тот же MIDI и ту же нумерацию; расхождение чартов приведёт к
  `event_too_early/late`.
- Качество ноты — единственный вход, который приходит от клиента. Сервер
  проверяет индекс, время, уникальность и лимиты, но не может доказать, что
  игрок действительно сыграл ноту идеально; для более строгого античита
  нужен подписанный реплей/аудиодоказательство.

---

## 7. Коды ошибок REST

Ошибки имеют вид:

```json
{ "detail": { "code": "no_opponent", "message": "No opponent found in time" } }
```

| HTTP | code | Когда |
|---:|---|---|
| 403 | `not_participant` | пользователь не участник матча |
| 404 | `match_not_found`, `song_not_found`, `user_not_found` | объект не найден |
| 409 | `not_joinable`, `match_not_waiting`, `match_full` | нельзя войти |
| 409 | `no_opponent` | очередь истекла |
| 409 | `match_closed`, `match_cancelled` | матч уже закрыт/отменён |
| 409 | `match_not_started` | `ready`/`events`/`finish` до старта |
| 409 | `match_finished` | события после завершения |
| 409 | `participant_left` | игрок уже вышел |
| 409 | `match_not_finished` | результат ещё не готов |
| 422 | `song_not_published` | песня не опубликована |
| 422 | `batch_too_large` | > 100 событий |
| 429 | `rate_limited` | превышен лимит |
| 401 | — | нет/просрочен access token |

---

## 8. Клиенту (api-sync)

Что должен сделать JUCE-клиент:

1. Заменить локальный `ONLINE MATCH - SOON` на реальный поток:
   `POST /duels` (`mode=online`) → дождаться `join`/`ready` → `POST /ready`.
2. При `no_opponent` предложить бота и создать `mode=bot`.
3. Для бота использовать серверный `seed` и `note_count`; **не** генерировать
   бота локально, иначе результаты разойдутся.
4. Отправлять `POST /events` или WS `note` после каждой разрешённой ноты.
   `note_index` — индекс в чарте, `quality` — `perfect/great/good/miss`.
5. Не отправлять HP/урон/счёт; отображать `player_hp`/`opponent_hp` из
   `note`/`hp`/`state`.
6. На `finish` показывать результат из `state`/`GET /result`, а не локальный.
7. Подключить WS `?token=` и обрабатывать `connected`, `start`, `countdown`,
   `note`, `hp`, `block`, `finish`, `ping`.
8. При обрыве — переподключиться в течение 30 секунд и получить `state`.
9. Бот-матчи не меняют рейтинг; показывать «RATING UNCHANGED».

---

## 9. База и миграция

Новая миграция: `migrations/versions/0021_duels.py`
(`down_revision = "0020_song_time_signature"`). Только добавление таблиц:

- `duel_matches` — матч, frozen seed/rules/chart, `winner_user_id`.
- `duel_participants` — сторона, HP, счётчики, урон, XP, рейтинг, результат.
- `duel_events` — сырые события, `UNIQUE(match_id, user_id, note_index)`.

Индексы:

- `ix_duel_matches_status_mode_created` — очередь;
- `ix_duel_matches_finished_at` — история и период лидерборда;
- `ix_duel_matches_song_id`;
- `ix_duel_participants_user_id` — история/профиль;
- `ix_duel_events_user_id`.

Существующие таблицы и миграции не меняются. Файлы:

```text
app/models/duel.py
app/schemas/duels.py
app/services/duel_rules.py
app/services/duel_service.py
app/services/duel_rooms.py
app/api/v1/duels.py
migrations/versions/0021_duels.py
tests/test_duel_rules.py
tests/test_duels_api.py
```

---

## 10. Проверка

```powershell
cd D:\GuitarBattle\GuitarBattleServer
& .venv-test\Scripts\python.exe -m compileall app
& .venv-test\Scripts\python.exe -m pytest tests/test_duel_rules.py -q
# при запущенной тестовой PostgreSQL на 127.0.0.1:55432:
& .venv-test\Scripts\python.exe -m pytest tests/test_duels_api.py -q
```

Тестовая БД поднимается так (если Docker доступен):

```powershell
docker run --rm -d --name guitarbattle-test-db `
  -e POSTGRES_USER=guitarbattle_test `
  -e POSTGRES_PASSWORD=guitarbattle_test `
  -e POSTGRES_DB=guitarbattle_test `
  -p 127.0.0.1:55432:5432 postgres:16-alpine
```

`tests/test_duels_api.py` проверяет бота, идемпотентность событий и `finish`,
online-пару, Elo/лидерборд, `no_opponent`, `leave` (техническое поражение и
по HP) и WebSocket handshake. Без БД эти тесты штатно пропускаются.
