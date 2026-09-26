/* Cloudflare Pages Function: release-asset proxy (dodges browser CORS on
 * the GitHub redirect chain for the WebSerial flash section).
 * Only proxies this repo's release downloads; 4MB cap.
 */
export async function onRequest(context) {
  const url = new URL(context.request.url).searchParams.get("url") || "";
  if (!url.startsWith("https://github.com/ZkIsHere/QuorumESP/")) {
    return new Response("forbidden", { status: 403 });
  }
  const upstream = await fetch(url, {
    headers: { "User-Agent": "QuorumESP-portal-static" },
    redirect: "follow",
  });
  if (!upstream.ok) {
    return new Response("upstream " + upstream.status, { status: 502 });
  }
  const buf = await upstream.arrayBuffer();
  if (buf.byteLength > 4 * 1024 * 1024 || buf.byteLength === 0) {
    return new Response("bad size", { status: 413 });
  }
  return new Response(buf, {
    headers: {
      "Content-Type": "application/octet-stream",
      "Access-Control-Allow-Origin": "*",
      "Cache-Control": "public, max-age=3600",
    },
  });
}
