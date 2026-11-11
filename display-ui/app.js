/* ============================================================
   Customer Display — логика
   Разделение: state-машина · POS-шина · рекламный движок · рендер
   ============================================================ */

'use strict'

/* ============================ УТИЛИТЫ ============================ */

const $ = (id) => document.getElementById(id)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const fmtMoney = (v) =>
  new Intl.NumberFormat('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(v) + ' ₽'

/* ============================ СОСТОЯНИЕ ============================ */

const state = {
  screen: 'WAITING',   // WAITING | SHOPPING | PAYMENT | SUCCESS | ERROR
  lines: [],           // { id, name, qty, unitPrice, discount, lineTotal }
  total: 0,
  method: null,        // 'card' | 'cash'
  success: null,       // { total, positions }
  error: null,
}

const recalcTotal = () => (state.total = state.lines.reduce((s, l) => s + l.lineTotal, 0))
const positionsCount = () => state.lines.reduce((s, l) => s + l.qty, 0)

/* ============================ POS-ШИНА ============================ */

const busListeners = new Set()
const eventLog = []

function dispatch(event) {
  applyEvent(event)
  pushLog('▶ ' + describeEvent(event))
  render()
}

function describeEvent(e) {
  if (e.type === 'line/upsert') return `line/upsert  ${e.line.name}`
  if (e.type === 'line/remove') return `line/remove  ${e.id}`
  return e.type
}

function applyEvent(event) {
  switch (event.type) {
    case 'line/upsert': {
      if (state.screen === 'PAYMENT' || state.screen === 'SUCCESS' || state.screen === 'ERROR') return
      const i = state.lines.findIndex((l) => l.id === event.line.id)
      if (i >= 0) state.lines[i] = event.line
      else state.lines.push(event.line)
      recalcTotal()
      state.screen = 'SHOPPING'
      break
    }
    case 'line/remove': {
      if (state.screen !== 'SHOPPING') return
      state.lines = state.lines.filter((l) => l.id !== event.id)
      recalcTotal()
      if (state.lines.length === 0) state.screen = 'WAITING'
      break
    }
    case 'check/clear': {
      if (state.screen === 'PAYMENT' || state.screen === 'SUCCESS') return
      state.lines = []
      state.total = 0
      state.screen = 'WAITING'
      break
    }
    case 'payment/start': {
      if (state.lines.length === 0) return
      state.method = event.method || 'card'
      state.screen = 'PAYMENT'
      break
    }
    case 'payment/success': {
      if (state.screen !== 'PAYMENT') return
      state.success = { total: state.total, positions: positionsCount() }
      state.screen = 'SUCCESS'
      // РЕЖИМ 3: 4.5 с показа, затем возврат к полноэкранной рекламе
      setTimeout(() => {
        if (state.screen === 'SUCCESS') {
          state.lines = []
          state.total = 0
          state.success = null
          state.screen = 'WAITING'
          pushLog('◉ success → WAITING')
          render()
        }
      }, 4500)
      break
    }
    case 'payment/failed': {
      if (state.screen !== 'PAYMENT') return
      state.error = event.reason || 'Не удалось выполнить оплату. Повторите попытку.'
      state.screen = 'ERROR'
      setTimeout(() => {
        if (state.screen === 'ERROR') {
          state.error = null
          state.screen = state.lines.length > 0 ? 'SHOPPING' : 'WAITING'
          pushLog('◉ error → ' + state.screen)
          render()
        }
      }, 4000)
      break
    }
  }
}

function pushLog(entry) {
  const time = new Date().toLocaleTimeString('ru-RU', { hour12: false })
  eventLog.unshift(`${time}  ${entry}`)
  if (eventLog.length > 8) eventLog.pop()
  renderLog()
}

/* ============================ ФОРМАТИРОВАНИЕ ============================ */

const numAnims = new Map()

/** Плавная анимация числа (аналог AnimatedNumber) */
function animateNumber(el, to, fmt = fmtMoney) {
  const prev = numAnims.get(el)
  const from = prev ? prev.value : 0
  if (prev) cancelAnimationFrame(prev.raf)
  const start = performance.now()
  const dur = 550
  const rec = { value: to, raf: 0 }
  numAnims.set(el, rec)
  const step = (t) => {
    const p = Math.min(1, (t - start) / dur)
    const eased = 1 - Math.pow(1 - p, 3)
    rec.value = from + (to - from) * eased
    el.textContent = fmt(rec.value)
    if (p < 1) rec.raf = requestAnimationFrame(step)
  }
  rec.raf = requestAnimationFrame(step)
}

/* ============================ РЕНДЕР ЭКРАНОВ ============================ */

let currentScreen = ''

function setScreen(name) {
  if (currentScreen === name) return
  currentScreen = name
  for (const id of ['screenWaiting', 'screenShopping', 'screenPayment', 'screenSuccess', 'screenError']) {
    $(id).classList.toggle('active', id === 'screen' + name.charAt(0) + name.slice(1).toLowerCase())
  }
  $('devScreen').textContent = name
}

function render() {
  setScreen(state.screen)
  renderReceipt()
  renderTotals()
  renderPayment()
  renderSuccess()
  renderError()
  syncAds()
}

/* ---------------------------- чек ---------------------------- */

const lineNodes = new Map()

function renderReceipt() {
  const list = $('rcList')
  const ids = new Set(state.lines.map((l) => l.id))

  // удалить исчезнувшие
  for (const [id, node] of lineNodes) {
    if (!ids.has(id)) {
      node.classList.add('out')
      setTimeout(() => node.remove(), 240)
      lineNodes.delete(id)
    }
  }

  // добавить/обновить
  let masked = false
  state.lines.forEach((line) => {
    let node = lineNodes.get(line.id)
    if (!node) {
      node = document.createElement('div')
      node.className = 'rcLine'
      node.innerHTML =
        '<div class="rcLineTop"><span class="rcName"></span><span class="rcSum"></span></div>' +
        '<div class="rcLineSub"><span class="rcQtyPrice"></span><span class="rcDisc"></span></div>'
      lineNodes.set(line.id, node)
    }
    const name = node.querySelector('.rcName')
    const sum = node.querySelector('.rcSum')
    const qp = node.querySelector('.rcQtyPrice')
    const disc = node.querySelector('.rcDisc')

    if (name.textContent !== line.name) name.textContent = line.name
    const sumText = fmtMoney(line.lineTotal)
    if (sum.textContent !== sumText) {
      sum.textContent = sumText
      node.classList.remove('flash')
      void node.offsetWidth // reflow для перезапуска анимации
      node.classList.add('flash')
    }
    qp.textContent = `${line.qty} шт × ${fmtMoney(line.unitPrice)}`
    if (line.discount > 0) {
      disc.textContent = '−' + fmtMoney(line.discount)
      disc.classList.remove('hidden')
    } else {
      disc.classList.add('hidden')
    }
    list.appendChild(node) // порядок
  })

  // маска переполнения
  const totalH = state.lines.length * 108
  const maxH = list.clientHeight
  masked = totalH > maxH && maxH > 0
  $('rcListMask').classList.toggle('on', masked)
}

function renderTotals() {
  animateNumber($('rcTotal'), state.total)
  $('rcPositions').textContent = 'Позиций: ' + positionsCount()
  const discount = state.lines.reduce((s, l) => s + l.discount, 0)
  const d = $('rcDiscount')
  if (discount > 0) {
    d.textContent = 'Скидка: ' + fmtMoney(discount)
    d.classList.remove('hidden')
  } else {
    d.classList.add('hidden')
  }
}

/* ---------------------------- оплата / успех / ошибка ---------------------------- */

let payTotalShown = 0

function renderPayment() {
  if (state.screen !== 'PAYMENT') return
  animateNumber($('payAmount'), state.total)
  $('payMethod').textContent = state.method === 'cash' ? 'Оплата наличными' : 'Оплата картой'
}

function renderSuccess() {
  if (state.screen !== 'SUCCESS' || !state.success) return
  // перезапуск анимации галочки при каждом входе
  const svg = document.querySelector('.okCircle')
  const clone = svg.cloneNode(true)
  svg.replaceWith(clone)
  $('okTotal').textContent = fmtMoney(state.success.total)
  $('okPositions').textContent = 'Позиций: ' + state.success.positions
}

function renderError() {
  if (state.screen !== 'ERROR') return
  $('errMsg').textContent = state.error || ''
}

/* ============================ РЕКЛАМНЫЙ ДВИЖОК ============================ */

const ADS = [
  {
    template: 'productHero',
    eyebrow: 'НОВАЯ ПОДАЧА',
    title: 'КОФЕ ИЗ СВЕЖЕЙ ОБЖАРКИ',
    desc: 'Зерновой купаж этой недели — сварим за 90 секунд.',
    price: '390 ₽',
    oldPrice: '450 ₽',
  },
  {
    template: 'promotion',
    percent: '−25%',
    title: 'НА ВСЁ МОРОЖЕНОЕ',
    sub: 'Летняя коллекция вкусов уже в зале',
    period: 'АКЦИЯ ДО 30 ИЮНЯ',
  },
  {
    template: 'brandCampaign',
    brand: 'AURORA',
    tag: 'ВКУС СВОБОДЫ · НОВАЯ КОЛЛЕКЦИЯ',
  },
  {
    template: 'video',
    title: 'СЕЗОН ОТКРЫТ',
    sub: 'Готовим на живом огне — каждую неделю новое меню',
  },
  {
    template: 'qrPromotion',
    title: 'СКИДКА 15% В ПРИЛОЖЕНИИ',
    sub: 'Установите приложение и получите скидку на первый заказ',
    qrSeed: 'customer-display-15',
  },
]

let waitingIndex = 0
let shoppingIndex = 0
let adsPaused = false
let adTimerTick = 0
const WAITING_AD_SEC = 8
const SHOPPING_AD_SEC = 10

function el(tag, cls, html) {
  const n = document.createElement(tag)
  if (cls) n.className = cls
  if (html !== undefined) n.innerHTML = html
  return n
}

function phArt(tagText, withCore = true) {
  const art = el('div', 'phArt')
  if (withCore) art.appendChild(el('div', 'phCore', '<div class="phGlyph"></div>'))
  art.appendChild(el('div', 'phTag', tagText))
  return art
}

/** Плейсхолдер псевдо-QR (детерминированный, не сканируется) */
function qrSvg(seed) {
  const size = 25
  let s = 7
  for (const ch of seed) s = (s * 31 + ch.charCodeAt(0)) & 0x7fffffff
  const rnd = () => ((s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff)
  const m = Array.from({ length: size }, () => Array(size).fill(0))
  for (let r = 0; r < size; r++) for (let c = 0; c < size; c++) m[r][c] = rnd() > 0.52 ? 1 : 0
  const finder = (r0, c0) => {
    for (let r = -1; r < 8; r++)
      for (let c = -1; c < 8; c++) {
        const rr = r0 + r, cc = c0 + c
        if (rr < 0 || cc < 0 || rr >= size || cc >= size) continue
        const inRing = r >= 0 && r < 7 && c >= 0 && c < 7 && (r === 0 || r === 6 || c === 0 || c === 6)
        const inCore = r >= 2 && r <= 4 && c >= 2 && c <= 4
        m[rr][cc] = inRing || inCore ? 1 : 0
      }
  }
  finder(0, 0)
  finder(0, size - 7)
  finder(size - 7, 0)
  for (let i = 0; i < size; i++) { m[6][i] = i % 2 === 0 ? 1 : 0; m[i][6] = i % 2 === 0 ? 1 : 0 }
  let rects = ''
  for (let r = 0; r < size; r++)
    for (let c = 0; c < size; c++)
      if (m[r][c]) rects += `<rect x="${c}" y="${r}" width="1" height="1"/>`
  return `<svg viewBox="0 0 ${size} ${size}" fill="#111214" shape-rendering="crispEdges">${rects}</svg>`
}

function buildAd(ad) {
  const cls = {
    productHero: 'adProductHero',
    promotion: 'adPromotion',
    brandCampaign: 'adBrand',
    video: 'adVideo',
    qrPromotion: 'adQr',
  }[ad.template]
  const root = el('div', 'ad ' + cls)
  switch (ad.template) {
    case 'productHero':
      root.appendChild(phArt('ФОТО ТОВАРА'))
      root.appendChild(
        el('div', 'phText',
          `<div class="adEyebrow">${ad.eyebrow}</div>` +
          `<div class="adTitle">${ad.title}</div>` +
          `<div class="adDesc">${ad.desc}</div>` +
          `<div class="adPriceRow"><span class="adPrice">${ad.price}</span>` +
          `<span class="adOldPrice">${ad.oldPrice}</span></div>`),
      )
      break
    case 'promotion':
      root.appendChild(el('div', 'adPercent', ad.percent))
      root.appendChild(el('div', 'adPromoTitle', ad.title))
      root.appendChild(el('div', 'adPromoSub', ad.sub))
      root.appendChild(el('div', 'adPeriod', ad.period))
      break
    case 'brandCampaign':
      root.appendChild(el('div', 'adBrandArt'))
      root.appendChild(el('div', 'adBrandLogo', '<div class="logo"><span class="logoMark"></span><span class="logoText">МАГАЗИН</span></div>'))
      root.appendChild(el('div', 'adBrandMark', ad.brand))
      root.appendChild(el('div', 'adBrandTag', ad.tag))
      break
    case 'video':
      root.appendChild(el('div', 'adVideoArt', '<div class="blob b1"></div><div class="blob b2"></div>'))
      root.appendChild(
        el('div', 'adVideoOverlay',
          `<div class="adVideoTitle">${ad.title}</div><div class="adVideoSub">${ad.sub}</div>`),
      )
      root.appendChild(el('div', 'adVideoBadge', '<span class="dot"></span>ВИДЕО · БЕЗ ЗВУКА'))
      break
    case 'qrPromotion':
      root.appendChild(
        el('div', 'adQrText',
          `<div class="adEyebrow">ПРОМО</div>` +
          `<div class="adTitle">${ad.title}</div>` +
          `<div class="adDesc">${ad.sub}</div>` +
          `<div class="adQrHint"><span class="arr">→</span> Наведите камеру телефона</div>`),
      )
      root.appendChild(el('div', 'adQrCard', qrSvg(ad.qrSeed)))
      break
  }
  return root
}

function setAd(hostId, index) {
  const host = $(hostId)
  const ad = buildAd(ADS[index % ADS.length])
  ad.classList.add('adLayer')
  host.appendChild(ad)
  requestAnimationFrame(() => requestAnimationFrame(() => ad.classList.add('show')))
  // убрать предыдущие слои (кроме текущего)
  while (host.children.length > 1) {
    const old = host.children[0]
    old.classList.remove('show')
    setTimeout(() => old.remove(), 650)
    if (host.children[0] === old && host.children.length > 1) host.removeChild(old)
  }
  renderDots()
}

function renderDots() {
  const dots = $('waDots')
  dots.innerHTML = ''
  ADS.forEach((_, i) => {
    const d = el('span', 'waDot' + (i === waitingIndex ? ' active' : ''))
    dots.appendChild(d)
  })
}

function nextAd(hostId, getCurrent, setCurrent) {
  setCurrent((getCurrent() + 1) % ADS.length)
  setAd(hostId, getCurrent())
}

let lastScreenForAds = ''

function syncAds() {
  // первичная инициализация слоёв
  if ($('waitingAdLayers').children.length === 0) setAd('waitingAdLayers', waitingIndex)
  if ($('shoppingAdLayers').children.length === 0) setAd('shoppingAdLayers', shoppingIndex)

  if (lastScreenForAds !== state.screen) {
    lastScreenForAds = state.screen
    adTimerTick = 0 // сброс накопителя при смене экрана
  }

  if (!adsPaused) {
    if (state.screen === 'WAITING') {
      if (adTimerTick >= WAITING_AD_SEC) {
        adTimerTick = 0
        nextAd('waitingAdLayers', () => waitingIndex, (v) => (waitingIndex = v))
      }
    } else if (state.screen === 'SHOPPING') {
      if (adTimerTick >= SHOPPING_AD_SEC) {
        adTimerTick = 0
        nextAd('shoppingAdLayers', () => shoppingIndex, (v) => (shoppingIndex = v))
      }
    }
  }
}

/* ============================ ЧАСЫ (WAITING) ============================ */

function tickClock() {
  const n = new Date()
  $('waClock').textContent =
    String(n.getHours()).padStart(2, '0') + ':' + String(n.getMinutes()).padStart(2, '0')
}
setInterval(tickClock, 1000)
tickClock()

/* ============================ ДЕМО-ДАННЫЕ ============================ */

const DEMO_ITEMS = [
  { id: 'sku-101', name: 'Энергетический напиток Monster 0.5 жб', qty: 1, unitPrice: 1050, discount: 0 },
  { id: 'sku-102', name: 'Кофе латте на овсяном молоке', qty: 2, unitPrice: 690, discount: 138 },
  { id: 'sku-103', name: 'Сэндвич с курицей и томатами', qty: 1, unitPrice: 840, discount: 0 },
  { id: 'sku-104', name: 'Вода питьевая негазированная 0.5 л', qty: 3, unitPrice: 150, discount: 0 },
  { id: 'sku-105', name: 'Шоколадный батончик 60 г', qty: 2, unitPrice: 320, discount: 64 },
]

let itemSeq = 0
function addRandomItem(withDiscount = false) {
  const base = DEMO_ITEMS[itemSeq % DEMO_ITEMS.length]
  itemSeq++
  const line = { ...base, id: base.id + '-' + itemSeq }
  if (withDiscount) line.discount = Math.round(line.unitPrice * line.qty * 0.1)
  line.lineTotal = line.qty * line.unitPrice - line.discount
  dispatch({ type: 'line/upsert', line })
}

function removeLastItem() {
  const last = state.lines[state.lines.length - 1]
  if (last) dispatch({ type: 'line/remove', id: last.id })
}

/* ============================ ДЕМО-ЦИКЛ ============================ */

let autoDemo = true
let demoToken = 0

function stopAutoDemo() {
  autoDemo = false
  demoToken++
  $('btnAutoDemo').textContent = 'Авто-демо: выкл'
  $('btnAutoDemo').classList.remove('devBtnAccent')
}

async function runDemoCycle() {
  const my = ++demoToken
  const alive = () => autoDemo && my === demoToken

  dispatch({ type: 'check/clear' })
  await sleep(1000); if (!alive()) return

  for (let i = 0; i < 3; i++) {
    addRandomItem(i === 1)
    await sleep(850); if (!alive()) return
  }
  await sleep(1700); if (!alive()) return

  dispatch({ type: 'payment/start', method: 'card' })
  await sleep(2400); if (!alive()) return
  dispatch({ type: 'payment/success' })
  // SUCCESS держится 4.5 с (таймер в applyEvent), затем сам вернётся в WAITING
  await sleep(4700); if (!alive()) return
}

async function autoLoop() {
  while (autoDemo) {
    await runDemoCycle()
  }
}

/* ============================ ПАНЕЛЬ РАЗРАБОТЧИКА ============================ */

function renderLog() {
  const log = $('devLog')
  log.innerHTML = eventLog.map((e) => `<div>${e}</div>`).join('')
}

function wireDevPanel() {
  $('btnAutoDemo').addEventListener('click', () => {
    autoDemo = !autoDemo
    $('btnAutoDemo').textContent = autoDemo ? 'Авто-демо: вкл' : 'Авто-демо: выкл'
    $('btnAutoDemo').classList.toggle('devBtnAccent', autoDemo)
    if (autoDemo) autoLoop()
    else demoToken++
  })

  $('btnRunOnce').addEventListener('click', () => {
    if (!autoDemo) runDemoCycle()
  })

  $('btnAddItem').addEventListener('click', () => { stopAutoDemo(); addRandomItem(false) })
  $('btnAddDiscount').addEventListener('click', () => { stopAutoDemo(); addRandomItem(true) })
  $('btnRemoveLast').addEventListener('click', () => { stopAutoDemo(); removeLastItem() })
  $('btnClear').addEventListener('click', () => { stopAutoDemo(); dispatch({ type: 'check/clear' }) })

  $('btnPayStart').addEventListener('click', () => { stopAutoDemo(); dispatch({ type: 'payment/start', method: 'card' }) })
  $('btnPayOk').addEventListener('click', () => dispatch({ type: 'payment/success' }))
  $('btnPayFail').addEventListener('click', () => dispatch({ type: 'payment/failed', reason: 'Терминал отклонил операцию.' }))

  $('btnAdNext').addEventListener('click', () => {
    if (state.screen === 'WAITING') nextAd('waitingAdLayers', () => waitingIndex, (v) => (waitingIndex = v))
    else nextAd('shoppingAdLayers', () => shoppingIndex, (v) => (shoppingIndex = v))
  })
  $('btnAdPause').addEventListener('click', () => {
    adsPaused = !adsPaused
    $('btnAdPause').textContent = adsPaused ? 'Пуск' : 'Пауза'
  })
  $('devAdSelect').addEventListener('change', (e) => {
    const v = e.target.value
    if (v === '') return
    const idx = parseInt(v, 10)
    if (state.screen === 'WAITING') { waitingIndex = idx; setAd('waitingAdLayers', idx) }
    else { shoppingIndex = idx; setAd('shoppingAdLayers', idx) }
  })

  $('devCollapse').addEventListener('click', () => {
    const p = $('devPanel')
    p.classList.toggle('collapsed')
    $('devCollapse').textContent = p.classList.contains('collapsed') ? '‹' : '›'
    fitStage()
  })
}

/* ============================ МАСШТАБИРОВАНИЕ СЦЕНЫ ============================ */

function fitStage() {
  const wrap = $('stageWrap')
  const scale = Math.min(wrap.clientWidth / 1920, wrap.clientHeight / 1080)
  $('stage').style.transform = `scale(${scale})`
}
window.addEventListener('resize', fitStage)

/* ============================ СТАРТ ============================ */

wireDevPanel()
fitStage()
render()
setInterval(() => { adTimerTick++; syncAds() }, 1000)
autoLoop()

/* Мост для будущего драйвера: window.__pos.dispatch({type: 'line/upsert', line: {...}}) */
window.__pos = { dispatch, getState: () => state }
