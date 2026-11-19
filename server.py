"""
로컬 웹 서버 + 펌웨어 컴파일 API

  python server.py            -> http://localhost:8000/barrier.html

- web/ 폴더의 파일을 제공합니다 (카메라 사용을 위해 localhost로 열어야 함).
- GET  /api/sketches  : firmware/ 안의 스케치 목록과 arduino-cli 사용 가능 여부
- POST /api/compile   : {"sketch": "esp32s3_barrier", "usbMode": "cdc" | "uart"}
                        arduino-cli로 컴파일해 업로드할 바이너리를 돌려줍니다.
  arduino-cli가 없으면 setup_arduino.bat 을 먼저 실행하세요.
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import urllib.request
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WEB = ROOT / "web"
FIRMWARE = ROOT / "firmware"
BUILD = ROOT / "build"
PORT = int(os.environ.get("PORT", "8000"))
FQBN = "esp32:esp32:esp32s3"
SERVER_VERSION = 3   # 이전 버전 서버와 구분

# Arduino 보드 패키지 저장 위치. 기본값(AppData\Local\Arduino15)은 Microsoft Store 앱이 실행한
# 프로그램이 쓰면 앱 전용 위치로 가려져 다른 프로그램에서 안 보일 수 있으므로, 홈 폴더 아래로 고정합니다.
ARDUINO_DATA = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".arduino15"
ESP32_INDEX_URL = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"


def cli_env():
    env = dict(os.environ)
    env["ARDUINO_DIRECTORIES_DATA"] = str(ARDUINO_DATA)
    env["ARDUINO_DIRECTORIES_DOWNLOADS"] = str(ARDUINO_DATA / "staging")
    env["ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS"] = ESP32_INDEX_URL
    return env


def find_cli():
    local = ROOT / "tools" / "arduino-cli.exe"
    if local.exists():
        return str(local)
    found = shutil.which("arduino-cli")
    if found:
        return found
    default = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Arduino CLI" / "arduino-cli.exe"
    return str(default) if default.exists() else None


def list_sketches():
    out = []
    for d in sorted(FIRMWARE.iterdir()) if FIRMWARE.exists() else []:
        if d.is_dir() and (d / f"{d.name}.ino").exists():
            out.append(d.name)
    return out


def compile_sketch(name, usb_mode):
    cli = find_cli()
    if not cli:
        raise RuntimeError("arduino-cli를 찾을 수 없습니다. setup_arduino.bat 을 먼저 실행하세요.")
    if name not in list_sketches():
        raise RuntimeError(f"스케치를 찾을 수 없습니다: {name}")

    fqbn = FQBN + (":CDCOnBoot=cdc" if usb_mode == "cdc" else "")
    out_dir = BUILD / f"{name}-{usb_mode}"
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [cli, "compile", "--fqbn", fqbn, "--output-dir", str(out_dir), str(FIRMWARE / name)]
    if not (ARDUINO_DATA / "packages" / "esp32" / "hardware" / "esp32").exists():
        raise RuntimeError(f"ESP32 보드 패키지가 없습니다 ({ARDUINO_DATA}). setup_arduino.bat 을 실행하세요.")
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=900, env=cli_env())
    log = (proc.stdout + "\n" + proc.stderr).strip()
    if proc.returncode != 0:
        raise RuntimeError("컴파일 실패\n" + log[-4000:])

    ino = f"{name}.ino"
    merged = out_dir / f"{ino}.merged.bin"
    if merged.exists():
        parts = [(0x0, merged)]
    else:  # 구버전 코어: 개별 파일 (ESP32-S3 주소)
        parts = [(0x0, out_dir / f"{ino}.bootloader.bin"),
                 (0x8000, out_dir / f"{ino}.partitions.bin"),
                 (0x10000, out_dir / f"{ino}.bin")]
        boot_app0 = next(ARDUINO_DATA.glob(
            "packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin"), None)
        if boot_app0:
            parts.insert(2, (0xE000, boot_app0))
    files = []
    for addr, p in parts:
        if not p.exists():
            raise RuntimeError(f"빌드 결과 파일이 없습니다: {p.name}\n" + log[-2000:])
        files.append({"address": addr, "name": p.name, "b64": base64.b64encode(p.read_bytes()).decode()})
    return {"files": files, "log": log[-3000:]}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WEB), **kw)

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/sketches":
            return self._json(200, {"sketches": list_sketches(), "cli": bool(find_cli()), "version": SERVER_VERSION})
        return super().do_GET()

    def do_POST(self):
        if self.path != "/api/compile":
            return self._json(404, {"error": "not found"})
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            result = compile_sketch(str(req.get("sketch", "")), "cdc" if req.get("usbMode") == "cdc" else "uart")
            self._json(200, result)
        except Exception as e:  # 오류 내용을 웹페이지 로그에 표시
            self._json(500, {"error": str(e)})


class Server(ThreadingHTTPServer):
    # Windows에서 SO_REUSEADDR을 쓰면 이미 사용 중인 포트에도 중복으로 열려 버리므로 끔
    allow_reuse_address = False

    def server_bind(self):
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def port_in_use(port):
    for host in ("127.0.0.1", "::1"):
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return True
        except OSError:
            pass
    return False


def is_our_server(port):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/sketches", timeout=1) as r:
            return json.loads(r.read()).get("version") == SERVER_VERSION
    except Exception:
        return False


def main():
    no_browser = "--no-browser" in sys.argv
    srv = None
    for port in range(PORT, PORT + 20):
        # 이미 이 서버(같은 버전)가 실행 중이면 브라우저만 열고 종료
        if not os.environ.get("PORT") and is_our_server(port):
            url = f"http://127.0.0.1:{port}/barrier.html"
            print("이미 실행 중인 서버가 있습니다:", url)
            if not no_browser:
                webbrowser.open(url)
            sys.exit(3)   # start.bat이 창을 바로 닫도록 알림
        if port_in_use(port):
            print(f"포트 {port} 사용 중 -> 다음 포트 시도")
            continue
        try:
            # 외부 기기에서 접근하지 못하도록 이 PC(127.0.0.1)에만 열기
            srv = Server(("127.0.0.1", port), Handler)
            break
        except OSError:
            print(f"포트 {port} 사용 중 -> 다음 포트 시도")
    if srv is None:
        print("사용 가능한 포트를 찾지 못했습니다.")
        sys.exit(1)

    url = f"http://127.0.0.1:{srv.server_port}/barrier.html"
    print("=" * 60)
    print(" 웹 서버 실행 중:", url)
    print(" arduino-cli:", find_cli() or "없음 (setup_arduino.bat 실행 필요, .bin 직접 업로드는 가능)")
    print(" 종료하려면 이 창을 닫거나 Ctrl+C")
    print("=" * 60)
    if not no_browser:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
