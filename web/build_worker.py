# Builds worker.js (Cloudflare Worker that serves the phone page) from phone.html.
import base64, gzip, json, os, re
here = os.path.dirname(os.path.abspath(__file__))
html = open(os.path.join(here, 'phone.html'), encoding='utf-8').read()

# Russian ISPs cut Cloudflare connections after ~16 KB, so the page is minified and served pre-compressed.
def minify(s):
    css = re.search(r'<style>(.*?)</style>', s, re.S)
    c = re.sub(r'\s+', ' ', css.group(1))
    c = re.sub(r'\s*([{};:,>])\s*', r'\1', c).replace(';}', '}')
    s = s[:css.start(1)] + c + s[css.end(1):]
    js = re.search(r'<script>(.*?)</script>', s, re.S)
    lines = [l.strip() for l in js.group(1).split('\n')]
    lines = [l for l in lines if l and not l.startswith('//')]
    s = s[:js.start(1)] + '\n'.join(lines) + s[js.end(1):]
    return re.sub(r'>\s+<', '><', s)

page_gz = gzip.compress(minify(html).encode('utf-8'), 9, mtime=0)
page_b64 = base64.b64encode(page_gz).decode()
js = '''// PhoneCam - phone page for Telegram / browser + Telegram bot. Paste this whole file into the Cloudflare Worker editor.
// Bot: put the token from @BotFather into Settings -> Variables and Secrets as a Secret named BOT_TOKEN,
// then open https://<this worker>/setup once.
const PAGE_GZ = "%s";  // phone.html, minified + gzip
const PAGE = Uint8Array.from(atob(PAGE_GZ), (c) => c.charCodeAt(0));

const WELCOME =
  "<b>PhoneCam</b> - телефон как вебкамера для ПК и OBS.\\n\\n" +
  "1. На ПК в программе PhoneCam выбери «Телеграм» и нажми «Подключить».\\n" +
  "2. Нажми кнопку ниже, потом «Сканировать QR-код» и наведи на экран ПК (или введи код).\\n" +
  "3. Разреши доступ к камере.\\n\\n" +
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
''' % page_b64
open(os.path.join(here, 'worker.js'), 'w', encoding='utf-8').write(js)
print('worker.js', len(js.encode()), 'bytes, page gzip', len(page_gz))
