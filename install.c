/*
 * install.c — instalación por usuario, sin permisos de administrador.
 *
 *   %LOCALAPPDATA%\Programs\CornerRadius\CornerRadius.exe
 *   + inicio con Windows (HKCU\...\Run)
 *   + acceso directo en el menú Inicio (abre la configuración)
 *   + entrada en Configuración → Aplicaciones (desinstalación limpia)
 *
 * Es el mismo patrón que usan VS Code, Discord o Spotify en modo "usuario".
 */
#define COBJMACROS
#include "app.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>

#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CornerRadius"
#define EXE_NAME      L"CornerRadius.exe"

static BOOL KnownPath(REFKNOWNFOLDERID id, LPCWSTR tail, wchar_t *out)
{
    PWSTR base = NULL;
    BOOL ok = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &base))
              && lstrlenW(base) + lstrlenW(tail) + 2 < MAX_PATH;
    if (ok) wsprintfW(out, L"%s\\%s", base, tail);
    CoTaskMemFree(base);
    return ok;
}

static BOOL InstallDir(wchar_t *out)   { return KnownPath(&FOLDERID_UserProgramFiles, APP_NAME, out); }
static BOOL StartMenuLink(wchar_t *out) { return KnownPath(&FOLDERID_Programs, APP_NAME L".lnk", out); }

static BOOL InstalledExe(wchar_t *out)
{
    wchar_t dir[MAX_PATH];
    if (!InstallDir(dir) || lstrlenW(dir) + lstrlenW(EXE_NAME) + 2 >= MAX_PATH) return FALSE;
    wsprintfW(out, L"%s\\%s", dir, EXE_NAME);
    return TRUE;
}

static void SelfPath(wchar_t *out)
{
    out[0] = 0;
    GetModuleFileNameW(NULL, out, MAX_PATH);
}

BOOL Inst_IsRunningInstalled(void)
{
    wchar_t self[MAX_PATH], exe[MAX_PATH];
    SelfPath(self);
    return InstalledExe(exe) && CompareStringOrdinal(self, -1, exe, -1, TRUE) == CSTR_EQUAL;
}

BOOL Inst_Exists(void)
{
    wchar_t exe[MAX_PATH];
    return InstalledExe(exe) && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
}

/* Copia byte a byte (no CopyFile) para no arrastrar el flujo Zone.Identifier
 * de la descarga: así el inicio con Windows no muestra "¿Desea ejecutar este archivo?". */
static BOOL CopySelf(LPCWSTR dst)
{
    wchar_t self[MAX_PATH], tmp[MAX_PATH + 8];
    SelfPath(self);
    if (lstrlenW(dst) + 5 >= MAX_PATH) return FALSE;
    wsprintfW(tmp, L"%s.new", dst);

    HANDLE in = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return FALSE;
    HANDLE out = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok = out != INVALID_HANDLE_VALUE;

    static BYTE buf[64 * 1024];
    while (ok) {
        DWORD rd = 0, wr = 0;
        if (!ReadFile(in, buf, sizeof(buf), &rd, NULL)) { ok = FALSE; break; }
        if (!rd) break;
        ok = WriteFile(out, buf, rd, &wr, NULL) && wr == rd;
    }
    if (out != INVALID_HANDLE_VALUE) {
        if (ok) ok = FlushFileBuffers(out);
        CloseHandle(out);
    }
    CloseHandle(in);

    if (ok) ok = MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static void SetStr(HKEY k, LPCWSTR name, LPCWSTR v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((lstrlenW(v) + 1) * sizeof(wchar_t)));
}

static void SetDword(HKEY k, LPCWSTR name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

static void WriteUninstallInfo(LPCWSTR exe, LPCWSTR dir)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    wchar_t buf[MAX_PATH + 32];
    SYSTEMTIME st;
    GetLocalTime(&st);

    SetStr(k, L"DisplayName", APP_NAME);
    SetStr(k, L"DisplayVersion", APP_VERSION);
    SetStr(k, L"Publisher", APP_PUBLISHER);
    SetStr(k, L"InstallLocation", dir);
    wsprintfW(buf, L"\"%s\",0", exe);
    SetStr(k, L"DisplayIcon", buf);
    wsprintfW(buf, L"\"%s\" --uninstall", exe);
    SetStr(k, L"UninstallString", buf);
    SetStr(k, L"QuietUninstallString", buf);
    wsprintfW(buf, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    SetStr(k, L"InstallDate", buf);
    SetDword(k, L"NoModify", 1);
    SetDword(k, L"NoRepair", 1);

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa))
        SetDword(k, L"EstimatedSize", (fa.nFileSizeLow + 1023) / 1024);
    RegCloseKey(k);
}

static void CreateShortcut(LPCWSTR exe)
{
    wchar_t lnk[MAX_PATH];
    if (!StartMenuLink(lnk)) return;

    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)))
        return;
    IShellLinkW_SetPath(sl, exe);
    IShellLinkW_SetArguments(sl, L"--settings");
    IShellLinkW_SetDescription(sl, L"Esquinas redondeadas de pantalla \x2014 configuración");
    IShellLinkW_SetIconLocation(sl, exe, 0);

    IPersistFile *pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

/* ───────────────────────── Inicio con Windows ───────────────────────── */
BOOL Inst_IsStartup(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

void Inst_SetStartup(BOOL on, LPCWSTR exe)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t self[MAX_PATH], cmd[MAX_PATH + 4];
        if (!exe) { SelfPath(self); exe = self; }
        wsprintfW(cmd, L"\"%s\"", exe);
        SetStr(k, APP_NAME, cmd);
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

/* ───────────────────────── Instalar / desinstalar ───────────────────────── */
BOOL Inst_Install(wchar_t *outExe)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!InstallDir(dir) || !InstalledExe(exe)) return FALSE;
    CreateDirectoryW(dir, NULL);
    if (!Inst_IsRunningInstalled() && !CopySelf(exe)) return FALSE;

    Inst_SetStartup(TRUE, exe);
    WriteUninstallInfo(exe, dir);
    CreateShortcut(exe);
    lstrcpynW(outExe, exe, MAX_PATH);
    return TRUE;
}

/* Quita todo rastro en el registro y el menú Inicio (los archivos van aparte). */
void Inst_Unregister(void)
{
    Wn_RestoreBannersSync();
    Bar_ApplyClock(FALSE);
    Dock_RestoreTaskbar();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, APP_NAME);
        RegCloseKey(k);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_KEY);

    wchar_t lnk[MAX_PATH];
    if (StartMenuLink(lnk)) DeleteFileW(lnk);
}

/* Un .exe en ejecución no puede borrarse a sí mismo: si somos la copia instalada,
 * lanzamos una copia temporal que espera a que salgamos y borra la carpeta. */
void Inst_RemoveFiles(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!InstallDir(dir) || !InstalledExe(exe)) return;

    if (!Inst_IsRunningInstalled()) {
        DeleteFileW(exe);
        RemoveDirectoryW(dir);
        return;
    }

    wchar_t tmp[MAX_PATH], helper[MAX_PATH], cmd[MAX_PATH + 48];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n + 32 >= MAX_PATH) return;
    wsprintfW(helper, L"%sCornerRadius-uninstall.exe", tmp);
    if (!CopySelf(helper)) return;
    wsprintfW(cmd, L"\"%s\" --cleanup %lu", helper, GetCurrentProcessId());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(helper, cmd, NULL, NULL, FALSE, 0, NULL, tmp, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* Modo --cleanup: solo borra la carpeta de instalación conocida (nunca una ruta
 * recibida por línea de comandos) y solo si la app ya fue desregistrada. */
int Inst_Cleanup(DWORD waitPid)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return 1;
    }
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
    if (p) {
        WaitForSingleObject(p, 15000);
        CloseHandle(p);
    }

    wchar_t dir[MAX_PATH], exe[MAX_PATH], tmp[MAX_PATH + 8];
    if (!InstallDir(dir) || !InstalledExe(exe)) return 1;
    for (int i = 0; i < 40; ++i) {
        if (DeleteFileW(exe) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
        Sleep(250);
    }
    wsprintfW(tmp, L"%s.new", exe);
    DeleteFileW(tmp);
    RemoveDirectoryW(dir);
    return 0;
}
