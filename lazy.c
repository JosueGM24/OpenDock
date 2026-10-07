/*
 * lazy.c — carga diferida de las DLL del sistema que no son KnownDLLs.
 *
 * version.dll, winmm.dll, dwmapi.dll, wlanapi.dll y bthprops.cpl no están en la lista de
 * KnownDLLs: si el .exe las importara de forma estática, el cargador las buscaría primero en
 * la carpeta del propio .exe (Descargas, %TEMP%…) antes de que Harden() pueda restringir la
 * búsqueda, y una DLL plantada ahí se cargaría dentro de OpenDock.
 *
 * Aquí se definen los punteros de importación (__imp_X) que usa el resto del código: apuntan
 * a un trampolín que carga la DLL solo desde System32 la primera vez, se sustituye por la
 * función real y la llama. Si no se puede cargar, devuelve el error de "no disponible".
 * (Con MSVC no hace falta: build.bat enlaza con /DEPENDENTLOADFLAG:0x800.)
 */
#include <windows.h>

static FARPROC Resolve(LPCWSTR dll, LPCSTR name)
{
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? GetProcAddress(m, name) : NULL;
}

/* LAZY(dll, tipo, nombre, fallo, (parámetros), (argumentos)) */
#define LAZY(dll, ret, name, fail, params, args)                                 \
    static ret WINAPI lazy_##name params;                                        \
    void *__imp_##name = (void *)lazy_##name;                                    \
    static ret WINAPI lazy_##name params                                         \
    {                                                                            \
        FARPROC real_ = Resolve(dll, #name);                                         \
        if (!real_) return fail;                                                     \
        __imp_##name = (void *)real_;                                               \
        return ((ret (WINAPI *) params)(void *)real_) args;                          \
    }
#define LAZYV(dll, name, params, args)                                           \
    static void WINAPI lazy_##name params;                                       \
    void *__imp_##name = (void *)lazy_##name;                                    \
    static void WINAPI lazy_##name params                                        \
    {                                                                            \
        FARPROC real_ = Resolve(dll, #name);                                         \
        if (!real_) return;                                                        \
        __imp_##name = (void *)real_;                                               \
        ((void (WINAPI *) params)(void *)real_) args;                                \
    }

/* Para las cabeceras de mingw que no declaran dllimport (bcrypt, wintrust): la función con
 * su nombre, que resuelve la real la primera vez. */
#define LAZYF(dll, ret, name, fail, params, args)                                    ret WINAPI name params                                                           {                                                                                    static FARPROC real_;                                                            if (!real_) real_ = Resolve(dll, #name);                                         if (!real_) return fail;                                                         return ((ret (WINAPI *) params)(void *)real_) args;                          }

#define NA ERROR_PROC_NOT_FOUND

/* version.dll */
LAZY(L"version.dll", DWORD, GetFileVersionInfoSizeW, 0, (LPCWSTR f, LPDWORD h), (f, h))
LAZY(L"version.dll", BOOL, GetFileVersionInfoW, FALSE, (LPCWSTR f, DWORD h, DWORD n, LPVOID d), (f, h, n, d))
LAZY(L"version.dll", BOOL, VerQueryValueW, FALSE, (LPCVOID b, LPCWSTR s, LPVOID *p, PUINT n), (b, s, p, n))

/* winmm.dll */
LAZY(L"winmm.dll", BOOL, PlaySoundW, FALSE, (LPCWSTR s, HMODULE m, DWORD f), (s, m, f))

/* dwmapi.dll */
LAZY(L"dwmapi.dll", HRESULT, DwmGetWindowAttribute, E_FAIL, (HWND w, DWORD a, PVOID v, DWORD n), (w, a, v, n))
LAZY(L"dwmapi.dll", HRESULT, DwmSetWindowAttribute, E_FAIL, (HWND w, DWORD a, LPCVOID v, DWORD n), (w, a, v, n))
/* miniaturas en vivo de las ventanas (vista previa del dock) */
LAZY(L"dwmapi.dll", HRESULT, DwmRegisterThumbnail, E_FAIL, (HWND d, HWND s, HANDLE *t), (d, s, t))
LAZY(L"dwmapi.dll", HRESULT, DwmUnregisterThumbnail, E_FAIL, (HANDLE t), (t))
LAZY(L"dwmapi.dll", HRESULT, DwmUpdateThumbnailProperties, E_FAIL, (HANDLE t, const void *p), (t, p))
LAZY(L"dwmapi.dll", HRESULT, DwmQueryThumbnailSourceSize, E_FAIL, (HANDLE t, SIZE *s), (t, s))

/* bthprops.cpl (Bluetooth) */
LAZY(L"bthprops.cpl", HANDLE, BluetoothFindFirstRadio, NULL, (const void *p, HANDLE *r), (p, r))
LAZY(L"bthprops.cpl", BOOL, BluetoothFindRadioClose, FALSE, (HANDLE f), (f))
LAZY(L"bthprops.cpl", HANDLE, BluetoothFindFirstDevice, NULL, (const void *p, void *d), (p, d))
LAZY(L"bthprops.cpl", BOOL, BluetoothFindNextDevice, FALSE, (HANDLE f, void *d), (f, d))
LAZY(L"bthprops.cpl", BOOL, BluetoothFindDeviceClose, FALSE, (HANDLE f), (f))
LAZY(L"bthprops.cpl", BOOL, BluetoothIsConnectable, FALSE, (HANDLE r), (r))

/* iphlpapi.dll (red por cable) */
LAZY(L"iphlpapi.dll", ULONG, GetAdaptersAddresses, ERROR_NOT_SUPPORTED, (ULONG f, ULONG fl, PVOID r, PVOID a, PULONG n), (f, fl, r, a, n))
LAZY(L"iphlpapi.dll", DWORD, NotifyIpInterfaceChange, ERROR_NOT_SUPPORTED, (USHORT f, PVOID cb, PVOID ctx, BOOLEAN init, HANDLE *h), (f, cb, ctx, init, h))

/* wlanapi.dll (Wi-Fi) */
LAZY(L"wlanapi.dll", DWORD, WlanOpenHandle, NA, (DWORD v, PVOID r, PDWORD n, PHANDLE h), (v, r, n, h))
LAZY(L"wlanapi.dll", DWORD, WlanCloseHandle, NA, (HANDLE h, PVOID r), (h, r))
LAZY(L"wlanapi.dll", DWORD, WlanEnumInterfaces, NA, (HANDLE h, PVOID r, PVOID *l), (h, r, l))
LAZYV(L"wlanapi.dll", WlanFreeMemory, (PVOID p), (p))
LAZY(L"wlanapi.dll", DWORD, WlanQueryInterface, NA, (HANDLE h, const GUID *g, int op, PVOID r, PDWORD n, PVOID *d, int *t), (h, g, op, r, n, d, t))
LAZY(L"wlanapi.dll", DWORD, WlanGetAvailableNetworkList, NA, (HANDLE h, const GUID *g, DWORD f, PVOID r, PVOID *l), (h, g, f, r, l))
LAZY(L"wlanapi.dll", DWORD, WlanConnect, NA, (HANDLE h, const GUID *g, const void *p, PVOID r), (h, g, p, r))
LAZY(L"wlanapi.dll", DWORD, WlanScan, NA, (HANDLE h, const GUID *g, const void *s, const void *ie, PVOID r), (h, g, s, ie, r))
LAZY(L"wlanapi.dll", DWORD, WlanRegisterNotification, NA, (HANDLE h, DWORD src, BOOL ign, PVOID cb, PVOID ctx, PVOID r, PDWORD prev), (h, src, ign, cb, ctx, r, prev))

/* winhttp.dll, bcrypt.dll, wintrust.dll (actualizador: solo si el usuario lo activa o lo pide) */
LAZY(L"winhttp.dll", HANDLE, WinHttpOpen, NULL, (LPCWSTR a, DWORD t, LPCWSTR p, LPCWSTR b, DWORD f), (a, t, p, b, f))
LAZY(L"winhttp.dll", HANDLE, WinHttpConnect, NULL, (HANDLE s, LPCWSTR h, WORD p, DWORD r), (s, h, p, r))
LAZY(L"winhttp.dll", HANDLE, WinHttpOpenRequest, NULL, (HANDLE c, LPCWSTR v, LPCWSTR o, LPCWSTR ver, LPCWSTR ref, LPCWSTR *acc, DWORD f), (c, v, o, ver, ref, acc, f))
LAZY(L"winhttp.dll", BOOL, WinHttpSendRequest, FALSE, (HANDLE r, LPCWSTR h, DWORD hl, LPVOID o, DWORD ol, DWORD tl, DWORD_PTR ctx), (r, h, hl, o, ol, tl, ctx))
LAZY(L"winhttp.dll", BOOL, WinHttpReceiveResponse, FALSE, (HANDLE r, LPVOID x), (r, x))
LAZY(L"winhttp.dll", BOOL, WinHttpQueryHeaders, FALSE, (HANDLE r, DWORD l, LPCWSTR n, LPVOID b, LPDWORD bl, LPDWORD i), (r, l, n, b, bl, i))
LAZY(L"winhttp.dll", BOOL, WinHttpReadData, FALSE, (HANDLE r, LPVOID b, DWORD n, LPDWORD got), (r, b, n, got))
LAZY(L"winhttp.dll", BOOL, WinHttpCloseHandle, FALSE, (HANDLE h), (h))
LAZY(L"winhttp.dll", BOOL, WinHttpSetTimeouts, FALSE, (HANDLE h, int a, int b, int c, int d), (h, a, b, c, d))
LAZY(L"winhttp.dll", BOOL, WinHttpCrackUrl, FALSE, (LPCWSTR u, DWORD l, DWORD f, LPVOID c), (u, l, f, c))
LAZYF(L"bcrypt.dll", LONG, BCryptOpenAlgorithmProvider, (LONG)0xC0000001, (PVOID *a, LPCWSTR id, LPCWSTR impl, ULONG f), (a, id, impl, f))
LAZYF(L"bcrypt.dll", LONG, BCryptCloseAlgorithmProvider, (LONG)0xC0000001, (PVOID a, ULONG f), (a, f))
LAZYF(L"bcrypt.dll", LONG, BCryptHash, (LONG)0xC0000001, (PVOID a, PUCHAR s, ULONG sl, PUCHAR in, ULONG il, PUCHAR out, ULONG ol), (a, s, sl, in, il, out, ol))
LAZYF(L"wintrust.dll", LONG, WinVerifyTrust, (LONG)0x800B0001, (HWND h, GUID *a, LPVOID d), (h, a, d))
