// PhoneCam - phone page for Telegram / browser + Telegram bot. Paste this whole file into the Cloudflare Worker editor.
// Bot: put the token from @BotFather into Settings -> Variables and Secrets as a Secret named BOT_TOKEN,
// then open https://<this worker>/setup once.
const PAGE_GZ = "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";  // phone.html, minified + gzip
const PAGE = Uint8Array.from(atob(PAGE_GZ), (c) => c.charCodeAt(0));

const WELCOME =
  "<b>PhoneCam</b> - телефон как вебкамера для ПК и OBS.\n\n" +
  "1. На ПК в программе PhoneCam выбери «Телеграм» и нажми «Подключить».\n" +
  "2. Нажми кнопку ниже, потом «Сканировать QR-код» и наведи на экран ПК (или введи код).\n" +
  "3. Разреши доступ к камере.\n\n" +
  "Не сворачивай Телеграм, пока идёт трансляция.";

async function tg(env, method, body) {
  const r = await fetch(`https://api.telegram.org/bot${env.BOT_TOKEN}/${method}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  return r.json();
}

async function hookSecret(env) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode("phonecam-hook:" + env.BOT_TOKEN)));
  return Array.from(d.slice(0, 16), (b) => b.toString(16).padStart(2, "0")).join("");
}

async function onUpdate(env, origin, u) {
  const m = u.message;
  if (!m || !m.chat || m.chat.type !== "private") return;
  const text = (m.text || "").trim();
  // /start ABCD2345 (deep link from the PC QR code) opens the page with that code
  const arg = text.startsWith("/start ") ? text.slice(7).trim().toUpperCase().replace(/[^A-Z0-9]/g, "") : "";
  const url = origin + "/" + (arg.length === 8 ? "?code=" + arg : "");
  await tg(env, "sendMessage", {
    chat_id: m.chat.id,
    text: WELCOME,
    parse_mode: "HTML",
    reply_markup: { inline_keyboard: [[{ text: "📷 Открыть камеру", web_app: { url } }]] },
  });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/favicon.ico") return new Response(null, { status: 204 });

    if (url.pathname === "/tg" && request.method === "POST") {
      if (!env.BOT_TOKEN) return new Response("no token", { status: 500 });
      if (request.headers.get("x-telegram-bot-api-secret-token") !== (await hookSecret(env)))
        return new Response("forbidden", { status: 403 });
      const u = await request.json().catch(() => null);
      if (u) ctx.waitUntil(onUpdate(env, url.origin, u).catch(() => {}));
      return new Response("ok");
    }

    if (url.pathname === "/setup") {
      if (!env.BOT_TOKEN)
        return new Response("Добавь секрет BOT_TOKEN: Settings -> Variables and Secrets -> Add -> Secret, имя BOT_TOKEN, значение - токен от @BotFather. Потом Deploy и открой эту страницу ещё раз.",
          { headers: { "content-type": "text/plain; charset=utf-8" } });
      const hook = await tg(env, "setWebhook", {
        url: url.origin + "/tg", secret_token: await hookSecret(env), allowed_updates: ["message"], drop_pending_updates: true,
      });
      const menu = await tg(env, "setChatMenuButton", {
        menu_button: { type: "web_app", text: "Камера", web_app: { url: url.origin + "/" } },
      });
      const cmds = await tg(env, "setMyCommands", { commands: [{ command: "start", description: "Открыть PhoneCam" }] });
      const me = await tg(env, "getMe", {});
      const ok = hook.ok && menu.ok && cmds.ok;
      const name = me.ok ? "@" + me.result.username : "?";
      return new Response(ok ? `Готово! Бот ${name} настроен. Напиши ему /start.` :
        "Что-то не так: " + JSON.stringify({ hook, menu, cmds }),
        { headers: { "content-type": "text/plain; charset=utf-8" } });
    }

    return new Response(PAGE, {
      encodeBody: "manual",
      headers: {
        "content-type": "text/html; charset=utf-8",
        "content-encoding": "gzip",
        "cache-control": "no-cache",
        "permissions-policy": "camera=(self)",
      },
    });
  },
};
