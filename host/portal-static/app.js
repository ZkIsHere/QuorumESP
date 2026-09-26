/* QuorumESP static portal logic (browser only, no server).
 * Needs esptool-js (CDN bundle) + nvs.js. All device I/O over WebSerial.
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const logEl = () => $("log");
  function log(s) { logEl().textContent = s; }
  function logAdd(s) { logEl().textContent += "\n" + s; }

  let port = null;
  const NVS_ADDR = 0x9000, NVS_SIZE = 0x6000, APP_ADDR = 0x20000;
  const QTYPES = { ver: "u32", host: "string", wssid: "string", wpass: "string",
                   netmode: "u8", devid: "string", logl: "u8", qdport: "u16" };
  const ORDER = ["ver", "host", "wssid", "wpass", "netmode", "devid", "logl", "qdport"];

  function term() {
    return { clean() {}, writeLine(d) { logAdd(d); }, write(d) {} };
  }
  async function loader(baud) {
    const transport = new esptoolJS.Transport(port, true);
    const l = new esptoolJS.ESPLoader({ transport, baudrate: baud || 115200, terminal: term() });
    return { transport, loader: l };
  }

  $("bPort").onclick = async () => {
    try {
      port = await navigator.serial.requestPort({ filters: [
        { usbVendorId: 0x10c4 }, { usbVendorId: 0x1a86 }, { usbVendorId: 0x303a }] });
      log("port picked");
      for (const id of ["bInfo", "bVer", "bFlash", "bCfg", "bSave"])
        $(id).disabled = false;
    } catch (e) { log("port pick cancelled/failed: " + e); }
  };

  async function withLoader(fn, baud) {
    const { transport, loader } = await loader(baud);
    try {
      const chip = await loader.main();
      return await fn(loader, transport, chip);
    } finally {
      try { await transport.disconnect(); } catch (e) { /* already gone */ }
    }
  }

  $("bInfo").onclick = async () => {
    try {
      await withLoader(async (loader, transport, chip) => {
        let mac = "?";
        try { mac = await loader.readMac(); } catch (e) { mac = "(readMac unsupported: " + e + ")"; }
        $("board").textContent = chip + " / " + mac;
      });
    } catch (e) { log("board info failed: " + e); }
  };

  $("bVer").onclick = async () => {
    // Open raw serial, reset the board, catch "App version:" from boot.
    log("resetting board, reading boot log (12s)...");
    try {
      await port.open({ baudRate: 115200 });
      const writer = port.writable.getWriter();
      const reader = port.readable.getReader();
      await port.setSignals({ dataTerminalReady: true, requestToSend: false });
      await new Promise((r) => setTimeout(r, 200));
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      writer.releaseLock();
      const dec = new TextDecoder();
      let buf = "", ver = null;
      const t0 = Date.now();
      while (Date.now() - t0 < 12000) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        const m = buf.match(/App version:\s+(\S+)/);
        if (m) { ver = m[1]; break; }
      }
      try { reader.cancel(); } catch (e) { /* ignore */ }
      reader.releaseLock();
      await port.close();
      $("board").textContent = "app version: " + (ver || "not seen");
    } catch (e) { log("version read failed: " + e); try { await port.close(); } catch (x) {} }
  };

  async function loadReleases() {
    const sel = $("rel");
    try {
      const r = await fetch("https://api.github.com/repos/ZkIsHere/QuorumESP/releases?per_page=5");
      const list = await r.json();
      sel.innerHTML = "";
      for (const rel of list) {
        const a = (rel.assets || []).find((x) => x.name === "firmware.bin");
        if (!a) continue;
        const o = document.createElement("option");
        o.text = rel.tag_name; o.value = a.browser_download_url;
        sel.add(o);
      }
      if (!sel.options.length) sel.add(new Option("(no firmware assets)", ""));
    } catch (e) {
      sel.innerHTML = "";
      sel.add(new Option("(offline — use local .bin)", ""));
    }
  }
  $("bRel").onclick = loadReleases;
  loadReleases();

  async function firmwareBytes() {
    const f = $("locbin").files[0];
    if (f) return new Uint8Array(await f.arrayBuffer());
    const url = $("rel").value;
    if (!url || !url.startsWith("http")) throw new Error("choose a release or a local file");
    log("downloading release (via worker proxy)...");
    const r = await fetch("/api/dl?url=" + encodeURIComponent(url));
    if (!r.ok) throw new Error("download failed: " + r.status);
    return new Uint8Array(await r.arrayBuffer());
  }

  $("bFlash").onclick = async () => {
    try {
      const data = await firmwareBytes();
      await withLoader(async (loader) => {
        log("flashing " + data.length + " bytes to 0x" + APP_ADDR.toString(16) + "...");
        await loader.writeFlash({ fileArray: [{ data, address: APP_ADDR }],
          flashSize: "4MB", eraseAll: false, compress: true,
          flashMode: "dio", flashFreq: "40m",
          reportProgress: (i, w, t) => log("flashing " + ((w / t * 100).toFixed(1)) + "%") });
        await loader.after("hard_reset");
        log("done. NVS/config untouched.");
      }, 460800);
    } catch (e) { log("flash failed: " + e); }
  };

  async function readConfig() {
    return withLoader(async (loader) => {
      log("reading NVS...");
      const data = await loader.readFlash(NVS_ADDR, NVS_SIZE,
        () => {});
      const q = (window.QespNvs.parsePartition(new Uint8Array(data)).qesp) || {};
      return q;
    });
  }

  $("bCfg").onclick = async () => {
    try {
      const q = await readConfig();
      $("f_ssid").value = q.wssid || "";
      $("f_pass").value = q.wpass || "";
      $("f_host").value = q.host || "";
      $("f_qdport").value = q.qdport != null ? q.qdport : "";
      $("f_logl").value = q.logl != null ? q.logl : "";
      $("f_net").value = q.netmode != null ? q.netmode : "";
      $("cfgmsg").textContent = "loaded from board (keys preserved on save)";
      log("config loaded");
    } catch (e) { $("cfgmsg").textContent = "error: " + e; }
  };

  function numOr(v, dflt) {
    if (v === "" || v == null) return dflt;
    const n = Number(v);
    return Number.isFinite(n) ? n : dflt;
  }

  $("bSave").onclick = async () => {
    try {
      const cur = await readConfig();
      if (cur.ver == null) throw new Error("no qesp config on device");
      const get = (id, curKey) => {
        const v = $(id).value;
        return v === "" ? cur[curKey] : v;
      };
      const mapping = {
        ver: ["u32", 1],
        host: ["string", get("f_host", "host") || "quorumesp"],
        wssid: ["string", get("f_ssid", "wssid")],
        wpass: ["string", get("f_pass", "wpass") || ""],
        netmode: ["u8", numOr($("f_net").value, cur.netmode != null ? cur.netmode : 0)],
        devid: ["string", cur.devid || ""],
        logl: ["u8", numOr($("f_logl").value, cur.logl != null ? cur.logl : 3)],
        qdport: ["u16", numOr($("f_qdport").value, cur.qdport != null ? cur.qdport : 5403)],
      };
      if (!mapping.wssid[1] || mapping.wssid[1].length > 32) throw new Error("ssid 1..32");
      if (mapping.wpass[1].length > 64) throw new Error("password 0..64");
      if (!mapping.qdport[1]) throw new Error("qdport nonzero");
      const img = window.QespNvs.generate(mapping, ORDER, NVS_SIZE);
      await withLoader(async (loader) => {
        log("writing NVS (" + img.length + " bytes)...");
        await loader.writeFlash({ fileArray: [{ data: img, address: NVS_ADDR }],
          flashSize: "4MB", eraseAll: false, compress: true,
          flashMode: "dio", flashFreq: "40m" });
        await loader.after("hard_reset");
        log("saved. Press EN if the board does not rejoin by itself.");
      }, 460800);
    } catch (e) { $("cfgmsg").textContent = "error: " + e; }
  };
})();
