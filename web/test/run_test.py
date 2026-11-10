import asyncio, subprocess, sys, time, os
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--use-file-for-fake-video-capture=" + os.path.abspath("fake.y4m")])
        ctx = await b.new_context()
        await ctx.route("https://telegram.org/**", lambda r: r.fulfill(status=200, body="", content_type="text/javascript"))
        pg = await ctx.new_page()
        pg.on("console", lambda m: print("console:", m.text, flush=True))
        await pg.goto("http://localhost:8099/phone.html?debug=1&code=ABCD2345&broker=ws://127.0.0.1:9001")
        caps = await pg.evaluate("RTCRtpSender.getCapabilities('video').codecs.map(c=>c.mimeType+' '+(c.sdpFmtpLine||'')).join('\\n')")
        print("codecs:\n" + caps, flush=True)
        for i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 18):
            await asyncio.sleep(1)
            st = await pg.evaluate("document.getElementById('status').textContent + ' | ' + document.getElementById('pill').textContent")
            print("page:", st, flush=True)
        await pg.screenshot(path="page.png")
        await b.close()
asyncio.run(main())
