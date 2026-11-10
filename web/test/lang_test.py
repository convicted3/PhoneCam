import asyncio
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch()
        pg = await (await b.new_context(locale="ru-RU")).new_page()
        await pg.route("https://telegram.org/**", lambda r: r.fulfill(status=200, body="", content_type="text/javascript"))
        errs=[]; pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.goto("http://localhost:8099/phone.html?broker=ws://127.0.0.1:1")
        get = lambda: pg.evaluate("document.getElementById('go').textContent + ' | ' + document.getElementById('status').textContent + ' | ' + document.querySelector('#langs .on').textContent")
        print(await get())
        await pg.click("#langs button[data-l=en]"); print(await get())
        await pg.click("#langs button[data-l=uk]"); print(await get())
        await pg.reload(); await asyncio.sleep(0.5); print('after reload:', await get())
        print('errors', errs)
        await b.close()
asyncio.run(main())
