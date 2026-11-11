# CustomerDisplayNative — драйвер «Дисплей покупателя» для 1С:Предприятие 8.3

Внешняя компонента (Native API) для 1С:Предприятие 8.3, превращающая **обычный монитор
компьютера в дисплей покупателя**: полноэкранный кассовый экран или полоса-панель
у нижнего края с чеком, QR-кодом и статусами.

Реализует **стандартный интерфейс драйверов подключаемого оборудования 1С**
(its.1c.ru, i8104829): общий контракт + контракт «CustomerDisplay», а также
совместимость со старым интерфейсом шаблона InnovaIT (`УстановитьПараметр`,
`Подключить`, `Отключить`, `ТестУстройства`…). Подключается в типовых
конфигурациях (Розница 2.3 / УТ 11 / БПО 2.x) через механизм
«Драйверы оборудования → Универсальный драйвер».

## Возможности

- **Кассовый экран**: список позиций (левая часть / сумма), крупная строка «ИТОГО»
- **QR-код**: генерация из строки (встроенный кодировщик) или готовая PNG-картинка (Base64)
- **Статусы**: операция (ожидание / выполняется / не выполнена), рабочее место (0–4) + звуковой сигнал
- **Медиа**: картинки-заставки/реклама (PNG/JPEG/BMP, Base64 → файл)
- **Режимы окна**: весь монитор или полоса снизу («как панель программы»), высота полосы настраивается
- **Мультимониторность**: выбор монитора параметром `MonitorIndex`
- **Окно в панели задач и Alt+Tab**, закрывается правым кликом из панели;
  при следующем выводе от БПО переоткрывается автоматически
- **Журнал вызовов**: `%TEMP%\CustomerDisplayNative\driver.log` — каждый вызов платформы,
  параметры, результаты и состояние окна (самовосстанавливающийся)

## Состав контракта

**Новый интерфейс (ревизия 4.x):** `GetDescription`, `GetLastError`,
`EquipmentParameters`, `ConnectEquipment`, `DisconnectEquipment`, `EquipmentTest`,
`EquipmentAutoSetup`, `SetApplicationInformation`, `GetAdditionalActions`,
`DoAdditionalAction`, `GetLocalizationPattern`, `SetLocalization`,
`StringOutputOnCustomerDisplay`, `ClearCustomerDisplay`, `AddMediaFile`,
`QRCodeOutputOnCustomerDisplay`, `DisplayOperationStatus`,
`DisplayWorkplaceStatus`, `GetCustomerDisplayOptions`, `GetInterfaceRevision`

**Старый интерфейс (шаблон InnovaIT, ревизия 2.005):** `GetVersion`,
`GetDescription` (7 параметров), `GetLastError`, `GetParameters`,
`SetParameter`, `Open`, `Close`, `DeviceTest`, `GetAdditionalActions`,
`DoAdditionalAction`

Все методы регистрируются парами EN/RU + дополнительные алиасы — работают вызовы
на любом языке. `GetInfo()` = 2005 (как в рабочих драйверах InnovaIT).

## Параметры подключения (XML `Parameters`)

| Параметр | Тип | По умолчанию | Описание |
|---|---|---|---|
| `MonitorIndex` | Число | 2 | Номер монитора: 1 — основной, 2 — второй… |
| `Fullscreen` | Булево | Ложь | Истина — весь монитор; Ложь — полоса снизу |
| `BarHeightPct` | Число | 30 | Высота полосы снизу, % высоты экрана (15–60) |
| `FontScale` | Число | 100 | Масштаб шрифта, % (50–300) |
| `BackgroundColor` | Строка | 000000 | Цвет фона (RRGGBB) |
| `TextColor` | Строка | FFFFFF | Цвет текста (RRGGBB) |
| `AccentColor` | Строка | FFC000 | Цвет акцента/итога (RRGGBB) |
| `SoundEnabled` | Булево | Истина | Звуковые сигналы |
| `Columns` / `Rows` | Число | 40 / 10 | Объявляется платформе в опциях дисплея |

Не поддерживаются (возвращают ошибку + описание): `OutputHTML`,
`EquipmentAutoSetup`, локализация.

## Сборка

Требуются **VS 2022 Build Tools** с рабочей нагрузкой C++ (v143, Windows SDK 10).

```bat
build.cmd      :: x64 + x86 (Release, статический CRT /MT) -> bin\Release\
assemble.cmd   :: упаковка CustomerDisplayNative.zip (MANIFEST.xml + INFO.XML + DLL)
```

Проект также собирается CMake (см. `CMakeLists.txt`). Исходники — UTF-8,
компиляция с `/utf-8` (важно: кириллица в именах методов!).

## Установка в 1С

### Типовые конфигурации с БПО (Розница 2.3, УТ 11 и др.)

1. Администрирование → Подключаемое оборудование → **Драйверы оборудования** →
   создать/открыть драйвер → прикрепить `CustomerDisplayNative.zip` → Записать
2. Создать экземпляр оборудования «Дисплей покупателя», выбрать драйвер,
   заполнить параметры (номер монитора, режим), «Подключить»
3. В РМК дисплей получает строки чека автоматически

> После замены zip-файла **перезапустите сеанс 1С** — компонента кэшируется
> в памяти клиента.

### Прямое подключение из кода

```bsl
ПодключитьВнешнююКомпоненту("C:\path\CustomerDisplayNative.zip", "CDN", ТипВнешнейКомпоненты.Native);
Драйвер = Новый("AddIn.CDN.CustomerDisplay");   // или AddIn.CustomerDisplayNative

Идентификатор = "";
Параметры = "<Parameters>"
    + "<Parameter Name=""MonitorIndex"" Value=""2""/>"
    + "<Parameter Name=""Fullscreen"" Value=""false""/>"
    + "</Parameters>";
Драйвер.ConnectEquipment(Идентификатор, "CustomerDisplay", Параметры);

Драйвер.StringOutputOnCustomerDisplay(Идентификатор,
    "<Strings><String>Молоко 3,2% | 89,90</String>"
    + "<String>ИТОГО | 89,90</String></Strings>");
```

Пример полного теста — `tests/1c/test_1c_code.bsl`.

## Тесты

- `tests/smoke/` — консольный смоук-тест: загружает DLL напрямую, имитирует
  вызовы 1С, проверяет весь контракт и появление окна (`build_smoke.cmd`, `smoke.exe <dll>`)
- `tests/1c/` — тестовая внешняя обработка (XML-исходники) и скрипт пакетного прогона

## Структура

```
src/CustomerDisplay.h/.cpp   — компонента: контракт, устройства, журнал вызовов
src/DisplayWindow.h/.cpp     — поток окна, рендеринг GDI/GDI+, размещение
src/XmlUtil.h/.cpp           — мини-парсер XML (Parameters, Strings)
src/thirdparty/qrcodegen/    — QR-генератор (Project Nayuki, MIT)
include/                     — официальные заголовки 1С Native API (из шаблона)
build.cmd / assemble.cmd     — сборка и упаковка
MANIFEST.xml / INFO.XML      — дескрипторы компоненты для 1С
```

## Лицензия

GPL-3.0 — компонента основана на шаблоне
[innovait-rus/NativeAPI](https://github.com/innovait-rus/NativeAPI).
QR-генератор — [Nayuki/QR-Code-generator](https://github.com/Nayuki/QR-Code-generator) (MIT).
