"""Real CEF select popup: LoadURL -> pipe input -> composited BGRA pixels.

Run: python BrowserHost/tests/select_popup_smoke.py --host BrowserHost/build/Release/VeloWebViewBrowserHost.exe
Uses an isolated offscreen browser; sends no OS input. Optional --artifacts
writes the shared frames as PNGs for visual inspection.
"""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import struct
import threading
import time
import zlib

from input_smoke import Host


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except ConnectionResetError:
            # Chromium may close speculative connections when the host exits.
            pass


def pixel(frame, x, y):
    pixels, width, height = frame
    assert 0 <= x < width and 0 <= y < height
    offset = 4 * (y * width + x)
    return pixels[offset:offset + 4]


def wait_frame(host, label, predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        frame = host.frame()
        if frame and predicate(frame):
            print("PASS: " + label, flush=True)
            return frame
        time.sleep(0.05)
    raise AssertionError(label)


def color_points(frame, color):
    pixels, width, height = frame
    return [(x, y) for y in range(30, height - 5, 3)
            for x in range(45, width - 5, 3) if pixel(frame, x, y) == color]


def save_frame(frame, directory, name):
    if directory:
        directory.mkdir(parents=True, exist_ok=True)
        pixels, width, height = frame
        rgba = bytearray(pixels)
        rgba[0::4], rgba[2::4] = pixels[2::4], pixels[0::4]
        rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
        def chunk(kind, data):
            return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
        (directory / (name + ".png")).write_bytes(png)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "Content" / "Test"
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=str(root)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    host = Host(args.host.resolve(), url="about:blank")
    green = bytes.fromhex("8ac539ff")
    ready = bytes.fromhex("66cc33ff")
    background = bytes.fromhex("3e2414ff")
    try:
        host.connect()
        host.send(12, b"1")
        # Exercise the same LoadUrl protocol command used by UVeloWebView::LoadURL.
        url = "http://127.0.0.1:%d/SelectPopup.html" % server.server_port
        host.send(5, url.encode())
        closed = wait_frame(host, "LoadURL painted test page", lambda f: pixel(f, 10, 10) == ready)
        save_frame(closed, args.artifacts, "01_closed")
        host.send(12, b"1")
        host.click(100, 68)
        time.sleep(0.4)
        save_frame(host.frame(), args.artifacts, "02_open")
        opened = wait_frame(host, "option pixels visible below select",
                            lambda f: len(color_points(f, green)) > 30)
        points = color_points(opened, green)
        x, y = points[len(points) // 2]
        # A main-view repaint must preserve the independent popup surface.
        host.send(15, b"document.getElementById('underlay').style.background='#aacc22'")
        time.sleep(0.3)
        wait_frame(host, "popup survives main-view repaint", lambda f: pixel(f, x, y) == green)
        host.click(x, y)
        wait_frame(host, "click selects visible Green option and fires change",
                   lambda f: pixel(f, 310, 10) == ready and not color_points(f, green))
        wait_frame(host, "dismiss restores latest view pixels under popup",
                   lambda f: pixel(f, 90, 125) == bytes.fromhex("22ccaaff"))
        host.send(15, b"document.getElementById('underlay').style.background='#14243e'")
        host.click(100, 68)
        wait_frame(host, "popup reopens", lambda f: len(color_points(f, bytes.fromhex("5a3be5ff"))) > 30)
        host.key(27); host.key(27, up=True)
        wait_frame(host, "Escape removes popup without stale pixels",
                   lambda f: pixel(f, 90, 125) == background and not color_points(f, bytes.fromhex("5a3be5ff")))
        host.click(500, 458)
        edge = wait_frame(host, "viewport-edge popup visible", lambda f: len(color_points(f, green)) > 30)
        save_frame(edge, args.artifacts, "03_edge")
        x, y = color_points(edge, green)[len(color_points(edge, green)) // 2]
        host.send(15, b"document.getElementById('changed').style.background='#773333'")
        host.click(x, y)
        wait_frame(host, "viewport-edge option hit testing", lambda f: pixel(f, 310, 10) == ready and not color_points(f, green))
        # Resizing invalidates cached popup coordinates and surfaces.
        host.send(11, b"480,320,1")
        wait_frame(host, "view resized", lambda f: f[1:] == (480, 320))
        host.click(350, 298)
        resized = wait_frame(host, "popup after resize", lambda f: len(color_points(f, bytes.fromhex("5a3be5ff"))) > 30)
        save_frame(resized, args.artifacts, "04_resized")
        host.click(20, 210)
        wait_frame(host, "outside click dismisses popup", lambda f: not color_points(f, bytes.fromhex("5a3be5ff")))
        host.send(15, b"document.getElementById('normal').value='red';document.getElementById('changed').style.background='#773333'")
        wait_frame(host, "keyboard selection prepared", lambda f: pixel(f, 310, 10) == bytes.fromhex("333377ff"))
        host.click(100, 68)
        wait_frame(host, "popup for keyboard selection", lambda f: len(color_points(f, green)) > 30)
        host.key(40); host.key(40, up=True)
        host.key(13); host.key(13, up=True)
        wait_frame(host, "ArrowDown and Enter select Green", lambda f: pixel(f, 310, 10) == ready and not color_points(f, green))
        host.send(15, b"""(()=>{const s=document.getElementById('normal');s.innerHTML='';
          for(let i=0;i<30;i++){const o=new Option('Scroll option '+i,i);o.style.background=i>=20?'#478dff':'#e53b5a';s.add(o)}
          s.onchange=()=>document.getElementById('changed').style.background=Number(s.value)>=20?'#33cc66':'#773333';
          document.getElementById('changed').style.background='#773333'})()""")
        wait_frame(host, "long select prepared", lambda f: pixel(f, 310, 10) == bytes.fromhex("333377ff"))
        host.click(100, 68)
        wait_frame(host, "long popup visible", lambda f: len(color_points(f, bytes.fromhex("5a3be5ff"))) > 30)
        host.send(24, struct.pack("<iiIB3x", 100, 200, 0, 0))
        host.send(26, struct.pack("<iiiiI", 100, 200, 0, -1200, 0))
        blue = bytes.fromhex("ff8d47ff")
        scrolled = wait_frame(host, "wheel repaints scrolled popup", lambda f: len(color_points(f, blue)) > 30)
        save_frame(scrolled, args.artifacts, "05_scrolled")
        points = color_points(scrolled, blue)
        host.click(*points[len(points) // 2])
        wait_frame(host, "scrolled option hit testing", lambda f: pixel(f, 310, 10) == ready and not color_points(f, blue))
        host.click(100, 68)
        wait_frame(host, "popup before navigation", lambda f: len(color_points(f, blue)) > 30)
        host.send(5, (url + "?reload=1").encode())
        navigated = wait_frame(host, "navigation removes popup and resets page",
                               lambda f: pixel(f, 310, 10) == bytes.fromhex("333377ff") and not color_points(f, blue))
        save_frame(navigated, args.artifacts, "06_navigated")
        host.send(12, b"1")
        host.click(100, 68)
        wait_frame(host, "popup works after navigation", lambda f: len(color_points(f, green)) > 30)
        print("PASS: select popup rendering, mouse/keyboard/wheel input, repaint, dismissal, resize and navigation", flush=True)
    finally:
        host.close()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
