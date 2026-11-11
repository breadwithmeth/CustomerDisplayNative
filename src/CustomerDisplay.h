// CustomerDisplay.h — внешняя компонента 1С «Дисплей покупателя».
// Реализует стандартный интерфейс драйвера подключаемого оборудования 1С
// (глава 2 «Общий контракт» и глава 3.3 «CustomerDisplay» методических
// рекомендаций i8104829) поверх технологии Native API.
//
// В качестве дисплея покупателя используется обычный монитор: компонента
// создаёт полноэкранное окно на выбранном мониторе (по умолчанию — втором).
#pragma once

#include "stdafx.h"
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include <stdio.h>
#include <wchar.h>
#include <string>
#include <locale>

#include "DisplayWindow.h"

///////////////////////////////////////////////////////////////////////////////
// class CustomerDisplay
class CustomerDisplay : public IComponentBase
{
public:
    enum Props
    {
        epVersion,
        epLast
    };

    enum Methods
    {
        emGetDescription = 0,
        emGetLastError,
        emEquipmentParameters,
        emConnectEquipment,
        emDisconnectEquipment,
        emEquipmentTest,
        emEquipmentAutoSetup,
        emSetApplicationInformation,
        emGetAdditionalActions,
        emDoAdditionalAction,
        emGetLocalizationPattern,
        emSetLocalization,
        emStringOutputOnCustomerDisplay,
        emClearCustomerDisplay,
        emAddMediaFile,
        emOutputHTML,
        emQrCodeOutputOnCustomerDisplay,
        emDisplayOperationStatus,
        emDisplayWorkplaceStatus,
        emGetCustomerDisplayOptions,
        emGetInterfaceRevision,
        emGetVersion,
        // ---- старый интерфейс драйвера (шаблон InnovaIT, ревизия 2.004) ----
        emGetParameters,
        emSetParameter,
        emOpen,
        emClose,
        emDeviceTest,
        emGetDescriptionOld,
        emLast
    };

    CustomerDisplay(void);
    virtual ~CustomerDisplay();

    // IInitDoneBase
    virtual bool ADDIN_API Init(void*);
    virtual bool ADDIN_API setMemManager(void* mem);
    virtual long ADDIN_API GetInfo();
    virtual void ADDIN_API Done();
    // ILanguageExtenderBase
    virtual bool ADDIN_API RegisterExtensionAs(WCHAR_T**);
    virtual long ADDIN_API GetNProps();
    virtual long ADDIN_API FindProp(const WCHAR_T* wsPropName);
    virtual const WCHAR_T* ADDIN_API GetPropName(long lPropNum, long lPropAlias);
    virtual bool ADDIN_API GetPropVal(const long lPropNum, tVariant* pvarPropVal);
    virtual bool ADDIN_API SetPropVal(const long lPropNum, tVariant* varPropVal);
    virtual bool ADDIN_API IsPropReadable(const long lPropNum);
    virtual bool ADDIN_API IsPropWritable(const long lPropNum);
    virtual long ADDIN_API GetNMethods();
    virtual long ADDIN_API FindMethod(const WCHAR_T* wsMethodName);
    virtual const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum,
                            const long lMethodAlias);
    virtual long ADDIN_API GetNParams(const long lMethodNum);
    virtual bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum,
                            tVariant* pvarParamDefValue);
    virtual bool ADDIN_API HasRetVal(const long lMethodNum);
    virtual bool ADDIN_API CallAsProc(const long lMethodNum,
                    tVariant* paParams, const long lSizeArray);
    virtual bool ADDIN_API CallAsFunc(const long lMethodNum,
                tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray);
    // внутренняя реализация (без журналирования) — вызывается из CallAsFunc
    bool CallAsFuncImpl(const long lMethodNum, tVariant* pvarRetValue,
                        tVariant* paParams, const long lSizeArray);
    // LocaleBase
    virtual void ADDIN_API SetLocale(const WCHAR_T* loc);

private:
    // ---- инфраструктура ----
    long findName(const wchar_t* names[], const wchar_t* name, const uint32_t size) const;
    void addError(uint32_t wcode, const wchar_t* source, const wchar_t* descriptor, long code);
    bool sendEvent(const wchar_t* msg, const wchar_t* data);
    void setWStringToTVariant(tVariant* dest, const std::wstring& source);
    void setBoolToTVariant(tVariant* dest, bool value);
    void setLongToTVariant(tVariant* dest, long value);
    static std::wstring getStrParam(tVariant* paParams, long index);
    static long getLongParam(tVariant* paParams, long index, long fallback);
    static bool getBoolParam(tVariant* paParams, long index, bool fallback);
    void setError(long code, const std::wstring& text);

    // ---- устройства ----
    struct Device
    {
        std::wstring id;
        cdp::DisplaySettings settings;
        int columns = 40;
        int rows = 10;
        std::wstring mediaDir;
        cdp::DisplayContent lastContent;        // зеркало текущего экрана
        std::unique_ptr<cdp::DisplayWindow> window;
    };
    Device* FindDevice(const std::wstring& id);
    // Разрешение устройства для методов дисплея: точный ID -> legacy-устройство
    // -> единственное подключённое -> ленивое создание (БПО присылает свой ID)
    Device* ResolveDevice(const std::wstring& id, bool allowLazyCreate);
    // Гарантирует живое окно устройства: если пользователь закрыл окно из
    // панели задач — переоткрывает и восстанавливает последний экран
    bool EnsureDeviceWindow(Device* dev);
    void DestroyDevice(const std::wstring& id);
    static bool ParseSettings(const std::map<std::wstring, std::wstring>& params,
                              cdp::DisplaySettings& settings, int& columns, int& rows,
                              std::wstring& errorText);
    static std::wstring GenerateDeviceId();
    bool RunDemo(cdp::DisplayWindow* window);   // включить демонстрационный цикл
    // Демонстрация на любом доступном устройстве (legacy/новое/лениво созданное)
    bool RunDemoOnAnyDevice();

    // ---- реализация методов контракта ----
    bool mGetDescription(tVariant* retVal, tVariant* driverDescription);
    bool mGetLastError(tVariant* retVal, tVariant* errorDescription);
    bool mEquipmentParameters(tVariant* retVal, tVariant* equipmentType, tVariant* tableParameters);
    bool mConnectEquipment(tVariant* retVal, tVariant* deviceId, tVariant* equipmentType,
                           tVariant* connectionParameters);
    bool mDisconnectEquipment(tVariant* retVal, tVariant* deviceId);
    bool mEquipmentTest(tVariant* retVal, tVariant* equipmentType, tVariant* connectionParameters,
                        tVariant* description);
    bool mEquipmentAutoSetup(tVariant* retVal, tVariant* equipmentType,
                             tVariant* connectionParametersIn, tVariant* connectionParametersOut,
                             tVariant* showAutoSetupWindow, tVariant* timeout);
    bool mSetApplicationInformation(tVariant* retVal, tVariant* applicationSettings);
    bool mGetAdditionalActions(tVariant* retVal, tVariant* tableActions);
    bool mDoAdditionalAction(tVariant* retVal, tVariant* actionName);
    bool mGetLocalizationPattern(tVariant* retVal, tVariant* localizationPattern);
    bool mSetLocalization(tVariant* retVal, tVariant* languageCode, tVariant* localizationPattern);
    bool mStringOutputOnCustomerDisplay(tVariant* retVal, tVariant* deviceId, tVariant* strings);
    bool mClearCustomerDisplay(tVariant* retVal, tVariant* deviceId);
    bool mAddMediaFile(tVariant* retVal, tVariant* deviceId, tVariant* fileName, tVariant* fileData);
    bool mOutputHTML(tVariant* retVal, tVariant* deviceId, tVariant* html);
    bool mQrCodeOutputOnCustomerDisplay(tVariant* retVal, tVariant* deviceId,
                                        tVariant* qrCodeValue, tVariant* qrCodePicture);
    bool mDisplayOperationStatus(tVariant* retVal, tVariant* deviceId, tVariant* operationStatus);
    bool mDisplayWorkplaceStatus(tVariant* retVal, tVariant* deviceId,
                                 tVariant* workplaceStatus, tVariant* soundSignal);
    bool mGetCustomerDisplayOptions(tVariant* retVal, tVariant* deviceId,
                                    tVariant* customerDisplayOptions);
    bool mGetInterfaceRevision(tVariant* retVal);
    bool mGetVersion(tVariant* retVal);

    // ---- старый интерфейс драйвера (шаблон InnovaIT) ----
    bool mGetParametersLegacy(tVariant* retVal, tVariant* xml);
    bool mSetParameterLegacy(tVariant* retVal, tVariant* name, tVariant* value);
    bool mOpenLegacy(tVariant* retVal, tVariant* id);
    bool mCloseLegacy(tVariant* retVal, tVariant* id);
    bool mDeviceTestLegacy(tVariant* retVal, tVariant* description, tVariant* demoModeIsActivated);
    bool mGetDescriptionOld(tVariant* retVal, tVariant* name, tVariant* description,
                            tVariant* equipmentType, tVariant* interfaceRevision,
                            tVariant* integrationLibrary, tVariant* mainDriverInstalled,
                            tVariant* getDownloadURL);
    std::wstring BuildSettingsXml() const;

    // ---- атрибуты ----
    IAddInDefBase* m_iConnect;
    IMemoryManager* m_iMemory;

    CRITICAL_SECTION m_devicesLock;
    std::vector<std::unique_ptr<Device>> m_devices;
    // устройство старого интерфейса (Подключить/Отключить без DeviceID)
    std::unique_ptr<Device> m_legacyDevice;
    std::map<std::wstring, std::wstring> m_legacyParams;
    std::wstring m_lastError;
    long m_lastErrorCode = 0;
};
