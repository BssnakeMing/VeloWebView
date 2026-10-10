"""Windows OSR integration check: pipe input -> CEF DOM -> shared BGRA frame.

Run: python BrowserHost/tests/input_smoke.py --host BrowserHost/build/Release/VeloWebViewBrowserHost.exe
This talks to an offscreen BrowserHost test instance; it sends no OS input.
"""
import argparse
import base64
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

TESTS = ["move", "click", "right", "double", "text", "unicode", "ctrl", "delete",
         "key_down", "key_up", "wheel", "scroll", "drag"]
HTML = r"""<!doctype html><style>
body{margin:0;background:#14243e}button,input,#scroll{position:absolute}
#button{left:20px;top:20px;width:220px;height:50px}
#text{left:20px;top:95px;width:300px;height:40px;box-sizing:border-box}
#range{left:20px;top:165px;width:300px}
#scroll{left:20px;top:230px;width:350px;height:140px;overflow:auto;background:#ddd}
#markers{position:absolute;left:10px;top:420px;display:flex;gap:4px}
#markers div{width:18px;height:18px;background:#773333}
</style><button id="button">CLICK</button><input id="text"><input id="range" type="range" value="10">
<div id="scroll"><div style="height:900px">Scroll</div></div><div id="markers"></div>
<script>
const ids=TEST_IDS,markers=document.getElementById('markers');
ids.forEach(id=>{const d=document.createElement('div');d.id='m_'+id;markers.appendChild(d)});
function mark(id){document.getElementById('m_'+id).style.background='#33cc66'}
const b=document.getElementById('button'),t=document.getElementById('text');let selected=false;
b.onmousemove=()=>mark('move');b.onclick=()=>mark('click');b.ondblclick=()=>mark('double');
b.oncontextmenu=e=>{e.preventDefault();mark('right')};
t.oninput=()=>{if(t.value==='Velo')mark('text');if(t.value==='Velo\u4e2d')mark('unicode');if(!t.value&&selected)mark('delete')};
t.onkeydown=e=>{if(e.ctrlKey&&e.key.toLowerCase()==='a'){selected=true;mark('ctrl')}
 if(e.key==='ArrowLeft'||e.key==='Left'||e.keyCode===37||e.code==='ArrowLeft')mark('key_down')};
t.onkeyup=e=>{if(e.key==='ArrowLeft'||e.key==='Left'||e.keyCode===37||e.code==='ArrowLeft')mark('key_up')};
const sc=document.getElementById('scroll');sc.onwheel=()=>mark('wheel');sc.onscroll=()=>{if(sc.scrollTop>0)mark('scroll')};
document.getElementById('range').oninput=e=>{if(Number(e.target.value)>70)mark('drag')};
</script>""".replace("TEST_IDS", json.dumps(TESTS))


class Host:
    def __init__(self, executable):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self.kernel
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        k.CreateFileW.restype = wintypes.HANDLE
        k.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        k.OpenFileMappingW.restype = wintypes.HANDLE
        k.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                    wintypes.DWORD, ctypes.c_size_t]
        k.MapViewOfFile.restype = ctypes.c_void_p
        k.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.instance = 1000000000 + os.getpid()
        self.pipe_name = "VeloInputSmoke_%d" % os.getpid()
        self.sequence = 0
        self.pipe = None
        self.mapping = None
        self.view = None
        self.temp = tempfile.TemporaryDirectory(prefix="VeloInputSmoke_")
        self.process = subprocess.Popen([
            str(executable), "--disable-gpu", "--width=640", "--height=480",
            "--instance-id=%d" % self.instance, "--pipe=" + self.pipe_name,
            "--cache-path=" + self.temp.name,
            "--url=data:text/html;base64," + base64.b64encode(HTML.encode()).decode()
        ], cwd=str(executable.parent), creationflags=subprocess.CREATE_NO_WINDOW)

    def connect(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Host exited during startup: %s" % self.process.returncode)
            pipe = self.kernel.CreateFileW("\\\\.\\pipe\\" + self.pipe_name,
                                           0xC0000000, 0, None, 3, 0, None)
            if pipe != ctypes.c_void_p(-1).value:
                self.pipe = pipe
                return
            time.sleep(0.05)
        raise TimeoutError("BrowserHost pipe did not open")

    def send(self, message_type, payload=b""):
        self.sequence += 1
        header = struct.pack("<IHHIIQ", 0x56454C4F, 1, message_type,
                             24 + len(payload), self.instance, self.sequence)
        for data in (header, payload):
            if not data:
                continue
            written = wintypes.DWORD()
            if not self.kernel.WriteFile(self.pipe, data, len(data), ctypes.byref(written), None) or written.value != len(data):
                raise OSError(ctypes.get_last_error(), "pipe write")

    def mouse(self, x, y, button=0, up=False, count=1):
        modifiers = 0 if up else (16, 32, 64)[button]
        self.send(25, struct.pack("<iiIBBBB", x, y, modifiers, button, up, count, 0))

    def click(self, x, y, button=0, count=1):
        self.mouse(x, y, button, count=count)
        self.mouse(x, y, button, up=True, count=count)

    def key(self, vk, up=False, modifiers=0):
        scan = self.user.MapVirtualKeyW(vk, 0)
        native = 1 | (scan << 16)
        if vk in (37, 38, 39, 40, 45, 46):
            native |= 1 << 24
        if up:
            native |= (1 << 30) | (1 << 31)
        self.send(27, struct.pack("<BBHIIIII", 2 if up else 0, 0, 0, vk, native, 0, 0, modifiers))

    def char(self, character):
        value = ord(character)
        self.send(27, struct.pack("<BBHIIIII", 3, 0, 0, value, 0, value, value, 0))

    def frame(self):
        if not self.mapping:
            self.mapping = self.kernel.OpenFileMappingW(4, False, "Local\\VeloWebView_%d_frames" % self.instance)
            if not self.mapping:
                return None
            self.view = self.kernel.MapViewOfFile(self.mapping, 4, 0, 0, 0)
        header = struct.unpack("<IHHIIIIIIQII", ctypes.string_at(self.view, 48))
        sequence, slot_index = header[9:11]
        if not sequence:
            return None
        address = self.view + header[2] + slot_index * header[8]
        slot = struct.unpack("<QQQIIIIIIQQIiiii", ctypes.string_at(address, 84))
        if slot[0] != sequence or not slot[6] & 2:
            return None
        pixels = ctypes.string_at(address + 84, slot[11])
        if struct.unpack("<Q", ctypes.string_at(address, 8))[0] != sequence:
            return None
        return pixels, slot[3], slot[4]

    def passed(self, name):
        frame = self.frame()
        if not frame:
            return False
        pixels, width, height = frame
        x, y = 18 + 22 * TESTS.index(name), 428
        if x >= width or y >= height:
            return False
        offset = 4 * (y * width + x)
        return pixels[offset:offset + 4] == bytes.fromhex("66cc33ff")

    def wait(self, name):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.passed(name):
                print("PASS: " + name, flush=True)
                return
            time.sleep(0.05)
        raise AssertionError("DOM/frame assertion failed: " + name)

    def close(self):
        if self.view:
            self.kernel.UnmapViewOfFile(self.view)
        if self.mapping:
            self.kernel.CloseHandle(self.mapping)
        if self.pipe:
            self.kernel.CloseHandle(self.pipe)
        if self.process.poll() is None:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.process.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.process.wait(timeout=10)
        self.temp.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    args = parser.parse_args()
    host = Host(args.host.resolve())
    try:
        host.connect()
        host.send(12, b"1")
        # Wait until DOM content is painted, then check real user-input behavior.
        time.sleep(1)
        host.send(24, struct.pack("<iiIB3x", 100, 40, 0, 0)); host.wait("move")
        host.click(100, 40); host.wait("click")
        host.click(100, 40, button=2); host.wait("right")
        host.click(100, 40, count=2); host.wait("double")
        host.click(100, 115)
        for character in "Velo": host.char(character)
        host.wait("text")
        host.char("中"); host.wait("unicode")
        host.key(37); host.wait("key_down")
        host.key(37, up=True); host.wait("key_up")
        host.key(17, modifiers=4); host.key(65, modifiers=4)
        host.key(65, up=True, modifiers=4); host.key(17, up=True)
        host.wait("ctrl")
        host.key(8); host.char("\b"); host.key(8, up=True); host.wait("delete")
        host.send(26, struct.pack("<iiiiI", 100, 280, 0, -120, 0))
        host.wait("wheel"); host.wait("scroll")
        host.mouse(55, 173)
        host.send(24, struct.pack("<iiIB3x", 285, 173, 16, 0))
        host.mouse(285, 173, up=True); host.wait("drag")
        print("PASS: all %d pipe -> DOM -> OSR assertions" % len(TESTS))
    finally:
        host.close()


if __name__ == "__main__":
    main()
