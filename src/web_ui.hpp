#pragma once
// Single-page search UI, embedded so the binary is fully self-contained.
// (Kept under MSVC's 16 KB string-literal limit.)

namespace lumen {

inline constexpr const char* kWebUi = R"LUMEN(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Lumen — private search</title>
<style>
  :root { --bg:#fbfaf7; --fg:#1d1d1b; --muted:#6b6a65; --line:#e6e3dc; --accent:#3d5afe; --mark:#fff1a8; --card:#fff; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#131313; --fg:#ecebe6; --muted:#9a998f; --line:#2a2a28; --accent:#8c9eff; --mark:#5c4d00; --card:#1b1b1a; }
  }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font:16px/1.55 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
  main { max-width:760px; margin:0 auto; padding:56px 20px 80px; }
  header { display:flex; align-items:baseline; gap:12px; margin-bottom:22px; }
  h1 { font:600 28px/1 ui-serif, Georgia, serif; letter-spacing:-.02em; margin:0; }
  h1 span { color:var(--accent); }
  .tag { color:var(--muted); font-size:14px; }
  form { display:flex; gap:8px; }
  input[type=search] { flex:1; padding:13px 16px; font-size:17px; border:1px solid var(--line); border-radius:10px; background:var(--card); color:var(--fg); outline:none; }
  input[type=search]:focus { border-color:var(--accent); box-shadow:0 0 0 3px color-mix(in srgb, var(--accent) 20%, transparent); }
  button { padding:0 18px; border:0; border-radius:10px; background:var(--accent); color:#fff; font-size:15px; font-weight:600; cursor:pointer; }
  .help { color:var(--muted); font-size:13px; margin:10px 2px 0; }
  .help code { background:var(--card); border:1px solid var(--line); border-radius:5px; padding:0 5px; font-size:12px; }
  #meta { color:var(--muted); font-size:14px; margin:26px 2px 8px; min-height:20px; }
  #meta a { color:var(--accent); cursor:pointer; }
  .hit { padding:16px 2px; border-bottom:1px solid var(--line); }
  .hit a.title { color:var(--accent); font-size:18px; font-weight:600; text-decoration:none; }
  .hit a.title:hover { text-decoration:underline; }
  .path { font:12.5px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; color:var(--muted); margin:2px 0 6px; word-break:break-all; }
  .snip { font-size:15px; }
  mark { background:var(--mark); color:inherit; border-radius:3px; padding:0 2px; }
  .score { float:right; font:12px ui-monospace, monospace; color:var(--muted); }
  #more { display:none; margin:22px auto 0; background:transparent; color:var(--accent); border:1px solid var(--line); padding:10px 18px; }
  footer { color:var(--muted); font-size:12.5px; margin-top:40px; text-align:center; }
</style>
</head>
<body>
<main>
  <header><h1>Lumen<span>.</span></h1><div class="tag">your files, searched privately</div></header>
  <form id="f" autocomplete="off">
    <input id="q" type="search" placeholder="Search your documents…  (press / to focus)" autofocus>
    <button type="submit">Search</button>
  </form>
  <div class="help">Try <code>"exact phrase"</code> <code>cache OR buffer</code> <code>-exclude</code> <code>optim*</code></div>
  <div id="meta"></div>
  <div id="results"></div>
  <button id="more">More results</button>
  <footer id="stats"></footer>
</main>
<script>
const $ = (id) => document.getElementById(id);
const PAGE = 10;
let current = "", offset = 0;

function esc(s) { return s.replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c])); }

async function run(q, append) {
  if (!q.trim()) return;
  if (!append) { offset = 0; current = q; $("results").innerHTML = ""; }
  const url = `/api/search?q=${encodeURIComponent(q)}&limit=${PAGE}&offset=${offset}`;
  const res = await fetch(url);
  const data = await res.json();
  if (data.error) { $("meta").textContent = data.error; return; }

  let meta = `${data.total.toLocaleString()} result${data.total === 1 ? "" : "s"} in ${data.tookMs.toFixed(2)} ms`;
  if (data.cached) meta += " (cached)";
  if (data.relaxed) meta += " · no document had every term, showing partial matches";
  $("meta").innerHTML = esc(meta);
  if (data.suggestion) {
    $("meta").innerHTML += ` · Did you mean <a id="sugg">${esc(data.suggestion)}</a>?`;
    $("sugg").onclick = () => { $("q").value = data.suggestion; submit(); };
  }

  for (const h of data.hits) {
    const div = document.createElement("div");
    div.className = "hit";
    div.innerHTML = `<span class="score">${h.score.toFixed(2)}</span>
      <a class="title" href="/api/doc?id=${h.id}" target="_blank" rel="noopener">${esc(h.title)}</a>
      <div class="path">${esc(h.path)}</div>
      <div class="snip">${h.snippet}</div>`;
    $("results").appendChild(div);
  }
  offset += data.hits.length;
  $("more").style.display = offset < data.total ? "block" : "none";
}

function submit() {
  const q = $("q").value;
  history.replaceState(null, "", q ? `?q=${encodeURIComponent(q)}` : "/");
  run(q, false);
}

$("f").addEventListener("submit", (e) => { e.preventDefault(); submit(); });
$("more").addEventListener("click", () => run(current, true));
document.addEventListener("keydown", (e) => {
  if (e.key === "/" && document.activeElement !== $("q")) { e.preventDefault(); $("q").focus(); $("q").select(); }
});

fetch("/api/stats").then(r => r.json()).then(s => {
  $("stats").textContent = `${s.documents.toLocaleString()} documents · ${s.terms.toLocaleString()} unique terms · ${(s.corpusBytes/1048576).toFixed(1)} MB indexed · nothing leaves this machine`;
});

const initial = new URLSearchParams(location.search).get("q");
if (initial) { $("q").value = initial; run(initial, false); }
</script>
</body>
</html>
)LUMEN";

}  // namespace lumen
