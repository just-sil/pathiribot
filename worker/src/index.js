// Telegram -> Cloudflare Worker -> (ffmpeg converter) -> KV -> ESP32
//
// /latest response (little-endian), 204 if nothing newer than ?since=:
//   u32 id | u8 type (0 text,1 image,2 anim) | u8 fps | u16 count | data
//   text : count = byte length, data = ASCII/UTF-8
//   media: count = frames, data = count * 1024 bytes (128x64, 1bpp, MSB first)

const MAX_TEXT = 1000;

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    if (req.method === "POST" && url.pathname === "/telegram") return telegram(req, env, ctx);
    if (req.method === "GET" && url.pathname === "/latest") return latest(url, env);
    return new Response("not found", { status: 404 });
  },
};

async function latest(url, env) {
  if (url.searchParams.get("key") !== env.DEVICE_KEY) return new Response("forbidden", { status: 403 });
  const since = parseInt(url.searchParams.get("since") || "0", 10);
  const id = parseInt((await env.KV.get("latest_id")) || "0", 10);
  if (!id || id === since) return new Response(null, { status: 204 });
  const body = await env.KV.get("latest", "arrayBuffer");
  if (!body) return new Response(null, { status: 204 });
  return new Response(body, { headers: { "content-type": "application/octet-stream" } });
}

async function telegram(req, env, ctx) {
  if (req.headers.get("x-telegram-bot-api-secret-token") !== env.WEBHOOK_SECRET)
    return new Response("forbidden", { status: 403 });
  const update = await req.json();
  const msg = update.message;
  // Always answer 200 fast so Telegram doesn't retry.
  if (msg && msg.from && String(msg.from.id) === String(env.ALLOWED_USER_ID)) {
    ctx.waitUntil(
      handleMessage(msg, update.update_id, env).catch((e) =>
        tg(env, "sendMessage", { chat_id: msg.chat.id, text: "Failed: " + e.message })
      )
    );
  }
  return new Response("ok");
}

async function handleMessage(msg, id, env) {
  if (msg.text) {
    const data = new TextEncoder().encode(msg.text.slice(0, MAX_TEXT));
    return store(env, id, 0, 0, data.length, data);
  }
  let fileId, kind;
  if (msg.photo) { fileId = msg.photo[msg.photo.length - 1].file_id; kind = "image"; }
  else if (msg.animation) { fileId = msg.animation.file_id; kind = "video"; }
  else if (msg.video) { fileId = msg.video.file_id; kind = "video"; }
  else if (msg.video_note) { fileId = msg.video_note.file_id; kind = "video"; }
  else if (msg.sticker && !msg.sticker.is_animated) { fileId = msg.sticker.file_id; kind = msg.sticker.is_video ? "video" : "image"; }
  else if (msg.document) {
    const m = msg.document.mime_type || "";
    if (m === "image/gif" || m.startsWith("video/")) kind = "video";
    else if (m.startsWith("image/")) kind = "image";
    fileId = msg.document.file_id;
  }
  if (!fileId || !kind) throw new Error("unsupported message type");

  const info = await tg(env, "getFile", { file_id: fileId });
  if (!info.ok) throw new Error("file too big (Telegram bot limit is 20 MB)");
  const file = await fetch(`https://api.telegram.org/file/bot${env.BOT_TOKEN}/${info.result.file_path}`);
  const res = await fetch(`${env.CONVERTER_URL}/convert?kind=${kind}`, {
    method: "POST",
    headers: { "x-auth": env.CONVERTER_KEY },
    body: await file.arrayBuffer(),
  });
  if (!res.ok) throw new Error("converter: " + (await res.text()).slice(0, 100));
  const out = new Uint8Array(await res.arrayBuffer()); // u8 type | u8 fps | u16 count | frames
  await storeRaw(env, id, out);
}

async function store(env, id, type, fps, count, data) {
  const out = new Uint8Array(4 + data.length);
  out[0] = type; out[1] = fps; out[2] = count & 255; out[3] = count >> 8;
  out.set(data, 4);
  await storeRaw(env, id, out);
}

async function storeRaw(env, id, body4) {
  const out = new Uint8Array(4 + body4.length);
  new DataView(out.buffer).setUint32(0, id, true);
  out.set(body4, 4);
  await env.KV.put("latest", out);       // body first,
  await env.KV.put("latest_id", String(id)); // then id, so the device never sees a stale body
}

function tg(env, method, body) {
  return fetch(`https://api.telegram.org/bot${env.BOT_TOKEN}/${method}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  }).then((r) => r.json());
}
