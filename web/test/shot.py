import asyncio, os
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
        ctx = await b.new_context(viewport={"width": 390, "height": 760}, device_scale_factor=2, is_mobile=True, has_touch=True)
        await ctx.route("https://telegram.org/**", lambda r: r.fulfill(status=200, body="", content_type="text/javascript"))
        pg = await ctx.new_page()
        await pg.goto("http://localhost:8099/phone.html?broker=ws://127.0.0.1:9001")
        await asyncio.sleep(1); await pg.screenshot(path="m1.png")
        await pg.fill("#code", "abcd2345"); await pg.click("#go")
        await asyncio.sleep(6); await pg.screenshot(path="m2.png")
        await b.close()
asyncio.run(main())
