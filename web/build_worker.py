# Builds worker.js (Cloudflare Worker that serves the phone page) from phone.html.
import base64, gzip, json, os, re
here = os.path.dirname(os.path.abspath(__file__))
import sys
sys.path.insert(0, os.path.join(here, '..', 'i18n'))
from web_strings import T as WT
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

# one pre-compressed page per language (only that language's dictionary), to stay small
def variant(lang):  # 0 ru, 1 en, 2 uk
    d = {} if lang == 0 else {ru: ([en, None] if lang == 1 else [None, uk]) for ru, (en, uk) in WT.items() if not ru.startswith('BOT_')}
    a = html.index('/*I18N*/'); b = html.index('/*I18N_END*/')
    h = html[:a] + '/*I18N*/' + json.dumps(d, ensure_ascii=False) + html[b:]
    a = h.index('/*PL*/'); b = h.index('/*PL_END*/')
    h = h[:a] + str(lang) + h[b + len('/*PL_END*/'):]
    return gzip.compress(minify(h).encode('utf-8'), 9, mtime=0)
pages = [variant(i) for i in range(3)]
page_gz = max(pages, key=len)
page_b64 = json.dumps([base64.b64encode(p).decode() for p in pages])

RU_WELCOME = ("<b>PhoneCam</b> - телефон как вебкамера для ПК и OBS.\n\n"
              "1. На ПК в программе PhoneCam выбери «Телеграм» и нажми «Подключить».\n"
              "2. Нажми кнопку ниже, потом «Сканировать QR-код» и наведи на экран ПК (или введи код).\n"
              "3. Разреши доступ к камере.\n\n"
              "Не сворачивай Телеграм, пока идёт трансляция.")
BOT = {
    "welcome": [RU_WELCOME, WT["BOT_WELCOME"][0], WT["BOT_WELCOME"][1]],
    "button": ["📷 Открыть камеру", WT["BOT_BUTTON"][0], WT["BOT_BUTTON"][1]],
    "menu": ["Камера", WT["BOT_MENU"][0], WT["BOT_MENU"][1]],
    "cmd": ["Открыть PhoneCam", WT["BOT_CMD"][0], WT["BOT_CMD"][1]],
}
js = '''// PhoneCam - phone page for Telegram / browser + Telegram bot. Paste this whole file into the Cloudflare Worker editor.
// Bot: put the token from @BotFather into Settings -> Variables and Secrets as a Secret named BOT_TOKEN,
// then open https://<this worker>/setup once.
const PAGES_GZ = %s;  // phone.html per language [ru, en, uk], minified + gzip
const PAGES = PAGES_GZ.map((b) => Uint8Array.from(atob(b), (c) => c.charCodeAt(0)));

const BOT_TEXT = %s;  // [ru, en, uk]
function botLang(code) { code = String(code || "").toLowerCase(); return /^(ru|be|kk)/.test(code) ? 0 : /^uk/.test(code) ? 2 : 1; }

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

const LANG_ROW = [["RU", "ru"], ["EN", "en"], ["UA", "uk"]];
function botMarkup(origin, L, code) {
  const q = "?lang=" + ["ru", "en", "uk"][L] + (code ? "&code=" + code : "");
  return {
    inline_keyboard: [
      [{ text: BOT_TEXT.button[L], web_app: { url: origin + "/" + q } }],
      LANG_ROW.map(([label, lc]) => ({ text: (botLang(lc) === L ? "• " : "") + label, callback_data: "lang:" + lc + ":" + (code || "") })),
    ],
  };
}

async function onUpdate(env, origin, u) {
  const cq = u.callback_query;
  if (cq && cq.message && /^lang:/.test(cq.data || "")) {
    const [, lc, code] = cq.data.split(":");
    const L = botLang(lc);
    await tg(env, "editMessageText", {
      chat_id: cq.message.chat.id, message_id: cq.message.message_id, text: BOT_TEXT.welcome[L], parse_mode: "HTML",
      reply_markup: botMarkup(origin, L, code),
    }).catch(() => {});
    await tg(env, "answerCallbackQuery", { callback_query_id: cq.id }).catch(() => {});
    return;
  }
  const m = u.message;
  if (!m || !m.chat || m.chat.type !== "private") return;
  const text = (m.text || "").trim();
  // /start ABCD2345 (deep link from the PC QR code) opens the page with that code
  const arg = text.startsWith("/start ") ? text.slice(7).trim().toUpperCase().replace(/[^A-Z0-9]/g, "") : "";
  const L = botLang(m.from && m.from.language_code);
  await tg(env, "sendMessage", {
    chat_id: m.chat.id,
    text: BOT_TEXT.welcome[L],
    parse_mode: "HTML",
    reply_markup: botMarkup(origin, L, arg.length === 8 ? arg : ""),
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
        url: url.origin + "/tg", secret_token: await hookSecret(env), allowed_updates: ["message", "callback_query"], drop_pending_updates: true,
      });
      const menu = await tg(env, "setChatMenuButton", {
        menu_button: { type: "web_app", text: BOT_TEXT.menu[0], web_app: { url: url.origin + "/" } },
      });
      // command description per language (Telegram picks by the user's app language)
      let cmds = await tg(env, "setMyCommands", { commands: [{ command: "start", description: BOT_TEXT.cmd[1] }] });
      for (const [lc, i] of [["ru", 0], ["uk", 2], ["en", 1]])
        await tg(env, "setMyCommands", { commands: [{ command: "start", description: BOT_TEXT.cmd[i] }], language_code: lc });
      const me = await tg(env, "getMe", {});
      const ok = hook.ok && menu.ok && cmds.ok;
      const name = me.ok ? "@" + me.result.username : "?";
      return new Response(ok ? `Готово! Бот ${name} настроен. Напиши ему /start.` :
        "Что-то не так: " + JSON.stringify({ hook, menu, cmds }),
        { headers: { "content-type": "text/plain; charset=utf-8" } });
    }

    const want = url.searchParams.get("lang") || (request.headers.get("accept-language") || "ru").split(",")[0];
    return new Response(PAGES[botLang(want)], {
      encodeBody: "manual",
      headers: {
        "content-type": "text/html; charset=utf-8",
        "content-encoding": "gzip",
        "cache-control": "no-cache",
        "vary": "accept-language",
        "permissions-policy": "camera=(self)",
      },
    });
  },
};
''' % (page_b64, json.dumps(BOT, ensure_ascii=False))
open(os.path.join(here, 'worker.js'), 'w', encoding='utf-8').write(js)
print('worker.js', len(js.encode()), 'bytes, page gzip', [len(p) for p in pages])
