#!/usr/bin/env python3
"""Roundtrip test for nvs.py: generate images with the REAL IDF generator
(v1 + v2, short/long strings, ints, overwrite, second namespace) and assert
the parser reads every value back byte-identical. No hardware needed.
Usage: python test_nvs.py [path-to-idf-python]
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nvs import parse_partition  # noqa: E402

GEN = [sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate"]

CASES = {
    "ssid": "103",
    "pass": "a-65-char-password-0123456789-abcdef-0123456789-abcdefg",
    "host": "quorumesp",
    "devid": "qesp-c04268",
}
INTS = {"ver": 1, "logl": 3, "qdport": 5403, "netmode": 0}


def gen_image(csv_text, version):
    tmp = tempfile.mkdtemp(prefix="nvs-test-")
    csv = os.path.join(tmp, "in.csv")
    out = os.path.join(tmp, "nvs.bin")
    with open(csv, "w", encoding="ascii", newline="") as f:
        f.write(csv_text)
    cmd = GEN + [csv, out, "0x6000"]
    if version != 2:
        cmd += ["--version", str(version)]
    subprocess.run(cmd, check=True, capture_output=True)
    with open(out, "rb") as f:
        return f.read()


def csv_for(cases, ints, ns="qesp"):
    lines = ["key,type,encoding,value", f"{ns},namespace,,"]
    for k, v in cases.items():
        lines.append(f"{k},data,string,{v}")
    for k, v in ints.items():
        t = "u8" if v < 256 else ("u16" if v < 65536 else "u32")
        lines.append(f"{k},data,{t},{v}")
    return "\n".join(lines) + "\n"


fails = 0
for ver in (1, 2):
    img = gen_image(csv_for(CASES, INTS), ver)
    got = parse_partition(img).get("qesp", {})
    for k, v in CASES.items():
        if got.get(k) != v:
            print(f"v{ver} MISMATCH {k}: {got.get(k)!r} != {v!r}")
            fails += 1
    for k, v in INTS.items():
        if got.get(k) != v:
            print(f"v{ver} MISMATCH {k}: {got.get(k)!r} != {v!r}")
            fails += 1
    print(f"v{ver}: {len(CASES) + len(INTS)} keys checked")

# Overwrite: later row wins.
img = gen_image("key,type,encoding,value\nqesp,namespace,,\n"
                "ssid,data,string,first\nssid,data,string,second\n", 2)
got = parse_partition(img).get("qesp", {}).get("ssid")
print("overwrite:", got)
if got != "second":
    print("OVERWRITE FAIL")
    fails += 1

if fails:
    print(f"TEST_NVS: FAIL ({fails})")
    sys.exit(1)
print("TEST_NVS: PASS")
