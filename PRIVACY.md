# Privacy policy / Политика конфиденциальности

**English.** PhoneCam does not collect, store or send any personal data, analytics or telemetry. No accounts.

- **Wi-Fi / USB mode:** video goes only between your phone and your PC (your local network or the USB cable).
  The PC program looks for the phone with a broadcast on your local network.
- **Telegram / browser mode** (only when you choose it): to find each other, the phone and the PC exchange short
  connection messages through public MQTT servers (broker.emqx.io, broker.hivemq.com, test.mosquitto.org).
  These messages are encrypted with a key derived from your pairing code and contain only connection details
  (network addresses for the direct connection), never video. Public STUN servers (Google, Cloudflare) are used
  to find a direct route. The video itself goes directly from the phone to the PC.
  The phone page and the Telegram bot are served by Cloudflare Workers; the bot only answers `/start`
  and does not store messages.
- Logs (`%APPDATA%\PhoneCam\log.txt`) and settings stay on your PC.

**Русский.** PhoneCam не собирает, не хранит и не отправляет личные данные, аналитику или телеметрию. Аккаунтов нет.

- **Wi-Fi / USB:** видео идёт только между твоим телефоном и ПК (локальная сеть или кабель).
  Программа ищет телефон широковещательным запросом в локальной сети.
- **Телеграм / браузер** (только если выбрать этот режим): чтобы найти друг друга, телефон и ПК обмениваются
  короткими служебными сообщениями через публичные MQTT-серверы (broker.emqx.io, broker.hivemq.com, test.mosquitto.org).
  Сообщения зашифрованы ключом из кода сопряжения и содержат только данные для соединения (сетевые адреса), не видео.
  Для поиска прямого пути используются публичные STUN-серверы (Google, Cloudflare). Само видео идёт напрямую с телефона на ПК.
  Страница телефона и бот работают на Cloudflare Workers; бот только отвечает на `/start` и не хранит сообщения.
- Журнал (`%APPDATA%\PhoneCam\log.txt`) и настройки остаются на твоём ПК.
