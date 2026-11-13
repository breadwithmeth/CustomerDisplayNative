// Харнесс: прогон display-ui/app.js с DOM-заглушками и потоком событий БПО.
// Запуск: node tests/ui/test_bpo.js display-ui/app.js
'use strict';
const fs = require('fs');

function fakeEl() {
  const target = {};
  return new Proxy(target, {
    get(t, p) {
      if (p === 'style') return (t._style = t._style || {});
      if (p === 'classList') return (t._cl = t._cl ||
        { add() {}, remove() {}, toggle() {}, contains() { return false; } });
      if (p === 'addEventListener' || p === 'removeEventListener') return () => {};
      if (p === 'appendChild' || p === 'removeChild' || p === 'insertBefore' ||
          p === 'remove' || p === 'replaceWith') return () => {};
      if (p === 'querySelector' || p === 'closest') return () => fakeEl();
      if (p === 'querySelectorAll' || p === 'children' || p === 'childNodes') return [];
      if (p === 'clientWidth') return 1920;
      if (p === 'clientHeight') return 1080;
      if (p === 'offsetWidth') return 100;
      if (typeof p === 'string' && !(p in t)) return undefined;
      return t[p];
    },
    set(t, p, v) { t[p] = v; return true; },
    has() { return true; },
  });
}

// элементы по id — стабильные (после render можно читать textContent)
const elements = new Map();
const document = {
  getElementById: (id) => {
    if (!elements.has(id)) elements.set(id, fakeEl());
    return elements.get(id);
  },
  addEventListener: () => {},
  querySelector: () => fakeEl(),
  querySelectorAll: () => [],
  createElement: () => fakeEl(),
};
const window = { addEventListener: () => {} };
const location = { search: '?kiosk=1' };

const appJs = process.argv[2];
const src = fs.readFileSync(appJs, 'utf8');
const make = new Function('window', 'document', 'location', 'setInterval', 'setTimeout',
  'requestAnimationFrame', 'cancelAnimationFrame', 'performance',
  src + '\nreturn window.__pos;');

// отложенные колбэки страницы (setTimeout) — можно «прожечь» по времени
const pageTimers = [];
const fireTimers = (elapsedMs) => {
  const due = pageTimers.filter((t) => t.ms <= elapsedMs);
  pageTimers.length = 0;
  for (const t of due) t.fn();
};

const pos = make(window, document, location, () => 0,
  (fn, ms) => { pageTimers.push({ fn, ms }); return pageTimers.length; },
  () => {}, () => {}, { now: () => 0 });

let failures = 0;
function check(cond, msg) {
  console.log((cond ? '  [OK]   ' : '  [FAIL] ') + msg);
  if (!cond) failures++;
}

// ---- сценарий 1: реальный поток из лога (БПО: «имя\rцена» двумя событиями) ----
console.log('Сценарий 1: позиция «имя\\rцена» + ИТОГО + повтор позиции');
pos.dispatch({ type: 'check/item', name: 'Энергетический напиток Monster Zero Ultra 0.5 жб' });
pos.dispatch({ type: 'check/item', name: '1\u00A0050,00 тг.' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 1050,00' });
// РМК повторно шлёт позицию после итога
pos.dispatch({ type: 'check/item', name: 'Энергетический напиток Monster Zero Ultra 0.5 жб' });
pos.dispatch({ type: 'check/item', name: '1\u00A0050,00 тг.' });

let st = pos.getState();
check(st.lines.length === 1, 'в чеке одна позиция, а не ' + st.lines.length);
check(st.lines[0] && st.lines[0].sumText === '1\u00A0050,00 тг.',
  'у позиции есть цена: ' + JSON.stringify(st.lines[0] && st.lines[0].sumText));
check(st.totalText === 'ИТОГО: 1050,00', 'итог отображается: ' + JSON.stringify(st.totalText));

// ---- сценарий 2: шум пересчёта — голая сумма без пары ----
console.log('Сценарий 2: голая сумма без пары (шум пересчёта)');
pos.dispatch({ type: 'check/item', name: '2\u00A0100,00 тг.' });
st = pos.getState();
check(st.lines.length === 1, 'шум не добавил позицию (позиций: ' + st.lines.length + ')');
check(st.lines[0].sumText === '1\u00A0050,00 тг.', 'шум не затёр цену: ' + JSON.stringify(st.lines[0].sumText));

// ---- сценарий 3: новый чек, имя и цена одной строкой ----
console.log('Сценарий 3: новый чек, имя и цена одной строкой');
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Молоко «Простоквашино» 3,2% 89,00' });
pos.dispatch({ type: 'check/item', name: 'Хлеб «Бородинский» 54,50' });
st = pos.getState();
check(st.lines.length === 2, 'две позиции (позиций: ' + st.lines.length + ')');
check(st.lines[0] && st.lines[0].sumText === '89,00', 'цена первой: ' + JSON.stringify(st.lines[0] && st.lines[0].sumText));
check(st.lines[1] && st.lines[1].sumText === '54,50', 'цена второй: ' + JSON.stringify(st.lines[1] && st.lines[1].sumText));

// ---- сценарий 4: qty-обновление «имя\r2xцена=сумма» ----
console.log('Сценарий 4: обновление количества');
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Молоко 3,2%' });
pos.dispatch({ type: 'check/item', name: '89,00' });
pos.dispatch({ type: 'check/item', name: '2x89,00 тг.=178,00 тг.' });
st = pos.getState();
check(st.lines.length === 1, 'позиция одна (позиций: ' + st.lines.length + ')');
check(st.lines[0] && st.lines[0].qtyText === '2×89,00', 'количество: ' + JSON.stringify(st.lines[0] && st.lines[0].qtyText));
check(st.lines[0] && st.lines[0].sumText === '178,00', 'сумма: ' + JSON.stringify(st.lines[0] && st.lines[0].sumText));

// ---- сценарий 5: повтор позиции после qty-обновления ----
console.log('Сценарий 5: повтор позиции после qty-обновления');
pos.dispatch({ type: 'check/item', name: 'Молоко 3,2%' });
pos.dispatch({ type: 'check/item', name: '89,00' });
st = pos.getState();
check(st.lines[0] && st.lines[0].sumText === '89,00', 'цена восстановилась: ' + JSON.stringify(st.lines[0] && st.lines[0].sumText));

// ---- сценарий 6: «ВНЕСЕНО: X\rСДАЧА: Y» разрезом (как шлёт РМК) ----
console.log('Сценарий 6: внесено + сдача отдельной строкой');
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Стакан фирменный одноразовый 0.5' });
pos.dispatch({ type: 'check/item', name: '50,00 тг.' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 50,00' });
// драйвер режет строку оплаты: op/paid (сдача пустая) + op/change
pos.dispatch({ type: 'op/paid', paid: 'ВНЕСЕНО: 50,00', change: '' });
pos.dispatch({ type: 'op/change', text: 'СДАЧА: 0.00' });
st = pos.getState();
check(st.screen === 'PAYMENT', 'экран оплаты: ' + st.screen);
check(st.lines.length === 1, 'сдача не добавилась позицией (позиций: ' + st.lines.length + ')');
check(st.paidText === 'ВНЕСЕНО: 50,00' && st.changeText === 'СДАЧА: 0.00',
  'внесено/сдача: ' + JSON.stringify([st.paidText, st.changeText]));

// ---- сценарий 7: чистка по таймеру после оплаты (reason=paid) ----
console.log('Сценарий 7: чистка после оплаты и защита нового чека');
// таймер драйвера сработал через 5 с
pos.dispatch({ type: 'check/clear', reason: 'paid' });
st = pos.getState();
check(st.screen === 'WAITING' && st.lines.length === 0,
  'чек очищен, экран заставки: ' + st.screen + ', позиций ' + st.lines.length);
// кассир начал новый чек раньше сработавшего таймера — чистка игнорируется
pos.dispatch({ type: 'check/item', name: 'Майка фирменная' });
pos.dispatch({ type: 'check/item', name: '30,00 тг.' });
pos.dispatch({ type: 'check/clear', reason: 'paid' }); // опоздавшая чистка
st = pos.getState();
check(st.lines.length === 1 && st.screen === 'SHOPPING',
  'новый чек пережил опоздавшую чистку (' + st.screen + ', позиций ' + st.lines.length + ')');
// повторный итог от РМК без позиций не выталкивает пустой чек
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 50,00' });
st = pos.getState();
check(st.screen === 'WAITING', 'итог без позиций — остаёмся на заставке: ' + st.screen);
// первый товар нового чека сбрасывает итог прошлого
pos.dispatch({ type: 'check/item', name: 'Молоко' });
st = pos.getState();
check(st.totalText === '', 'итог прошлого чека сброшен: ' + JSON.stringify(st.totalText));

// ---- сценарий 8: экран оплаты без заглушки «Оплата картой» ----
console.log('Сценарий 8: подпись экрана оплаты');
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Майка фирменная' });
pos.dispatch({ type: 'check/item', name: '30,00 тг.' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 30,00' });
pos.dispatch({ type: 'op/paid', paid: 'ВНЕСЕНО: 30,00', change: '' });
pos.dispatch({ type: 'op/change', text: 'СДАЧА: 0.00' });
st = pos.getState();
check(st.screen === 'PAYMENT', 'экран оплаты: ' + st.screen);
check(!document.getElementById('payMethod').textContent,
  'заглушка «Оплата картой» не показывается: ' +
  JSON.stringify(document.getElementById('payMethod').textContent));
check(document.getElementById('payPaid').textContent === 'ВНЕСЕНО: 30,00' &&
      document.getElementById('payChange').textContent === 'СДАЧА: 0.00',
  'внесено и сдача на экране');
// реальная строка операции из РМК по-прежнему показывается
pos.dispatch({ type: 'op/show', text: 'ОПЛАТА КАРТОЙ' });
check(document.getElementById('payMethod').textContent === 'ОПЛАТА КАРТОЙ',
  'текст операции из РМК показывается: ' +
  JSON.stringify(document.getElementById('payMethod').textContent));
// и скрывается при следующей оплате наличными
pos.dispatch({ type: 'op/paid', paid: 'ВНЕСЕНО: 30,00', change: '' });
check(!document.getElementById('payMethod').textContent,
  'после наличной оплаты подпись скрыта');

// ---- сценарий 9: самостоятельный переход в рекламу после оплаты ----
console.log('Сценарий 9: переход в рекламу без таймера драйвера');
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Майка фирменная' });
pos.dispatch({ type: 'check/item', name: '30,00 тг.' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 30,00' });
pos.dispatch({ type: 'op/paid', paid: 'ВНЕСЕНО: 30,00', change: '' });
pos.dispatch({ type: 'op/change', text: 'СДАЧА: 0.00' });
st = pos.getState();
check(st.screen === 'PAYMENT', 'экран оплаты: ' + st.screen);
fireTimers(5000); // прошло 5 с — JS-таймер страницы (таймер драйвера потерялся)
st = pos.getState();
check(st.screen === 'WAITING' && st.lines.length === 0,
  'сами ушли в полноэкранную рекламу: ' + st.screen + ', позиций ' + st.lines.length);
// новый чек до истечения 5 с отменяет собственный переход
pos.dispatch({ type: 'check/clear' });
pos.dispatch({ type: 'check/item', name: 'Молоко' });
pos.dispatch({ type: 'check/item', name: '89,00' });
pos.dispatch({ type: 'check/total', text: 'ИТОГО: 89,00' });
pos.dispatch({ type: 'op/paid', paid: 'ВНЕСЕНО: 89,00', change: '' });
pos.dispatch({ type: 'check/item', name: 'Хлеб' }); // кассир начал новый чек
fireTimers(5000);                                     // таймер платы сработал
st = pos.getState();
check(st.screen === 'SHOPPING' && st.lines.length === 2,
  'новый чек пережил собственный таймер перехода: ' + st.screen +
  ', позиций ' + st.lines.length);

console.log('\nИтог: ' + (failures ? 'ЕСТЬ ОШИБКИ (' + failures + ')' : 'УСПЕХ'));
process.exit(failures ? 1 : 0);
