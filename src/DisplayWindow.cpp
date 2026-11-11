#include "stdafx.h"
#include "DisplayWindow.h"
#include "XmlUtil.h"
#include "thirdparty/qrcodegen/qrcodegen.hpp"

#include <chrono>
#include <mutex>

using namespace std::chrono;

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
        return 0;

    case WM_PAINT:
        {
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

    case WM_ERASEBKGND:
        return 1;

    case WM_DISPLAYCHANGE:
        ::PostMessageW(hwnd, WM_APP_SETTINGS, 0, 0);
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, 1);
        ::KillTimer(hwnd, 2);
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

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    impl->hwnd = nullptr;
    impl->FreeBuffers();
    if (gdiToken) Gdiplus::GdiplusShutdown(gdiToken);
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

bool DisplayWindow::Start(const DisplaySettings& settings, const std::wstring& deviceTitle)
{
    Impl* impl = m_impl;
    ::EnterCriticalSection(&impl->lock);
    if (impl->thread) {
        ::LeaveCriticalSection(&impl->lock);
        return true; // уже запущено
    }
    impl->settings = settings;
    impl->title = deviceTitle.empty() ? L"Дисплей покупателя" : deviceTitle;
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
