#include "stdafx.h"
#include "DisplayWindow.h"
#include "XmlUtil.h"
#include "thirdparty/qrcodegen/qrcodegen.hpp"

#include <chrono>
#include <mutex>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

using namespace std::chrono;
using namespace Microsoft::WRL;

// экземпляр DLL — для определения папки ui/ (см. dllmain.cpp)
extern HMODULE g_hDllInstance;

namespace cdp {

namespace {

// ------------------------------------------------------------------ утилиты

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

const wchar_t* kWindowClass = L"CustomerDisplayNativeWnd";

constexpr UINT WM_APP_SETTINGS = WM_APP + 1;
constexpr UINT WM_APP_CONTENT  = WM_APP + 2;
constexpr UINT WM_APP_POS      = WM_APP + 3; // очередь JS-событий интерфейса
constexpr UINT TIMER_PAID_CLEAR = 2;          // очистка чека после «ВНЕСЕНО»

// Высоты шрифтов относительно высоты клиента (до масштаба)
struct LayoutMetrics
{
    int hWelcome  = 70;   // приветствие, ‰ высоты
    int hClock    = 280;  // часы
    int hDate     = 60;
    int hItem     = 80;   // строка товара
    int hTotal    = 200;  // итог
    int hStatus   = 160;  // статусы
    int hStatusSub = 70;
};

HFONT MakeFont(int heightPx, bool bold)
{
    return ::CreateFontW(-heightPx, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

// Очистка GDI+ объектов Image
struct ImageHolder
{
    ULONG_PTR token = 0;
    IStream* stream = nullptr;                 // должен жить, пока жив Image
    std::unique_ptr<Gdiplus::Image> image;

    void Reset()
    {
        image.reset();
        if (stream) { stream->Release(); stream = nullptr; }
    }

    bool LoadFromMemory(const std::vector<uint8_t>& bytes)
    {
        Reset();
        if (bytes.empty()) return false;
        HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!hGlobal) return false;
        void* p = ::GlobalLock(hGlobal);
        if (!p) { ::GlobalFree(hGlobal); return false; }
        memcpy(p, bytes.data(), bytes.size());
        ::GlobalUnlock(hGlobal);
        HRESULT hr = ::CreateStreamOnHGlobal(hGlobal, TRUE, &stream); // TRUE: освободит hGlobal
        if (FAILED(hr) || !stream) { ::GlobalFree(hGlobal); return false; }
        image.reset(new Gdiplus::Image(stream));
        return image->GetLastStatus() == Gdiplus::Ok && image->GetWidth() > 0;
    }

    bool LoadFromFile(const std::wstring& path)
    {
        Reset();
        if (path.empty()) return false;
        image.reset(new Gdiplus::Image(path.c_str()));
        return image->GetLastStatus() == Gdiplus::Ok && image->GetWidth() > 0;
    }
};

// Рисование QR-матрицы в прямоугольник (без тихой зоны — она добавляется отступом)
void DrawQrMatrix(Gdiplus::Graphics& g, const qrcodegen::QrCode& qr,
                  Gdiplus::REAL x, Gdiplus::REAL y, Gdiplus::REAL size)
{
    const int n = qr.getSize();
    const Gdiplus::REAL module = size / (Gdiplus::REAL)n;
    Gdiplus::SolidBrush blackBrush(Gdiplus::Color(255, 0, 0, 0));
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < n; ++col) {
            if (qr.getModule(col, row)) {
                Gdiplus::RectF rc(x + col * module, y + row * module,
                                  module + 0.5f, module + 0.5f); // +0.5: убрать швы
                g.FillRectangle(&blackBrush, rc);
            }
        }
    }
}

} // anonymous namespace

// ------------------------------------------------------------------ rows

void BuildRowsFromLines(DisplayContent& content, std::vector<std::wstring> lines)
{
    content.rows.clear();
    content.hasTotal = false;

    // пустые хвостовые строки убираем
    while (!lines.empty()) {
        std::wstring last = lines.back();
        size_t b = 0, e = last.size();
        while (b < e && iswspace(last[b])) ++b;
        while (e > b && iswspace(last[e - 1])) --e;
        if (b != e) break;
        lines.pop_back();
    }

    // Итоговой считаем последнюю строку, если в ней есть "итог"
    if (lines.size() >= 2) {
        std::wstring low = lines.back();
        for (auto& c : low) c = (wchar_t)::towlower(c);
        if (low.find(L"итог") != std::wstring::npos)
            content.hasTotal = true;
    }

    for (const std::wstring& line : lines) {
        std::wstring left = line, right;
        // колонки: "Наименование | Сумма" или "Наименование\tСумма"
        size_t sep = line.rfind(L'|');
        if (sep == std::wstring::npos)
            sep = line.rfind(L'\t');
        if (sep != std::wstring::npos) {
            left = line.substr(0, sep);
            right = line.substr(sep + 1);
            // трим
            size_t b = 0, e = left.size();
            while (b < e && iswspace(left[b])) ++b;
            while (e > b && iswspace(left[e - 1])) --e;
            left = left.substr(b, e - b);
            b = 0; e = right.size();
            while (b < e && iswspace(right[b])) ++b;
            while (e > b && iswspace(right[e - 1])) --e;
            right = right.substr(b, e - b);
        }
        content.rows.emplace_back(left, right);
    }
}

// ------------------------------------------------------------------ Impl

struct DisplayWindow::Impl
{
    CRITICAL_SECTION lock;
    DisplaySettings settings;
    DisplayContent pending;          // то, что положил внешний поток
    bool hasPending = false;
    HWND hwnd = nullptr;
    HANDLE thread = nullptr;
    HANDLE started = nullptr;
    std::wstring title;
    std::wstring lastError;

    // ---- доступ только из GUI-потока ----
    DisplayContent content;
    LayoutMetrics metrics;

    // двойная буферизация
    HDC memDc = nullptr;
    HBITMAP memBmp = nullptr;
    HBITMAP oldBmp = nullptr;
    int bmpW = 0, bmpH = 0;

    // тестовый режим
    DisplayContent savedBeforeTest;
    bool inTest = false;
    steady_clock::time_point testStart;
    int testStep = 0;

    // медиа
    ImageHolder media;
    std::wstring mediaLoadedFrom;

    // ---- WebView2 (создаётся в GUI-потоке; недоступен → GDI-фоллбэк) ----
    ComPtr<ICoreWebView2Environment> wvEnv;
    ComPtr<ICoreWebView2Controller> wvController;
    ComPtr<ICoreWebView2> wv;
    bool wvReady = false;
    bool gdiFallback = false;
    bool controllerRetried = false;
    EventRegistrationToken navToken = {};
    std::wstring uiFolder;      // <каталог DLL>\ui — https://app.customer/
    std::wstring mediaDir;      // медиафайлы устройства — https://app.media/
    std::wstring userDataDir;   // профиль WebView2 (temp, на устройство)
    std::vector<std::wstring> pendingJs; // события до готовности страницы

    Impl()
    {
        ::InitializeCriticalSectionAndSpinCount(&lock, 0x400);
    }
    ~Impl()
    {
        FreeBuffers();
        ::DeleteCriticalSection(&lock);
    }

    void FreeBuffers()
    {
        if (memDc) {
            if (oldBmp) { ::SelectObject(memDc, oldBmp); oldBmp = nullptr; }
            if (memBmp) { ::DeleteObject(memBmp); memBmp = nullptr; }
            ::DeleteDC(memDc);
            memDc = nullptr;
        }
        bmpW = bmpH = 0;
        media.Reset();
        mediaLoadedFrom.clear();
    }

    void EnsureBuffers(HDC windowDc, int w, int h)
    {
        if (memDc && w == bmpW && h == bmpH)
            return;
        FreeBuffers();
        memDc = ::CreateCompatibleDC(windowDc);
        memBmp = ::CreateCompatibleBitmap(windowDc, w, h);
        oldBmp = (HBITMAP)::SelectObject(memDc, memBmp);
        bmpW = w; bmpH = h;
    }

    int Scale(int perMille, int clientH) const
    {
        double v = (double)clientH * perMille / 1000.0 * settings.fontScalePct / 100.0;
        return std::max(8, (int)(v + 0.5));
    }
};

// ------------------------------------------------------------------ рендер

namespace {

using Impl = DisplayWindow::Impl; // вложенный тип: упрощаем подписи функций рендера

void DrawWelcome(HDC dc, Impl& impl, int w, int h)
{
    COLORREF text = impl.settings.textColor;
    COLORREF accent = impl.settings.accentColor;

    wchar_t timeBuf[16] = { 0 }, dateBuf[64] = { 0 };
    SYSTEMTIME st; ::GetLocalTime(&st);
    ::GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, timeBuf, 16);
    ::GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, dateBuf, 64);

    HFONT fTitle = MakeFont(impl.Scale(impl.metrics.hWelcome, h), false);
    HFONT fClock = MakeFont(impl.Scale(impl.metrics.hClock, h), true);
    HFONT fDate  = MakeFont(impl.Scale(impl.metrics.hDate, h), false);

    RECT rc = { 0, h / 12, w, h / 12 + impl.Scale(impl.metrics.hWelcome, h) + 20 };
    HFONT old = (HFONT)::SelectObject(dc, fTitle);
    ::SetTextColor(dc, accent);
    ::SetBkMode(dc, TRANSPARENT);
    ::DrawTextW(dc, L"Добро пожаловать!", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int clockH = impl.Scale(impl.metrics.hClock, h);
    rc = { 0, (h - clockH) / 2, w, (h - clockH) / 2 + clockH + 40 };
    ::SelectObject(dc, fClock);
    ::SetTextColor(dc, text);
    ::DrawTextW(dc, timeBuf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int dateH = impl.Scale(impl.metrics.hDate, h);
    rc = { 0, h * 3 / 4, w, h * 3 / 4 + dateH + 20 };
    ::SelectObject(dc, fDate);
    ::SetTextColor(dc, text);
    ::DrawTextW(dc, dateBuf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ::SelectObject(dc, old);
    ::DeleteObject(fTitle); ::DeleteObject(fClock); ::DeleteObject(fDate);
}

void DrawLines(HDC dc, Impl& impl, int w, int h)
{
    const auto& rows = impl.content.rows;
    if (rows.empty())
        return;

    COLORREF text = impl.settings.textColor;
    COLORREF accent = impl.settings.accentColor;
    ::SetBkMode(dc, TRANSPARENT);

    bool hasTotal = impl.content.hasTotal && rows.size() >= 2;
    size_t itemCount = hasTotal ? rows.size() - 1 : rows.size();
    if (itemCount == 0) { hasTotal = false; itemCount = rows.size(); }

    int totalH = hasTotal ? impl.Scale(impl.metrics.hTotal, h) : 0;
    int itemsTop = h / 16;
    int itemsAreaH = h - itemsTop - totalH - (hasTotal ? h / 20 : h / 16);
    if (itemsAreaH < 10) { itemsAreaH = 10; }

    // Строки товаров
    int rowH = itemsAreaH / (int)std::max<size_t>(itemCount, 1);
    int itemFontH = impl.Scale(impl.metrics.hItem, h);
    if (itemFontH > rowH - 6 && rowH > 24)
        itemFontH = rowH - 6;
    HFONT fItem = MakeFont(itemFontH, false);
    HFONT old = (HFONT)::SelectObject(dc, fItem);

    int y = itemsTop;
    int shown = 0;
    for (size_t i = 0; i < rows.size() && shown < (int)itemCount; ++i) {
        if (hasTotal && i == rows.size() - 1)
            break; // итог рисуем отдельно
        const auto& r = rows[i];
        RECT rl = { w / 40, y, (int)(w * 0.62), y + rowH };
        RECT rr = { (int)(w * 0.62), y, w - w / 40, y + rowH };
        ::SetTextColor(dc, text);
        ::DrawTextW(dc, r.first.c_str(), -1, &rl,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!r.second.empty())
            ::DrawTextW(dc, r.second.c_str(), -1, &rr,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += rowH;
        ++shown;
    }

    // Итог
    if (hasTotal) {
        int yTotal = h - totalH - h / 24;
        // разделитель
        HPEN pen = ::CreatePen(PS_SOLID, std::max(1, h / 300), RGB(
            GetRValue(text) / 3 + 40, GetGValue(text) / 3 + 40, GetBValue(text) / 3 + 40));
        HPEN oldPen = (HPEN)::SelectObject(dc, pen);
        ::MoveToEx(dc, w / 40, yTotal - h / 60, nullptr);
        ::LineTo(dc, w - w / 40, yTotal - h / 60);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
        // (перо возвращено в исходное состояние перед удалением)

        HFONT fTotal = MakeFont(impl.Scale(impl.metrics.hTotal, h), true);
        ::SelectObject(dc, fTotal);
        ::SetTextColor(dc, accent);
        const auto& total = rows.back();
        RECT rl = { w / 40, yTotal, (int)(w * 0.5), yTotal + totalH };
        RECT rr = { (int)(w * 0.5), yTotal, w - w / 40, yTotal + totalH };
        std::wstring label = total.first.empty() ? L"ИТОГО" : total.first;
        ::DrawTextW(dc, label.c_str(), -1, &rl,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::DrawTextW(dc, total.second.c_str(), -1, &rr,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(dc, old);
        ::DeleteObject(fTotal);
    }
    else {
        ::SelectObject(dc, old);
    }
    ::DeleteObject(fItem);
}

void DrawQr(HDC dc, Impl& impl, Gdiplus::Graphics& g, int w, int h)
{
    // Белая панель с QR
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::REAL panelW = (Gdiplus::REAL)std::min(w, h) * 0.78f;
    Gdiplus::REAL panelH = panelW;
    bool needValue = !impl.content.qrValue.empty();
    if (needValue)
        panelH += panelW * 0.22f;
    if (panelH > (Gdiplus::REAL)h * 0.92f) {
        panelH = (Gdiplus::REAL)h * 0.92f;
        panelW = needValue ? panelH / 1.22f : panelH;
    }
    Gdiplus::REAL px = ((Gdiplus::REAL)w - panelW) / 2.0f;
    Gdiplus::REAL py = ((Gdiplus::REAL)h - panelH) / 2.0f;
    g.FillRectangle(&white, px, py, panelW, panelH);

    Gdiplus::REAL qrSize = panelW * 0.94f;
    Gdiplus::REAL qx = px + (panelW - qrSize) / 2.0f;
    Gdiplus::REAL qy = py + panelW * 0.03f;

    bool drawn = false;
    if (!impl.content.qrPng.empty()) {
        // готовая картинка — масштабируем с сохранением пропорций
        ImageHolder holder;
        if (holder.LoadFromMemory(impl.content.qrPng)) {
            Gdiplus::REAL iw = (Gdiplus::REAL)holder.image->GetWidth();
            Gdiplus::REAL ih = (Gdiplus::REAL)holder.image->GetHeight();
            Gdiplus::REAL k = std::min(qrSize / iw, qrSize / ih);
            g.DrawImage(holder.image.get(), qx + (qrSize - iw * k) / 2.0f,
                        qy + (qrSize - ih * k) / 2.0f, iw * k, ih * k);
            drawn = true;
        }
    }
    if (!drawn && !impl.content.qrValue.empty()) {
        try {
            using namespace qrcodegen;
            QrCode qr = QrCode::encodeText(WideToUtf8(impl.content.qrValue).c_str(),
                                           QrCode::Ecc::MEDIUM);
            DrawQrMatrix(g, qr, qx, qy, qrSize);
            drawn = true;
        }
        catch (...) {
            drawn = false;
        }
    }

    if (needValue) {
        // значение под QR тёмным по белому
        int fontH = std::max(12, (int)(panelW * 0.05f));
        HFONT f = MakeFont(fontH, false);
        HFONT old = (HFONT)::SelectObject(dc, f);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(20, 20, 20));
        RECT rc = { (int)px + 8, (int)(py + panelW) + 4, (int)(px + panelW) - 8, (int)(py + panelH) - 4 };
        ::DrawTextW(dc, impl.content.qrValue.c_str(), -1, &rc,
                    DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        ::SelectObject(dc, old);
        ::DeleteObject(f);
    }
}

void DrawMedia(HDC dc, Impl& impl, Gdiplus::Graphics& g, int w, int h)
{
    if (impl.content.mediaPath != impl.mediaLoadedFrom) {
        impl.media.LoadFromFile(impl.content.mediaPath);
        impl.mediaLoadedFrom = impl.content.mediaPath;
    }
    if (!impl.media.image)
        return;
    Gdiplus::REAL iw = (Gdiplus::REAL)impl.media.image->GetWidth();
    Gdiplus::REAL ih = (Gdiplus::REAL)impl.media.image->GetHeight();
    Gdiplus::REAL k = std::min((Gdiplus::REAL)w / iw, (Gdiplus::REAL)h / ih);
    g.DrawImage(impl.media.image.get(), ((Gdiplus::REAL)w - iw * k) / 2.0f,
                ((Gdiplus::REAL)h - ih * k) / 2.0f, iw * k, ih * k);
}

void DrawBigStatus(HDC dc, Impl& impl, int w, int h,
                   const std::wstring& text, const std::wstring& symbol,
                   COLORREF bgColor, COLORREF fgColor)
{
    HBRUSH brush = ::CreateSolidBrush(bgColor);
    RECT rcFull = { 0, 0, w, h };
    ::FillRect(dc, &rcFull, brush);
    ::DeleteObject(brush);

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, fgColor);

    // символ
    int symH = impl.Scale(220, h);
    HFONT fSym = ::CreateFontW(-symH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Segoe UI Symbol");
    HFONT old = (HFONT)::SelectObject(dc, fSym);
    RECT rcSym = { 0, (h - symH) / 2 - symH / 8, w, (h - symH) / 2 + symH + 20 };
    ::DrawTextW(dc, symbol.c_str(), -1, &rcSym, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // текст
    int txtH = impl.Scale(impl.metrics.hStatus, h);
    HFONT fTxt = MakeFont(txtH, true);
    ::SelectObject(dc, fTxt);
    RECT rcTxt = { w / 20, (h + symH) / 2 + h / 40, w - w / 20,
                   (h + symH) / 2 + h / 40 + txtH + 60 };
    ::DrawTextW(dc, text.c_str(), -1, &rcTxt,
                DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    ::SelectObject(dc, old);
    ::DeleteObject(fSym);
    ::DeleteObject(fTxt);
}

void DrawOperationStatus(HDC dc, Impl& impl, int w, int h)
{
    switch (impl.content.statusCode) {
    case 1:
        DrawBigStatus(dc, impl, w, h, L"Операция выполняется…", L"●",
                      impl.settings.accentColor, impl.settings.bgColor);
        break;
    case 2:
        DrawBigStatus(dc, impl, w, h, L"Операция не выполнена", L"✖",
                      RGB(178, 34, 34), RGB(255, 255, 255));
        break;
    default:
        DrawBigStatus(dc, impl, w, h, L"Ожидание операции", L"●",
                      impl.settings.bgColor, impl.settings.textColor);
        break;
    }
}

void DrawWorkplaceStatus(HDC dc, Impl& impl, int w, int h)
{
    switch (impl.content.statusCode) {
    case 1:
        DrawBigStatus(dc, impl, w, h, L"Рабочее место готово", L"✔",
                      RGB(30, 132, 73), RGB(255, 255, 255));
        break;
    case 2:
        DrawBigStatus(dc, impl, w, h, L"Открытие смены", L"▶",
                      impl.settings.accentColor, impl.settings.bgColor);
        break;
    case 3:
        DrawBigStatus(dc, impl, w, h, L"Закрытие смены", L"■",
                      RGB(230, 126, 34), RGB(255, 255, 255));
        break;
    case 4:
        DrawBigStatus(dc, impl, w, h, L"Пауза", L"⏸",
                      RGB(241, 196, 15), RGB(40, 40, 40));
        break;
    default:
        DrawBigStatus(dc, impl, w, h, L"Рабочее место не активировано", L"○",
                      impl.settings.bgColor, impl.settings.textColor);
        break;
    }
}

void DrawTest(HDC dc, Impl& impl, Gdiplus::Graphics& g, int w, int h)
{
    // Демонстрация: шаг цикла выбирается по testStep (меняется таймером)
    switch (impl.testStep % 3) {
    case 0:
        {
            DisplayContent saved = impl.content;
            impl.content.rows = impl.content.testRows.empty()
                ? std::vector<std::pair<std::wstring, std::wstring>>{
                    { L"Молоко «Простоквашино» 3,2%", L"89,00" },
                    { L"Хлеб «Бородинский»", L"54,50" },
                    { L"ИТОГО", L"143,50" } }
                : impl.content.testRows;
            impl.content.hasTotal = true;
            DrawLines(dc, impl, w, h);
            impl.content = saved;
        }
        break;
    case 1:
        {
            std::wstring savedVal = impl.content.qrValue;
            impl.content.qrValue = L"Демонстрация QR-кода";
            DrawQr(dc, impl, g, w, h);
            impl.content.qrValue = savedVal;
        }
        break;
    default:
        {
            int savedCode = impl.content.statusCode;
            impl.content.statusCode = 1;
            DrawOperationStatus(dc, impl, w, h);
            impl.content.statusCode = savedCode;
        }
        break;
    }
}

void PaintContent(HDC dc, Impl& impl, int w, int h)
{
    // фон
    HBRUSH bg = ::CreateSolidBrush(impl.settings.bgColor);
    RECT rcFull = { 0, 0, w, h };
    ::FillRect(dc, &rcFull, bg);
    ::DeleteObject(bg);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    switch (impl.content.mode) {
    case ScreenMode::Welcome:          DrawWelcome(dc, impl, w, h); break;
    case ScreenMode::Lines:            DrawLines(dc, impl, w, h); break;
    case ScreenMode::QrCode:           DrawQr(dc, impl, g, w, h); break;
    case ScreenMode::Media:            DrawMedia(dc, impl, g, w, h); break;
    case ScreenMode::OperationStatus:  DrawOperationStatus(dc, impl, w, h); break;
    case ScreenMode::WorkplaceStatus:  DrawWorkplaceStatus(dc, impl, w, h); break;
    case ScreenMode::Test:             DrawTest(dc, impl, g, w, h); break;
    }
}

// ------------------------------------------------------------------ WebView2

std::wstring GetDllDirectory()
{
    wchar_t buf[MAX_PATH + 1] = { 0 };
    ::GetModuleFileNameW(g_hDllInstance, buf, MAX_PATH);
    std::wstring p(buf);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}

void WvLog(const std::wstring& msg)
{
    wchar_t tempPath[MAX_PATH + 1] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CustomerDisplayNative";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = ::CreateFileW((dir + L"\\webview.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t ts[48];
        ::swprintf(ts, 48, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::wstring full = std::wstring(ts) + msg + L"\r\n";
        DWORD written = 0;
        ::WriteFile(h, full.c_str(), (DWORD)(full.size() * sizeof(wchar_t)), &written, nullptr);
        ::CloseHandle(h);
    }
}

/** Рекурсивное создание каталога (родители тоже) */
bool EnsureDirTree(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring cur;
    size_t pos = 0;
    while (pos < path.size())
    {
        size_t next = path.find_first_of(L"\\/", pos + 1);
        if (next == std::wstring::npos) next = path.size();
        cur = path.substr(0, next);
        // сегмент-диск вида "C:" пропускаем (точная длина: полный путь "C:\..."
        // тоже начинается с "C:" — его создавать нельзя пропускать)
        if (cur.size() == 2 && cur[1] == L':') { pos = next; continue; }
        if (!cur.empty() && ::GetFileAttributesW(cur.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (!::CreateDirectoryW(cur.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        pos = next;
    }
    return true;
}

/** Распаковка ресурса DLL в файл */
bool ExtractUiResource(int resId, const std::wstring& destFile)
{
    HRSRC hrsc = ::FindResourceW(g_hDllInstance, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hrsc)
    {
        WvLog(L"  resource NOT FOUND id=" + std::to_wstring(resId) +
              L" err=" + std::to_wstring((unsigned)::GetLastError()));
        return false;
    }
    HGLOBAL h = ::LoadResource(g_hDllInstance, hrsc);
    if (!h) return false;
    const void* data = ::LockResource(h);
    const SIZE_T size = ::SizeofResource(g_hDllInstance, hrsc);
    if (!data || !size) return false;
    HANDLE f = ::CreateFileW(destFile.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        WvLog(L"  write FAILED " + destFile +
              L" err=" + std::to_wstring((unsigned)::GetLastError()));
        return false;
    }
    DWORD written = 0;
    ::WriteFile(f, data, (DWORD)size, &written, nullptr);
    ::CloseHandle(f);
    return written == size;
}

/** Распаковка интерфейса покупателя из ресурсов DLL во временную папку.
    Возвращает папку для виртуального хоста (пусто при неудаче).
    Ресурсы — единственный надёжный источник: 1С распаковывает zip компоненты
    без гарантии сохранения подпапок рядом с DLL. */
std::wstring ExtractUi(const std::wstring& baseDir)
{
    const std::wstring ui = baseDir + L"\\ui";
    const std::wstring assets = ui + L"\\assets";
    EnsureDirTree(assets.c_str() ? assets : ui); // создаёт и ui, и assets с родителями

    struct Item { int id; const wchar_t* name; };
    const Item items[] = {
        { 201, L"index.html" },   { 202, L"styles.css" }, { 203, L"app.js" },
        { 204, L"assets\\ad1.jpg" },      { 205, L"assets\\ad1_full.jpg" },
        { 206, L"assets\\ad2.jpg" },      { 207, L"assets\\ad2_full.jpg" },
    };
    bool allOk = true;
    for (const Item& it : items)
        if (!ExtractUiResource(it.id, ui + L"\\" + it.name))
        {
            WvLog(L"extract FAILED: " + std::wstring(it.name));
            allOk = false;
        }
    // индекс обязан быть
    if (!allOk)
    {
        DWORD attr = ::GetFileAttributesW((ui + L"\\index.html").c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            return std::wstring();
    }
    WvLog(L"ui extracted to " + ui);
    return ui;
}

void FlushPendingJs(Impl* impl)
{
    if (!impl->wv)
        return;
    std::vector<std::wstring> batch;
    ::EnterCriticalSection(&impl->lock);
    batch.swap(impl->pendingJs);
    ::LeaveCriticalSection(&impl->lock);
    for (const auto& json : batch)
    {
        HRESULT hr = impl->wv->PostWebMessageAsJson(json.c_str());
        wchar_t b[96];
        ::swprintf(b, 96, L"flush dispatch hr=0x%08X json=%.60s", (unsigned)hr, json.c_str());
        WvLog(b);
    }
}

/** Подключение готового контроллера: настройки, хосты, навигация (GUI-поток) */
HRESULT AttachWebview(Impl* impl, ICoreWebView2Controller* ctrl)
{
    impl->wvController = ctrl;
    WvLog(L"controller OK");
    ComPtr<ICoreWebView2> wv;
    if (FAILED(impl->wvController->get_CoreWebView2(&wv)) || !wv)
    {
        impl->gdiFallback = true;
        return E_FAIL;
    }
    impl->wv = wv;
    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(wv->get_Settings(&st)) && st)
    {
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_AreDevToolsEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        // AreBrowserAcceleratorKeysEnabled — ICoreWebView2Settings3
        ComPtr<ICoreWebView2Settings3> st3;
        if (SUCCEEDED(st->QueryInterface(IID_PPV_ARGS(&st3))) && st3)
            st3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    }
                            // маппинг виртуальных хостов — ICoreWebView2_3
                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv3))) && wv3)
                            {
                                HRESULT hm = wv3->SetVirtualHostNameToFolderMapping(
                                    L"app.customer", impl->uiFolder.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                WvLog(SUCCEEDED(hm) ? L"map app.customer OK"
                                                    : (L"map app.customer FAILED hr=0x" + [&] { wchar_t b[16]; ::swprintf(b, 16, L"%08X", (unsigned)hm); return std::wstring(b); }()));
                                if (!impl->mediaDir.empty())
                                {
                                    HRESULT hm2 = wv3->SetVirtualHostNameToFolderMapping(
                                        L"app.media", impl->mediaDir.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    WvLog(SUCCEEDED(hm2) ? L"map app.media OK"
                                                         : L"map app.media FAILED");
                                }
                            }
                            else
                            {
                                WvLog(L"ICoreWebView2_3 not available");
                            }
    RECT rc;
    ::GetClientRect(impl->hwnd, &rc);
    impl->wvController->put_Bounds(rc);
    // события, накопленные до загрузки страницы, уходят после неё
    impl->wv->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [impl](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                impl->wvReady = true;
                // диагностика: есть ли мост на странице
                impl->wv->ExecuteScript(
                    L"String(!!window.__pos)+'/'+String(!!(window.chrome&&window.chrome.webview))",
                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                        [impl](HRESULT hr, LPCWSTR res) -> HRESULT {
                            wchar_t b[128];
                            ::swprintf(b, 128, L"probe __pos/chrome.webview = %s (hr=0x%08X)",
                                       res ? res : L"?", (unsigned)hr);
                            WvLog(b);
                            return S_OK;
                        }).Get());
                WvLog(L"navigation completed — интерфейс покупателя активен");
                FlushPendingJs(impl);
                return S_OK;
            }).Get(),
        &impl->navToken);
    impl->wv->Navigate(L"https://app.customer/index.html?kiosk=1");
    WvLog(L"navigate -> app.customer/index.html?kiosk=1");
    return S_OK;
}

void StartWebView2(Impl* impl)
{
    WvLog(L"StartWebView2: begin");
    // рантайм WebView2 установлен?
    LPWSTR ver = nullptr;
    HRESULT hr = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    if (FAILED(hr) || !ver || !*ver)
    {
        if (ver) ::CoTaskMemFree(ver);
        impl->gdiFallback = true; // GDI-рендеринг остаётся
        WvLog(L"StartWebView2: runtime not available");
        return;
    }
    std::wstring verStr = ver;
    ::CoTaskMemFree(ver);
    WvLog(L"StartWebView2: runtime " + verStr);

    // интерфейс покупателя: распаковка из ресурсов DLL (надёжнее файлов рядом с DLL)
    impl->uiFolder = ExtractUi(impl->userDataDir);
    if (impl->uiFolder.empty())
    {
        impl->gdiFallback = true;
        WvLog(L"StartWebView2: ui extract FAILED — GDI fallback");
        return;
    }
    DWORD uiAttr = ::GetFileAttributesW(impl->uiFolder.c_str());
    if (uiAttr == INVALID_FILE_ATTRIBUTES)
    {
        impl->gdiFallback = true;
        WvLog(L"StartWebView2: ui folder missing — GDI fallback");
        return;
    }

    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, impl->userDataDir.c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [impl](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env)
                {
                    wchar_t b[64];
                    ::swprintf(b, 64, L"env FAILED hr=0x%08X", (unsigned)result);
                    WvLog(b);
                    impl->gdiFallback = true;
                    return E_FAIL;
                }
                impl->wvEnv = env;
                WvLog(L"env OK");
                return impl->wvEnv->CreateCoreWebView2Controller(
                    impl->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [impl](HRESULT result2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            // при одновременном старте нескольких устройств создание
                            // контроллера может прерваться (E_ABORT) — повторяем один раз
                            if (result2 == E_ABORT && !ctrl && impl->wvEnv && !impl->controllerRetried)
                            {
                                impl->controllerRetried = true;
                                WvLog(L"controller aborted — retry");
                                ::Sleep(300);
                                return impl->wvEnv->CreateCoreWebView2Controller(
                                    impl->hwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [impl](HRESULT result3, ICoreWebView2Controller* ctrl2) -> HRESULT {
                                            if (FAILED(result3) || !ctrl2)
                                            {
                                                wchar_t b[64];
                                                ::swprintf(b, 64, L"controller retry FAILED hr=0x%08X", (unsigned)result3);
                                                WvLog(b);
                                                impl->gdiFallback = true;
                                                return E_FAIL;
                                            }
                                            return AttachWebview(impl, ctrl2);
                                        }).Get());
                            }
                            if (FAILED(result2) || !ctrl)
                            {
                                wchar_t b[64];
                                ::swprintf(b, 64, L"controller FAILED hr=0x%08X", (unsigned)result2);
                                WvLog(b);
                                impl->gdiFallback = true;
                                return E_FAIL;
                            }
                            return AttachWebview(impl, ctrl);
                        }).Get());
            }).Get());
    if (FAILED(hr))
    {
        wchar_t b[64];
        ::swprintf(b, 64, L"CreateEnvironment call FAILED hr=0x%08X", (unsigned)hr);
        WvLog(b);
        impl->gdiFallback = true;
    }
}

// ------------------------------------------------------------------ мониторы

struct MonitorRect { RECT rc; bool primary; };

std::vector<MonitorRect> EnumerateMonitors()
{
    std::vector<MonitorRect> result;
    // EnumDisplayMonitors корректно отдаёт прямоугольники каждого монитора
    struct Ctx { std::vector<MonitorRect>* out; } ctx{ &result };
    ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* out = ((Ctx*)lp)->out;
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(hMon, &mi)) {
            out->push_back({ mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        }
        return TRUE;
    }, (LPARAM)&ctx);
    // сортировка: основной первый, далее слева направо (и сверху вниз)
    std::sort(result.begin(), result.end(), [](const MonitorRect& a, const MonitorRect& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
        return a.rc.top < b.rc.top;
    });
    return result;
}

RECT MonitorRectByIndex(int index1based)
{
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) {
        return { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    }
    int idx = index1based - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)monitors.size()) idx = (int)monitors.size() - 1;
    return monitors[idx].rc;
}

// Вычисление прямоугольника окна по режиму: весь экран или полоса снизу
void ComputeWindowPlacement(const DisplaySettings& settings, const RECT& rcMon,
                            int& x, int& y, int& w, int& h)
{
    w = rcMon.right - rcMon.left;
    h = rcMon.bottom - rcMon.top;
    if (settings.fullscreen) {
        x = rcMon.left;
        y = rcMon.top;
    }
    else {
        // полоса-«панель» у нижнего края экрана, во всю ширину
        int barPct = std::min(60, std::max(15, settings.barHeightPct));
        h = h * barPct / 100;
        x = rcMon.left;
        y = rcMon.bottom - h;
    }
}

// ------------------------------------------------------------------ окно

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Impl* impl = (Impl*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        {
            auto* cs = (CREATESTRUCTW*)lParam;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            impl = (Impl*)cs->lpCreateParams;
            ::SetTimer(hwnd, 1, 1000, nullptr);   // часы
            ::SetTimer(hwnd, 2, 1500, nullptr);   // цикл демонстрации
        }
        return 0;

    case WM_APP_SETTINGS:
        {
            // перепозиционирование (монитор мог измениться); без topmost
            RECT rcMon = MonitorRectByIndex(impl->settings.monitorIndex);
            int x, y, w, h;
            ComputeWindowPlacement(impl->settings, rcMon, x, y, w, h);
            ::SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_APP_CONTENT:
        {
            // предыдущее содержимое сохраняем ДО перезаписи (нужно для Test)
            DisplayContent previous = impl->content;

            ::EnterCriticalSection(&impl->lock);
            impl->content = impl->pending;
            impl->hasPending = false;
            ::LeaveCriticalSection(&impl->lock);

            if (impl->content.mode == ScreenMode::Test) {
                impl->savedBeforeTest = previous;
                impl->inTest = true;
                impl->testStart = steady_clock::now();
                impl->testStep = 0;
            }
            else if (impl->content.mode == ScreenMode::Media) {
                // медиа загрузится лениво при первой отрисовке
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wParam == 1) {
            // content живёт только в GUI-потоке, блокировка не нужна
            if (!impl->inTest && impl->content.mode == ScreenMode::Welcome)
                ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == 2 && impl->inTest) {
            auto elapsed = duration_cast<seconds>(steady_clock::now() - impl->testStart).count();
            if (elapsed >= 6) {
                // завершение демонстрации — вернуть прежний экран
                impl->inTest = false;
                ::EnterCriticalSection(&impl->lock);
                impl->pending = impl->savedBeforeTest;
                impl->pending.mode = (impl->pending.mode == ScreenMode::Test)
                                     ? ScreenMode::Welcome : impl->pending.mode;
                impl->hasPending = true;
                ::LeaveCriticalSection(&impl->lock);
                ::PostMessageW(hwnd, WM_APP_CONTENT, 0, 0);
            }
            else {
                impl->testStep++;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else if (wParam == TIMER_PAID_CLEAR)
        {
            // «ВНЕСЕНО» — наличная оплата: чистим чек (Win32-таймер работает
            // даже при троттлинге таймеров WebView2/RDP)
            ::KillTimer(hwnd, TIMER_PAID_CLEAR);
            if (impl->wvReady && impl->wv)
            {
                impl->wv->PostWebMessageAsJson(L"{\"type\":\"check/clear\"}");
                WvLog(L"paid timer: check/clear dispatched");
            }
        }
        return 0;

    case WM_PAINT:
        {
            if (impl->wvReady)
                break; // WebView2 отрисовал интерфейс сам
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc; ::GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left; if (w < 1) w = 1;
            int h = rc.bottom - rc.top; if (h < 1) h = 1;

            impl->EnsureBuffers(dc, w, h);
            PaintContent(impl->memDc, *impl, w, h);
            ::BitBlt(dc, 0, 0, w, h, impl->memDc, 0, 0, SRCCOPY);
            ::EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_SIZE:
        if (impl->wvController)
        {
            RECT rc; ::GetClientRect(hwnd, &rc);
            impl->wvController->put_Bounds(rc);
        }
        break;

    case WM_APP_POS:
        if (impl->wvReady && impl->wv)
        {
            std::vector<std::wstring> batch;
            ::EnterCriticalSection(&impl->lock);
            batch.swap(impl->pendingJs);
            ::LeaveCriticalSection(&impl->lock);
            for (const auto& json : batch)
            {
                HRESULT hr = impl->wv->PostWebMessageAsJson(json.c_str());
                wchar_t b[128];
                ::swprintf(b, 128, L"dispatch hr=0x%08X json=%.80s", (unsigned)hr, json.c_str());
                WvLog(b);
                // «ВНЕСЕНО» — наличная оплата: через 5 с очищаем чек.
                // Таймер Win32 (не JS) — работает даже при троттлинге WebView2
                if (json.find(L"op/paid") != std::wstring::npos)
                    ::SetTimer(hwnd, TIMER_PAID_CLEAR, 5000, nullptr);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DISPLAYCHANGE:
        ::PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0);
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, 1);
        ::KillTimer(hwnd, 2);
        ::KillTimer(hwnd, TIMER_PAID_CLEAR);
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

std::once_flag g_classOnce;
ATOM RegisterWindowClassOnce()
{
    std::call_once(g_classOnce, []() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = nullptr;                 // скрыть курсор над окном
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClass;
        ::RegisterClassExW(&wc);
    });
    return 0;
}

unsigned __stdcall DisplayThreadProc(void* p)
{
    auto* impl = (DisplayWindow::Impl*)p;

    // COM (STA) обязателен для WebView2
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartupInput gdiInput;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok)
        gdiToken = 0;

    RegisterWindowClassOnce();

    RECT rcMon = MonitorRectByIndex(impl->settings.monitorIndex);
    int x, y, w, h;
    ComputeWindowPlacement(impl->settings, rcMon, x, y, w, h);
    DWORD style = WS_POPUP;
    // обычное окно: без topmost — Alt+Tab и переключение на 1С работают как
    // у любой программы; окно доступно из панели задач и Alt+Tab
    DWORD exStyle = 0;

    HWND hwnd = ::CreateWindowExW(exStyle, kWindowClass, impl->title.c_str(), style,
                                  x, y, w, h, nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), impl);
    if (!hwnd) {
        ::SetEvent(impl->started);
        if (gdiToken) Gdiplus::GdiplusShutdown(gdiToken);
        return 1;
    }

    impl->hwnd = hwnd;
    ::SetEvent(impl->started);
    ::PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0);

    // интерфейс покупателя (WebView2); при отсутствии рантайма — GDI-фоллбэк
    StartWebView2(impl);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // COM-объекты WebView2 освобождаем в GUI-потоке
    if (impl->wvController)
        impl->wvController->Close();
    impl->wvController.Reset();
    impl->wv.Reset();
    impl->wvEnv.Reset();
    impl->wvReady = false;

    impl->hwnd = nullptr;
    impl->FreeBuffers();
    if (gdiToken) Gdiplus::GdiplusShutdown(gdiToken);
    ::CoUninitialize();
    return 0;
}

} // anonymous namespace

// ------------------------------------------------------------------ DisplayWindow

DisplayWindow::DisplayWindow()
    : m_impl(new Impl)
{
}

DisplayWindow::~DisplayWindow()
{
    Stop();
    delete m_impl;
}

bool DisplayWindow::Start(const DisplaySettings& settings, const std::wstring& deviceTitle,
                          const std::wstring& mediaDir)
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    if (impl->thread) {
        ::LeaveCriticalSection(&impl->lock);
        return true; // уже запущено
    }
    impl->settings = settings;
    impl->title = deviceTitle.empty() ? L"Дисплей покупателя" : deviceTitle;
    impl->mediaDir = mediaDir;
    impl->uiFolder = GetDllDirectory() + L"\\ui";
    wchar_t tempPath[MAX_PATH + 1] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);
    static std::atomic<int> wvSeq(0);
    impl->userDataDir = std::wstring(tempPath) + L"CustomerDisplayNative\\webview\\w" +
                        std::to_wstring(++wvSeq);
    impl->wvReady = false;
    impl->gdiFallback = false;
    impl->controllerRetried = false;
    impl->lastError.clear();
    ::LeaveCriticalSection(&impl->lock);

    impl->started = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->started) {
        impl->lastError = L"Не удалось создать событие запуска";
        return false;
    }

    unsigned threadId = 0;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, DisplayThreadProc, impl, 0, &threadId);
    if (!h) {
        ::CloseHandle(impl->started);
        impl->started = nullptr;
        impl->lastError = L"Не удалось запустить поток отображения";
        return false;
    }
    impl->thread = h;

    // ждём создания окна
    ::WaitForSingleObject(impl->started, 7000);

    if (!impl->hwnd) {
        // поток может ещё жить (завис на создании) — сигнализируем ошибку
        if (::WaitForSingleObject(h, 2000) != WAIT_OBJECT_0) {
            impl->lastError = L"Окно дисплея не создано (поток не ответил)";
        }
        ::CloseHandle(h);
        if (impl->thread == h) impl->thread = nullptr;
        ::CloseHandle(impl->started);
        impl->started = nullptr;
        if (impl->lastError.empty())
            impl->lastError = L"Окно дисплея не создано";
        return false;
    }

    ::CloseHandle(impl->started);
    impl->started = nullptr;
    return true;
}

void DisplayWindow::Stop()
{
    Impl* impl = m_impl;
    HANDLE h = nullptr;
    ::EnterCriticalSection(&impl->lock);
    h = impl->thread;
    impl->thread = nullptr;
    ::LeaveCriticalSection(&impl->lock);

    if (!h)
        return;

    if (impl->hwnd)
        ::PostMessageW(impl->hwnd, WM_CLOSE, 0, 0);

    if (::WaitForSingleObject(h, 5000) != WAIT_OBJECT_0) {
        // крайний случай — не ждём вечно
        ::TerminateThread(h, 1);
    }
    ::CloseHandle(h);

    if (impl->started) { ::CloseHandle(impl->started); impl->started = nullptr; }
}

bool DisplayWindow::IsRunning() const
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    bool running = impl->thread != nullptr && impl->hwnd != nullptr;
    ::LeaveCriticalSection(&impl->lock);
    return running;
}

bool DisplayWindow::ApplySettings(const DisplaySettings& settings)
{
    Impl* impl = m_impl;
    if (!IsRunning())
        return false;
    ::EnterCriticalSection(&impl->lock);
    impl->settings = settings;
    ::LeaveCriticalSection(&impl->lock);
    ::PostMessageW(impl->hwnd, WM_APP_SETTINGS, 0, 0);
    return true;
}

void DisplayWindow::SetContent(const DisplayContent& content)
{
    Impl* impl = m_impl;
    if (!IsRunning())
        return;
    ::EnterCriticalSection(&impl->lock);
    impl->pending = content;
    impl->hasPending = true;
    ::LeaveCriticalSection(&impl->lock);
    ::PostMessageW(impl->hwnd, WM_APP_CONTENT, 0, 0);
}

bool DisplayWindow::IsWindowVisibleNow() const
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    HWND hwnd = impl->hwnd;
    ::LeaveCriticalSection(&impl->lock);
    return hwnd && ::IsWindow(hwnd) && ::IsWindowVisible(hwnd);
}

const std::wstring& DisplayWindow::LastError() const
{
    return m_impl->lastError;
}

void DisplayWindow::PostPosEvent(const std::wstring& jsonEvent)
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    if (impl->gdiFallback) { // GDI-режим: интерфейсных событий нет
        ::LeaveCriticalSection(&impl->lock);
        return;
    }
    impl->pendingJs.push_back(jsonEvent);
    HWND hwnd = impl->hwnd;
    ::LeaveCriticalSection(&impl->lock);
    if (hwnd)
        ::PostMessageW(hwnd, WM_APP_POS, 0, 0);
}

void DisplayWindow::PostPosEvents(const std::vector<std::wstring>& jsonEvents)
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    if (impl->gdiFallback) {
        ::LeaveCriticalSection(&impl->lock);
        return;
    }
    for (const auto& j : jsonEvents)
        impl->pendingJs.push_back(j);
    HWND hwnd = impl->hwnd;
    ::LeaveCriticalSection(&impl->lock);
    if (hwnd)
        ::PostMessageW(hwnd, WM_APP_POS, 0, 0);
}

bool DisplayWindow::IsWebviewActive() const
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    bool active = impl->wvReady && !impl->gdiFallback;
    ::LeaveCriticalSection(&impl->lock);
    return active;
}

std::wstring DisplayWindow::DescribeState() const
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    HWND hwnd = impl->hwnd;
    ::LeaveCriticalSection(&impl->lock);
    if (!hwnd || !::IsWindow(hwnd))
        return L"нет окна";
    RECT rc;
    ::GetWindowRect(hwnd, &rc);
    wchar_t buf[128];
    ::swprintf(buf, 128, L"visible=%d rect=[%ld,%ld - %ld,%ld] monitor=%d",
               ::IsWindowVisible(hwnd) ? 1 : 0,
               (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom,
               impl->settings.monitorIndex);
    return buf;
}

} // namespace cdp
