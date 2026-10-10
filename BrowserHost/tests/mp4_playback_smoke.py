"""Check a real H.264/AAC MP4 through the offscreen BrowserHost."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--page", type=Path, required=True)
    args = parser.parse_args()
    executable = args.host.resolve()
    page = args.page.resolve().as_uri()
    instance = 1300000000 + os.getpid()
    pipe_name = "VeloMP4Smoke_%d" % os.getpid()
    cache = tempfile.TemporaryDirectory(prefix="VeloMP4Smoke_")
    process = subprocess.Popen([
        str(executable), "--disable-gpu", "--width=640", "--height=360",
        "--url=" + page, "--instance-id=%d" % instance,
        "--pipe=" + pipe_name, "--cache-path=" + cache.name
    ], cwd=str(executable.parent), creationflags=subprocess.CREATE_NO_WINDOW)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.OpenFileMappingW.restype = wintypes.HANDLE
    kernel.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
    kernel.MapViewOfFile.restype = ctypes.c_void_p
    kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    pipe = None
    mapping = None
    view = None
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            pipe = kernel.CreateFileW("\\\\.\\pipe\\" + pipe_name, 0xC0000000, 0, None, 3, 0, None)
            if pipe != ctypes.c_void_p(-1).value:
                break
            pipe = None
            time.sleep(0.05)
        if not pipe:
            raise RuntimeError("BrowserHost pipe did not open")

        sequence = 1
        def send(command, payload=b""):
            nonlocal sequence
            header = struct.pack("<IHHIIQ", 0x56454C4F, 1, command,
                                 24 + len(payload), instance, sequence)
            sequence += 1
            for data in (header, payload):
                if not data:
                    continue
                written = wintypes.DWORD()
                if not kernel.WriteFile(pipe, data, len(data), ctypes.byref(written), None):
                    raise OSError(ctypes.get_last_error(), "named pipe write failed")

        send(1, str(instance).encode())
        time.sleep(1.5)
        script = """(()=>{const v=document.querySelector('video');if(!v){document.body.style.background='#ff0000';return}v.muted=true;v.play().catch(()=>{});const f=()=>{document.body.style.background=v.error?'#ff0000':(v.readyState>=3&&v.currentTime>0.1?'#00ff00':'#ffff00')};['loadedmetadata','canplay','playing','timeupdate','error'].forEach(e=>v.addEventListener(e,f));setInterval(f,100);f()})()"""
        send(15, script.encode())

        mapping_name = "Local\\VeloWebView_%d_frames" % instance
        result = None
        while time.monotonic() < deadline:
            mapping = kernel.OpenFileMappingW(4, False, mapping_name)
            if mapping:
                view = kernel.MapViewOfFile(mapping, 4, 0, 0, 0)
                if view:
                    header = struct.unpack("<IHHIIIIIIQII", ctypes.string_at(view, 48))
                    if header[9]:
                        address = view + header[2] + header[10] * header[8]
                        slot = struct.unpack("<QQQIIIIIIQQIiiii", ctypes.string_at(address, 84))
                        pixel = ctypes.string_at(address + 84, 4)
                        if pixel in (bytes.fromhex("00ff00ff"), bytes.fromhex("0000ffff"), bytes.fromhex("00ffffff")):
                            result = (slot[0], pixel.hex())
                            if pixel == bytes.fromhex("00ff00ff"):
                                break
                if view:
                    kernel.UnmapViewOfFile(view)
                    view = None
                kernel.CloseHandle(mapping)
                mapping = None
            time.sleep(0.05)
        if result is None:
            raise RuntimeError("MP4 page did not publish a recognized status frame")
        print("frame sequence=%d status_pixel=%s" % result)
        if result[1] != "00ff00ff":
            raise RuntimeError("MP4 did not reach playing state before timeout")
        print("PASS: H.264/AAC MP4 reached playing state")
    finally:
        if view:
            kernel.UnmapViewOfFile(view)
        if mapping:
            kernel.CloseHandle(mapping)
        if pipe:
            kernel.CloseHandle(pipe)
        if process.poll() is None:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process.wait(timeout=10)
        try:
            cache.cleanup()
        except (PermissionError, NotADirectoryError):
            pass


if __name__ == "__main__":
    main()
