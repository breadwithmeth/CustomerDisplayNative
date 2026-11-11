#include "stdafx.h"
#include "CustomerDisplay.h"
#include "XmlUtil.h"

#include <cwchar>

// Имена методов и тексты заданы в UTF-8; компиляция выполняется с /utf-8,
// поэтому прагма setlocale не нужна (и вредна: ломает кириллицу в литералах)

// ---------------------------------------------------------------------------
// Регистрация компоненты
static const wchar_t g_kClassNames[] = L"CustomerDisplay";
static const wchar_t* g_extensionName = L"CustomerDisplayNative";
static const wchar_t* g_extensionVersion = L"1.0.1.3";
static const wchar_t* g_driverName = L"Дисплей покупателя (монитор)";
static const wchar_t* g_driverDescription =
    L"Вывод информации на дисплей покупателя. В качестве дисплея используется "
    L"обычный монитор компьютера: полноэкранное окно на выбранном экране.";

static AppCapabilities g_capabilities = eAppCapabilitiesInvalid;

// Таблицы имён методов: [0] — английские (основные), [1] — русские
static const wchar_t* g_MethodNames[] = {
    L"GetDescription",
    L"GetLastError",
    L"EquipmentParameters",
    L"ConnectEquipment",
    L"DisconnectEquipment",
    L"EquipmentTest",
    L"EquipmentAutoSetup",
    L"SetApplicationInformation",
    L"GetAdditionalActions",
    L"DoAdditionalAction",
    L"GetLocalizationPattern",
    L"SetLocalization",
    L"StringOutputOnCustomerDisplay",
    L"ClearCustomerDisplay",
    L"AddMediaFile",
    L"OutputHTML",
    L"QRCodeOutputOnCustomerDisplay",
    L"DisplayOperationStatus",
    L"DisplayWorkplaceStatus",
    L"GetCustomerDisplayOptions",
    L"GetInterfaceRevision",
    L"GetVersion",
    // ---- старый интерфейс драйвера ----
    L"GetParameters",
    L"SetParameter",
    L"Open",
    L"Close",
    L"DeviceTest",
    L"GetDescriptionOld",
};

static const wchar_t* g_MethodNamesRu[] = {
    L"ПолучитьОписаниеДрайвера",
    L"ПолучитьОшибку",
    L"ПолучитьПараметрыОборудования",
    L"ПодключитьОборудование",
    L"ОтключитьОборудование",
    L"ТестОборудования",
    L"АвтонастройкаОборудования",
    L"УстановитьИнформациюОПриложении",
    L"ПолучитьДополнительныеДействия",
    L"ВыполнитьДополнительноеДействие",
    L"ПолучитьШаблонЛокализации",
    L"УстановитьЛокализацию",
    L"ВывестиСтрокиНаДисплейПокупателя",
    L"ОчиститьДисплейПокупателя",
    L"ДобавитьФайлМедиа",
    L"ВывестиHTML",
    L"ВывестиQRКодНаДисплейПокупателя",
    L"ПоказатьСтатусОперации",
    L"ПоказатьСтатусРабочегоМеста",
    L"ПолучитьПараметрыДисплейПокупателя",
    L"ПолучитьВерсиюИнтерфейса",
    L"ПолучитьНомерВерсии",
    // ---- старый интерфейс драйвера ----
    L"ПолучитьПараметры",
    L"УстановитьПараметр",
    L"Подключить",
    L"Отключить",
    L"ТестУстройства",
    L"ПолучитьОписание",
};

// Дополнительные алиасы (варианты написания из разных редакций стандарта)
struct NameAlias { const wchar_t* name; long methodIndex; };
static const NameAlias g_ExtraAliases[] = {
    { L"ВывестиСтрокуНаДисплейПокупателя",   12 }, // StringOutputOnCustomerDisplay
    { L"ДобавитьМедиаФайл",                  14 }, // AddMediaFile
    { L"ОтобразитьСтатусОперации",           17 }, // DisplayOperationStatus
    { L"ОтобразитьСтатусРабочегоМеста",      18 }, // DisplayWorkplaceStatus
    { L"ПолучитьОпцииДисплеяПокупателя",     19 }, // GetCustomerDisplayOptions
    { L"АвтоустановкаОборудования",           6 }, // EquipmentAutoSetup
    { L"GetInterfaceVersion",                20 }, // GetInterfaceRevision
    { L"ПолучитьРевизиюИнтерфейса",          20 }, // GetInterfaceRevision (имя из эталона InnovaIT)
};

static const wchar_t* g_PropNames[] = { L"Version" };
static const wchar_t* g_PropNamesRu[] = { L"Версия" };

// Ревизия интерфейса стандартного драйвера (i8104829, на момент стандарта — 4008)
static const long g_interfaceRevision = 4008;

// ---------------------------------------------------------------------------
// Экспортируемые функции
long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface)
{
    if (!pInterface)
        return 0;
    if (!*pInterface)
    {
        *pInterface = new CustomerDisplay;
        return (long)*pInterface;
    }
    return 0;
}
//---------------------------------------------------------------------------//
AppCapabilities SetPlatformCapabilities(const AppCapabilities capabilities)
{
    g_capabilities = capabilities;
    return eAppCapabilitiesLast;
}
//---------------------------------------------------------------------------//
long DestroyObject(IComponentBase** pIntf)
{
    if (!*pIntf)
        return -1;
    delete *pIntf;
    *pIntf = 0;
    return 0;
}
//---------------------------------------------------------------------------//
const WCHAR_T* GetClassNames()
{
    // На Windows WCHAR_T совпадает с wchar_t (4 байта — только в сборках под Linux)
    return (const WCHAR_T*)g_kClassNames;
}
//---------------------------------------------------------------------------//
// Преобразование WCHAR_T <-> wchar_t (по шаблону NativeAPI)
uint32_t convToShortWchar(WCHAR_T** Dest, const wchar_t* Source, uint32_t len = 0);
uint32_t convFromShortWchar(wchar_t** Dest, const WCHAR_T* Source, uint32_t len = 0);
uint32_t getLenShortWcharStr(const WCHAR_T* Source);

uint32_t convToShortWchar(WCHAR_T** Dest, const wchar_t* Source, uint32_t len)
{
    if (!len)
        len = (uint32_t)::wcslen(Source) + 1;
    if (!*Dest)
        *Dest = new WCHAR_T[len];
    WCHAR_T* tmpShort = *Dest;
    wchar_t* tmpWChar = (wchar_t*)Source;
    uint32_t res = 0;
    ::memset(*Dest, 0, len * sizeof(WCHAR_T));
    for (; len; --len, ++res, ++tmpWChar, ++tmpShort)
        *tmpShort = (WCHAR_T)*tmpWChar;
    return res;
}
//---------------------------------------------------------------------------//
uint32_t convFromShortWchar(wchar_t** Dest, const WCHAR_T* Source, uint32_t len)
{
    if (!len)
        len = getLenShortWcharStr(Source) + 1;
    if (!*Dest)
        *Dest = new wchar_t[len];
    wchar_t* tmpWChar = *Dest;
    WCHAR_T* tmpShort = (WCHAR_T*)Source;
    uint32_t res = 0;
    ::memset(*Dest, 0, len * sizeof(wchar_t));
    for (; len; --len, ++res, ++tmpWChar, ++tmpShort)
        *tmpWChar = (wchar_t)*tmpShort;
    return res;
}
//---------------------------------------------------------------------------//
uint32_t getLenShortWcharStr(const WCHAR_T* Source)
{
    uint32_t res = 0;
    WCHAR_T* tmpShort = (WCHAR_T*)Source;
    while (*tmpShort++)
        ++res;
    return res;
}
//---------------------------------------------------------------------------//
namespace {

std::vector<uint8_t> Base64Decode(const std::wstring& in)
{
    auto val = [](wchar_t c) -> int {
        if (c >= L'A' && c <= L'Z') return c - L'A';
        if (c >= L'a' && c <= L'z') return c - L'a' + 26;
        if (c >= L'0' && c <= L'9') return c - L'0' + 52;
        if (c == L'+') return 62;
        if (c == L'/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3 + 3);
    unsigned buf = 0;
    int bits = 0;
    for (wchar_t c : in) {
        if (c == L'=')
            break; // паддинг — дальше мусор не читаем
        int v = val(c);
        if (v < 0)
            continue; // переводы строк и прочее — пропускаем
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::wstring ToLowerRu(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
        out += (wchar_t)::towlower(c);
    return out;
}

bool ParseBoolValue(const std::wstring& v, bool fallback)
{
    if (v.empty()) return fallback;
    std::wstring s = ToLowerRu(v);
    if (s == L"1" || s == L"true" || s == L"истина" || s == L"да" || s == L"-1" || s == L"yes")
        return true;
    if (s == L"0" || s == L"false" || s == L"ложь" || s == L"нет" || s == L"no")
        return false;
    return fallback;
}

// Рекурсивное создание каталога
bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring cur;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find_first_of(L"\\/", pos + 1);
        if (next == std::wstring::npos) next = path.size();
        cur = path.substr(0, next);
        // сегмент-диск вида "C:" пропускаем; ВАЖНО: точная длина, иначе
        // полный путь "C:\..." тоже начинается с "C:" и не будет создан
        if (cur.size() == 2 && cur[1] == L':') { pos = next; continue; }
        if (!cur.empty() && ::GetFileAttributesW(cur.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!::CreateDirectoryW(cur.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        pos = next;
    }
    return true;
}

bool DeleteDirectoryTree(const std::wstring& path)
{
    std::wstring pattern = path;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return ::RemoveDirectoryW(path.c_str()) != 0;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = path;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteDirectoryTree(full);
        else
            ::DeleteFileW(full.c_str());
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return ::RemoveDirectoryW(path.c_str()) != 0;
}

std::wstring SanitizeFileName(const std::wstring& name)
{
    std::wstring base = name;
    size_t p = base.find_last_of(L"/\\");
    if (p != std::wstring::npos)
        base = base.substr(p + 1);
    std::wstring out;
    for (wchar_t c : base) {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'|' || c == L'?' || c == L'*' || (c >= 0 && c < 32))
            out += L'_';
        else
            out += c;
    }
    return out.empty() ? std::wstring(L"media.bin") : out;
}

// Демо-строки для теста/дополнительного действия
std::vector<std::wstring> DemoLines()
{
    return {
        L"Молоко «Простоквашино» 3,2% | 89,00",
        L"Хлеб «Бородинский» | 54,50",
        L"Сыр «Российский» 500 г | 349,90",
        L"ИТОГО | 493,40",
    };
}

// ------------------------------- журнал вызовов ---------------------------
// Каждое обращение платформы к методам компоненты пишется в
// %TEMP%\CustomerDisplayNative\driver.log — для диагностики цепочки БПО.
CRITICAL_SECTION g_logLock;
bool g_logLockInit = false;

void LogLine(const std::wstring& line)
{
    if (!g_logLockInit)
    {
        ::InitializeCriticalSectionAndSpinCount(&g_logLock, 0x400);
        g_logLockInit = true;
    }
    ::EnterCriticalSection(&g_logLock);
    wchar_t tempPath[MAX_PATH + 1] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CustomerDisplayNative";
    HANDLE h = ::CreateFileW((dir + L"\\driver.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // каталог мог быть удалён/не создан — пересоздаём и пробуем снова
        EnsureDirectory(dir);
        h = ::CreateFileW((dir + L"\\driver.log").c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t ts[48];
        ::swprintf(ts, 48, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
                   st.wSecond, st.wMilliseconds);
        std::wstring full = std::wstring(ts) + line + L"\r\n";
        DWORD written = 0;
        ::WriteFile(h, full.c_str(), (DWORD)(full.size() * sizeof(wchar_t)), &written, nullptr);
        ::CloseHandle(h);
    }
    ::LeaveCriticalSection(&g_logLock);
}

std::wstring ParamToLogString(tVariant* p)
{
    if (!p)
        return L"<null>";
    switch (TV_VT(p))
    {
    case VTYPE_PWSTR:
        {
            if (!TV_WSTR(p))
                return L"str:<null>";
            size_t len = p->wstrLen;
            std::wstring s((wchar_t*)TV_WSTR(p), std::min<size_t>(len, (size_t)300));
            if (len > 300)
                s += L"...";
            for (auto& c : s)
                if (c == L'\r' || c == L'\n') c = L' ';
            return L"str:" + s;
        }
    case VTYPE_I4:    return L"i4:" + std::to_wstring((long)TV_I4(p));
    case VTYPE_BOOL:  return TV_BOOL(p) ? L"bool:true" : L"bool:false";
    case VTYPE_R8:    return L"r8:" + std::to_wstring(p->dblVal);
    case VTYPE_EMPTY: return L"empty";
    default:          return L"vt=" + std::to_wstring((int)TV_VT(p));
    }
}

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////
// CustomerDisplay

CustomerDisplay::CustomerDisplay()
{
    m_iMemory = 0;
    m_iConnect = 0;
    ::InitializeCriticalSectionAndSpinCount(&m_devicesLock, 0x400);
}
//---------------------------------------------------------------------------//
CustomerDisplay::~CustomerDisplay()
{
    Done();
    ::DeleteCriticalSection(&m_devicesLock);
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::Init(void* pConnection)
{
    m_iConnect = (IAddInDefBase*)pConnection;
    if (m_iConnect)
        m_iConnect->SetEventBufferDepth(50);
    LogLine(L"INIT экземпляр компоненты создан");
    return true;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::GetInfo()
{
    // Версия технологии компоненты — как в рабочих драйверах InnovaIT (2.005)
    return 2005;
}
//---------------------------------------------------------------------------//
void CustomerDisplay::Done()
{
    // Останавливаем все окна устройств (новый и старый интерфейс)
    ::EnterCriticalSection(&m_devicesLock);
    std::vector<std::unique_ptr<Device>> devices;
    devices.swap(m_devices);
    std::unique_ptr<Device> legacy = std::move(m_legacyDevice);
    ::LeaveCriticalSection(&m_devicesLock);
    if (legacy && legacy->window)
        legacy->window->Stop();
}
/////////////////////////////////////////////////////////////////////////////
// ILanguageExtenderBase
//---------------------------------------------------------------------------//
bool CustomerDisplay::RegisterExtensionAs(WCHAR_T** wsExtensionName)
{
    const wchar_t* wsExtension = g_extensionName;
    size_t iActualSize = ::wcslen(wsExtension) + 1;
    if (m_iMemory)
    {
        if (m_iMemory->AllocMemory((void**)wsExtensionName, (unsigned long)(iActualSize * sizeof(WCHAR_T))))
            ::convToShortWchar(wsExtensionName, wsExtension, (uint32_t)iActualSize);
        return true;
    }
    return false;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::GetNProps()
{
    return epLast;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::FindProp(const WCHAR_T* wsPropName)
{
    long plPropNum = -1;
    wchar_t* propName = 0;
    ::convFromShortWchar(&propName, wsPropName);
    plPropNum = findName(g_PropNames, propName, epLast);
    if (plPropNum == -1)
        plPropNum = findName(g_PropNamesRu, propName, epLast);
    delete[] propName;
    return plPropNum;
}
//---------------------------------------------------------------------------//
const WCHAR_T* CustomerDisplay::GetPropName(long lPropNum, long lPropAlias)
{
    if (lPropNum >= epLast)
        return NULL;
    wchar_t* wsCurrentName = NULL;
    WCHAR_T* wsPropName = NULL;
    int iActualSize = 0;
    switch (lPropAlias)
    {
    case 0: wsCurrentName = (wchar_t*)g_PropNames[lPropNum]; break;
    case 1: wsCurrentName = (wchar_t*)g_PropNamesRu[lPropNum]; break;
    default: return 0;
    }
    iActualSize = (int)::wcslen(wsCurrentName) + 1;
    if (m_iMemory && wsCurrentName)
    {
        if (m_iMemory->AllocMemory((void**)&wsPropName, iActualSize * sizeof(WCHAR_T)))
            ::convToShortWchar(&wsPropName, wsCurrentName, (uint32_t)iActualSize);
    }
    return wsPropName;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::GetPropVal(const long lPropNum, tVariant* pvarPropVal)
{
    switch (lPropNum)
    {
    case epVersion:
        setWStringToTVariant(pvarPropVal, g_extensionVersion);
        return true;
    default:
        return false;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::SetPropVal(const long lPropNum, tVariant* varPropVal)
{
    switch (lPropNum)
    {
    default:
        return false;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::IsPropReadable(const long lPropNum)
{
    switch (lPropNum)
    {
    case epVersion: return true;
    default: return false;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::IsPropWritable(const long lPropNum)
{
    switch (lPropNum)
    {
    default: return false;
    }
}
//---------------------------------------------------------------------------//
long CustomerDisplay::GetNMethods()
{
    return emLast;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::FindMethod(const WCHAR_T* wsMethodName)
{
    long plMethodNum = -1;
    wchar_t* name = 0;
    ::convFromShortWchar(&name, wsMethodName);
    plMethodNum = findName(g_MethodNames, name, emLast);
    if (plMethodNum == -1)
        plMethodNum = findName(g_MethodNamesRu, name, emLast);
    if (plMethodNum == -1)
    {
        for (const NameAlias& a : g_ExtraAliases)
        {
            if (!::wcscmp(a.name, name)) { plMethodNum = a.methodIndex; break; }
        }
    }
    delete[] name;
    return plMethodNum;
}
//---------------------------------------------------------------------------//
const WCHAR_T* CustomerDisplay::GetMethodName(const long lMethodNum, const long lMethodAlias)
{
    if (lMethodNum >= emLast)
        return NULL;
    wchar_t* wsCurrentName = NULL;
    WCHAR_T* wsMethodName = NULL;
    int iActualSize = 0;
    switch (lMethodAlias)
    {
    case 0: wsCurrentName = (wchar_t*)g_MethodNames[lMethodNum]; break;
    case 1: wsCurrentName = (wchar_t*)g_MethodNamesRu[lMethodNum]; break;
    default: return 0;
    }
    iActualSize = (int)::wcslen(wsCurrentName) + 1;
    if (m_iMemory && wsCurrentName)
    {
        if (m_iMemory->AllocMemory((void**)&wsMethodName, iActualSize * sizeof(WCHAR_T)))
            ::convToShortWchar(&wsMethodName, wsCurrentName, (uint32_t)iActualSize);
    }
    return wsMethodName;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::GetNParams(const long lMethodNum)
{
    switch (lMethodNum)
    {
    case emGetDescription:               return 1;
    case emGetLastError:                 return 1;
    case emEquipmentParameters:          return 2;
    case emConnectEquipment:             return 3;
    case emDisconnectEquipment:          return 1;
    case emEquipmentTest:                return 3;
    case emEquipmentAutoSetup:           return 5;
    case emSetApplicationInformation:    return 1;
    case emGetAdditionalActions:         return 1;
    case emDoAdditionalAction:           return 1;
    case emGetLocalizationPattern:       return 1;
    case emSetLocalization:              return 2;
    case emStringOutputOnCustomerDisplay:return 2;
    case emClearCustomerDisplay:         return 1;
    case emAddMediaFile:                 return 3;
    case emOutputHTML:                   return 2;
    case emQrCodeOutputOnCustomerDisplay:return 3;
    case emDisplayOperationStatus:       return 2;
    case emDisplayWorkplaceStatus:       return 3;
    case emGetCustomerDisplayOptions:    return 2;
    case emGetInterfaceRevision:         return 0;
    case emGetVersion:                   return 0;
    case emGetParameters:                return 1;
    case emSetParameter:                 return 2;
    case emOpen:                         return 1;
    case emClose:                        return 1;
    case emDeviceTest:                   return 2;
    case emGetDescriptionOld:            return 7;
    default: return 0;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::GetParamDefValue(const long lMethodNum, const long lParamNum,
                                       tVariant* pvarParamDefValue)
{
    TV_VT(pvarParamDefValue) = VTYPE_EMPTY;
    switch (lMethodNum)
    {
    default:
        return false;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::HasRetVal(const long lMethodNum)
{
    switch (lMethodNum)
    {
    case emGetDescription:
    case emGetLastError:
    case emEquipmentParameters:
    case emConnectEquipment:
    case emDisconnectEquipment:
    case emEquipmentTest:
    case emEquipmentAutoSetup:
    case emSetApplicationInformation:
    case emGetAdditionalActions:
    case emDoAdditionalAction:
    case emGetLocalizationPattern:
    case emSetLocalization:
    case emStringOutputOnCustomerDisplay:
    case emClearCustomerDisplay:
    case emAddMediaFile:
    case emOutputHTML:
    case emQrCodeOutputOnCustomerDisplay:
    case emDisplayOperationStatus:
    case emDisplayWorkplaceStatus:
    case emGetCustomerDisplayOptions:
    case emGetInterfaceRevision:
    case emGetVersion:
    case emGetParameters:
    case emSetParameter:
    case emOpen:
    case emClose:
    case emDeviceTest:
    case emGetDescriptionOld:
        return true;
    default:
        return false;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray)
{
    return false; // все методы — функции
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue,
                                 tVariant* paParams, const long lSizeArray)
{
    // журнал вызова (диагностика цепочки БПО)
    std::wstring mName = (lMethodNum >= 0 && lMethodNum < emLast)
                         ? g_MethodNames[lMethodNum] : L"<unknown>";
    {
        std::wstring line = L"CALL " + mName + L" n=" + std::to_wstring(lSizeArray);
        for (long i = 0; i < lSizeArray; ++i)
            line += L" [" + std::to_wstring(i) + L"]=" + ParamToLogString(&paParams[i]);
        LogLine(line);
    }

    bool result = CallAsFuncImpl(lMethodNum, pvarRetValue, paParams, lSizeArray);
    LogLine(L"RET  " + mName + L" -> " + (result ? L"true" : L"false") +
            L" err(" + std::to_wstring(m_lastErrorCode) + L")=" + m_lastError);
    return result;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::CallAsFuncImpl(const long lMethodNum, tVariant* pvarRetValue,
                                     tVariant* paParams, const long lSizeArray)
{
    // 1С всегда передаёт GetNParams слотов; дополнительно страхуемся по размеру
    const long n = lSizeArray;
    switch (lMethodNum)
    {
    case emGetDescription:
        return mGetDescription(pvarRetValue, paParams);
    case emGetLastError:
        return mGetLastError(pvarRetValue, paParams);
    case emEquipmentParameters:
        return (n >= 2) && mEquipmentParameters(pvarRetValue, paParams, &paParams[1]);
    case emConnectEquipment:
        return (n >= 3) && mConnectEquipment(pvarRetValue, paParams, &paParams[1], &paParams[2]);
    case emDisconnectEquipment:
        return (n >= 1) && mDisconnectEquipment(pvarRetValue, paParams);
    case emEquipmentTest:
        return (n >= 3) && mEquipmentTest(pvarRetValue, paParams, &paParams[1], &paParams[2]);
    case emEquipmentAutoSetup:
        return (n >= 5) && mEquipmentAutoSetup(pvarRetValue, paParams, &paParams[1], &paParams[2], &paParams[3], &paParams[4]); // OUT тип; IN/OUT параметры; IN окно; IN таймаут
    case emSetApplicationInformation:
        return (n >= 1) && mSetApplicationInformation(pvarRetValue, paParams);
    case emGetAdditionalActions:
        return (n >= 1) && mGetAdditionalActions(pvarRetValue, paParams);
    case emDoAdditionalAction:
        return (n >= 1) && mDoAdditionalAction(pvarRetValue, paParams);
    case emGetLocalizationPattern:
        return (n >= 1) && mGetLocalizationPattern(pvarRetValue, paParams);
    case emSetLocalization:
        return (n >= 2) && mSetLocalization(pvarRetValue, paParams, &paParams[1]);
    case emStringOutputOnCustomerDisplay:
        return (n >= 2) && mStringOutputOnCustomerDisplay(pvarRetValue, paParams, &paParams[1]);
    case emClearCustomerDisplay:
        return (n >= 1) && mClearCustomerDisplay(pvarRetValue, paParams);
    case emAddMediaFile:
        return (n >= 3) && mAddMediaFile(pvarRetValue, paParams, &paParams[1], &paParams[2]);
    case emOutputHTML:
        return (n >= 2) && mOutputHTML(pvarRetValue, paParams, &paParams[1]);
    case emQrCodeOutputOnCustomerDisplay:
        return (n >= 3) && mQrCodeOutputOnCustomerDisplay(pvarRetValue, paParams, &paParams[1], &paParams[2]);
    case emDisplayOperationStatus:
        return (n >= 2) && mDisplayOperationStatus(pvarRetValue, paParams, &paParams[1]);
    case emDisplayWorkplaceStatus:
        return (n >= 3) && mDisplayWorkplaceStatus(pvarRetValue, paParams, &paParams[1], &paParams[2]);
    case emGetCustomerDisplayOptions:
        return (n >= 2) && mGetCustomerDisplayOptions(pvarRetValue, paParams, &paParams[1]);
    case emGetInterfaceRevision:
        return mGetInterfaceRevision(pvarRetValue);
    case emGetVersion:
        return mGetVersion(pvarRetValue);
    // ---- старый интерфейс драйвера ----
    case emGetParameters:
        return (n >= 1) && mGetParametersLegacy(pvarRetValue, paParams);
    case emSetParameter:
        return (n >= 2) && mSetParameterLegacy(pvarRetValue, paParams, &paParams[1]);
    case emOpen:
        return (n >= 1) && mOpenLegacy(pvarRetValue, paParams);
    case emClose:
        return (n >= 1) && mCloseLegacy(pvarRetValue, paParams);
    case emDeviceTest:
        return (n >= 2) && mDeviceTestLegacy(pvarRetValue, paParams, &paParams[1]);
    case emGetDescriptionOld:
        return (n >= 7) && mGetDescriptionOld(pvarRetValue, paParams, &paParams[1], &paParams[2], &paParams[3], &paParams[4], &paParams[5], &paParams[6]);
    default:
        return false;
    }
}
//---------------------------------------------------------------------------//
void CustomerDisplay::SetLocale(const WCHAR_T* loc)
{
    _wsetlocale(LC_ALL, loc);
}
/////////////////////////////////////////////////////////////////////////////
// LocaleBase
//---------------------------------------------------------------------------//
bool CustomerDisplay::setMemManager(void* mem)
{
    m_iMemory = (IMemoryManager*)mem;
    return m_iMemory != 0;
}
//---------------------------------------------------------------------------//
void CustomerDisplay::addError(uint32_t wcode, const wchar_t* source,
                               const wchar_t* descriptor, long code)
{
    if (m_iConnect)
    {
        WCHAR_T* err = 0;
        WCHAR_T* descr = 0;
        ::convToShortWchar(&err, source);
        ::convToShortWchar(&descr, descriptor);
        m_iConnect->AddError(wcode, err, descr, code);
        delete[] err;
        delete[] descr;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::sendEvent(const wchar_t* msg, const wchar_t* data)
{
    if (m_iConnect)
    {
        WCHAR_T* who = 0, * what = 0, * wdata = 0;
        ::convToShortWchar(&who, g_extensionName);
        ::convToShortWchar(&what, msg);
        ::convToShortWchar(&wdata, data);
        bool res = m_iConnect->ExternalEvent(who, what, wdata);
        delete[] who;
        delete[] what;
        delete[] wdata;
        return res;
    }
    return false;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::findName(const wchar_t* names[], const wchar_t* name,
                               const uint32_t size) const
{
    long ret = -1;
    for (uint32_t i = 0; i < size; i++)
    {
        if (!::wcscmp(names[i], name))
        {
            ret = (long)i;
            break;
        }
    }
    return ret;
}
//---------------------------------------------------------------------------//
void CustomerDisplay::setWStringToTVariant(tVariant* dest, const std::wstring& source)
{
    size_t len = source.size() + 1;
    TV_VT(dest) = VTYPE_PWSTR;
    if (m_iMemory->AllocMemory((void**)&dest->pwstrVal, (unsigned long)(len * sizeof(WCHAR_T))))
        ::convToShortWchar(&dest->pwstrVal, source.c_str(), (uint32_t)len);
    dest->wstrLen = (uint32_t)source.size();
}
//---------------------------------------------------------------------------//
void CustomerDisplay::setBoolToTVariant(tVariant* dest, bool value)
{
    TV_VT(dest) = VTYPE_BOOL;
    TV_BOOL(dest) = value;
}
//---------------------------------------------------------------------------//
void CustomerDisplay::setLongToTVariant(tVariant* dest, long value)
{
    TV_VT(dest) = VTYPE_I4;
    TV_I4(dest) = value;
}
//---------------------------------------------------------------------------//
std::wstring CustomerDisplay::getStrParam(tVariant* paParams, long index)
{
    if (!paParams || TV_VT(&paParams[index]) != VTYPE_PWSTR || !TV_WSTR(&paParams[index]))
        return std::wstring();
    wchar_t* w = 0;
    ::convFromShortWchar(&w, TV_WSTR(&paParams[index]));
    std::wstring res = w;
    delete[] w;
    return res;
}
//---------------------------------------------------------------------------//
long CustomerDisplay::getLongParam(tVariant* paParams, long index, long fallback)
{
    if (!paParams)
        return fallback;
    tVariant* p = &paParams[index];
    switch (TV_VT(p))
    {
    case VTYPE_I4:    return (long)TV_I4(p);
    case VTYPE_BOOL:  return TV_BOOL(p) ? 1 : 0;
    case VTYPE_R4:    return (long)p->fltVal;
    case VTYPE_R8:    return (long)p->dblVal;
    case VTYPE_PWSTR: return (long)::_wtoi(getStrParam(paParams, index).c_str());
    default:          return fallback;
    }
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::getBoolParam(tVariant* paParams, long index, bool fallback)
{
    if (!paParams)
        return fallback;
    tVariant* p = &paParams[index];
    switch (TV_VT(p))
    {
    case VTYPE_BOOL:  return TV_BOOL(p) != 0;
    case VTYPE_I4:    return TV_I4(p) != 0;
    case VTYPE_PWSTR: return ParseBoolValue(getStrParam(paParams, index), fallback);
    default:          return fallback;
    }
}
//---------------------------------------------------------------------------//
void CustomerDisplay::setError(long code, const std::wstring& text)
{
    m_lastErrorCode = code;
    m_lastError = text;
}
/////////////////////////////////////////////////////////////////////////////
// Устройства
//---------------------------------------------------------------------------//
CustomerDisplay::Device* CustomerDisplay::FindDevice(const std::wstring& id)
{
    for (auto& d : m_devices)
    {
        if (_wcsicmp(d->id.c_str(), id.c_str()) == 0)
            return d.get();
    }
    return nullptr;
}
//---------------------------------------------------------------------------//
CustomerDisplay::Device* CustomerDisplay::ResolveDevice(const std::wstring& id,
                                                        bool allowLazyCreate)
{
    // вызывать под m_devicesLock
    // 1) точное совпадение ID
    Device* dev = FindDevice(id);
    if (dev)
        return dev;

    // 2) устройство старого интерфейса (Подключить без ID / с ID БПО)
    if (m_legacyDevice)
    {
        if (!id.empty() && m_legacyDevice->id.empty())
            m_legacyDevice->id = id; // запоминаем ID, под которым БПО знает устройство
        return m_legacyDevice.get();
    }

    // 3) единственное устройство нового интерфейса
    if (m_devices.size() == 1)
        return m_devices.front().get();

    // 4) ленивое создание: БПО прислало неизвестный ID, устройства ещё нет
    if (allowLazyCreate)
    {
        std::unique_ptr<Device> lazy(new Device());
        lazy->columns = 40;
        lazy->rows = 10;
        lazy->id = id;
        std::wstring settingsError;
        ParseSettings(m_legacyParams, lazy->settings, lazy->columns, lazy->rows, settingsError);

        lazy->window.reset(new cdp::DisplayWindow());
        if (!lazy->window->Start(lazy->settings, L"Дисплей покупателя"))
            return nullptr;
        cdp::DisplayContent welcome;
        welcome.mode = cdp::ScreenMode::Welcome;
        lazy->window->SetContent(welcome);
        lazy->lastContent = welcome;

        // каталог для медиафайлов
        wchar_t tempPath[MAX_PATH + 1] = { 0 };
        ::GetTempPathW(MAX_PATH, tempPath);
        std::wstring dirId = lazy->id.empty() ? GenerateDeviceId() : lazy->id;
        dirId.erase(std::remove(dirId.begin(), dirId.end(), L'{'), dirId.end());
        dirId.erase(std::remove(dirId.begin(), dirId.end(), L'}'), dirId.end());
        lazy->mediaDir = std::wstring(tempPath) + L"CustomerDisplayNative\\" + dirId;
        EnsureDirectory(lazy->mediaDir);

        Device* raw = lazy.get();
        if (raw->id.empty())
            raw->id = GenerateDeviceId();
        m_devices.emplace_back(std::move(lazy));
        return raw;
    }
    return nullptr;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::EnsureDeviceWindow(Device* dev)
{
    // вызывать под m_devicesLock
    if (!dev)
        return false;
    if (!dev->window)
        dev->window.reset(new cdp::DisplayWindow());
    if (!dev->window->IsRunning())
    {
        // окно закрыли вручную (панель задач / Alt+F4) — переоткрываем
        if (!dev->window->Start(dev->settings, L"Дисплей покупателя"))
            return false;
        dev->window->SetContent(dev->lastContent);
        LogLine(L"WINDOW reopen: " + dev->window->DescribeState());
    }
    return true;
}
//---------------------------------------------------------------------------//
void CustomerDisplay::DestroyDevice(const std::wstring& id)
{
    std::unique_ptr<Device> doomed;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
    {
        if (_wcsicmp((*it)->id.c_str(), id.c_str()) == 0)
        {
            doomed = std::move(*it);
            m_devices.erase(it);
            break;
        }
    }
    if (doomed)
    {
        if (doomed->window)
            doomed->window->Stop();
        if (!doomed->mediaDir.empty())
            DeleteDirectoryTree(doomed->mediaDir);
    }
}
//---------------------------------------------------------------------------//
std::wstring CustomerDisplay::GenerateDeviceId()
{
    GUID g;
    if (SUCCEEDED(::CoCreateGuid(&g)))
    {
        wchar_t buf[64] = { 0 };
        ::StringFromGUID2(g, buf, 64);
        return buf;
    }
    wchar_t buf[48] = { 0 };
    ::swprintf(buf, 48, L"DisplayDevice-%08X", ::GetTickCount());
    return buf;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::ParseSettings(const std::map<std::wstring, std::wstring>& params,
                                    cdp::DisplaySettings& settings, int& columns, int& rows,
                                    std::wstring& errorText)
{
    for (const auto& kv : params)
    {
        const std::wstring& name = kv.first;
        const std::wstring& value = kv.second;
        bool isTrue = ParseBoolValue(value, false);
        long asLong = ::_wtoi(value.c_str());

        if (name == L"MonitorIndex" || name == L"НомерМонитора" || name == L"Монитор")
            settings.monitorIndex = asLong;
        else if (name == L"Fullscreen" || name == L"ПолноэкранныйРежим")
            settings.fullscreen = isTrue;
        else if (name == L"BarHeightPct" || name == L"ВысотаПолосы")
            settings.barHeightPct = std::min(60, std::max(15, (int)asLong));
        else if (name == L"FontScale" || name == L"МасштабШрифта")
            settings.fontScalePct = std::min(300, std::max(50, (int)asLong));
        else if (name == L"BackgroundColor" || name == L"ЦветФона")
            settings.bgColor = xmlutil::ParseColor(value, settings.bgColor);
        else if (name == L"TextColor" || name == L"ЦветТекста")
            settings.textColor = xmlutil::ParseColor(value, settings.textColor);
        else if (name == L"AccentColor" || name == L"ЦветАкцента")
            settings.accentColor = xmlutil::ParseColor(value, settings.accentColor);
        else if (name == L"SoundEnabled" || name == L"ЗвуковыеСигналы")
            settings.soundEnabled = isTrue;
        else if (name == L"Columns" || name == L"СтолбцовВСтроке")
            columns = std::min(200, std::max(10, (int)asLong));
        else if (name == L"Rows" || name == L"СтрокНаЭкране")
            rows = std::min(40, std::max(2, (int)asLong));
        // неизвестные параметры игнорируем
    }
    return true;
}
/////////////////////////////////////////////////////////////////////////////
// Методы общего контракта
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetDescription(tVariant* retVal, tVariant* driverDescription)
{
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += L"<DriverDescription ";
    xml += L"Name=\"" + xmlutil::EscapeAttr(g_driverName) + L"\" ";
    xml += L"Description=\"" + xmlutil::EscapeAttr(g_driverDescription) + L"\" ";
    xml += L"EquipmentType=\"CustomerDisplay\" ";
    xml += L"Language=\"RU,EN\" ";
    xml += L"IntegrationComponent=\"true\" ";
    xml += L"MainDriverInstalled=\"true\" ";
    xml += L"DriverVersion=\"" + xmlutil::EscapeAttr(g_extensionVersion) + L"\" ";
    xml += L"IntegrationComponentVersion=\"" + xmlutil::EscapeAttr(g_extensionVersion) + L"\" ";
    xml += L"IsEmulator=\"true\" ";
    xml += L"LocalizationSupported=\"false\" ";
    xml += L"AutoSetup=\"false\"/>";
    setWStringToTVariant(driverDescription, xml);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetLastError(tVariant* retVal, tVariant* errorDescription)
{
    setWStringToTVariant(errorDescription, m_lastError);
    setLongToTVariant(retVal, m_lastErrorCode);
    return true;
}
//---------------------------------------------------------------------------//
std::wstring CustomerDisplay::BuildSettingsXml() const
{
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += L"<Settings>";
    xml += L"<Page Caption=\"Дисплей покупателя\">";
    xml += L"<Group Caption=\"Подключение\">";
    xml += L"<Parameter Name=\"MonitorIndex\" Caption=\"Номер монитора\" "
           L"Description=\"Порядковый номер монитора: 1 — основной, 2 — второй и т.д.\" "
           L"TypeValue=\"Number\" DefaultValue=\"2\">";
    xml += L"<ChoiceList>"
           L"<Item Value=\"1\">1 — основной</Item>"
           L"<Item Value=\"2\">2</Item>"
           L"<Item Value=\"3\">3</Item>"
           L"<Item Value=\"4\">4</Item>"
           L"</ChoiceList>";
    xml += L"</Parameter>";
    xml += L"<Parameter Name=\"Fullscreen\" Caption=\"Полноэкранный режим\" "
           L"Description=\"Истина — весь монитор; Ложь — полоса снизу как панель программы\" "
           L"TypeValue=\"Boolean\" DefaultValue=\"False\"/>";
    xml += L"<Parameter Name=\"BarHeightPct\" Caption=\"Высота полосы снизу, % экрана\" "
           L"TypeValue=\"Number\" DefaultValue=\"30\"/>";
    xml += L"<Parameter Name=\"SoundEnabled\" Caption=\"Звуковые сигналы\" "
           L"TypeValue=\"Boolean\" DefaultValue=\"True\"/>";
    xml += L"</Group>";
    xml += L"<Group Caption=\"Оформление\">";
    xml += L"<Parameter Name=\"FontScale\" Caption=\"Масштаб шрифта, %\" "
           L"TypeValue=\"Number\" DefaultValue=\"100\"/>";
    xml += L"<Parameter Name=\"BackgroundColor\" Caption=\"Цвет фона (RRGGBB)\" "
           L"TypeValue=\"String\" DefaultValue=\"000000\"/>";
    xml += L"<Parameter Name=\"TextColor\" Caption=\"Цвет текста (RRGGBB)\" "
           L"TypeValue=\"String\" DefaultValue=\"FFFFFF\"/>";
    xml += L"<Parameter Name=\"AccentColor\" Caption=\"Цвет акцента (RRGGBB)\" "
           L"TypeValue=\"String\" DefaultValue=\"FFC000\"/>";
    xml += L"</Group>";
    xml += L"<Group Caption=\"Режим текста\">";
    xml += L"<Parameter Name=\"Columns\" Caption=\"Столбцов в строке\" "
           L"Description=\"Объявляется платформе в опциях дисплея\" "
           L"TypeValue=\"Number\" DefaultValue=\"40\"/>";
    xml += L"<Parameter Name=\"Rows\" Caption=\"Строк на экране\" "
           L"TypeValue=\"Number\" DefaultValue=\"10\"/>";
    xml += L"</Group>";
    xml += L"</Page>";
    xml += L"</Settings>";
    return xml;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mEquipmentParameters(tVariant* retVal, tVariant* equipmentType,
                                           tVariant* tableParameters)
{
    setWStringToTVariant(tableParameters, BuildSettingsXml());
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mConnectEquipment(tVariant* retVal, tVariant* deviceId,
                                        tVariant* equipmentType, tVariant* connectionParameters)
{
    std::wstring type = ToLowerRu(getStrParam(equipmentType, 0));
    if (type.empty())
    {
        setError(100, L"Не указан тип оборудования (ожидался CustomerDisplay)");
        setBoolToTVariant(retVal, false);
        return true;
    }
    if (type != L"customerdisplay" && type != L"дисплейпокупателя")
    {
        setError(101, L"Тип оборудования не поддерживается: " + getStrParam(equipmentType, 0));
        setBoolToTVariant(retVal, false);
        return true;
    }

    auto params = xmlutil::ParseParameters(getStrParam(connectionParameters, 0));
    Device* dev = new Device;
    dev->columns = 40;
    dev->rows = 10;
    std::wstring settingsError; // параметры опциональны — ошибки быть не может
    ParseSettings(params, dev->settings, dev->columns, dev->rows, settingsError);

    dev->id = GenerateDeviceId();

    dev->window.reset(new cdp::DisplayWindow());
    if (!dev->window->Start(dev->settings, L"Дисплей покупателя"))
    {
        std::wstring err = dev->window->LastError();
        setError(102, L"Не удалось открыть окно дисплея" +
                      (err.empty() ? std::wstring() : (L": " + err)));
        delete dev;
        setBoolToTVariant(retVal, false);
        return true;
    }

    cdp::DisplayContent welcome;
    welcome.mode = cdp::ScreenMode::Welcome;
    dev->window->SetContent(welcome);
    dev->lastContent = welcome;
    LogLine(L"WINDOW ConnectEquipment: " + dev->window->DescribeState());

    // каталог для медиафайлов
    wchar_t tempPath[MAX_PATH + 1] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);
    std::wstring dirId = dev->id;
    // убираем фигурные скобки из GUID
    dirId.erase(std::remove(dirId.begin(), dirId.end(), L'{'), dirId.end());
    dirId.erase(std::remove(dirId.begin(), dirId.end(), L'}'), dirId.end());
    dev->mediaDir = std::wstring(tempPath) + L"CustomerDisplayNative\\" + dirId;
    EnsureDirectory(dev->mediaDir);

    ::EnterCriticalSection(&m_devicesLock);
    m_devices.emplace_back(dev);
    ::LeaveCriticalSection(&m_devicesLock);

    setWStringToTVariant(deviceId, dev->id);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mDisconnectEquipment(tVariant* retVal, tVariant* deviceId)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = FindDevice(id);
    bool isLegacy = false;
    if (!dev && m_legacyDevice)
    {
        dev = m_legacyDevice.get();
        isLegacy = true;
    }
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev)
    {
        setError(110, L"Устройство не найдено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }
    if (isLegacy)
    {
        ::EnterCriticalSection(&m_devicesLock);
        if (m_legacyDevice)
        {
            if (m_legacyDevice->window)
                m_legacyDevice->window->Stop();
            m_legacyDevice.reset();
        }
        ::LeaveCriticalSection(&m_devicesLock);
    }
    else
    {
        DestroyDevice(id);
    }
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mEquipmentTest(tVariant* retVal, tVariant* equipmentType,
                                     tVariant* connectionParameters, tVariant* description)
{
    std::wstring type = ToLowerRu(getStrParam(equipmentType, 0));
    if (!type.empty() && type != L"customerdisplay" && type != L"дисплейпокупателя")
    {
        setError(101, L"Тип оборудования не поддерживается: " + getStrParam(equipmentType, 0));
        setBoolToTVariant(retVal, false);
        return true;
    }

    // Настройки из параметров подключения (если переданы)
    cdp::DisplaySettings settings;
    int columns = 40, rows = 10;
    std::wstring connXml = getStrParam(connectionParameters, 0);
    if (!connXml.empty())
    {
        auto params = xmlutil::ParseParameters(connXml);
        ParseSettings(params, settings, columns, rows, m_lastError);
    }

    // Если устройство уже подключено (legacy или новое) — демонстрация на его окне
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = m_legacyDevice ? m_legacyDevice.get()
                                 : (m_devices.empty() ? nullptr : m_devices.front().get());
    ::LeaveCriticalSection(&m_devicesLock);

    if (dev && dev->window && dev->window->IsRunning())
    {
        RunDemo(dev->window.get());
        setWStringToTVariant(description,
            L"Демонстрация выведена на дисплей покупателя и будет заменена "
            L"рабочим экраном через несколько секунд");
        setBoolToTVariant(retVal, true);
        setError(0, L"");
        return true;
    }

    // Устройство не подключено — временное окно, закроется само
    cdp::DisplayWindow* win = new cdp::DisplayWindow();
    if (!win->Start(settings, L"Дисплей покупателя — тест"))
    {
        std::wstring err = win->LastError();
        setError(103, L"Не удалось открыть окно дисплея" +
                      (err.empty() ? std::wstring() : (L": " + err)));
        delete win;
        setBoolToTVariant(retVal, false);
        return true;
    }
    RunDemo(win);

    struct Closer { static unsigned __stdcall run(void* p) {
        ::Sleep(8000);
        auto* w = (cdp::DisplayWindow*)p;
        w->Stop();
        delete w;
        return 0;
    } };
    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, Closer::run, win, 0, &tid);
    if (h)
        ::CloseHandle(h);

    setWStringToTVariant(description,
        L"Демонстрация выведена на монитор; окно закроется автоматически");
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::RunDemo(cdp::DisplayWindow* window)
{
    if (!window || !window->IsRunning())
        return false;
    cdp::DisplayContent test;
    test.mode = cdp::ScreenMode::Test;
    cdp::BuildRowsFromLines(test, DemoLines());
    test.testRows = test.rows;
    window->SetContent(test);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::RunDemoOnAnyDevice()
{
    // любое устройство: legacy, новое или лениво созданное
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(std::wstring(), true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !dev->window)
        return false;
    if (!dev->window->IsRunning())
    {
        if (!dev->window->Start(dev->settings, L"Дисплей покупателя"))
            return false;
    }
    return RunDemo(dev->window.get());
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mEquipmentAutoSetup(tVariant* retVal, tVariant* equipmentType,
                                          tVariant* connectionParametersIn,
                                          tVariant* connectionParametersOut,
                                          tVariant* showAutoSetupWindow, tVariant* timeout)
{
    setError(120, L"Автонастройка не поддерживается: выберите номер монитора в параметрах подключения");
    setBoolToTVariant(retVal, false);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mSetApplicationInformation(tVariant* retVal, tVariant* applicationSettings)
{
    // Принимаем и запоминаем не требуется — признаём метод успешным
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetAdditionalActions(tVariant* retVal, tVariant* tableActions)
{
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += L"<Actions>";
    xml += L"<Action Name=\"Демонстрация\" Caption=\"Демонстрация на дисплее\"/>";
    xml += L"</Actions>";
    setWStringToTVariant(tableActions, xml);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mDoAdditionalAction(tVariant* retVal, tVariant* actionName)
{
    std::wstring name = getStrParam(actionName, 0);
    if (name == L"Демонстрация" || _wcsicmp(name.c_str(), L"Demo") == 0 ||
        _wcsicmp(name.c_str(), L"TestDemo") == 0)
    {
        // демонстрация на любом устройстве (legacy/новое/лениво созданное)
        if (RunDemoOnAnyDevice())
        {
            setBoolToTVariant(retVal, true);
            setError(0, L"");
            return true;
        }
        setError(121, L"Не удалось открыть окно дисплея для демонстрации");
        setBoolToTVariant(retVal, false);
        return true;
    }
    setError(122, L"Неизвестное дополнительное действие: " + name);
    setBoolToTVariant(retVal, false);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetLocalizationPattern(tVariant* retVal, tVariant* localizationPattern)
{
    setError(123, L"Локализация не поддерживается");
    setBoolToTVariant(retVal, false);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mSetLocalization(tVariant* retVal, tVariant* languageCode,
                                       tVariant* localizationPattern)
{
    setError(123, L"Локализация не поддерживается");
    setBoolToTVariant(retVal, false);
    return true;
}
/////////////////////////////////////////////////////////////////////////////
// Методы CustomerDisplay
//---------------------------------------------------------------------------//
bool CustomerDisplay::mStringOutputOnCustomerDisplay(tVariant* retVal, tVariant* deviceId,
                                                     tVariant* strings)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }

    std::vector<std::wstring> lines = xmlutil::ParseDisplayLines(getStrParam(strings, 0));
    if (lines.empty())
    {
        // нечего выводить — не считаем ошибкой
        setBoolToTVariant(retVal, true);
        setError(0, L"");
        return true;
    }

    cdp::DisplayContent content;
    content.mode = cdp::ScreenMode::Lines;
    cdp::BuildRowsFromLines(content, std::move(lines));
    dev->window->SetContent(content);
    dev->lastContent = content;
    LogLine(L"WINDOW StringOutput: " + dev->window->DescribeState());

    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mClearCustomerDisplay(tVariant* retVal, tVariant* deviceId)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }
    cdp::DisplayContent welcome;
    welcome.mode = cdp::ScreenMode::Welcome;
    dev->window->SetContent(welcome);
    dev->lastContent = welcome;
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mAddMediaFile(tVariant* retVal, tVariant* deviceId,
                                    tVariant* fileName, tVariant* fileData)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }

    std::wstring name = getStrParam(fileName, 0);
    std::wstring data = getStrParam(fileData, 0);

    // Пустые данные = удаление файла (стандарт, версия 4.8)
    if (data.empty())
    {
        std::wstring full = dev->mediaDir + L"\\" + SanitizeFileName(name);
        ::DeleteFileW(full.c_str());
        if (dev->lastContent.mode == cdp::ScreenMode::Media &&
            _wcsicmp(dev->lastContent.mediaPath.c_str(), full.c_str()) == 0)
        {
            cdp::DisplayContent welcome;
            welcome.mode = cdp::ScreenMode::Welcome;
            dev->window->SetContent(welcome);
            dev->lastContent = welcome;
        }
        setBoolToTVariant(retVal, true);
        setError(0, L"");
        return true;
    }

    std::vector<uint8_t> bytes = Base64Decode(data);
    if (bytes.empty())
    {
        setError(131, L"Не удалось декодировать данные файла (ожидается Base64)");
        setBoolToTVariant(retVal, false);
        return true;
    }
    if (name.empty())
    {
        SYSTEMTIME st; ::GetLocalTime(&st);
        wchar_t buf[64];
        ::swprintf(buf, 64, L"media_%04d%02d%02d_%02d%02d%02d.png",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        name = buf;
    }
    EnsureDirectory(dev->mediaDir);
    std::wstring full = dev->mediaDir + L"\\" + SanitizeFileName(name);
    HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        setError(132, L"Не удалось сохранить файл: " + full);
        setBoolToTVariant(retVal, false);
        return true;
    }
    DWORD written = 0;
    ::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    ::CloseHandle(h);

    cdp::DisplayContent content;
    content.mode = cdp::ScreenMode::Media;
    content.mediaPath = full;
    dev->window->SetContent(content);
    dev->lastContent = content;
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mOutputHTML(tVariant* retVal, tVariant* deviceId, tVariant* html)
{
    setError(140, L"Вывод HTML не поддерживается этой версией драйвера");
    setBoolToTVariant(retVal, false);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mQrCodeOutputOnCustomerDisplay(tVariant* retVal, tVariant* deviceId,
                                                     tVariant* qrCodeValue, tVariant* qrCodePicture)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }

    std::wstring value = getStrParam(qrCodeValue, 0);
    std::vector<uint8_t> png = Base64Decode(getStrParam(qrCodePicture, 0));
    if (value.empty() && png.empty())
    {
        setError(141, L"Не заданы ни значение QR-кода, ни картинка");
        setBoolToTVariant(retVal, false);
        return true;
    }

    cdp::DisplayContent content;
    content.mode = cdp::ScreenMode::QrCode;
    content.qrPng = std::move(png);
    content.qrValue = value;
    dev->window->SetContent(content);
    dev->lastContent = content;
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mDisplayOperationStatus(tVariant* retVal, tVariant* deviceId,
                                              tVariant* operationStatus)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }
    cdp::DisplayContent content;
    content.mode = cdp::ScreenMode::OperationStatus;
    content.statusCode = std::min(2L, std::max(0L, getLongParam(operationStatus, 0, 0)));
    dev->window->SetContent(content);
    dev->lastContent = content;
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mDisplayWorkplaceStatus(tVariant* retVal, tVariant* deviceId,
                                              tVariant* workplaceStatus, tVariant* soundSignal)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev || !EnsureDeviceWindow(dev))
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }
    cdp::DisplayContent content;
    content.mode = cdp::ScreenMode::WorkplaceStatus;
    content.statusCode = std::min(4L, std::max(0L, getLongParam(workplaceStatus, 0, 0)));
    bool beep = getLongParam(soundSignal, 0, 0) != 0;
    dev->window->SetContent(content);
    dev->lastContent = content;
    if (beep && dev->settings.soundEnabled)
        ::MessageBeep(MB_ICONASTERISK);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetCustomerDisplayOptions(tVariant* retVal, tVariant* deviceId,
                                                 tVariant* customerDisplayOptions)
{
    std::wstring id = getStrParam(deviceId, 0);
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = ResolveDevice(id, true);
    ::LeaveCriticalSection(&m_devicesLock);
    if (!dev)
    {
        setError(130, L"Устройство не подключено: " + id);
        setBoolToTVariant(retVal, false);
        return true;
    }
    wchar_t buf[64];
    ::swprintf(buf, 64, L"%d", dev->columns);
    std::wstring cols = buf;
    ::swprintf(buf, 64, L"%d", dev->rows);
    std::wstring rws = buf;

    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += L"<CustomerDisplayOptions ";
    xml += L"Columns=\"" + cols + L"\" ";
    xml += L"Rows=\"" + rws + L"\" ";
    xml += L"DisplayText=\"true\" ";
    xml += L"DisplayGraphics=\"true\" ";
    xml += L"DisplayQRCode=\"true\" ";
    xml += L"DisplayHTML=\"false\" ";
    xml += L"DisplayStatus=\"true\" ";
    xml += L"DisplayWorkplaceStatus=\"true\" ";
    xml += L"DisplayInteractiveMode=\"false\"/>";
    setWStringToTVariant(customerDisplayOptions, xml);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetInterfaceRevision(tVariant* retVal)
{
    // Ревизия 2005 — как в рабочем драйвере InnovaIT, чтобы старый интерфейс
    // (УстановитьПараметр/Подключить/Отключить) работал в типовой цепочке
    setLongToTVariant(retVal, 2005);
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetVersion(tVariant* retVal)
{
    setWStringToTVariant(retVal, g_extensionVersion);
    return true;
}
/////////////////////////////////////////////////////////////////////////////
// Методы старого интерфейса драйвера (шаблон InnovaIT, ревизия 2.004)
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetParametersLegacy(tVariant* retVal, tVariant* xml)
{
    setWStringToTVariant(xml, BuildSettingsXml());
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
namespace {
// БПО передаёт значения параметров нативными типами (bool/число/строка) —
// приводим всё к строке для хранилища настроек
std::wstring VariantToWString(tVariant* p)
{
    if (!p)
        return std::wstring();
    switch (TV_VT(p))
    {
    case VTYPE_PWSTR:
        {
            if (!TV_WSTR(p))
                return std::wstring();
            wchar_t* w = 0;
            ::convFromShortWchar(&w, TV_WSTR(p));
            std::wstring res = w;
            delete[] w;
            return res;
        }
    case VTYPE_BOOL:  return TV_BOOL(p) ? L"true" : L"false";
    case VTYPE_I4:    return std::to_wstring((long)TV_I4(p));
    case VTYPE_R4:    return std::to_wstring((double)p->fltVal);
    case VTYPE_R8:    return std::to_wstring(p->dblVal);
    default:          return std::wstring();
    }
}
} // anonymous namespace

bool CustomerDisplay::mSetParameterLegacy(tVariant* retVal, tVariant* name, tVariant* value)
{
    std::wstring paramName = getStrParam(name, 0);
    std::wstring paramValue = VariantToWString(value);
    if (paramName.empty())
    {
        setError(150, L"Не указано имя параметра");
        setBoolToTVariant(retVal, false);
        return true;
    }

    ::EnterCriticalSection(&m_devicesLock);
    m_legacyParams[paramName] = paramValue;
    // если дисплей уже открыт — применяем настройки на лету
    Device* dev = m_legacyDevice.get();
    if (dev && dev->window && dev->window->IsRunning())
    {
        cdp::DisplaySettings settings = dev->settings;
        int columns = dev->columns, rows = dev->rows;
        std::wstring settingsError;
        ParseSettings(m_legacyParams, settings, columns, rows, settingsError);
        dev->settings = settings;
        dev->columns = columns;
        dev->rows = rows;
        dev->window->ApplySettings(settings);
    }
    ::LeaveCriticalSection(&m_devicesLock);

    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mOpenLegacy(tVariant* retVal, tVariant* id)
{
    // Параметр может быть идентификатором устройства или XML с настройками
    std::wstring param = getStrParam(id, 0);

    ::EnterCriticalSection(&m_devicesLock);
    if (!param.empty() && param.find(L'<') != std::wstring::npos)
    {
        auto params = xmlutil::ParseParameters(param);
        for (const auto& kv : params)
            m_legacyParams[kv.first] = kv.second;
    }
    if (!m_legacyDevice)
    {
        m_legacyDevice.reset(new Device());
        m_legacyDevice->columns = 40;
        m_legacyDevice->rows = 10;
        std::wstring settingsError;
        ParseSettings(m_legacyParams, m_legacyDevice->settings,
                      m_legacyDevice->columns, m_legacyDevice->rows, settingsError);
    }
    Device* dev = m_legacyDevice.get();
    // запоминаем ID, под которым устройство знает вызывающая сторона (БПО)
    if (!param.empty() && param.find(L'<') == std::wstring::npos && dev->id.empty())
        dev->id = param;
    ::LeaveCriticalSection(&m_devicesLock);

    if (!dev->window)
        dev->window.reset(new cdp::DisplayWindow());

    if (!dev->window->IsRunning())
    {
        if (!dev->window->Start(dev->settings, L"Дисплей покупателя"))
        {
            std::wstring err = dev->window->LastError();
            setError(102, L"Не удалось открыть окно дисплея" +
                          (err.empty() ? std::wstring() : (L": " + err)));
            setBoolToTVariant(retVal, false);
            return true;
        }
        cdp::DisplayContent welcome;
        welcome.mode = cdp::ScreenMode::Welcome;
        dev->window->SetContent(welcome);
        dev->lastContent = welcome;
        LogLine(L"WINDOW Open: " + dev->window->DescribeState());
    }
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mCloseLegacy(tVariant* retVal, tVariant* id)
{
    ::EnterCriticalSection(&m_devicesLock);
    if (m_legacyDevice)
    {
        if (m_legacyDevice->window)
            m_legacyDevice->window->Stop();
        m_legacyDevice.reset();
    }
    ::LeaveCriticalSection(&m_devicesLock);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mDeviceTestLegacy(tVariant* retVal, tVariant* description,
                                        tVariant* demoModeIsActivated)
{
    // демонстрация на открытом дисплее или во временном окне
    ::EnterCriticalSection(&m_devicesLock);
    Device* dev = m_legacyDevice.get();
    ::LeaveCriticalSection(&m_devicesLock);

    if (dev && dev->window && dev->window->IsRunning())
    {
        RunDemo(dev->window.get());
        setWStringToTVariant(description,
            L"Демонстрация выведена на дисплей покупателя; рабочий экран вернётся через несколько секунд");
        setBoolToTVariant(demoModeIsActivated, false);
        setBoolToTVariant(retVal, true);
        setError(0, L"");
        return true;
    }

    cdp::DisplaySettings settings;
    int columns = 40, rows = 10;
    std::wstring settingsError;
    ::EnterCriticalSection(&m_devicesLock);
    ParseSettings(m_legacyParams, settings, columns, rows, settingsError);
    ::LeaveCriticalSection(&m_devicesLock);

    cdp::DisplayWindow* win = new cdp::DisplayWindow();
    if (!win->Start(settings, L"Дисплей покупателя — тест"))
    {
        std::wstring err = win->LastError();
        setError(103, L"Не удалось открыть окно дисплея" +
                      (err.empty() ? std::wstring() : (L": " + err)));
        delete win;
        setBoolToTVariant(retVal, false);
        return true;
    }
    RunDemo(win);

    struct Closer { static unsigned __stdcall run(void* p) {
        ::Sleep(8000);
        auto* w = (cdp::DisplayWindow*)p;
        w->Stop();
        delete w;
        return 0;
    } };
    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, Closer::run, win, 0, &tid);
    if (h)
        ::CloseHandle(h);

    setWStringToTVariant(description,
        L"Демонстрация выведена на монитор; окно закроется автоматически");
    setBoolToTVariant(demoModeIsActivated, false);
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
bool CustomerDisplay::mGetDescriptionOld(tVariant* retVal, tVariant* name, tVariant* description,
                                         tVariant* equipmentType, tVariant* interfaceRevision,
                                         tVariant* integrationLibrary,
                                         tVariant* mainDriverInstalled, tVariant* getDownloadURL)
{
    // Сигнатура и порядок параметров — как в шаблоне InnovaIT (ревизия 2.004)
    setWStringToTVariant(name, g_driverName);
    setWStringToTVariant(description, g_driverDescription);
    setWStringToTVariant(equipmentType, L"ДисплейПокупателя");
    setLongToTVariant(interfaceRevision, 2004);
    setBoolToTVariant(integrationLibrary, false);
    setBoolToTVariant(mainDriverInstalled, true);
    setWStringToTVariant(getDownloadURL, L"");
    setBoolToTVariant(retVal, true);
    setError(0, L"");
    return true;
}
//---------------------------------------------------------------------------//
