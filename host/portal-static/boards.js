/* Board catalog: static 2D art + model per supported board.
 * Images are inline SVG (stylized top view, not photos).
 */
(function () {
  "use strict";

  // Generic ESP32 devkit top view: USB-C, MCU module, pins, buttons.
  const DEVKIT_SVG =
    '<svg viewBox="0 0 200 120" xmlns="http://www.w3.org/2000/svg">' +
    '<rect x="8" y="8" width="184" height="104" rx="6" fill="#14324a" stroke="#2a4a63"/>' +
    '<rect x="80" y="2" width="40" height="14" rx="3" fill="#5a6b7a"/>' +
    '<rect x="14" y="14" width="10" height="92" fill="#c8a24a"/>' +
    '<rect x="176" y="14" width="10" height="92" fill="#c8a24a"/>' +
    '<rect x="55" y="30" width="90" height="60" rx="4" fill="#1d2126" stroke="#3a4148"/>' +
    '<rect x="65" y="40" width="70" height="24" fill="#2c333b"/>' +
    '<text x="100" y="56" fill="#8fa0b3" font-size="9" text-anchor="middle" font-family="monospace">ESP32</text>' +
    '<circle cx="40" cy="96" r="6" fill="#7a2020"/>' +
    '<circle cx="160" cy="96" r="6" fill="#20507a"/>' +
    "</svg>";

  window.QESP_BOARDS = [
    {
      id: "wroom32",
      model: "ESP32-WROOM-32D",
      desc: "QuorumESP dev board · Wi-Fi",
      svg: DEVKIT_SVG,
      filters: [{ usbVendorId: 0x10c4 }, { usbVendorId: 0x1a86 }, { usbVendorId: 0x303a }],
    },
    // Future hardware (documented, not selectable yet):
    // { id: "wt32", model: "WT32-ETH01", desc: "Ethernet (planned)", ... }
  ];
})();
