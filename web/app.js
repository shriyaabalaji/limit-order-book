// UI for the order book playground. All matching happens in the C++ engine
// (dist/lob.wasm); this file only renders state and sends orders.
import createLob from "./dist/lob.js";

const YOU = 1;           // your owner id (self-trade prevention applies to you)
const DEPTH = 12;        // levels shown per side
const MID = 10000;       // $100.00 in cents

const $ = (id) => document.getElementById(id);
const fmt = (cents) => (cents / 100).toFixed(2);
const fmtQty = (q) => q.toLocaleString();

let Module, book;
let side = 0;                    // 0 buy, 1 sell
let myOrders = new Map();        // id -> {side, price, qty}
let botOrders = [];              // ids of simulated orders (for random cancels)
let trades = [];                 // newest first
let flashPrices = new Set();
let simTimer = null;

// ---------------------------------------------------------------- engine

function newBook() {
  if (book) book.delete();
  book = new Module.Playground();
  myOrders.clear();
  botOrders = [];
  trades = [];
  seed();
}

// Random owner per bot order so bots don't self-trade-prevent each other.
const botOwner = () => 100 + Math.floor(Math.random() * 100000);
const randInt = (a, b) => a + Math.floor(Math.random() * (b - a + 1));

function place(s, price, qty, owner) {
  const r = book.place(s, price, qty, owner);
  for (const t of r.trades) {
    trades.unshift({ ...t, at: Date.now(), you: owner === YOU || myOrders.has(t.makerId) });
    flashPrices.add(t.price);
  }
  if (trades.length > 100) trades.length = 100;
  if (r.rested) {
    if (owner === YOU) myOrders.set(r.id, { side: s, price, qty });
    else botOrders.push(r.id);
  }
  return r;
}

function seed() {
  for (let i = 1; i <= 15; i++) {
    for (let k = 0; k < randInt(1, 3); k++) {
      place(0, MID - i * 2, randInt(1, 8) * 50, botOwner());
      place(1, MID + i * 2, randInt(1, 8) * 50, botOwner());
    }
  }
}

function simStep() {
  const bid = book.bestBid() ?? MID - 1;
  const ask = book.bestAsk() ?? MID + 1;
  const mid = Math.round((bid + ask) / 2);
  const r = Math.random();
  const s = Math.random() < 0.5 ? 0 : 1;
  if (r < 0.3 && botOrders.length) {
    const i = randInt(0, botOrders.length - 1);
    book.cancel(botOrders[i]);
    botOrders.splice(i, 1);
  } else if (r < 0.85) {
    const off = 1 + Math.floor(Math.abs(gauss()) * 8);
    place(s, s === 0 ? mid - off : mid + off, randInt(1, 6) * 50, botOwner());
  } else {
    // aggressive: cross by a few ticks
    const px = s === 0 ? ask + randInt(0, 3) : bid - randInt(0, 3);
    place(s, px, randInt(1, 6) * 100, botOwner());
  }
  botOrders = botOrders.filter((id) => book.remaining(id) > 0);
  render();
}

function gauss() {
  let u = 0, v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

// ---------------------------------------------------------------- render

function render() {
  // refresh remaining qty on your orders, drop filled ones
  for (const [id, o] of myOrders) {
    const rem = book.remaining(id);
    if (rem === 0) myOrders.delete(id);
    else o.qty = rem;
  }
  const minePrices = new Set([...myOrders.values()].map((o) => `${o.side}:${o.price}`));

  const asks = book.depth(1, DEPTH);
  const bids = book.depth(0, DEPTH);
  const maxQty = Math.max(1, ...asks.map((l) => l.qty), ...bids.map((l) => l.qty));

  const rowHtml = (l, cls, s) => `
    <div class="row ${cls}${minePrices.has(`${s}:${l.price}`) ? " has-mine" : ""}${flashPrices.has(l.price) ? " flash" : ""}"
         data-price="${l.price}">
      <div class="bar" style="width:${(100 * l.qty) / maxQty}%"></div>
      <span class="price">${fmt(l.price)}</span>
      <span>${fmtQty(l.qty)}</span>
      <span>${l.count}</span>
    </div>`;

  $("asks").innerHTML = asks.length
    ? asks.slice().reverse().map((l) => rowHtml(l, "ask", 1)).join("")
    : `<div class="empty">No asks</div>`;
  $("bids").innerHTML = bids.length
    ? bids.map((l) => rowHtml(l, "bid", 0)).join("")
    : `<div class="empty">No bids</div>`;

  const bb = book.bestBid(), ba = book.bestAsk();
  $("spread").textContent =
    bb != null && ba != null
      ? `Spread $${fmt(ba - bb)}  ·  Mid $${((bb + ba) / 200).toFixed(3)}`
      : "One side of the book is empty";

  $("mine").innerHTML = myOrders.size
    ? [...myOrders.entries()].reverse().map(([id, o]) => `
        <div class="item">
          <span class="${o.side === 0 ? "b" : "s"}">${o.side === 0 ? "BUY" : "SELL"}</span>
          <span>$${fmt(o.price)}</span>
          <span>${fmtQty(o.qty)}</span>
          <button data-cancel="${id}">Cancel</button>
        </div>`).join("")
    : `<p class="muted">None yet.</p>`;

  $("tape").innerHTML = trades.length
    ? trades.map((t) => `
        <div class="item${t.you ? " you" : ""}">
          <span class="${t.takerSide === 0 ? "b" : "s"}">${t.takerSide === 0 ? "BUY" : "SELL"}</span>
          <span>$${fmt(t.price)}</span>
          <span>${fmtQty(t.qty)}</span>
          <span class="tag">${t.you ? "you" : ""}</span>
        </div>`).join("")
    : `<p class="muted">No trades yet. Cross the spread to trade.</p>`;
  $("tape").classList.add("tape");

  flashPrices.clear();
}

function showResult(r, qty) {
  const el = $("result");
  el.classList.remove("warn");
  if (!r.accepted) {
    el.textContent = "Rejected: price must be between $90.00 and $110.00 and quantity at least 1.";
    el.classList.add("warn");
    return;
  }
  const parts = [];
  if (r.filled) {
    const avg = r.trades.reduce((a, t) => a + t.price * t.qty, 0) / r.filled;
    parts.push(`Filled ${fmtQty(r.filled)} of ${fmtQty(qty)} at avg $${(avg / 100).toFixed(3)} across ${r.trades.length} trade${r.trades.length > 1 ? "s" : ""}.`);
  }
  if (r.rested) parts.push(`${fmtQty(qty - r.filled - r.cancelled)} resting in the book.`);
  if (r.cancelled) {
    parts.push(r.reason === "self-trade"
      ? `${fmtQty(r.cancelled)} cancelled: it would have traded with your own order (self-trade prevention).`
      : `${fmtQty(r.cancelled)} cancelled: the book is full.`);
    el.classList.add("warn");
  }
  if (!r.filled && !r.cancelled) parts.unshift("No match.");
  el.textContent = parts.join(" ");
}

// ---------------------------------------------------------------- events

function setSide(s) {
  side = s;
  for (const b of document.querySelectorAll(".side")) {
    const on = Number(b.dataset.side) === s;
    b.classList.toggle("active", on);
    b.setAttribute("aria-pressed", String(on));
  }
  const p = $("place");
  p.textContent = s === 0 ? "Place buy" : "Place sell";
  p.className = `primary ${s === 0 ? "buy" : "sell"}`;
}

function submit(s, priceCents) {
  const qty = Math.floor(Number($("qty").value));
  if (!Number.isFinite(priceCents) || !(qty > 0)) {
    showResult({ accepted: false }, qty);
    return;
  }
  const r = place(s, priceCents, qty, YOU);
  showResult(r, qty);
  render();
}

function wire() {
  for (const b of document.querySelectorAll(".side")) {
    b.addEventListener("click", () => setSide(Number(b.dataset.side)));
  }
  $("place").addEventListener("click", () => submit(side, Math.round(Number($("price").value) * 100)));
  $("hitAsk").addEventListener("click", () => { const a = book.bestAsk(); if (a != null) submit(0, a); });
  $("hitBid").addEventListener("click", () => { const b = book.bestBid(); if (b != null) submit(1, b); });

  $("book").addEventListener("click", (e) => {
    const row = e.target.closest(".row");
    if (row) $("price").value = fmt(Number(row.dataset.price));
  });
  $("mine").addEventListener("click", (e) => {
    const id = e.target.dataset.cancel;
    if (!id) return;
    book.cancel(Number(id));
    myOrders.delete(Number(id));
    $("result").classList.remove("warn");
    $("result").textContent = "Order cancelled.";
    render();
  });
  $("sim").addEventListener("change", (e) => {
    clearInterval(simTimer);
    simTimer = e.target.checked ? setInterval(simStep, 350) : null;
  });
  $("reset").addEventListener("click", () => {
    newBook();
    $("result").textContent = "";
    render();
  });
}

// ---------------------------------------------------------------- boot

(async () => {
  try {
    Module = await createLob();
    newBook();
    wire();
    render();
    $("loading").remove();
  } catch (err) {
    $("loading").textContent = "Couldn't load the engine: " + err;
  }
})();
