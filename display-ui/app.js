/* ============================================================
   Customer Display — логика
   Разделение: state-машина · POS-шина · рекламный движок · рендер
   ============================================================ */

'use strict'

// Версия UI — в window, чтобы драйвер мог проверить по probe
const UI_VERSION = '1.1.3.5'
window.UI_VERSION = UI_VERSION

/* Kiosk-режим: страница открыта драйвером (WebView2) — панель разработчика
   скрыта, авто-демо выключено (чек появляется только от событий кассы) */
const KIOSK = new URLSearchParams(location.search).has('kiosk')

/* ============================ УТИЛИТЫ ============================ */

const $ = (id) => document.getElementById(id)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const fmtMoney = (v) =>
  new Intl.NumberFormat('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(v) + ' ₸'

/** Из строки БПО выделяет структуру:
    «Наименование 1 050,00 тг.»              → { name, sum }
    «…имя… 2x1 050,00 тг.=2 100,00 тг.»      → { qtyUpdate, nameBefore }
    «-5x1 110,00 тг.=4 440,00 тг.»           → qtyUpdate с отрицательным qty (отмена)
    «СКИДКА: 1110,00»                        → { discount }
    «ВНЕСЕНО: 30,00 СДАЧА: 0.00»             → { paid, change }
    «1 110,00 тг.» (только сумма)            → { sumOnly } */
function splitItemLine(line) {
  // «…N×цена = сумма» — в конце строки (qty может быть с минусом = отмена)
  const q = line.match(/(?:^|\s)(-?\d+)\s*[x×]\s*(-?\d[\d\s\u00A0]*[.,]\d{2})\s*(?:тг|₸|₽|тенге)?\.?\s*=\s*(-?\d[\d\s\u00A0]*[.,]\d{2})\s*(?:тг|₸|₽|тенге)?\.?\s*$/i)
  if (q) {
    return {
      qtyUpdate: { qty: parseInt(q[1], 10), price: q[2].trim(), total: q[3].trim() },
      nameBefore: q.index > 0 ? line.slice(0, q.index).trim() : '',
    }
  }
  // скидка
  const d = line.match(/^\s*скидка\s*:?\s*(.*)$/i)
  if (d) return { discount: d[1].trim() }
  // внесено/сдача (наличная оплата)
  const paid = line.match(/^\s*внесено\s*:?\s*(.+?)\s*сдача\s*:?\s*(\S+)\s*$/i)
  if (paid) return { paid: paid[1].trim(), change: paid[2].trim() }
  // «имя … сумма» — сумма в конце строки
  const m = line.match(/^(.*?)[\s\u00A0]*((?:\d[\d\s\u00A0]*[.,]\d{2})\s*(?:тг|₸|₽|тенге)?\.?)\s*$/i)
  if (m) {
    const name = m[1].trim()
    const sum = m[2].trim()
    // без наименования или «имя» — чистая сумма: не позиция
    if (!name || /^[\d\s\u00A0.,]+(?:тг|₸|₽|тенге)?\.?$/i.test(name)) return { sumOnly: sum }
    return { name, sum }
  }
  return { name: line.trim(), sum: '' }
}

let itemSeq = 0

/* ============================ СОСТОЯНИЕ ============================ */

const state = {
  screen: 'WAITING',   // WAITING | SHOPPING | PAYMENT | SUCCESS | ERROR | QR | MEDIA | MESSAGE
  lines: [],           // { id, name, qty, unitPrice, discount, lineTotal, sumText?, qtyText? }
  total: 0,
  totalText: '',       // итог строкой (из драйвера), приоритетнее total
  discountText: '',    // скидка строкой (из драйвера)
  lastTouchedId: null, // позиция для «N×цена = сумма»
  awaitingPrice: false, // ждём цену отдельной строкой («имя\rцена» из БПО)
  awaitingPaidClear: false, // ждём чистку чека после «ВНЕСЕНО»
  method: null,        // 'card' | 'cash'
  opText: '',          // текст строки оплаты из РМК
  success: null,       // { total, positions }
  error: null,
  message: null,       // текст сообщения (приветствие/простой/отмена)
  paidText: '',        // «ВНЕСЕНО: X» при наличной оплате
  changeText: '',      // «СДАЧА: Y» при наличной оплате
  qrDataUrl: null,     // data:image/png;base64,... (QR-оплата из драйвера)
  mediaUrl: null,      // url медиафайла (реклама из драйвера)
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
      // отложенная чистка после «ВНЕСЕНО»: если уже начал пробиваться новый
      // чек (прошёл check/item) — не затираем его
      if (event.reason === 'paid' && !state.awaitingPaidClear) return
      state.awaitingPaidClear = false
      // сброс всегда: «ОТМЕНА ОПЛАТЫ»/«КАССА НЕ РАБОТАЕТ» из БПО должны
      // очищать дисплей даже посреди оплаты
      state.lines = []
      state.total = 0
      state.totalText = ''
      state.discountText = ''
      state.lastTouchedId = null
      state.awaitingPrice = false
      state.screen = 'WAITING'
      break
    }
    case 'payment/start': {
      if (state.lines.length === 0) return
      state.method = event.method || 'card'
      state.screen = 'PAYMENT'
      break
    }
    // ---- чек готовыми строками из драйвера («Наименование | Сумма») ----
    case 'check/lines': {
      if (state.screen === 'PAYMENT' || state.screen === 'SUCCESS') return
      const rows = event.lines || []
      const totalRow = rows.find((r) => r.total)
      state.lines = rows.filter((r) => !r.total).map((r, i) => ({
        id: 'l' + i,
        name: r.name || '',
        qty: null,
        unitPrice: null,
        discount: null,
        lineTotal: null,
        sumText: r.sum || '',
      }))
      state.totalText = totalRow ? totalRow.sum || '' : ''
      state.total = 0
      state.screen = 'SHOPPING'
      break
    }
    // ---- события из драйвера (WebView2-интеграция) ----
    case 'check/item': {
      if (state.screen === 'SUCCESS') return
      // новый чек начался — отменяем отложенную чистку после «ВНЕСЕНО»
      state.awaitingPaidClear = false
      const p = splitItemLine(event.name || '')

      // оплата наличными: «ВНЕСЕНО: X СДАЧА: Y» — экран оплаты;
      // очистка чека через 5 с делает драйвер (Win32-таймер, event check/clear)
      if (p.paid !== undefined) {
        state.paidText = 'ВНЕСЕНО: ' + p.paid
        state.changeText = p.change ? 'СДАЧА: ' + p.change : ''
        state.awaitingPaidClear = true
        state.screen = 'PAYMENT'
        render()
        return
      }

      if (state.screen === 'SUCCESS') return

      // скидка
      if (p.discount !== undefined) {
        state.discountText = p.discount
        state.screen = 'SHOPPING'
        return
      }

      // «N×цена = сумма» — обновление количества (или отмена при отрицательном qty)
      if (p.qtyUpdate) {
        let target = state.lines.find((l) => l.id === state.lastTouchedId) ||
                     state.lines[state.lines.length - 1]
        if (p.nameBefore) {
          const named = state.lines.find((l) => l.name === p.nameBefore)
          if (named) target = named
        }
        if (!target) return
        if (p.qtyUpdate.qty < 0) {
          // отмена позиции — убираем её из чека
          state.lines = state.lines.filter((l) => l !== target)
          if (state.lastTouchedId === target.id) state.lastTouchedId = null
          state.awaitingPrice = false
          if (state.lines.length === 0) state.screen = 'WAITING'
          else if (state.lines.length > 0) state.screen = 'SHOPPING'
          return
        }
        target.qty = null
        target.qtyText = p.qtyUpdate.qty + '×' + p.qtyUpdate.price
        target.sumText = p.qtyUpdate.total
        state.lastTouchedId = target.id
        state.awaitingPrice = false
        state.screen = 'SHOPPING'
        return
      }

      // БПО шлёт позицию ВФД-строкой «имя\rцена»: наименование и сумма
      // приходят двумя событиями. Сразу после наименования цепляем сумму
      // к последней позиции; голые суммы без пары — по-прежнему шум
      // пересчёта, их игнорируем
      if (p.sumOnly) {
        if (state.awaitingPrice) {
          const target = state.lines.find((l) => l.id === state.lastTouchedId) ||
                         state.lines[state.lines.length - 1]
          if (target) target.sumText = p.sumOnly
          state.awaitingPrice = false
        }
        return
      }

      const name = p.name
      if (!name) return

      // позиция уже в чеке
      const existing = state.lines.find((l) => l.name === name)
      if (existing) {
        // «0,00»-повтор — шум, не трогаем; реальная сумма — обновляем
        if (p.sum && !/^0[.,]00/.test(p.sum)) {
          existing.sumText = p.sum
          state.lastTouchedId = existing.id
          state.awaitingPrice = false
          state.screen = 'SHOPPING'
        } else if (!p.sum) {
          // наименование без суммы — цена придёт отдельной строкой
          state.lastTouchedId = existing.id
          state.awaitingPrice = true
        }
        return
      }

      // первый товар нового чека — итог/скидка прошлого чека больше не актуальны
      if (state.lines.length === 0) {
        state.totalText = ''
        state.discountText = ''
      }
      state.lines.push({
        id: 'i' + (++itemSeq) + '-' + Date.now(),
        name,
        qty: null, unitPrice: null, discount: null, lineTotal: null,
        sumText: p.sum,
      })
      state.lastTouchedId = state.lines[state.lines.length - 1].id
      state.awaitingPrice = !p.sum
      state.screen = 'SHOPPING'
      break
    }
    case 'check/total': {
      if (state.screen === 'SUCCESS') return
      // при экране оплаты (внесено) остаёмся на PAYMENT, обновляя сумму
      state.totalText = event.text || ''
      state.awaitingPrice = false // итог: пара «имя-цена» закрыта
      // итог без позиций (повтор от РМК после оплаты) не вытаскивает
      // пустой чек на экран — остаёмся на заставке
      if (state.screen !== 'PAYMENT' && state.lines.length > 0) state.screen = 'SHOPPING'
      break
    }
    case 'message/show': {
      state.message = event.text || ''
      state.awaitingPaidClear = false
      state.screen = 'MESSAGE'
      setTimeout(() => {
        if (state.screen === 'MESSAGE' && state.message === event.text) {
          state.message = null
          state.screen = state.lines.length > 0 ? 'SHOPPING' : 'WAITING'
          render()
        }
      }, 4500)
      break
    }
    case 'op/paid': {
      // наличная оплата: крупные «ВНЕСЕНО» / «СДАЧА»; чистку делает драйвер
      state.opText = ''
      state.paidText = event.paid || ''
      state.changeText = event.change || ''
      state.awaitingPaidClear = true
      state.method = null
      state.screen = 'PAYMENT'
      break
    }
    case 'op/change': {
      // «ВНЕСЕНО: X\rСДАЧА: Y» режется драйвером на две строки: сдача
      // дописывается на экран оплаты отдельным событием
      state.changeText = event.text || ''
      state.awaitingPaidClear = true
      if (state.screen !== 'PAYMENT') state.screen = 'PAYMENT'
      break
    }
    case 'op/show': {
      // строка оплаты из РМК («ОПЛАТА КАРТОЙ», «ВНЕСЕНИЕ ОПЛАТЫ»…) —
      // экран PAYMENT с текущим итогом и текстом
      state.opText = event.text || ''
      state.method = null
      state.screen = 'PAYMENT'
      break
    }
    case 'op/status': {
      // 0 — операция не выполняется, 1 — выполняется, 2 — не выполнена
      if (event.code === 1 && state.lines.length > 0) {
        state.method = 'card'
        state.screen = 'PAYMENT'
      } else if (event.code === 2 && state.screen === 'PAYMENT') {
        state.error = 'Терминал отклонил операцию.'
        state.screen = 'ERROR'
        setTimeout(() => {
          if (state.screen === 'ERROR') {
            state.error = null
            state.screen = state.lines.length > 0 ? 'SHOPPING' : 'WAITING'
            render()
          }
        }, 4000)
      } else if (event.code === 0 && state.screen === 'PAYMENT') {
        state.success = { total: state.total, positions: positionsCount() }
        state.screen = 'SUCCESS'
        setTimeout(() => {
          if (state.screen === 'SUCCESS') {
            state.lines = []
            state.total = 0
            state.success = null
            state.screen = 'WAITING'
            render()
          }
        }, 4500)
      }
      break
    }
    case 'qr/show': {
      state.qrDataUrl = event.dataUrl || null
      state.screen = 'QR'
      break
    }
    case 'media/show': {
      state.mediaUrl = event.url || null
      state.screen = 'MEDIA'
      break
    }
    case 'demo/run': {
      if (!autoDemo) runDemoCycle()
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
  for (const id of ['screenWaiting', 'screenShopping', 'screenPayment', 'screenSuccess', 'screenError', 'screenQr', 'screenMedia', 'screenMessage']) {
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
  renderQr()
  renderMedia()
  renderMessage()
  syncAds()
}

function renderMessage() {
  if (state.screen !== 'MESSAGE') return
  $('msgText').textContent = state.message || ''
}

function renderQr() {
  if (state.screen !== 'QR') return
  const card = $('qrPayCard')
  card.innerHTML = ''
  if (state.qrDataUrl) {
    const img = document.createElement('img')
    img.src = state.qrDataUrl
    card.appendChild(img)
  }
}

function renderMedia() {
  if (state.screen !== 'MEDIA') return
  const img = $('mediaImg')
  if (state.mediaUrl && img.getAttribute('src') !== state.mediaUrl)
    img.src = state.mediaUrl
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
    const sumText = line.sumText || fmtMoney(line.lineTotal)
    if (sum.textContent !== sumText) {
      sum.textContent = sumText
      node.classList.remove('flash')
      void node.offsetWidth // reflow для перезапуска анимации
      node.classList.add('flash')
    }
    if (line.qtyText) {
      qp.textContent = line.qtyText
      qp.classList.remove('hidden')
    } else if (line.qty) {
      qp.textContent = `${line.qty} шт × ${fmtMoney(line.unitPrice)}`
      qp.classList.remove('hidden')
    } else {
      qp.textContent = ''
      qp.classList.add('hidden')
    }
    if (line.discount > 0) {
      disc.textContent = '−' + fmtMoney(line.discount)
      disc.classList.remove('hidden')
    } else {
      disc.textContent = ''
      disc.classList.add('hidden')
    }
    list.appendChild(node) // порядок
  })

  // маска переполнения
  const totalH = state.lines.length * 108
  const maxH = list.clientHeight
  const masked = totalH > maxH && maxH > 0
  $('rcListMask').classList.toggle('on', masked)
}

function renderTotals() {
  if (state.totalText) {
    // итог пришёл строкой из драйвера — гасим активную анимацию числа,
    // иначе её кадры перезаписывают textContent
    const el = $('rcTotal')
    const prev = numAnims.get(el)
    if (prev) { cancelAnimationFrame(prev.raf); numAnims.delete(el) }
    if (el.textContent !== state.totalText)
      el.textContent = state.totalText
  } else {
    animateNumber($('rcTotal'), state.total)
  }
  $('rcPositions').textContent = 'Позиций: ' + state.lines.length
  const d = $('rcDiscount')
  if (state.discountText) {
    d.textContent = 'Скидка: ' + state.discountText
    d.classList.remove('hidden')
  } else {
    const numericDiscount = state.lines.reduce((s, l) => s + (l.discount || 0), 0)
    if (numericDiscount > 0) {
      d.textContent = 'Скидка: ' + fmtMoney(numericDiscount)
      d.classList.remove('hidden')
    } else {
      d.classList.add('hidden')
    }
  }
}

/* ---------------------------- оплата / успех / ошибка ---------------------------- */

let payTotalShown = 0

function renderPayment() {
  if (state.screen !== 'PAYMENT') return
  const el = $('payAmount')
  if (state.totalText) {
    // итог строкой из драйвера — гасим анимацию
    const prev = numAnims.get(el)
    if (prev) { cancelAnimationFrame(prev.raf); numAnims.delete(el) }
    if (el.textContent !== state.totalText) el.textContent = state.totalText
  } else {
    animateNumber(el, state.total)
  }
  // подпись — только реальная строка операции из РМК («ОПЛАТА КАРТОЙ» и
  // т.п.); заглушки «Оплата картой» больше нет — при наличной оплате всё
  // говорят строки ВНЕСЕНО/СДАЧА
  const cap = $('payMethod')
  cap.textContent = state.opText || ''
  cap.classList.toggle('hidden', !state.opText)

  // внесено/сдача при наличной оплате
  const paid = $('payPaid')
  const change = $('payChange')
  if (state.paidText) {
    paid.textContent = state.paidText
    paid.classList.remove('hidden')
  } else {
    paid.classList.add('hidden')
  }
  if (state.changeText) {
    change.textContent = state.changeText
    change.classList.remove('hidden')
  } else {
    change.classList.add('hidden')
  }
  const hint = $('payHint')
  hint.textContent = state.paidText ? 'Оплата принята' : 'Ожидание подтверждения терминалом…'
  const spin = document.querySelector('.paySpinner')
  if (spin) spin.style.display = state.paidText ? 'none' : ''
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
    image: 'assets/ad1.jpg',
    imageFull: 'assets/ad1_full.jpg',
  },
  {
    template: 'productHero',
    image: 'assets/ad2.jpg',
    imageFull: 'assets/ad2_full.jpg',
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

function buildAd(ad, context) {
  // Реклама = просто картинка на всю область (WAITING — полноэкранная,
  // SHOPPING — портретная версия на всю правую панель), без текста поверх
  const root = el('div', 'ad adImgFull')
  const img = el('img', 'adImg', '')
  img.src = context === 'waiting' ? (ad.imageFull || ad.image) : (ad.image || ad.imageFull)
  root.appendChild(img)
  return root
}

function setAd(hostId, index) {
  const host = $(hostId)
  const context = hostId === 'waitingAdLayers' ? 'waiting' : 'shopping'
  const ad = buildAd(ADS[index % ADS.length], context)
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

let autoDemo = !KIOSK // авто-демо только в браузерном прототипе
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
  // cover: заполняем всё окно (без чёрных полей), обрезая излишек;
  // начало координат слева — чек всегда виден целиком, обрезка уходит в рекламу
  const scale = Math.max(wrap.clientWidth / 1920, wrap.clientHeight / 1080)
  const stage = $('stage')
  stage.style.transform = `scale(${scale})`
  stage.style.transformOrigin = 'left center'
}
window.addEventListener('resize', fitStage)

/* ============================ СТАРТ ============================ */

wireDevPanel()
fitStage()
render()
setInterval(() => { adTimerTick++; syncAds() }, 1000)
if (autoDemo) autoLoop()

/* Мост для драйвера: события приходят двумя путями —
   1) PostWebMessageAsJson (штатный канал WebView2, основной)
   2) ExecuteScript → window.__pos.dispatch(...) (резервный) */
if (KIOSK) document.getElementById('devPanel').style.display = 'none'

try {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (e) => {
      try {
        const data = e.data
        if (data && Array.isArray(data.events)) data.events.forEach((x) => { if (x && x.type) dispatch(x) })
        else if (Array.isArray(data)) data.forEach((x) => { if (x && x.type) dispatch(x) })
        else if (data && data.type) dispatch(data)
      } catch (err) { /* игнор */ }
    })
  }
} catch (e) { /* не WebView2 — просто браузерный прототип */ }

window.__pos = { dispatch, getState: () => state }
