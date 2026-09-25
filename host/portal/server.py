"""QuorumESP PC portal — external management web (no on-device updater).
Runs on the user's computer, talks to ESP32 boards over USB serial:
- list serial ports (pick your ESP32)
- flash app firmware (.bin upload or GitHub release asset) to 0x20000
- provision Wi-Fi (SSID/pass -> NVS at 0x9000)
- read back the running app version from the boot log

Stdlib only. Needs esptool + pyserial on PATH (ESP-IDF python env has
both) and IDF_PATH set for nvs_partition_gen.py.
Usage: python server.py [port]   (default 127.0.0.1:8080)
"""
import http.server
import json
import os
import re
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
HERE = os.path.dirname(os.path.abspath(__file__))
IDF_PATH = os.environ.get("IDF_PATH", "")
JOBS = {}
JOB_SEQ = [0]
JOBS_LOCK = threading.Lock()


def run_job(name, cmd, cwd=None):
    """Run cmd in background, collect lines. Returns job id."""
    with JOBS_LOCK:
        JOB_SEQ[0] += 1
        jid = str(JOB_SEQ[0])
        JOBS[jid] = {"name": name, "lines": [], "done": False, "rc": None}

    def worker():
        try:
            p = subprocess.Popen(
                cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1)
            for line in p.stdout:
                with JOBS_LOCK:
                    JOBS[jid]["lines"].append(line.rstrip("\n"))
            p.wait()
            with JOBS_LOCK:
                JOBS[jid]["rc"] = p.returncode
                JOBS[jid]["done"] = True
        except Exception as e:  # noqa: BLE001 - surfaced to UI
            with JOBS_LOCK:
                JOBS[jid]["lines"].append(f"portal error: {e}")
                JOBS[jid]["rc"] = 1
                JOBS[jid]["done"] = True

    threading.Thread(target=worker, daemon=True).start()
    return jid


def list_ports():
    try:
        from serial.tools import list_ports as lp
        return [p.device for p in lp.comports()]
    except Exception:  # noqa: BLE001 - fallback scan
        pass
    found = []
    if sys.platform == "win32":
        for i in range(1, 21):
            name = f"COM{i}"
            try:
                import serial
                s = serial.Serial(name)
                s.close()
                found.append(name)
            except Exception:  # noqa: BLE001 - port absent, skip
                pass
    else:
        import glob
        found = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return found


def github_releases():
    try:
        req = urllib.request.Request(
            "https://api.github.com/repos/ZkIsHere/QuorumESP/releases?per_page=5",
            headers={"User-Agent": "QuorumESP-portal"})
        with urllib.request.urlopen(req, timeout=15) as r:
            data = json.load(r)
        out = []
        for rel in data:
            bins = [a["browser_download_url"] for a in rel.get("assets", [])
                    if a.get("name") == "firmware.bin"]
            if bins:
                out.append({"tag": rel.get("tag_name"), "url": bins[0]})
        return out
    except Exception as e:  # noqa: BLE001 - offline, UI shows error
        return {"error": str(e)}


def download(url, dst):
    req = urllib.request.Request(url, headers={"User-Agent": "QuorumESP-portal"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dst, "wb") as f:
        while True:
            chunk = r.read(65536)
            if not chunk:
                break
            f.write(chunk)


def read_version(port, baud=115200, seconds=12):
    """Reset the board by opening serial, catch 'App version:' from boot."""
    try:
        import serial
        ser = serial.Serial(port, baud, timeout=1)
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.5)
        # Toggle EN via DTR to force a clean reboot (then release).
        ser.setDTR(True)
        time.sleep(0.2)
        ser.setDTR(False)
        t0 = time.time()
        buf = b""
        ver = None
        while time.time() - t0 < seconds:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                m = re.search(rb"App version:\s+(\S+)", buf)
                if m:
                    ver = m.group(1).decode("ascii", "replace")
                    break
        ser.close()
        return ver or "not seen (see full log in job output)"
    except Exception as e:  # noqa: BLE001 - surfaced to UI
        return f"serial error: {e}"


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "QuorumESP-portal/1"

    def log_message(self, *a):  # quiet
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/ports":
            self.send_json({"ports": list_ports()})
        elif path == "/api/releases":
            self.send_json({"releases": github_releases()})
        elif path.startswith("/api/job/"):
            jid = path.rsplit("/", 1)[-1]
            with JOBS_LOCK:
                job = dict(JOBS.get(jid, {"error": "no such job"}))
            self.send_json(job)
        elif path.startswith("/api/version"):
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            port = (q.get("port") or [""])[0]
            self.send_json({"version": read_version(port)})
        else:
            self.send_error(404)

    def read_json(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return None
        if n <= 0 or n > 4096:
            return None
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except ValueError:
            return None

    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler API
        path = urllib.parse.urlsplit(self.path).path
        if path == "/api/flash":
            body = self.read_json() or {}
            port, tag, url = (body.get("port") or "", body.get("tag") or "",
                              body.get("url") or "")
            if not port:
                self.send_json({"error": "no port"}, 400)
                return
            tmp = tempfile.mkdtemp(prefix="qesp-portal-")
            dst = os.path.join(tmp, "firmware.bin")
            try:
                if url:
                    download(url, dst)
                else:
                    # Local .bin path pasted by the user (same machine).
                    with open(tag, "rb") as src, open(dst, "wb") as out:
                        out.write(src.read())
            except Exception as e:  # noqa: BLE001 - surfaced to UI
                self.send_json({"error": f"fetch firmware: {e}"}, 400)
                return
            jid = run_job(f"flash {port}", [sys.executable, "-m", "esptool",
                                             "--chip", "esp32", "-p", port,
                                             "write_flash", "0x20000", dst])
            self.send_json({"job": jid})
        elif path == "/api/provision":
            body = self.read_json() or {}
            port, ssid, password = (body.get("port") or "", body.get("ssid") or "",
                                    body.get("password") or "")
            if not port or not ssid:
                self.send_json({"error": "port + ssid required"}, 400)
                return
            if len(ssid) > 32 or len(password) > 64:
                self.send_json({"error": "ssid<=32, password<=64"}, 400)
                return
            gen = os.path.join(IDF_PATH, "components", "nvs_flash",
                               "nvs_partition_generator", "nvs_partition_gen.py")
            if not os.path.isfile(gen):
                self.send_json({"error": "IDF_PATH not set (need nvs_partition_gen.py)"},
                               400)
                return
            tmp = tempfile.mkdtemp(prefix="qesp-portal-")
            csv = os.path.join(tmp, "wifi.csv")
            out = os.path.join(tmp, "nvs-wifi.bin")
            with open(csv, "w", encoding="ascii",
                      errors="replace", newline="") as f:
                f.write("key,type,encoding,value\nqesp,namespace,,\n"
                        f"wssid,data,string,{ssid}\n"
                        f"wpass,data,string,{password}\n")
            try:
                subprocess.run([sys.executable, gen, "generate", csv, out,
                                "0x6000"], check=True, capture_output=True,
                               text=True, timeout=60)
            except Exception as e:  # noqa: BLE001 - surfaced to UI
                self.send_json({"error": f"nvs generate: {e}"}, 400)
                return
            jid = run_job(f"provision {port}", [sys.executable, "-m", "esptool",
                                                 "--chip", "esp32", "-p", port,
                                                 "write_flash", "0x9000", out])
            self.send_json({"job": jid})
        else:
            self.send_error(404)


if __name__ == "__main__":
    with socketserver.TCPServer((HOST, PORT), Handler) as httpd:
        print(f"QuorumESP portal on http://{HOST}:{PORT}", flush=True)
        httpd.serve_forever()
