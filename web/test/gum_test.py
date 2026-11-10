import asyncio, os
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
        ctx = await b.new_context(locale="ru-RU")
        await ctx.add_init_script("window.__gum=0; const o=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices); navigator.mediaDevices.getUserMedia=(c)=>{window.__gum++; return o(c);};")
        await ctx.route("https://telegram.org/**", lambda r: r.fulfill(status=200, body="", content_type="text/javascript"))
        pg = await ctx.new_page()
        errs=[]; pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.goto("http://localhost:8099/phone.html?code=ABCD2345&broker=ws://127.0.0.1:9001")
        await asyncio.sleep(9)
        print('after connect+CFG: gum =', await pg.evaluate("window.__gum"), await pg.evaluate("document.getElementById('status').textContent"))
        await pg.click("#langs button[data-l=en]"); await asyncio.sleep(1)
        await pg.click("#dim"); await pg.click("#dark"); await asyncio.sleep(1)
        print('after lang/torch/dim: gum =', await pg.evaluate("window.__gum"), await pg.evaluate("document.getElementById('go').textContent"))
        await pg.click("#cam"); await asyncio.sleep(2)
        print('after camera switch: gum =', await pg.evaluate("window.__gum"))
        print('errors', errs)
        await b.close()
asyncio.run(main())
