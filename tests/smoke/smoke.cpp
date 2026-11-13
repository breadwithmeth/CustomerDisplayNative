// smoke.cpp — консольный смоук-тест драйвера CustomerDisplayNative.
// Загружает DLL напрямую (LoadLibrary), имитирует вызовы 1С и проверяет,
// что контракт методов работает и окно дисплея появляется.
//
// Запуск: smoke.exe <путь к CustomerDisplayNative_64.dll>
#include "stdafx.h"
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include <stdio.h>
#include <string>
#include <vector>

// ---- фейковый менеджер памяти (как 1С) ----
class FakeMemoryManager : public IMemoryManager
{
public:
    virtual bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte)
    {
        *pMemory = ::malloc(ulCountByte);
        return *pMemory != nullptr;
    }
    virtual void ADDIN_API FreeMemory(void** pMemory)
    {
        if (pMemory && *pMemory) { ::free(*pMemory); *pMemory = nullptr; }
    }
};

// ---- экспортируемые типы DLL ----
typedef long (*TGetClassObject)(const WCHAR_T*, IComponentBase**);
typedef AppCapabilities (*TSetPlatformCapabilities)(AppCapabilities);
typedef long (*TDestroyObject)(IComponentBase**);
typedef const WCHAR_T* (*TGetClassNames)();

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { if (cond) { printf("  [OK]   %s\n", msg); } \
         else { printf("  [FAIL] %s\n", msg); ++g_failures; } } while (0)

static std::wstring OutStr(tVariant* v)
{
    if (!v || TV_VT(v) != VTYPE_PWSTR || !TV_WSTR(v))
        return L"<empty>";
    return std::wstring((wchar_t*)TV_WSTR(v), v->wstrLen);
}

// ВНИМАНИЕ: строки должны жить дольше вызова — передаём литералы/долгоживущие буферы
static void SetInStr(tVariant* v, const wchar_t* s)
{
    TV_VT(v) = VTYPE_PWSTR;
    TV_WSTR(v) = (WCHAR_T*)s;
    v->wstrLen = (uint32_t)::wcslen(s);
}

static void SetInLong(tVariant* v, long val)
{
    TV_VT(v) = VTYPE_I4;
    TV_I4(v) = val;
}

static void PrintLast(IComponentBase* comp)
{
    tVariant errOut; memset(&errOut, 0, sizeof(errOut));
    tVariant errRet; memset(&errRet, 0, sizeof(errRet));
    comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"GetLastError"), &errRet, &errOut, 1);
    std::wstring text = OutStr(&errOut);
    if (!text.empty())
    {
        char buf[1024] = { 0 };
        ::WideCharToMultiByte(CP_OEMCP, 0, text.c_str(), -1, buf, sizeof(buf) - 1, "?", nullptr);
        printf("         ошибка(%ld): %s\n", TV_I4(&errRet), buf);
    }
}

static void PrintXml(const std::wstring& xml, size_t maxLen = 220)
{
    std::wstring s = xml;
    if (s.size() > maxLen) s = s.substr(0, maxLen) + L"...";
    // переводим в консольную кодировку
    char buf[1024] = { 0 };
    ::WideCharToMultiByte(CP_OEMCP, 0, s.c_str(), -1, buf, sizeof(buf) - 1, "?", nullptr);
    printf("         %s\n", buf);
}

// Поиск окна дисплея ТОЛЬКО текущего процесса (в системе может быть
// несколько экземпляров — например, окно из сеанса 1С пользователя)
static HWND FindOwnDisplayWindow()
{
    struct Ctx { DWORD pid; HWND hwnd; } ctx;
    ctx.pid = ::GetCurrentProcessId();
    ctx.hwnd = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64] = { 0 };
        ::GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"CustomerDisplayNativeWnd") == 0)
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid) { c->hwnd = h; return FALSE; }
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.hwnd;
}

// главное окно дисплея нашего процесса (точный заголовок — окно демо
// «Дисплей покупателя — тест» не считается)
static HWND FindOwnMainWindow()
{
    struct Ctx { DWORD pid; HWND hwnd; } ctx;
    ctx.pid = ::GetCurrentProcessId();
    ctx.hwnd = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64] = { 0 };
        ::GetClassNameW(h, cls, 64);
        wchar_t title[128] = { 0 };
        if (wcscmp(cls, L"CustomerDisplayNativeWnd") == 0)
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid)
            {
                ::GetWindowTextW(h, title, 128);
                if (wcscmp(title, L"Дисплей покупателя") == 0)
                {
                    c->hwnd = h;
                    return FALSE;
                }
            }
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.hwnd;
}

// есть ли живое окно дисплея другого процесса (например, 1С)?
static bool ForeignDisplayWindowExists()
{
    struct Ctx { DWORD pid; bool found; } ctx;
    ctx.pid = ::GetCurrentProcessId();
    ctx.found = false;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64] = { 0 };
        ::GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"CustomerDisplayNativeWnd") == 0)
        {
            wchar_t title[128] = { 0 };
            ::GetWindowTextW(h, title, 128);
            if (wcscmp(title, L"Дисплей покупателя") == 0)
            {
                DWORD pid = 0;
                ::GetWindowThreadProcessId(h, &pid);
                if (pid != c->pid) { c->found = true; return FALSE; }
            }
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

// ---- webview.log (UTF-16LE, %TEMP%\CustomerDisplayNative) ----
static std::wstring WebviewLogPath()
{
    wchar_t tempPath[MAX_PATH + 1] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"CustomerDisplayNative\\webview.log";
}

static size_t WebviewLogSize()
{
    HANDLE h = ::CreateFileW(WebviewLogPath().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER size = { 0 };
    ::GetFileSizeEx(h, &size);
    ::CloseHandle(h);
    return (size_t)size.QuadPart;
}

// подстрока в части webview.log, записанной после fromByte
static bool WebviewLogContainsFrom(size_t fromByte, const wchar_t* needle)
{
    HANDLE h = ::CreateFileW(WebviewLogPath().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = { 0 };
    ::GetFileSizeEx(h, &size);
    if ((size_t)size.QuadPart <= fromByte)
    {
        ::CloseHandle(h);
        return false;
    }
    LARGE_INTEGER start = { 0 };
    start.QuadPart = (LONGLONG)fromByte;
    ::SetFilePointerEx(h, start, nullptr, FILE_BEGIN);
    std::vector<char> buf((size_t)size.QuadPart - fromByte + 2, 0);
    DWORD read = 0;
    ::ReadFile(h, buf.data(), (DWORD)(buf.size() - 2), &read, nullptr);
    ::CloseHandle(h);
    std::wstring w;
    w.reserve(read / 2);
    for (DWORD i = 0; i + 1 < read; i += 2)
        w += (wchar_t)((unsigned char)buf[i] | ((unsigned char)buf[i + 1] << 8));
    return w.find(needle) != std::wstring::npos;
}

// ждём появления подстроки в хвосте webview.log (страница WebView2 грузится не мгновенно)
static bool WaitWebviewLogFrom(size_t fromByte, const wchar_t* needle, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 250)
    {
        if (WebviewLogContainsFrom(fromByte, needle))
            return true;
        ::Sleep(250);
    }
    return false;
}

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1) ? argv[1] : "CustomerDisplayNative_64.dll";

    printf("Смоук-тест CustomerDisplayNative: %s\n\n", dllPath);

    // на машине может быть открыто живое окно дисплея (1С) — разрешаем тесту
    // своё окно; логику «одно окно на кассу» проверяем отдельно в конце
    ::SetEnvironmentVariableW(L"CDN_ALLOW_SECOND_WINDOW", L"1");

    HMODULE dll = ::LoadLibraryA(dllPath);
    CHECK(dll != nullptr, "LoadLibrary");
    if (!dll)
    {
        printf("GetLastError=%u\n", ::GetLastError());
        return 2;
    }

    TGetClassObject getClassObject = (TGetClassObject)::GetProcAddress(dll, "GetClassObject");
    TSetPlatformCapabilities setCaps = (TSetPlatformCapabilities)::GetProcAddress(dll, "SetPlatformCapabilities");
    TDestroyObject destroyObject = (TDestroyObject)::GetProcAddress(dll, "DestroyObject");
    TGetClassNames getClassNames = (TGetClassNames)::GetProcAddress(dll, "GetClassNames");
    CHECK(getClassObject && setCaps && destroyObject && getClassNames, "экспорты GetClassObject/SetPlatformCapabilities/DestroyObject/GetClassNames");

    setCaps(eAppCapabilities1);
    std::wstring classNames((wchar_t*)getClassNames());
    printf("  Класс компоненты: %ls\n", classNames.c_str());
    CHECK(_wcsicmp(classNames.c_str(), L"CustomerDisplay") == 0, "имя класса = CustomerDisplay");

    IComponentBase* comp = nullptr;
    getClassObject((const WCHAR_T*)L"CustomerDisplay", &comp);
    CHECK(comp != nullptr, "GetClassObject создал компоненту");

    FakeMemoryManager memMgr;
    CHECK(comp->setMemManager(&memMgr), "setMemManager");
    CHECK(comp->Init(nullptr) == true, "Init(nullptr) tolerated");
    CHECK(comp->GetInfo() == 2005, "GetInfo == 2005 (как в рабочих драйверах InnovaIT)");

    // Регистрация имён методов (RU и EN)
    CHECK(comp->FindMethod((const WCHAR_T*)L"ConnectEquipment") >= 0, "FindMethod(ConnectEquipment)");
    CHECK(comp->FindMethod((const WCHAR_T*)L"ПодключитьОборудование") >= 0, "FindMethod(ПодключитьОборудование)");
    CHECK(comp->FindMethod((const WCHAR_T*)L"StringOutputOnCustomerDisplay") >= 0, "FindMethod(StringOutputOnCustomerDisplay)");
    CHECK(comp->FindMethod((const WCHAR_T*)L"ВывестиСтрокиНаДисплейПокупателя") >= 0, "FindMethod(ВывестиСтрокиНаДисплейПокупателя)");
    CHECK(comp->FindMethod((const WCHAR_T*)L"GetCustomerDisplayOptions") >= 0, "FindMethod(GetCustomerDisplayOptions)");

    tVariant ret; memset(&ret, 0, sizeof(ret));

    // --- GetDescription ---
    {
        tVariant out; memset(&out, 0, sizeof(out));
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"GetDescription"), &ret, &out, 1);
        CHECK(ok && TV_BOOL(&ret), "GetDescription -> true");
        std::wstring xml = OutStr(&out);
        printf("  DriverDescription: %u байт\n", (unsigned)xml.size());
        CHECK(xml.find(L"CustomerDisplay") != std::wstring::npos, "XML содержит CustomerDisplay");
        PrintXml(xml);
    }

    // --- GetLastError (пустая) ---
    {
        tVariant out; memset(&out, 0, sizeof(out));
        comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"GetLastError"), &ret, &out, 1);
        CHECK(TV_I4(&ret) == 0, "GetLastError код = 0 после успеха");
    }

    // --- EquipmentParameters ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], L"CustomerDisplay");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"EquipmentParameters"), &ret, params, 2);
        CHECK(ok && TV_BOOL(&ret), "EquipmentParameters -> true");
        std::wstring xml = OutStr(&params[1]);
        CHECK(xml.find(L"MonitorIndex") != std::wstring::npos, "TableParameters содержит MonitorIndex");
        PrintXml(xml);
    }

    // --- GetCustomerDisplayOptions на неизвестном ID -> ленивое создание, затем отключение ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], L"{lazy-device}");
        comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"GetCustomerDisplayOptions"), &ret, params, 2);
        CHECK(TV_BOOL(&ret), "GetCustomerDisplayOptions(неизвестный ID) -> ленивое создание");
        // убираем лениво созданное устройство, чтобы не осталось лишнего окна
        tVariant disc[1]; memset(disc, 0, sizeof(disc));
        SetInStr(&disc[0], L"{lazy-device}");
        comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisconnectEquipment"), &ret, disc, 1);
    }

    // --- ConnectEquipment: окно на мониторе 1 ---
    std::wstring deviceId;
    {
        tVariant params[3]; memset(params, 0, sizeof(params));
        SetInStr(&params[1], L"CustomerDisplay");
        SetInStr(&params[2],
            L"<Parameters>"
            L"<Parameter Name=\"MonitorIndex\" Value=\"1\"/>"
            L"<Parameter Name=\"Fullscreen\" Value=\"true\"/>"
            L"</Parameters>");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"ConnectEquipment"), &ret, params, 3);
        CHECK(ok && TV_BOOL(&ret), "ConnectEquipment -> true");
        if (!ok || !TV_BOOL(&ret))
            PrintLast(comp);
        deviceId = OutStr(&params[0]);
        printf("  DeviceID: %ls\n", deviceId.c_str());
        CHECK(deviceId.find(L"{") != std::wstring::npos, "DeviceID (GUID) возвращён");

        ::Sleep(1500);
        HWND wnd = FindOwnDisplayWindow();
        CHECK(wnd != nullptr && ::IsWindowVisible(wnd), "окно дисплея создано и видимо");
        RECT rc; if (wnd) { ::GetWindowRect(wnd, &rc); printf("  окно: %ldx%ld @ (%ld,%ld)\n", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top); }
    }

    // --- StringOutputOnCustomerDisplay ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        SetInStr(&params[1],
            L"<Strings>"
            L"<String>Молоко «Простоквашино» 3,2% | 89,00</String>"
            L"<String>Хлеб «Бородинский» | 54,50</String>"
            L"<String>ИТОГО | 143,50</String>"
            L"</Strings>");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"StringOutputOnCustomerDisplay"), &ret, params, 2);
        CHECK(ok && TV_BOOL(&ret), "StringOutputOnCustomerDisplay -> true");
        ::Sleep(800);
    }

    // --- QRCodeOutputOnCustomerDisplay (генерация из значения) ---
    {
        tVariant params[3]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        SetInStr(&params[1], L"https://example.com/pay/143.50");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"QRCodeOutputOnCustomerDisplay"), &ret, params, 3);
        CHECK(ok && TV_BOOL(&ret), "QRCodeOutputOnCustomerDisplay -> true");
        ::Sleep(800);
    }

    // --- DisplayOperationStatus(2) ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        SetInLong(&params[1], 2);
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisplayOperationStatus"), &ret, params, 2);
        CHECK(ok && TV_BOOL(&ret), "DisplayOperationStatus(2) -> true");
        ::Sleep(800);
    }

    // --- DisplayWorkplaceStatus(1) со звуком ---
    {
        tVariant params[3]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        SetInLong(&params[1], 1);
        SetInLong(&params[2], 1);
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisplayWorkplaceStatus"), &ret, params, 3);
        CHECK(ok && TV_BOOL(&ret), "DisplayWorkplaceStatus(1, звук) -> true");
        ::Sleep(800);
    }

    // --- GetCustomerDisplayOptions на подключённом ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"GetCustomerDisplayOptions"), &ret, params, 2);
        CHECK(ok && TV_BOOL(&ret), "GetCustomerDisplayOptions -> true");
        std::wstring xml = OutStr(&params[1]);
        CHECK(xml.find(L"DisplayQRCode=\"true\"") != std::wstring::npos, "опции: DisplayQRCode=true");
        PrintXml(xml);
    }

    // --- EquipmentTest (демо на подключённом устройстве) ---
    {
        tVariant params[3]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], L"CustomerDisplay");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"EquipmentTest"), &ret, params, 3);
        CHECK(ok && TV_BOOL(&ret), "EquipmentTest -> true");
        ::Sleep(1200);
    }

    // --- DisconnectEquipment ---
    {
        tVariant params[1]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisconnectEquipment"), &ret, params, 1);
        CHECK(ok && TV_BOOL(&ret), "DisconnectEquipment -> true");
        ::Sleep(500);
        HWND wnd = FindOwnDisplayWindow();
        CHECK(wnd == nullptr || !::IsWindowVisible(wnd), "окно дисплея закрыто");
    }

    // --- Одно окно на кассу: второе устройство не создаёт окно, ---
    // --- а переконфигурирует существующее в полосу ---
    {
        std::wstring dev1, dev2;

        // первое устройство — обычное окно во весь экран (вторые окна ещё
        // разрешены: на машине может быть живое окно дисплея 1С)
        {
            tVariant params[3]; memset(params, 0, sizeof(params));
            SetInStr(&params[1], L"CustomerDisplay");
            SetInStr(&params[2],
                L"<Parameters>"
                L"<Parameter Name=\"MonitorIndex\" Value=\"1\"/>"
                L"<Parameter Name=\"Fullscreen\" Value=\"true\"/>"
                L"</Parameters>");
            bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"ConnectEquipment"), &ret, params, 3);
            CHECK(ok && TV_BOOL(&ret), "ConnectEquipment (окно 1) -> true");
            dev1 = OutStr(&params[0]);
        }
        ::Sleep(1200);
        HWND wnd1 = FindOwnMainWindow();
        CHECK(wnd1 != nullptr && ::IsWindowVisible(wnd1), "окно 1 создано и видимо");
        RECT rc1 = { 0, 0, 0, 0 };
        if (wnd1) ::GetWindowRect(wnd1, &rc1);

        // включаем подавление и подключаем второе устройство — тот же
        // монитор, полоса 30%: своего окна не создаёт, а переконфигурирует
        // существующее (явные настройки передаются)
        ::SetEnvironmentVariableW(L"CDN_ALLOW_SECOND_WINDOW", nullptr);
        {
            tVariant params[3]; memset(params, 0, sizeof(params));
            SetInStr(&params[1], L"CustomerDisplay");
            SetInStr(&params[2],
                L"<Parameters>"
                L"<Parameter Name=\"MonitorIndex\" Value=\"1\"/>"
                L"<Parameter Name=\"Fullscreen\" Value=\"false\"/>"
                L"<Parameter Name=\"BarHeightPct\" Value=\"30\"/>"
                L"</Parameters>");
            bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"ConnectEquipment"), &ret, params, 3);
            CHECK(ok && TV_BOOL(&ret), "ConnectEquipment (второе устройство, полоса) -> true");
            dev2 = OutStr(&params[0]);
        }
        ::Sleep(700);
        HWND wnd2 = FindOwnMainWindow();
        CHECK(wnd2 == wnd1, "второго окна не появилось (одно окно на кассу)");

        // переконфигурацию в полосу проверяем только на чистой машине:
        // при живом окне другого процесса настройки могло получить оно
        if (wnd1 && wnd2 == wnd1 && !ForeignDisplayWindowExists())
        {
            RECT rc2; ::GetWindowRect(wnd2, &rc2);
            CHECK(rc2.bottom - rc2.top < rc1.bottom - rc1.top,
                  "существующее окно переконфигурировано в полосу");
            printf("  окно: %ldx%ld -> %ldx%ld\n",
                   rc1.right - rc1.left, rc1.bottom - rc1.top,
                   rc2.right - rc2.left, rc2.bottom - rc2.top);
        }
        else
        {
            printf("  (переконфигурация не проверяется: есть чужое окно другого процесса)\n");
        }

        // вывод на подавленное устройство проходит без ошибки
        {
            tVariant params[2]; memset(params, 0, sizeof(params));
            SetInStr(&params[0], dev2.c_str());
            SetInStr(&params[1], L"<Strings><String>ТЕСТ | 1,00</String></Strings>");
            bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"StringOutputOnCustomerDisplay"), &ret, params, 2);
            CHECK(ok && TV_BOOL(&ret), "StringOutput на второе устройство -> true");
        }

        // прибираемся: оба устройства отключены, окно закрыто
        {
            tVariant d; memset(&d, 0, sizeof(d));
            SetInStr(&d, dev2.c_str());
            comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisconnectEquipment"), &ret, &d, 1);
            SetInStr(&d, dev1.c_str());
            comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisconnectEquipment"), &ret, &d, 1);
            ::Sleep(500);
            HWND wnd = FindOwnMainWindow();
            CHECK(wnd == nullptr || !::IsWindowVisible(wnd), "окно дисплея закрыто");
        }
    }

    // --- Наличная оплата: «ВНЕСЕНО\rСДАЧА» и чистка чека по таймеру ---
    {
        ::SetEnvironmentVariableW(L"CDN_ALLOW_SECOND_WINDOW", L"1");
        std::wstring devPay;
        {
            tVariant params[3]; memset(params, 0, sizeof(params));
            SetInStr(&params[1], L"CustomerDisplay");
            SetInStr(&params[2],
                L"<Parameters><Parameter Name=\"MonitorIndex\" Value=\"1\"/></Parameters>");
            bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"ConnectEquipment"), &ret, params, 3);
            CHECK(ok && TV_BOOL(&ret), "ConnectEquipment (оплата) -> true");
            devPay = OutStr(&params[0]);
        }

        size_t mark = WebviewLogSize();
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], devPay.c_str());
        // РМК шлёт строку оплаты с переводом: «ВНЕСЕНО: X\r\nСДАЧА: Y»
        SetInStr(&params[1], L"ВНЕСЕНО: 50,00\r\nСДАЧА: 0.00");
        bool ok = comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"StringOutputOnCustomerDisplay"), &ret, params, 2);
        CHECK(ok && TV_BOOL(&ret), "StringOutput (внесено+сдача) -> true");

        CHECK(WaitWebviewLogFrom(mark, L"\"type\":\"op/paid\"", 12000), "op/paid ушёл в интерфейс");
        CHECK(WebviewLogContainsFrom(mark, L"\"type\":\"op/change\""), "сдача ушла как op/change (не позицией)");
        CHECK(!WebviewLogContainsFrom(mark, L"check/item\",\"name\":\"\\u0421\\u0414\\u0410\\u0427\\u0410"),
              "сдача не ушла позицией в чек");
        // Win32-таймер чистки срабатывает через 5 с после op/paid
        CHECK(WaitWebviewLogFrom(mark, L"paid timer: check/clear(paid) dispatched", 15000),
              "таймер чистки после ВНЕСЕНО сработал");

        tVariant d; memset(&d, 0, sizeof(d));
        SetInStr(&d, devPay.c_str());
        comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"DisconnectEquipment"), &ret, &d, 1);
        ::SetEnvironmentVariableW(L"CDN_ALLOW_SECOND_WINDOW", nullptr);
    }

    // --- Вывод HTML должен быть отклонён ---
    {
        tVariant params[2]; memset(params, 0, sizeof(params));
        SetInStr(&params[0], deviceId.c_str());
        SetInStr(&params[1], L"PGh0bWw+PC9odG1sPg==");
        comp->CallAsFunc(comp->FindMethod((const WCHAR_T*)L"OutputHTML"), &ret, params, 2);
        CHECK(!TV_BOOL(&ret), "OutputHTML -> false (не поддерживается)");
    }

    destroyObject(&comp);
    ::FreeLibrary(dll);

    printf("\nИтог: %s (%d ошибок)\n", g_failures == 0 ? "УСПЕХ" : "ЕСТЬ ОШИБКИ", g_failures);
    return g_failures == 0 ? 0 : 1;
}
