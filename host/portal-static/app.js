/* QuorumESP static portal: pick a board card, connect over USB,
 * show firmware version on top, flash release firmware to 0x20000.
 * No server, no file picker, no config UI. Needs esptool-js (CDN).
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const msg = (s) => { $("msg").textContent = s; };

  let port = null;
  let boardDef = null;
  const APP_ADDR = 0x20000;

  // Render board cards from the static catalog.
  const grid = $("boards");
  window.QESP_BOARDS.forEach((b, i) => {
    const d = document.createElement("div");
    d.className = "board";
    d.innerHTML = b.svg + "<h3></h3><p></p>";
    d.querySelector("h3").textContent = b.model;
    d.querySelector("p").textContent = b.desc;
    d.onclick = () => connect(i);
    b.el = d;
    grid.appendChild(d);
  });

  function term() {
    return { clean() {}, writeLine(d) { msg(d); }, write(d) {} };
  }

  async function connect(i) {
    boardDef = window.QESP_BOARDS[i];
    try {
      port = await navigator.serial.requestPort({ filters: boardDef.filters });
    } catch (e) { msg("port pick cancelled"); return; }
    window.QESP_BOARDS.forEach((b) => b.el.classList.remove("sel"));
    boardDef.el.classList.add("sel");
    msg("connected: " + boardDef.model + " — reading firmware version...");
    $("bFlash").disabled = false;
    await loadReleases();
    await readVersion();
  }

  async function readVersion() {
    // Raw serial: reset the board, catch "App version:" from boot.
    try {
      await port.open({ baudRate: 115200 });
      await port.setSignals({ dataTerminalReady: true, requestToSend: false });
      await new Promise((r) => setTimeout(r, 200));
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      const reader = port.readable.getReader();
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
      $("fw").textContent = "firmware " + (ver || "(not seen — press EN)");
      msg(ver ? "ready" : "version not seen");
    } catch (e) {
      msg("version read failed: " + e);
      try { await port.close(); } catch (x) { /* ignore */ }
    }
  }

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
      sel.add(new Option("(offline)", ""));
    }
  }

  $("bFlash").onclick = async () => {
    const url = $("rel").value;
    if (!url || !url.startsWith("http")) { msg("pick a release first"); return; }
    try {
      msg("downloading release (via worker proxy)...");
      const r = await fetch("/api/dl?url=" + encodeURIComponent(url));
      if (!r.ok) throw new Error("download failed: " + r.status);
      const data = new Uint8Array(await r.arrayBuffer());
      const transport = new esptoolJS.Transport(port, true);
      const loader = new esptoolJS.ESPLoader({ transport, baudrate: 115200, terminal: term() });
      msg("connecting...");
      const chip = await loader.main();
      msg("connected: " + chip + " — flashing " + data.length + " bytes...");
      await loader.writeFlash({ fileArray: [{ data, address: APP_ADDR }],
        flashSize: "4MB", eraseAll: false, compress: true,
        flashMode: "dio", flashFreq: "40m",
        reportProgress: (i, w, t) => msg("flashing " + ((w / t * 100).toFixed(1)) + "%") });
      await loader.after("hard_reset");
      msg("done. Config untouched — reading new version...");
      await transport.disconnect();
      await readVersion();
    } catch (e) { msg("flash failed: " + e); }
  };
})();
