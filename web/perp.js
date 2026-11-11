// UI for the perp playground. Positions, margin, liquidation and funding all
// happen in the C++ engine (PerpExchange in dist/lob.wasm). This file only
// runs the bots, renders state and sends orders.
import createLob from "./dist/lob.js";

const YOU = 1;
const MAKERS = range(2, 21);   // market makers: deep pockets, quote around spot
const DEGENS = range(22, 41);  // small accounts that go ~9.5x in the trend's direction
const DEPTH = 12;
const START = 10000;           // $100.00 spot
const YOUR_DEPOSIT = 1_000_000;        // $10,000
const MAKER_DEPOSIT = 100_000_000;     // $1,000,000
const DEGEN_DEPOSIT = 150_000;         // $1,500
const FUNDING_EVERY = 40;              // sim steps per funding interval (~14s = "8 hours")
const STEP_MS = 350;

const $ = (id) => document.getElementById(id);
const fmt = (c) => (c / 100).toFixed(2);
const money = (c) => (c < 0 ? "−$" : "$") + Math.abs(c / 100).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const signed = (c) => (c > 0 ? "+" : "") + money(c);
const who = (o) => (o === YOU ? "You" : DEGENS.includes(o) ? `Degen #${o - 21}` : `Maker #${o - 1}`);
function range(a, b) { return Array.from({ length: b - a + 1 }, (_, i) => a + i); }
const randInt = (a, b) => a + Math.floor(Math.random() * (b - a + 1));
const pick = (arr) => arr[Math.floor(Math.random() * arr.length)];

let Module, ex;
let side = 0;
let index = START, indexHistory = [];
let myOrders = new Map();   // id -> {side, price}
let botOrders = [];         // {id, owner}
let trades = [], liqs = [];
let flashPrices = new Set();
let simTimer = null, step = 0, lastFunding = null;

// ---------------------------------------------------------------- engine

function newExchange() {
  if (ex) ex.delete();
  ex = new Module.PerpPlayground();
  index = START; indexHistory = []; step = 0; lastFunding = null;
  myOrders.clear(); botOrders = []; trades = []; liqs = [];
  ex.deposit(YOU, YOUR_DEPOSIT);
  for (const m of MAKERS) ex.deposit(m, MAKER_DEPOSIT);
  for (const d of DEGENS) ex.deposit(d, DEGEN_DEPOSIT);
  seed();
}

// Every engine call returns the trades + liquidations it caused.
function absorb(r) {
  for (const t of r.trades) {
    trades.unshift({ ...t, you: t.takerOwner === YOU || t.makerOwner === YOU });
    flashPrices.add(t.price);
  }
  if (trades.length > 120) trades.length = 120;
  for (const l of r.liquidations) {
    liqs.unshift({ ...l, at: new Date() });
    if (l.who === YOU) {
      const all = l.closed === l.size;
      setResult(`You were liquidated: ${Math.abs(l.closed)} of your ${Math.abs(l.size)} contracts market-${l.size > 0 ? "sold" : "bought"} into the book${all ? "" : " (the book was too thin for the rest; it's retried as the price moves)"}.`, true);
    }
  }
  if (liqs.length > 60) liqs.length = 60;
  return r;
}

function place(owner, s, price, qty) {
  price = Math.max(9000, Math.min(11000, Math.round(price)));
  const r = absorb(ex.place(owner, s, price, qty));
  if (r.rested) {
    if (owner === YOU) myOrders.set(r.id, { side: s, price });
    else botOrders.push({ id: r.id, owner, price, side: s });
  }
  return r;
}

function seed() {
  for (let i = 1; i <= 15; i++) {
    for (let k = 0; k < randInt(1, 2); k++) {
      place(pick(MAKERS), 0, START - i * 5, randInt(2, 8) * 25);
      place(pick(MAKERS), 1, START + i * 5, randInt(2, 8) * 25);
    }
  }
  // A few degens already in leveraged positions.
  DEGENS.slice(0, 8).forEach((d, i) => degenEnter(d, i % 2 === 0 ? 0 : 1));
}

// Open ~8x in direction s at market.
function degenEnter(d, s) {
  const acct = ex.account(d);
  const px = s === 0 ? ex.bestAsk() : ex.bestBid();
  if (px == null || acct.equity <= 0) return;
  const qty = Math.floor((acct.equity * 9.5) / px);
  if (qty > 0) place(d, s, s === 0 ? px + 30 : px - 30, qty);
  // anything that rested is pulled: degens only take
  botOrders = botOrders.filter((o) => (o.owner === d ? (absorb(ex.cancel(d, o.id)), false) : true));
}

function gauss() {
  let u = 0, v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

function simStep() {
  step++;
  index = Math.max(9100, Math.min(10900, index + Math.round(gauss() * 3)));
  indexHistory.push(index);
  if (indexHistory.length > 30) indexHistory.shift();

  // Makers: instantly pull quotes on the wrong side of spot (a bid above
  // spot is free money for anyone who can sell spot), then a couple of stale ones.
  botOrders = botOrders.filter((o) => {
    const wrong = o.side === 0 ? o.price > index + 2 : o.price < index - 2;
    if (wrong) absorb(ex.cancel(o.owner, o.id));
    return !wrong;
  });
  for (let k = 0; k < 2 && botOrders.length; k++) {
    const i = Math.random() < 0.6 ? farthestQuote() : randInt(0, botOrders.length - 1);
    const o = botOrders[i];
    absorb(ex.cancel(o.owner, o.id));
    botOrders.splice(i, 1);
  }
  const quote = (s) => {
    const off = 2 + Math.floor(Math.abs(gauss()) * 10);
    place(pick(MAKERS), s, s === 0 ? index - off : index + off, randInt(2, 8) * 25);
  };
  for (let k = 0; k < 2; k++) quote(Math.random() < 0.5 ? 0 : 1);
  // Keep both sides stocked: makers refill a thin side (up to 4 quotes a step).
  for (const s of [0, 1]) {
    const have = botOrders.filter((o) => o.side === s && MAKERS.includes(o.owner)).length;
    for (let k = have; k < 12 && k < have + 4; k++) quote(s);
  }
  // Occasional aggressive flow.
  if (Math.random() < 0.15) {
    const s = Math.random() < 0.5 ? 0 : 1;
    const px = s === 0 ? ex.bestAsk() : ex.bestBid();
    if (px != null) place(pick(MAKERS), s, s === 0 ? px + 3 : px - 3, randInt(1, 4) * 25);
  }
  // Degens chase the trend; broke ones get replaced by a fresh deposit.
  if (Math.random() < 0.06) {
    const d = pick(DEGENS);
    const a = ex.account(d);
    if (a.size === 0) {
      if (a.equity < 30000) ex.deposit(d, DEGEN_DEPOSIT);
      const trend = index - indexHistory[0];
      degenEnter(d, trend >= 0 ? 0 : 1);
    }
  }
  if (step % FUNDING_EVERY === 0) {
    const r = absorb(ex.funding(index));
    lastFunding = r.perContract;
  }
  botOrders = botOrders.filter((o) => ex.remaining(o.id) > 0);
  render();
}

// Index of the bot quote farthest from spot (the stalest one).
function farthestQuote() {
  let best = 0, dist = -1;
  botOrders.forEach((o, i) => {
    const d = Math.abs(o.price - index);
    if (d > dist) { dist = d; best = i; }
  });
  return best;
}

// ---------------------------------------------------------------- render

function render() {
  for (const [id] of myOrders) if (ex.remaining(id) === 0) myOrders.delete(id);
  const minePrices = new Set([...myOrders.values()].map((o) => `${o.side}:${o.price}`));

  const asks = ex.depth(1, DEPTH), bids = ex.depth(0, DEPTH);
  const maxQty = Math.max(1, ...asks.map((l) => l.qty), ...bids.map((l) => l.qty));
  const a = ex.account(YOU);
  const liqPx = a.size !== 0 ? Math.round(a.liqPrice) : null;

  const rowHtml = (l, cls, s) => `
    <div class="row ${cls}${minePrices.has(`${s}:${l.price}`) ? " has-mine" : ""}${flashPrices.has(l.price) ? " flash" : ""}"
         data-price="${l.price}">
      <div class="bar" style="width:${(100 * l.qty) / maxQty}%"></div>
      <span class="price">${fmt(l.price)}</span>
      <span>${l.qty.toLocaleString()}</span>
      <span>${l.count}</span>
    </div>`;
  $("asks").innerHTML = asks.length ? asks.slice().reverse().map((l) => rowHtml(l, "ask", 1)).join("") : `<div class="empty">No asks</div>`;
  $("bids").innerHTML = bids.length ? bids.map((l) => rowHtml(l, "bid", 0)).join("") : `<div class="empty">No bids</div>`;

  const bb = ex.bestBid(), ba = ex.bestAsk(), mark = ex.mark();
  $("spread").textContent = bb != null && ba != null ? `Spread $${fmt(ba - bb)}` : "One side of the book is empty";

  const prem = mark != null ? mark - index : null;
  const fundIn = simTimer ? Math.ceil(((FUNDING_EVERY - (step % FUNDING_EVERY)) * STEP_MS) / 1000) + "s" : "paused";
  $("marketStats").innerHTML = `
    <div><span>Mark</span><b>${mark != null ? "$" + fmt(mark) : "—"}</b></div>
    <div><span>Spot index</span><b>$${fmt(index)}</b></div>
    <div><span>Premium</span><b class="${prem > 0 ? "up" : prem < 0 ? "down" : ""}">${prem != null ? (prem >= 0 ? "+" : "−") + "$" + fmt(Math.abs(prem)) : "—"}</b></div>
    <div><span>Next funding</span><b>${fundIn}</b></div>
    <div><span>Last funding</span><b>${lastFunding == null ? "—" : lastFunding === 0 ? "none" : (lastFunding > 0 ? "longs paid " : "shorts paid ") + "$" + fmt(Math.abs(lastFunding)) + "/contract"}</b></div>`;

  // Account
  const pos = a.size === 0 ? "Flat"
    : `<span class="${a.size > 0 ? "b" : "s"}">${a.size > 0 ? "LONG" : "SHORT"} ${Math.abs(a.size)}</span> @ $${(a.entry / 100).toFixed(3)}`;
  const lev = a.size !== 0 && mark != null && a.equity > 0 ? ((Math.abs(a.size) * mark) / a.equity).toFixed(1) + "x" : "—";
  $("acct").innerHTML = `
    <div><span>Equity</span><b>${money(a.equity)}</b></div>
    <div><span>Position</span><b>${pos}</b></div>
    <div><span>Leverage</span><b>${lev}</b></div>
    <div><span>Unrealized</span><b class="${a.unrealized > 0 ? "up" : a.unrealized < 0 ? "down" : ""}">${signed(a.unrealized)}</b></div>
    <div><span>Realized</span><b>${signed(a.realized)}</b></div>
    <div><span>Funding</span><b>${signed(a.funding)}</b></div>
    <div><span>Liq. price</span><b class="${liqPx ? "warn-text" : ""}">${liqPx && liqPx > 0 ? "$" + fmt(liqPx) : "—"}</b></div>
    <div><span>Margin in use</span><b>${money(a.initialUsed)}</b></div>`;

  // Health: how far equity is above maintenance (0% = liquidation).
  let health = 100, text = "No position: nothing can be liquidated.";
  if (a.size !== 0 && a.maintenance > 0) {
    health = Math.max(0, Math.min(100, ((a.equity - a.maintenance) / (a.equity || 1)) * 100));
    text = `Liquidated if equity < ${money(a.maintenance)} (5% of position).`;
  }
  $("healthBar").style.width = health + "%";
  $("healthBar").className = "health-bar" + (health < 25 ? " danger" : health < 50 ? " caution" : "");
  $("healthText").textContent = text;

  // Max size hint
  const px = Math.round(Number($("price").value) * 100);
  const free = Math.max(0, a.equity - a.initialUsed);
  if (px > 0) $("maxHint").textContent = `Free margin ${money(free)}: about ${Math.floor(free / (px * 0.1)).toLocaleString()} more contracts at 10x.`;

  $("mine").innerHTML = myOrders.size
    ? [...myOrders.entries()].reverse().map(([id, o]) => `
        <div class="item">
          <span class="${o.side === 0 ? "b" : "s"}">${o.side === 0 ? "LONG" : "SHORT"}</span>
          <span>$${fmt(o.price)}</span>
          <span>${ex.remaining(id).toLocaleString()}</span>
          <button data-cancel="${id}">Cancel</button>
        </div>`).join("")
    : `<p class="muted">None yet.</p>`;

  const bad = ex.badDebt();
  $("badDebt").textContent = bad > 0 ? `· bad debt ${money(bad)}` : "";
  $("liqs").innerHTML = liqs.length
    ? liqs.map((l) => `
        <div class="liq${l.who === YOU ? " you" : ""}">
          <b>${who(l.who)}</b> ${l.size > 0 ? "long" : "short"} ${Math.abs(l.size)}
          ${l.closed !== l.size ? `<span class="muted">(closed ${Math.abs(l.closed)})</span>` : ""}
          ${l.badDebt > 0 ? `<span class="warn-text">bad debt ${money(l.badDebt)}</span>` : ""}
        </div>`).join("")
    : `<p class="muted">None yet. Try Spot −6%.</p>`;

  $("tape").innerHTML = trades.length
    ? trades.map((t) => `
        <div class="item${t.you ? " you" : ""}">
          <span class="${t.takerSide === 0 ? "b" : "s"}">${t.takerSide === 0 ? "BUY" : "SELL"}</span>
          <span>$${fmt(t.price)}</span>
          <span>${t.qty.toLocaleString()}</span>
          <span class="tag">${t.liquidation ? '<span class="liq-tag">LIQ</span>' : t.you ? "you" : ""}</span>
        </div>`).join("")
    : `<p class="muted">No trades yet.</p>`;

  flashPrices.clear();
}

function setResult(msg, warn = false) {
  const el = $("result");
  el.textContent = msg;
  el.classList.toggle("warn", warn);
}

function showResult(r, qty) {
  if (r.marginRejected) return setResult("Rejected: not enough margin. Equity must cover 10% of your worst-case position (open orders count).", true);
  if (!r.accepted) return setResult("Rejected: price must be between $90.00 and $110.00 and size at least 1.", true);
  const parts = [];
  if (r.filled) {
    const mine = r.trades.filter((t) => t.takerOwner === YOU && !t.liquidation);
    const avg = mine.reduce((s, t) => s + t.price * t.qty, 0) / (r.filled || 1);
    parts.push(`Filled ${r.filled} of ${qty} at avg $${(avg / 100).toFixed(3)}.`);
  }
  if (r.rested) parts.push(`${qty - r.filled - r.cancelled} resting in the book.`);
  if (r.cancelled) parts.push(`${r.cancelled} cancelled (${r.reason === "self-trade" ? "would have traded with your own order" : "book full"}).`);
  if (!r.filled && !r.cancelled && !r.rested) parts.push("No match.");
  const others = r.liquidations.filter((l) => l.who !== YOU).length;
  if (others) parts.push(`Your order set off ${others} liquidation${others > 1 ? "s" : ""}.`);
  setResult(parts.join(" "), r.cancelled > 0);
}

// ---------------------------------------------------------------- events

function setSide(s) {
  side = s;
  for (const b of document.querySelectorAll(".side")) {
    const on = Number(b.dataset.side) === s;
    b.classList.toggle("active", on);
    b.setAttribute("aria-pressed", String(on));
  }
  $("place").textContent = s === 0 ? "Place long" : "Place short";
  $("place").className = `primary ${s === 0 ? "buy" : "sell"}`;
}

function submit(s, priceCents, qtyOverride) {
  const qty = qtyOverride ?? Math.floor(Number($("qty").value));
  if (!Number.isFinite(priceCents) || !(qty > 0)) return showResult({ accepted: false }, qty);
  const r = place(YOU, s, priceCents, qty);
  showResult(r, qty);
  render();
}

function shock(pct) {
  index = Math.max(9100, Math.min(10900, Math.round(index * (1 + pct))));
  indexHistory = [index - Math.sign(pct) * 50]; // makes degens read it as a trend
  if (!simTimer) { $("sim").checked = true; startSim(); }
  render();
}

function startSim() { clearInterval(simTimer); simTimer = setInterval(simStep, STEP_MS); }

function wire() {
  for (const b of document.querySelectorAll(".side")) b.addEventListener("click", () => setSide(Number(b.dataset.side)));
  $("place").addEventListener("click", () => submit(side, Math.round(Number($("price").value) * 100)));
  $("hitAsk").addEventListener("click", () => { const a = ex.bestAsk(); if (a != null) submit(0, a); });
  $("hitBid").addEventListener("click", () => { const b = ex.bestBid(); if (b != null) submit(1, b); });
  $("close").addEventListener("click", () => {
    const a = ex.account(YOU);
    if (a.size === 0) return setResult("No position to close.");
    const s = a.size > 0 ? 1 : 0;
    const px = s === 1 ? ex.bestBid() : ex.bestAsk();
    if (px == null) return setResult("Nobody on the other side of the book.", true);
    const r = place(YOU, s, s === 1 ? 9000 : 11000, Math.abs(a.size)); // market: far limit, then pull the rest
    if (r.rested) { absorb(ex.cancel(YOU, r.id)); myOrders.delete(r.id); }
    showResult(r, Math.abs(a.size));
    render();
  });
  $("price").addEventListener("input", render);
  $("book").addEventListener("click", (e) => {
    const row = e.target.closest(".row");
    if (row) { $("price").value = fmt(Number(row.dataset.price)); render(); }
  });
  $("mine").addEventListener("click", (e) => {
    const id = e.target.dataset.cancel;
    if (!id) return;
    absorb(ex.cancel(YOU, Number(id)));
    myOrders.delete(Number(id));
    setResult("Order cancelled.");
    render();
  });
  $("sim").addEventListener("change", (e) => {
    if (e.target.checked) startSim();
    else { clearInterval(simTimer); simTimer = null; render(); }
  });
  $("shockDown").addEventListener("click", () => shock(-0.06));
  $("shockUp").addEventListener("click", () => shock(0.06));
  $("reset").addEventListener("click", () => { newExchange(); setResult(""); render(); });
}

(async () => {
  try {
    Module = await createLob();
    newExchange();
    wire();
    render();
    $("loading").remove();
  } catch (err) {
    $("loading").textContent = "Couldn't load the engine: " + err;
  }
})();
