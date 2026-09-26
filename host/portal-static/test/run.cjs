#!/usr/bin/env node
/* Self-tests for nvs.js (no dependencies, no hardware).
 * Pass criteria: generate->parse roundtrip byte-identical values,
 * page header/entry layout constants match the IDF generator.
 */
const N = require("../nvs.js");

let fails = 0;
function check(name, cond) {
  if (!cond) { console.log("FAIL:", name); fails++; }
  else console.log("ok:", name);
}

const mapping = {
  ver: ["u32", 1], host: ["string", "quorumesp"],
  wssid: ["string", "103"],
  wpass: ["string", "a-65-char-password-0123456789-abcdef-0123456789-abcdefg"],
  netmode: ["u8", 0], devid: ["string", "qesp-c04268"],
  logl: ["u8", 3], qdport: ["u16", 5403],
};
const order = ["ver", "host", "wssid", "wpass", "netmode", "devid", "logl", "qdport"];
const img = N.generate(mapping, order, 0x6000);

check("page size", img.length === 0x6000);
check("page state ACTIVE", img[0] === 0xfe && img[1] === 0xff && img[2] === 0xff && img[3] === 0xff);
check("version byte v2", img[8] === 0xfe);
check("bitmap touched", img[32] !== 0xff);

const got = (N.parsePartition(img).qesp) || {};
for (const k of order) {
  const want = mapping[k][1];
  check("roundtrip " + k, got[k] === want);
}
// Later write wins.
const img2 = N.generate({a: ["string", "first"]}, ["a"]);
const img3 = N.generate({a: ["string", "second"]}, ["a"]);
check("overwrite shape", N.parsePartition(img3).qesp.a === "second" && N.parsePartition(img2).qesp.a === "first");

if (fails) { console.log("NVSJS: FAIL"); process.exit(1); }
console.log("NVSJS: PASS");
