"""Check WebRTC H.264 capability through the real offscreen BrowserHost."""
import base64
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time


HTML = """<!doctype html><meta charset='utf-8'><body><script>
const codecs=RTCRtpReceiver.getCapabilities?.('video')?.codecs||[];
const h264=codecs.some(c=>(c.mimeType||'').toLowerCase()==='video/h264');
document.body.style='margin:0;width:100vw;height:100vh;background:'+(h264?'#00ff00':'#ff0000');
document.body.textContent=h264?'H264':'NO-H264';
</script></body>"""


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    args = parser.parse_args()
    executable = args.host.resolve()
    instance = 1200000000 + os.getpid()
    pipe_name = "VeloCodecSmoke_%d" % os.getpid()
    cache = tempfile.TemporaryDirectory(prefix="VeloCodecSmoke_")
    url = "data:text/html;base64," + base64.b64encode(HTML.encode()).decode()
    process = subprocess.Popen([
        str(executable), "--disable-gpu", "--width=64", "--height=32",
        "--url=" + url, "--instance-id=%d" % instance,
        "--pipe=" + pipe_name, "--cache-path=" + cache.name
    ], cwd=str(executable.parent), creationflags=subprocess.CREATE_NO_WINDOW)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.OpenFileMappingW.restype = wintypes.HANDLE
    kernel.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
    kernel.MapViewOfFile.restype = ctypes.c_void_p
    kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    mapping = None
    view = None
    try:
        name = "Local\\VeloWebView_%d_frames" % instance
        deadline = time.monotonic() + 15
        result = None
        while time.monotonic() < deadline:
            mapping = kernel.OpenFileMappingW(4, False, name)
            if mapping:
                view = kernel.MapViewOfFile(mapping, 4, 0, 0, 0)
                if view:
                    header = struct.unpack("<IHHIIIIIIQII", ctypes.string_at(view, 48))
                    if header[9]:
                        address = view + header[2] + header[10] * header[8]
                        slot = struct.unpack("<QQQIIIIIIQQIiiii", ctypes.string_at(address, 84))
                        pixel = ctypes.string_at(address + 84, 4)
                        if pixel in (bytes.fromhex("00ff00ff"), bytes.fromhex("0000ffff")):
                            result = (slot[0], slot[3], slot[4], pixel.hex())
                            break
                if view:
                    kernel.UnmapViewOfFile(view)
                    view = None
                kernel.CloseHandle(mapping)
                mapping = None
            time.sleep(0.05)
        if result is None:
            raise RuntimeError("BrowserHost did not publish a frame")
        sequence, width, height, pixel = result
        print("frame sequence=%d size=%dx%d first_pixel=%s" % (sequence, width, height, pixel))
        if pixel != "00ff00ff":
            raise RuntimeError("WebRTC H.264 capability page reported NO-H264 (expected green BGRA pixel)")
        print("PASS: BrowserHost reports video/H264")
    finally:
        if view:
            kernel.UnmapViewOfFile(view)
        if mapping:
            kernel.CloseHandle(mapping)
        if process.poll() is None:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process.wait(timeout=10)
        # CEF helper processes can hold chrome_debug.log briefly after the
        # browser process exits; leave the temporary cache for OS cleanup if
        # Windows still has an open handle.
        try:
            cache.cleanup()
        except (PermissionError, NotADirectoryError):
            pass


if __name__ == "__main__":
    main()
