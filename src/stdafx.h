// stdafx.h — общие включения драйвера дисплея покупателя.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifdef _WINDOWS
    #include <windows.h>
    #include <process.h>
#endif

#include <objbase.h>
#include <ocidl.h>
#include <GdiPlus.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>

// Библиотеки для линковки
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

#if defined(__linux__) || defined(__APPLE__)
    #define LINUX_OR_MACOS
#endif
