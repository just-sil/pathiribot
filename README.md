# Telegram -> ESP32 OLED

Telegram bot -> Cloudflare Worker (+KV) -> ESP32. Media is converted by `converter/` (ffmpeg) to
128x64 1-bit dithered frames (max 8 s @ 8 fps). Only messages from `ALLOWED_USER_ID` are kept; only the latest is stored.

## 1. Bot
Create a bot with @BotFather -> `BOT_TOKEN`. Get your numeric user id from @userinfobot.

## 2. Converter (needs ffmpeg; free: Render / Hugging Face Spaces, Docker)
Deploy `converter/` with env `CONVERTER_KEY=<random>`. Cold starts only delay media, not the ESP32.

## 3. Worker
```
cd worker
npx wrangler kv namespace create KV      # paste id into wrangler.toml
# edit ALLOWED_USER_ID and CONVERTER_URL in wrangler.toml
npx wrangler secret put BOT_TOKEN
npx wrangler secret put WEBHOOK_SECRET   # random string
npx wrangler secret put DEVICE_KEY       # random string, also goes in secrets.h
npx wrangler secret put CONVERTER_KEY    # same as converter
npx wrangler deploy
curl "https://api.telegram.org/bot<BOT_TOKEN>/setWebhook" \
  -d url=https://oled-telegram.<sub>.workers.dev/telegram -d secret_token=<WEBHOOK_SECRET>
```

## 4. ESP32
Edit `OLED_Video_Loop/secrets.h` (ENDPOINT_URL, DEVICE_KEY), flash. It checks on boot and every 5 min
(WiFi is off in between). New messages show for 20 s, then the eyes resume.
Text is ASCII only (Adafruit font).
