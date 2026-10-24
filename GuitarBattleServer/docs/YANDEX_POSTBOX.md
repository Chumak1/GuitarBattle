# Подключение Yandex Cloud Postbox к GuitarBattleServer

Документ описывает отправку транзакционных писем с кодами подтверждения и
восстановления пароля через Yandex Cloud Postbox. Отправитель:
`noreply@fretfight.online`, домен: `fretfight.online`.

> **Статус проверки.** Код и конфигурация подготовлены, но **реальная отправка
> не подтверждена**: в среде разработки нет доступа к консоли Yandex Cloud,
> API-ключу Postbox и DNS-панели домена. Проверено только сетевое подключение к
> `postbox.cloud.yandex.net` (порты 587/465, TLS 1.3, сервер предлагает
> `AUTH PLAIN LOGIN`). Аутентификация, отправка письма и прохождение
> SPF/DKIM/DMARC требуют ручного теста по разделу
> [«Проверка реальной отправкой»](#проверка-реальной-отправкой).

## 1. Что уже есть в проекте

- `app/services/email_service.py` — отправка через `smtplib` (стандартная
  библиотека Python), без внешних SDK.
- `app/api/v1/auth.py` — регистрация, повторная отправка кода, восстановление
  пароля; при ошибке доставки регистрация возвращает `503`.
- `app/config.py` — настройки `SMTP_*`.
- `docker-compose.yml` — прокидывает `SMTP_*` в контейнер `api`.
- `.env.example` — безопасные плейсхолдеры без реальных секретов.
- `scripts/check_smtp.py` — диагностика подключения/аутентификации и
  опциональная тестовая отправка. Скрипт не печатает `SMTP_PASSWORD`.

Поведение:

- `APP_ENV=test` — письма складываются в in-memory outbox для pytest.
- `APP_ENV=development` и пустой `SMTP_HOST` — текст письма пишется в лог
  контейнера `api` (это удобно локально, но код подтверждения нельзя логировать
  на публичном сервере).
- `APP_ENV=production` и пустой `SMTP_HOST` — регистрация возвращает `503`;
  fallback в лог отсутствует.
- После настройки `SMTP_HOST` письма отправляются через SMTP в любом окружении,
  кроме `test`.

## 2. Почему SMTP, а не API Postbox

Yandex Cloud Postbox поддерживает два способа отправки:

1. **SMTP** — `postbox.cloud.yandex.net`, порт 587 (STARTTLS) или 465 (SMTPS).
2. **AWS-совместимый API** — `https://postbox.cloud.yandex.net/v2/email/outbound-emails`
   с подписью AWS Signature V4 или заголовком `X-YaCloud-SubjectToken`.

Для текущего backend выбран **SMTP**:

- в проекте уже используется `smtplib`, не нужно добавлять AWS SDK или
  реализовывать SigV4;
- API-ключ Postbox работает как обычная пара username/password;
- меньше кода — меньше риска сломать существующий контракт `/auth/*`;
- для двух транзакционных писем (код подтверждения и сброс пароля) пропускной
  способности SMTP достаточно.

API имеет смысл, если понадобятся шаблоны, bulk-рассылки, вложения или
детальная статистика через AWS SDK. Тогда интеграцию можно добавить отдельным
сервисом, не меняя текущий SMTP-путь.

Параметры Postbox SMTP:

| Параметр | Значение |
|---|---|
| Host | `postbox.cloud.yandex.net` |
| Port + TLS | `587` + STARTTLS **или** `465` + implicit TLS (SMTPS) |
| Username | ID API-ключа сервисного аккаунта (scope `yc.postbox.send`) |
| Password | секретная часть этого API-ключа |
| From | любой адрес на подтверждённом домене, например `noreply@fretfight.online` |

Источники: [Sending an email](https://yandex.cloud/en/docs/postbox/operations/send-email),
[Quickstart](https://yandex.cloud/en/docs/postbox/quickstart).

## 3. Предпосылки

1. Аккаунт Yandex Cloud и **активный платёжный аккаунт** (`ACTIVE` или
   `TRIAL_ACTIVE`). Postbox — платный сервис.
2. Доступ к консоли Yandex Cloud и каталогу (folder), где будет создан адрес
   Postbox.
3. Права на создание сервисного аккаунта и назначение роли `postbox.sender`
   (или администратор, который это сделает).
4. Домен `fretfight.online` и доступ к DNS-провайдеру, который обслуживает
   авторитетную зону. **NS-серверы менять не нужно и нельзя в рамках этой
   задачи.**
5. Реальный тестовый почтовый ящик для проверки доставки.
6. Понимание текущих квот Postbox (по умолчанию): 200 писем за 24 часа,
   не более 1 письма в секунду, до 200 получателей на письмо, до 10 MB на
   письмо. Для публичного запуска почти наверняка потребуется увеличить квоту
   `postbox.emails_sent.count`. См.
   [Quotas and limits](https://yandex.cloud/en/docs/postbox/concepts/limits).

### Важное замечание про NS

На момент проверки (24.10.2026) публичные NS домена `fretfight.online`:
`ns1.yandexcloud.net` и `ns2.yandexcloud.net`, то есть зона, вероятно, уже
обслуживается Yandex Cloud DNS. Если вы ожидали другого DNS-провайдера,
проверьте делегирование у регистратора. В любом случае:

- **не меняйте NS-серверы**;
- **не переносите зону**;
- добавляйте записи туда, где сейчас лежит авторитетная зона: в Cloud DNS,
  если зона там, или в панель текущего DNS-провайдера.

Я не менял NS и не переносил зону; результат выше — только наблюдение.
Если вы уверены, что зона обслуживается другим провайдером, проверьте
делегирование у регистратора: возможно, NS изменились или ответ закэширован.
План работ от этого не меняется — записи добавляются в текущую авторитетную
зону.

Проверить NS можно так:

```powershell
Resolve-DnsName -Name fretfight.online -Type NS -Server 1.1.1.1
```

## 4. Шаг 1. Сервисный аккаунт и API-ключ

1. В консоли Yandex Cloud выберите каталог, в котором будет создан адрес
   Postbox. Сервисный аккаунт и адрес должны быть **в одном каталоге**, иначе
   отправка завершится ошибкой.
2. Создайте сервисный аккаунт, например `postbox-sender`.
3. Назначьте ему роль `postbox.sender`.
4. Создайте **API-ключ**:
   - при создании укажите scope `yc.postbox.send`;
   - сохраните **ID ключа** и **секретную часть**. Секрет показывается один раз;
   - ID ключа — это `SMTP_USERNAME`, секрет — `SMTP_PASSWORD`.

Не используйте для SMTP статический ключ доступа напрямую: для него нужен
отдельный скрипт `generate.py`, который преобразует секрет в SMTP-пароль.
IAM-токен тоже не подходит для постоянно работающего API: он живёт до 12 часов.

Официальная документация:
[Creating an address](https://yandex.cloud/en/docs/postbox/operations/create-address),
[Sending an email](https://yandex.cloud/en/docs/postbox/operations/send-email).

## 5. Шаг 2. Адрес Postbox

1. В консоли откройте **Cloud Postbox**.
2. Нажмите **Create address**.
3. В поле **Domain** укажите `fretfight.online` (без `@` и без `www`).
4. Выберите способ настройки DKIM:
   - **Easy (простая)** — рекомендуется: Postbox сам создаёт ключи и ротирует
     их; в консоли появятся **две CNAME-записи**.
   - **Advanced (расширенная)** — вы сами генерируете ключ (1024 или 2048 бит)
     и указываете selector; в консоли появится **одна TXT-запись**.
5. Опционально ограничьте список отправителей (allowed senders) адресом
   `noreply@fretfight.online` — это уменьшает риск злоупотребления адресом.
6. Опционально включите логирование и статистику.
7. Нажмите **Create address**.

После создания откройте адрес и раздел **Email signature configuration (DKIM)**.
Именно там находятся реальные значения DNS-записей. **Не выдумывайте их и не
копируйте из этого документа** — в документе только плейсхолдеры.

## 6. Шаг 3. DNS-записи

Добавьте записи у текущего DNS-провайдера. Если зона в Yandex Cloud DNS —
в консоли **Cloud DNS**; если у другого провайдера — в его панели. NS не
меняем.

### 6.1. Подтверждение домена и DKIM

В Postbox отдельного «verification TXT token» в официальной документации нет:
подтверждение владения доменом выполняется публикацией DKIM-записей. После
добавления записей статус на странице адреса должен смениться на `Success`.
Если консоль показывает **дополнительную** запись подтверждения — добавьте её
точно так, как показано в консоли; не подставляйте значения из этого документа.

#### Вариант Easy DKIM (две CNAME)

| Тип | Имя | Значение | Назначение | Где взять |
|---|---|---|---|---|
| CNAME | `<DKIM_CNAME_1_NAME_ИЗ_КОНСОЛИ>` (обычно `<selector>._domainkey`) | `<DKIM_CNAME_1_VALUE_ИЗ_КОНСОЛИ>` | Публикация DKIM-ключа и подтверждение домена | Postbox → адрес → **Email signature configuration (DKIM)** |
| CNAME | `<DKIM_CNAME_2_NAME_ИЗ_КОНСОЛИ>` (обычно второй `<selector>._domainkey`) | `<DKIM_CNAME_2_VALUE_ИЗ_КОНСОЛИ>` | Вторая запись для автоматической ротации ключей | Там же |

Обе CNAME-записи обязательны. Их нельзя удалять после успешной проверки:
Postbox периодически меняет DKIM-ключи, и CNAME-записи делегируют ему право
публиковать новый открытый ключ. Во время ротации TXT-ключ может быть доступен
по обеим CNAME-записям, в остальное время — по одной. Это нормально.

#### Вариант Advanced DKIM (одна TXT)

| Тип | Имя | Значение | Назначение | Где взять |
|---|---|---|---|---|
| TXT | `<DKIM_SELECTOR>._domainkey` (например, `postbox._domainkey`, если вы выбрали selector `postbox`) | `<DKIM_TXT_VALUE_ИЗ_КОНСОЛИ>` (начинается с `v=DKIM1;h=sha256;k=rsa;p=`) | Публикация открытого DKIM-ключа и подтверждение домена | Postbox → адрес → **Email signature configuration (DKIM)** |

Значение TXT должно быть заключено в кавычки в панели провайдера, если она
этого требует. Некоторые провайдеры ограничивают длину TXT; в этом случае
документация Postbox рекомендует ключ 1024 бита вместо 2048.

#### Практические замечания по CNAME

- В Yandex Cloud DNS имя указывается без домена, например
  `<selector>._domainkey`; у других провайдеров может требоваться полное имя
  `<selector>._domainkey.fretfight.online.`.
- CNAME не может сосуществовать с другими записями с тем же именем.
- Если DNS-провайдер поддерживает proxy/CDN (например, Cloudflare), для
  DKIM CNAME включите **DNS only**, иначе проверка DKIM может не пройти.
- TTL: 300–3600 секунд. После изменения подождите распространения; DNS
  кэшируется. Если статус не меняется 24 часа, нажмите **Run verification** на
  странице адреса.

### 6.2. SPF

SPF — TXT-запись в корне домена (`fretfight.online`). Нужно авторизовать
серверы Postbox. Официальная рекомендация Postbox:
`include:spf.postbox.yandexcloud.net`.

**Если SPF-записи ещё нет** (на момент проверки в корне домена TXT-записи не
было, но проверьте перед изменением):

| Тип | Имя | Значение | Назначение |
|---|---|---|---|
| TXT | `@` (корень `fretfight.online`) | `v=spf1 include:spf.postbox.yandexcloud.net ~all` | Разрешить отправку через Postbox |

**Если SPF-запись уже есть**, не создавайте вторую. Добавьте
`include:spf.postbox.yandexcloud.net` в существующую запись **перед**
механизмом `all`, сохранив остальные механизмы. Пример синтаксиса (не
копируйте `_spf.example.com` — это только иллюстрация):

```text
v=spf1 include:_spf.example.com include:spf.postbox.yandexcloud.net ~all
```

Правила:

- В домене должна быть **ровно одна** SPF-запись. Если TXT-записей, начинающихся
  с `v=spf1`, несколько, проверка SPF завершится ошибкой `PermError`, и письма
  будут отклоняться независимо от содержимого.
- Не затирайте существующие `include`, `ip4`, `ip6`, `a`, `mx`, `exists` и
  финальный `all`.
- Если сейчас стоит `-all` (жёсткий отказ), не меняйте его на `~all` без
  необходимости; просто добавьте `include` перед `all`.
- Помните про лимит 10 DNS-запросов при раскрытии SPF. После изменения проверьте
  запись валидатором, например MxToolbox SPF.
- Если домен вообще не отправляет почту и вы только добавляете Postbox,
  `~all` — безопасный старт; позже можно перейти на `-all`.

Проверить текущие TXT:

```powershell
Resolve-DnsName -Name fretfight.online -Type TXT -Server 1.1.1.1
```

### 6.3. DMARC

DMARC — TXT-запись с именем `_dmarc.fretfight.online` (в панели часто
указывается просто `_dmarc`). Она говорит получателям, что делать с письмами,
не прошедшими SPF/DKIM, и куда отправлять отчёты.

Минимальный безопасный старт:

| Тип | Имя | Значение | Назначение |
|---|---|---|---|
| TXT | `_dmarc` (`_dmarc.fretfight.online`) | `v=DMARC1;p=none` | Мониторинг без блокировки |

Рекомендуемый вариант с отчётами:

```text
v=DMARC1;p=none;rua=mailto:<DMARC_REPORT_MAILBOX>
```

`<DMARC_REPORT_MAILBOX>` — реальный ящик для агрегированных DMARC-отчётов,
например `dmarc@fretfight.online`, если он существует. Если ящика нет,
используйте минимальную запись без `rua` или создайте ящик. Не выдумывайте
адрес, который не принимает почту.

Правила:

- В домене должна быть **одна** DMARC-запись.
- Начните с `p=none`, соберите отчёты 1–2 недели, убедитесь, что SPF и DKIM
  проходят, затем переходите к `p=quarantine`, а потом к `p=reject`.
- DMARC проходит, если проходит SPF **или** DKIM и при этом домен в
  аутентификации совпадает с доменом `From` (alignment). Postbox подписывает
  письма DKIM для `fretfight.online`, а SPF-запись разрешает его IP-адреса.
- Не включайте `p=reject` сразу: если существующие легитимные отправители
  домена не настроены, их письма начнут отклоняться.

Проверить:

```powershell
Resolve-DnsName -Name _dmarc.fretfight.online -Type TXT -Server 1.1.1.1
```

Официальная документация:
[DNS records](https://yandex.cloud/en/docs/postbox/concepts/dns-records),
[Setting up a DMARC policy](https://yandex.cloud/en/docs/postbox/operations/setup-dmarc).

### 6.4. Чего НЕ нужно делать

- Не менять NS-серверы и не переносить DNS-зону.
- Не добавлять второй SPF- или DMARC-рекорд.
- Не добавлять MX-записи «для Postbox»: для отправки они не нужны.
- Не добавлять CAA-записи «на всякий случай»: Postbox этого не требует.
- Не копировать DKIM-значения из примеров/чатов: они уникальны для вашего
  адреса и видны только в консоли.

## 7. Шаг 4. Переменные окружения

В `.env` на сервере (файл не коммитится, не печатается в логи и чат):

```dotenv
SMTP_HOST=postbox.cloud.yandex.net
SMTP_PORT=587
SMTP_SECURITY=starttls
SMTP_USERNAME=<API_KEY_ID_ИЗ_КОНСОЛИ>
SMTP_PASSWORD=<API_KEY_SECRET_ИЗ_КОНСОЛИ>
SMTP_FROM_EMAIL=noreply@fretfight.online
SMTP_FROM_NAME=GuitarBattle
SMTP_TIMEOUT_SECONDS=10
```

Альтернатива с implicit TLS:

```dotenv
SMTP_PORT=465
SMTP_SECURITY=ssl
```

Соответствие:

- `SMTP_USERNAME` — **ID API-ключа**, не ID сервисного аккаунта, не email и не
  IAM-токен.
- `SMTP_PASSWORD` — **секретная часть API-ключа**, не пароль пользователя
  Yandex ID.
- `SMTP_FROM_EMAIL` — адрес на подтверждённом домене. Для `fretfight.online`
  можно использовать `noreply@fretfight.online`; адрес не обязан быть отдельно
  созданным «ящиком», но домен должен быть подтверждён. Указывайте только
  адрес, без display name: имя отправителя задаётся в `SMTP_FROM_NAME`.
- `SMTP_SECURITY=none` для Postbox не используйте: сервер требует TLS.
- Если секрет содержит `#`, пробелы или кавычки, заключите значение в кавычки:
  `SMTP_PASSWORD="..."`. Не добавляйте пробелы после значения.

Перезапуск API:

```powershell
cd D:\GuitarBattle\GuitarBattleServer
docker compose up --build -d api
docker compose ps
curl.exe -s http://localhost:8000/health/ready
```

После изменения `.env` нужен `docker compose up -d api` (Compose пересоздаст
контейнер с новыми переменными), а не только `docker compose restart api`.
Никогда не выполняйте `docker compose down -v`: это удаляет БД и файлы песен.

Безопасная проверка конфигурации внутри контейнера (печатает только флаги,
не секреты):

```powershell
docker compose exec api python -c "from app.config import settings; print(settings.smtp_host, settings.smtp_port, settings.smtp_security, settings.smtp_from_email, bool(settings.smtp_username), bool(settings.smtp_password))"
```

Ожидаемо: `postbox.cloud.yandex.net 587 starttls noreply@fretfight.online True True`.

## 8. Проверка реальной отправкой

### 8.1. Проверка DNS

```powershell
# DKIM CNAME (подставьте имя из консоли)
Resolve-DnsName -Name <DKIM_CNAME_NAME>.fretfight.online -Type CNAME -Server 1.1.1.1

# SPF
Resolve-DnsName -Name fretfight.online -Type TXT -Server 1.1.1.1

# DMARC
Resolve-DnsName -Name _dmarc.fretfight.online -Type TXT -Server 1.1.1.1
```

Для Advanced DKIM вместо CNAME проверяйте TXT:

```powershell
Resolve-DnsName -Name <DKIM_SELECTOR>._domainkey.fretfight.online -Type TXT -Server 1.1.1.1
```

Ожидаемо: CNAME резолвится в имя Postbox; TXT содержит `v=DKIM1`, SPF содержит
`include:spf.postbox.yandexcloud.net`, DMARC содержит `v=DMARC1`. После
добавления записей статус в Postbox должен стать `Success`.

### 8.2. Проверка SMTP без отправки письма

На хосте с зависимостями:

```powershell
cd D:\GuitarBattle\GuitarBattleServer
.venv-test\Scripts\python.exe scripts\check_smtp.py
```

Ожидаемый вывод (значения сервера могут отличаться):

```text
SMTP host: postbox.cloud.yandex.net
SMTP port: 587
SMTP security: starttls
SMTP from: noreply@fretfight.online
SMTP username set: True
SMTP password set: True
EHLO: 250 ...
STARTTLS: 220 ...
EHLO: 250 ...
AUTH: 235 ...
No message sent (use --send --to <address> for a real test).
```

`AUTH: 235` означает, что API-ключ принят. Если `535`/`534`/`538` — см.
раздел ошибок. Скрипт не печатает пароль.

### 8.3. Реальная тестовая отправка

```powershell
.venv-test\Scripts\python.exe scripts\check_smtp.py --send --to <TEST_MAILBOX>
```

Ожидаемо: `SEND: accepted by the SMTP server`. Это значит, что Postbox принял
письмо в очередь, а **не** что оно доставлено. Проверьте:

1. Письмо пришло на `<TEST_MAILBOX>` (включая «Спам»).
2. В исходных заголовках письма:
   - `Authentication-Results` содержит `spf=pass`, `dkim=pass`,
     `dmarc=pass`;
   - `DKIM-Signature` содержит `d=fretfight.online` и selector из консоли;
   - `From: GuitarBattle <noreply@fretfight.online>`.
3. В консоли Postbox: статистика/история отправленных писем/логи. Для успеха
   ожидается ответ вида `250 2.6.0 OK: queued as <message-id>`.

В Gmail: открыть письмо → три точки → **Show original**. В Outlook/Yandex Mail:
**Свойства** / **Показать исходное письмо**.

Если `dkim=pass`, но `dmarc=fail`, проверьте alignment: домен в `DKIM-Signature`
должен быть `fretfight.online` (или поддомен, настроенный как отдельный адрес
Postbox). Если `spf=pass`, но `dmarc=fail`, проверьте envelope-домен и
`Return-Path`; для DMARC важен alignment с `From`.

### 8.4. Проверка полного сценария приложения

После успешной проверки SMTP отправьте реальную регистрацию. Используйте
одноразовый тестовый ящик, не аккаунт реального пользователя:

```powershell
$body = @{
  username = "postbox_test_$((Get-Random))"
  email    = "<TEST_MAILBOX>"
  password = "at-least-10-characters"
} | ConvertTo-Json

curl.exe -s -X POST http://localhost:8000/api/v1/auth/register `
  -H "Content-Type: application/json" `
  -d $body
```

Ожидаемо: `201`, письмо с 6-значным кодом. Затем:

```powershell
$body = @{
  email = "<TEST_MAILBOX>"
  code  = "<CODE_FROM_EMAIL>"
} | ConvertTo-Json

curl.exe -s -X POST http://localhost:8000/api/v1/auth/verify-email `
  -H "Content-Type: application/json" `
  -d $body
```

Ожидаемо: `200` и access token. Если `503` — письмо не ушло; смотрите
`docker compose logs api` (код подтверждения в production в лог не пишется).

### 8.5. Симуляторы доставки Postbox

Можно проверить разные исходы без реального получателя (но с рабочими
кредами и подтверждённым доменом):

- `success@simulator.pstbx.ru` — успешная доставка;
- `bounce@simulator.pstbx.ru` — ошибка доставки;
- `delay@simulator.pstbx.ru` — задержка;
- `suppressed@simulator.pstbx.ru` — адрес в suppression list;
- `spam@simulator.pstbx.ru` — отклонение как спам.

Это проверяет принятие письма Postbox, но не заменяет проверку заголовков
SPF/DKIM/DMARC у реального почтового провайдера.

## 9. Ошибки и что делать

### Аутентификация

| Ответ SMTP | Причина | Действие |
|---|---|---|
| `535 5.7.8 Authentication failed` | Неверный API-ключ/секрет, ключ удалён, не тот каталог | Пересоздайте API-ключ, проверьте `SMTP_USERNAME`/`SMTP_PASSWORD`, каталог сервисного аккаунта и адреса |
| `534 5.7.8 Not enough permissions` | У сервисного аккаунта нет роли `postbox.sender` | Назначьте роль |
| `538 5.7.8 Authentication credentials invalid` | Логин/пароль не переданы или пароль в неверном формате | Проверьте, что пароль — сырой секрет API-ключа, без кавычек и лишних пробелов |
| `530 5.7.0 Must issue a STARTTLS command first` | Отправка без TLS | `SMTP_SECURITY=starttls` + порт 587 или `ssl` + порт 465 |
| `502 5.7.0 Please authenticate first` | `SMTP_USERNAME` пуст | Укажите ID API-ключа |
| HTTP `401`/`403` (только для API, не SMTP) | Неверная подпись/IAM-токен или нет прав | Для SMTP используйте коды 535/534/538; для API проверьте ключ и роль |

### Отправитель и домен

| Ответ SMTP | Причина | Действие |
|---|---|---|
| `550 5.4.1 identity not found` | Домен `From` не создан как адрес Postbox | Создайте адрес `fretfight.online`; для поддомена — отдельный адрес |
| `550 5.4.1 identity not verified` | DKIM-записи не опубликованы или не проверены | Добавьте CNAME/TXT из консоли, дождитесь `Success`, нажмите **Run verification** |
| `550 5.4.1 identity is blocked` | Адрес заблокирован | Обратитесь в поддержку Yandex Cloud с `RequestID` |
| `550 5.7.1 sender is not allowed` | `noreply@fretfight.online` не входит в allowed senders | Добавьте адрес в список или отключите ограничение |
| `550 5.4.5 Daily sending quota exceeded` | Исчерпана квота 200 писем/24 ч | Дождитесь сброса или запросите увеличение квоты |
| `550 Empty envelope senders not allowed` | Пустой `MAIL FROM` | Укажите корректный `From`; текущий код всегда передаёт `From` |

### Лимиты и TLS

| Ответ/ошибка | Причина | Действие |
|---|---|---|
| `454 Maximum sending rate exceeded` | Превышено 1 письмо/сек | Добавьте очередь/задержку, повторите позже |
| `552 5.3.4 message is too big` | Письмо больше 10 MB | Уменьшите вложение/тело |
| `421 4.4.2 Idle timeout` | Соединение простаивало | Не переиспользуйте idle-соединение; текущий код открывает новое на каждое письмо |
| TLS handshake error / certificate error | Неверный порт/режим, блокировка firewall, устаревшие CA | 587+STARTTLS или 465+SSL; проверьте исходящий доступ и системные CA; не отключайте проверку сертификата |
| `SMTPRecipientsRefused` / `550` на `RCPT TO` | Отклонил сервер получателя, а не Postbox | Проверьте адрес получателя и его политику |

### Ошибки приложения

- `503` от `/auth/register` — `EmailDeliveryError`: SMTP не настроен, неверные
  креды, TLS или Postbox отклонил отправителя. Смотрите `docker compose logs api`.
- В production код подтверждения не должен появляться в логах. Если вы видите
  код в логе на публичном сервере — значит, `APP_ENV` не `production` или
  `SMTP_HOST` пуст; исправьте конфигурацию.
- Если Postbox отвечает `RequestID: ...`, сохраните его для поддержки.

## 10. Безопасность

- `SMTP_PASSWORD` (секрет API-ключа) хранится только в `.env`/секрет-менеджере,
  не коммитится и не печатается. `scripts/check_smtp.py` выводит только
  `True/False`.
- Ограничьте allowed senders адресом `noreply@fretfight.online`.
- Ротируйте API-ключ при подозрении на утечку и после смены ответственного.
- Держите `APP_ENV=production` на публичном сервере: fallback с кодом в лог
  предназначен только для локальной разработки.
- Не отключайте проверку TLS-сертификата.
- Rate limit на `/auth/*` по-прежнему нужен на уровне Caddy/nginx: приложение
  ограничивает только частоту повторной отправки и число попыток кода.

## 11. Что проверено, а что нет

Проверено в этой среде:

- `python -m compileall app` — синтаксис Python-кода.
- Юнит-тесты email-сервиса с фейковым SMTP (STARTTLS, SMTPS, AUTH, заголовки,
  обработка ошибок, dev/prod/test-поведение).
- Сетевая доступность `postbox.cloud.yandex.net`:
  - порт 587: EHLO, `STARTTLS 220`, после TLS `AUTH PLAIN LOGIN`, TLS 1.3;
  - порт 465: implicit TLS, `AUTH PLAIN LOGIN`, TLS 1.3.
- Публичные DNS-записи домена на момент проверки: NS — Yandex Cloud; TXT в
  корне и `_dmarc` не найдены; распространённые DKIM-selectors не найдены.

Не проверено (нужен доступ пользователя):

- реальная аутентификация с API-ключом Postbox;
- реальная отправка письма и доставка;
- статус `Success` для домена в Postbox;
- прохождение SPF/DKIM/DMARC у получателя;
- работа в production-контейнере с реальным `.env`.

## 12. Что нужно от пользователя

1. Доступ к консоли Yandex Cloud (или администратор, который выполнит шаги
   раздела 4–5) и активный платёжный аккаунт.
2. Подтверждение, что домен `fretfight.online` можно добавить в Postbox.
3. Доступ к DNS-провайдеру авторитетной зоны (NS не меняем) для добавления
   записей.
4. Значения из консоли Postbox:
   - для Easy DKIM — имена и значения **двух CNAME**;
   - для Advanced DKIM — selector и значение **одной TXT**;
   - статус проверки домена.
5. ID и секрет API-ключа со scope `yc.postbox.send`; сервисный аккаунт с ролью
   `postbox.sender` в том же каталоге, что и адрес. Секрет передавайте только
   через `.env`, не в чат.
6. Реальный тестовый ящик для `--send --to`.
7. Текущее содержимое SPF-записи, если оно есть (на момент проверки SPF не
   найдено — перепроверьте), и адрес для DMARC-отчётов `rua`, если нужен.
8. После настройки — вывод `scripts/check_smtp.py` (без секретов) и исходные
   заголовки тестового письма, если что-то не проходит.

## 13. Краткий чек-лист

- [ ] Платёжный аккаунт Yandex Cloud активен.
- [ ] Сервисный аккаунт создан в том же каталоге, что и адрес Postbox.
- [ ] Роль `postbox.sender` назначена.
- [ ] API-ключ со scope `yc.postbox.send` создан; ID и секрет сохранены.
- [ ] В Postbox создан адрес `fretfight.online` (Easy или Advanced DKIM).
- [ ] DNS: добавлены две CNAME (Easy) или одна TXT (Advanced) из консоли.
- [ ] DNS: SPF содержит `include:spf.postbox.yandexcloud.net` ровно в одной
      записи; существующие механизмы сохранены.
- [ ] DNS: `_dmarc` содержит одну запись `v=DMARC1;p=none` (или с `rua`).
- [ ] В Postbox статус домена `Success`.
- [ ] `.env`: заполнены `SMTP_HOST`, `SMTP_PORT`, `SMTP_SECURITY`,
      `SMTP_USERNAME`, `SMTP_PASSWORD`, `SMTP_FROM_EMAIL`.
- [ ] `scripts/check_smtp.py` показывает `AUTH: 235`.
- [ ] `scripts/check_smtp.py --send --to ...` показывает `SEND: accepted`.
- [ ] Тестовое письмо получено; `Authentication-Results` содержит
      `spf=pass`, `dkim=pass`, `dmarc=pass`.
- [ ] Полный сценарий `/auth/register` → `/auth/verify-email` работает.
- [ ] NS-серверы и DNS-зона не менялись.

## 14. Официальные источники

- [Postbox Quickstart](https://yandex.cloud/en/docs/postbox/quickstart)
- [Creating an address](https://yandex.cloud/en/docs/postbox/operations/create-address)
- [Domain ownership verification](https://yandex.cloud/en/docs/postbox/operations/check-domain)
- [DNS records for Postbox](https://yandex.cloud/en/docs/postbox/concepts/dns-records)
- [Setting up a DMARC policy](https://yandex.cloud/en/docs/postbox/operations/setup-dmarc)
- [Sending an email](https://yandex.cloud/en/docs/postbox/operations/send-email)
- [SMTP responses](https://yandex.cloud/en/docs/postbox/api-ref/smtp-errors)
- [Quotas and limits](https://yandex.cloud/en/docs/postbox/concepts/limits)
- [Email delivery simulator](https://yandex.cloud/en/docs/postbox/concepts/mail-simulator)
