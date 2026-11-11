// DisplayWindow.h — полноэкранное окно дисплея покупателя.
// На каждое подключённое устройство (DeviceID) создаётся отдельный поток
// с окном на выбранном мониторе. Вызовы компоненты меняют состояние под
// критической секцией и уведомляют окно через PostMessage.
#pragma once

#include "stdafx.h"

namespace cdp {

enum class ScreenMode
{
    Welcome,            // заставка рабочего места
    Lines,              // список строк (товары + итог)
    QrCode,             // QR-код (готовая картинка PNG или значение)
    Media,              // медиафайл (картинка/реклама)
    OperationStatus,    // статус операции (0..2)
    WorkplaceStatus,    // статус рабочего места (0..4)
    Test                // демонстрация (циклически несколько экранов)
};

struct DisplaySettings
{
    int      monitorIndex = 2;     // 1-based; 1 — основной монитор
    bool     fullscreen   = true;  // true — весь экран; false — полоса снизу (как панель)
    int      barHeightPct = 30;    // высота полосы снизу, % высоты экрана (15..60)
    int      fontScalePct = 100;   // масштаб шрифта, 50..300
    COLORREF bgColor      = RGB(0, 0, 0);
    COLORREF textColor    = RGB(255, 255, 255);
    COLORREF accentColor  = RGB(255, 192, 0);
    bool     soundEnabled = true;
};

struct DisplayContent
{
    ScreenMode mode = ScreenMode::Welcome;

    // Lines: пары (левая часть, правая часть); hasTotal — последняя строка итог
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    bool hasTotal = false;

    // QrCode
    std::vector<uint8_t> qrPng;     // готовая картинка PNG (если передана)
    std::wstring qrValue;           // значение QR (для генерации/подписи)

    // Media
    std::wstring mediaPath;         // путь к файлу картинки

    // Статусы
    int statusCode = 0;

    // Test
    std::vector<std::pair<std::wstring, std::wstring>> testRows;
};

// Готовит DisplayContent::rows из набора строк: строки с '|' или '\t'
// разбиваются на левую/правую части, последняя строка помечается как итог.
void BuildRowsFromLines(DisplayContent& content, std::vector<std::wstring> lines);

// Окно одного устройства. Потокобезопасно: методы можно звать из любого потока.
class DisplayWindow
{
public:
    struct Impl; // реализация (private-данные, но тип доступен функциям рендера)

    DisplayWindow();
    ~DisplayWindow();

    // Запуск GUI-потока и создание окна. false — не удалось (GetLastErrorText).
    // mediaDir — папка медиафайлов устройства (маппится в WebView2 как https://app.media/)
    bool Start(const DisplaySettings& settings, const std::wstring& deviceTitle,
               const std::wstring& mediaDir = L"");

    // Остановка потока и уничтожение окна.
    void Stop();

    // Живое ли окно
    bool IsRunning() const;

    // Обновить настройки (положение/оформление) — окно перепозиционируется.
    bool ApplySettings(const DisplaySettings& settings);

    // Установить содержимое экрана и перерисовать (GDI-фоллбэк).
    void SetContent(const DisplayContent& content);

    // Событие интерфейса покупателя — готовый JS-вызов dispatch(...)
    // (потокобезопасно; в GDI-фоллбэке игнорируется)
    void PostPosEvent(const std::wstring& jsDispatchCall);

    // Активен ли WebView2 (иначе — GDI-фоллбэк)
    bool IsWebviewActive() const;

    // Проверка видимости (для смоук-тестов)
    bool IsWindowVisibleNow() const;

    // Состояние для журнала: "visible=1 rect=[...]"; "нет окна" если не запущено
    std::wstring DescribeState() const;

    // Текст последней ошибки запуска
    const std::wstring& LastError() const;

private:
    Impl* m_impl;

    DisplayWindow(const DisplayWindow&) = delete;
    DisplayWindow& operator=(const DisplayWindow&) = delete;
};

} // namespace cdp
