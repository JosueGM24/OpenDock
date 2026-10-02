/*
 * tray.c — los iconos de la bandeja de Windows (Tailscale, OneDrive…) para la barra.
 *
 * Cada Shell_NotifyIcon de una app va a la primera ventana de clase "Shell_TrayWnd" que
 * encuentra Windows (FindWindow, por orden Z). CornerRadius crea una propia, invisible y
 * siempre por encima de la de Explorer, en un hilo aparte: guarda una copia de cada icono
 * y le reenvía todo a Explorer, que sigue funcionando exactamente igual. Así la barra
 * puede pintar los iconos con su propio estilo y mandarles los clics como haría la barra
 * de tareas (lo mismo que hacen barras alternativas como RetroBar).
 *
 * Al arrancar se anuncia "TaskbarCreated" para que las apps vuelvan a registrar sus
 * iconos. Si Explorer se reinicia, la ventana propia se quita al momento (para no
 * confundirlo mientras arranca) y vuelve en cuanto su barra existe otra vez.
 */
#include "app.h"
#include <shellapi.h>
#include <stddef.h>

#define HOST_CLASS  L"Shell_TrayWnd"
#define TRAY_MAGIC  0x34753423
#define MAX_TRAY    48
#define CHECK_MS    2000

/* lo que manda shell32 (siempre con campos de 32 bits, también desde procesos de 64) */
typedef struct {
    DWORD cbSize, hWnd;
    UINT  uID, uFlags, uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState, dwStateMask;
    WCHAR szInfo[256];
    UINT  uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    DWORD hBalloonIcon;
} NID32;
typedef struct { DWORD sig, msg; NID32 nid; } TRAYMSG;
static const GUID kNoGuid = { 0 };

typedef struct {
    HWND  hwnd;
    UINT  uid, cb, ver;
    GUID  guid;
    BOOL  hasGuid, hidden;
    HICON icon;
    WCHAR tip[128];
} Slot;

static struct {
    CRITICAL_SECTION cs;
    BOOL   csInit;
    Slot   s[MAX_TRAY];
    int    n;
    HANDLE thread;
    DWORD  tid;
    HWND   host, explorer;
    HWND   notify;                  /* ventana a la que avisar de cambios */
    HWINEVENTHOOK hook;
    UINT   wmCreated;
    UINT_PTR timer;
} T;

/* La barra de tareas de verdad: la de Explorer, no la nuestra. */
HWND Tray_Explorer(void)
{
    HWND w = NULL;
    const DWORD me = GetCurrentProcessId();
    while ((w = FindWindowExW(NULL, w, HOST_CLASS, NULL)) != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != me) return w;
    }
    return NULL;
}

static void Changed(void)
{
    if (T.notify) PostMessageW(T.notify, WM_TRAYCHANGED, 0, 0);
}

static int Find(const NID32 *n, BOOL byGuid)
{
    for (int i = 0; i < T.n; ++i) {
        if (byGuid) { if (T.s[i].hasGuid && IsEqualGUID(&T.s[i].guid, &n->guidItem)) return i; }
        else if (T.s[i].hwnd == (HWND)(ULONG_PTR)n->hWnd && T.s[i].uid == n->uID) return i;
    }
    return -1;
}

static void Apply(Slot *s, const NID32 *n)
{
    if (n->uFlags & NIF_MESSAGE) s->cb = n->uCallbackMessage;
    if ((n->uFlags & NIF_ICON) && n->hIcon) {
        /* copia propia: la app puede destruir el suyo cuando quiera */
        HICON c = CopyIcon((HICON)(ULONG_PTR)n->hIcon);
        if (c) { if (s->icon) DestroyIcon(s->icon); s->icon = c; }
    }
    if (n->uFlags & NIF_TIP) lstrcpynW(s->tip, n->szTip, 128);
    if ((n->uFlags & NIF_STATE) && (n->dwStateMask & NIS_HIDDEN)) s->hidden = (n->dwState & NIS_HIDDEN) != 0;
}

static BOOL Handle(DWORD msg, const NID32 *n, DWORD size)
{
    const BOOL byGuid = size >= offsetof(TRAYMSG, nid.hBalloonIcon) && (n->uFlags & NIF_GUID) &&
                        !IsEqualGUID(&n->guidItem, &kNoGuid);
    BOOL ok = TRUE;
    EnterCriticalSection(&T.cs);
    int i = Find(n, byGuid);
    switch (msg) {
    case NIM_ADD:
        if (i < 0) {
            if (T.n >= MAX_TRAY) { ok = FALSE; break; }
            i = T.n++;
            ZeroMemory(&T.s[i], sizeof(Slot));
            T.s[i].hwnd = (HWND)(ULONG_PTR)n->hWnd;
            T.s[i].uid = n->uID;
            if (byGuid) { T.s[i].guid = n->guidItem; T.s[i].hasGuid = TRUE; }
        }
        Apply(&T.s[i], n);
        break;
    case NIM_MODIFY:
        if (i < 0) { ok = FALSE; break; }
        Apply(&T.s[i], n);
        break;
    case NIM_DELETE:
        if (i < 0) { ok = FALSE; break; }
        if (T.s[i].icon) DestroyIcon(T.s[i].icon);
        MoveMemory(&T.s[i], &T.s[i + 1], (SIZE_T)(T.n - i - 1) * sizeof(Slot));
        --T.n;
        break;
    case NIM_SETVERSION:
        if (i < 0) { ok = FALSE; break; }
        T.s[i].ver = n->uVersion;
        break;
    default:
        ok = FALSE;
    }
    LeaveCriticalSection(&T.cs);
    if (ok) Changed();
    return ok;
}

static void KeepOnTop(void)
{
    if (!T.host) return;
    /* por encima de la de Explorer (así FindWindow da con esta) y con su mismo tamaño, por
     * si alguna app mira dónde está "la barra" */
    RECT r = { 0 };
    if (T.explorer) GetWindowRect(T.explorer, &r);
    SetWindowPos(T.host, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

static void CALLBACK ExplorerHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t);

static void DestroyHost(void)
{
    if (T.hook) { UnhookWinEvent(T.hook); T.hook = NULL; }
    if (T.host) { DestroyWindow(T.host); T.host = NULL; }
    T.explorer = NULL;
}

static void CreateHost(void)
{
    if (T.host) return;
    T.explorer = Tray_Explorer();
    if (!T.explorer) return;
    T.host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, HOST_CLASS, L"", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!T.host) return;
    KeepOnTop();
    DWORD pid = 0;
    GetWindowThreadProcessId(T.explorer, &pid);
    T.hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, NULL, ExplorerHook, pid, 0, WINEVENT_OUTOFCONTEXT);
    /* que las apps vuelvan a registrar sus iconos (ahora llegan aquí y se reenvían) */
    PostMessageW(HWND_BROADCAST, T.wmCreated, 0, 0);
}

/* Explorer se cerró o se reinicia: fuera la ventana propia mientras tanto. */
static void CALLBACK ExplorerHook(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t)
{
    (void)hk; (void)ev; (void)obj; (void)child; (void)th; (void)t;
    if (w && w == T.explorer) DestroyHost();
}

static void CALLBACK CheckTimer(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    (void)h; (void)m; (void)id; (void)t;
    if (T.host && (!T.explorer || !IsWindow(T.explorer))) DestroyHost();
    if (!T.host) CreateHost();
    else KeepOnTop();
}

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COPYDATA) {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)l;
        BOOL mine = FALSE, ok = FALSE;
        DWORD msg = 0;
        if (cd && cd->dwData == 1 && cd->lpData && cd->cbData >= offsetof(TRAYMSG, nid.szTip)) {
            const TRAYMSG *tm = (const TRAYMSG *)cd->lpData;
            if (tm->sig == TRAY_MAGIC) {
                TRAYMSG copy = { 0 };
                CopyMemory(&copy, tm, min((DWORD)sizeof(copy), cd->cbData));
                copy.nid.szTip[127] = 0;
                msg = copy.msg;
                ok = Handle(msg, &copy.nid, cd->cbData);
                mine = TRUE;
            }
        }
        /* todo sigue llegando a Explorer: su bandeja, las AppBars, Shell_NotifyIconGetRect… */
        DWORD_PTR res = 0;
        HWND ex = T.explorer && IsWindow(T.explorer) ? T.explorer : Tray_Explorer();
        const BOOL sent = ex && SendMessageTimeoutW(ex, WM_COPYDATA, w, l, SMTO_ABORTIFHUNG, 3000, &res);
        if (mine && ok && (msg == NIM_ADD || msg == NIM_MODIFY || msg == NIM_SETVERSION)) return TRUE;
        if (sent) return (LRESULT)res;
        return mine ? ok : 0;
    }
    if (m == T.wmCreated) return 0;
    if (m == WM_CLOSE) return 0;                    /* nadie de fuera la cierra */
    /* el resto de lo que las apps le mandan a "la barra de tareas", a la de verdad */
    if ((m == WM_COMMAND || (m >= WM_USER && m < WM_APP)) && T.explorer) {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(T.explorer, m, w, l, SMTO_ABORTIFHUNG, 1000, &res);
        return (LRESULT)res;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI HostThread(LPVOID u)
{
    HANDLE ready = (HANDLE)u;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = HOST_CLASS;
    RegisterClassExW(&wc);
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);      /* la cola existe antes de avisar */
    SetEvent(ready);
    CreateHost();
    T.timer = SetTimer(NULL, 0, CHECK_MS, CheckTimer);
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    if (T.timer) KillTimer(NULL, T.timer);
    DestroyHost();
    UnregisterClassW(HOST_CLASS, g_inst);
    return 0;
}

/* ───────────────────────── API ───────────────────────── */
void Tray_Start(HWND notify)
{
    if (!T.csInit) { InitializeCriticalSection(&T.cs); T.csInit = TRUE; }
    T.notify = notify;
    if (T.thread) return;
    T.wmCreated = RegisterWindowMessageW(L"TaskbarCreated");
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    T.thread = CreateThread(NULL, 0, HostThread, ready, 0, &T.tid);
    if (ready) { if (T.thread) WaitForSingleObject(ready, 2000); CloseHandle(ready); }
}

void Tray_Stop(void)
{
    T.notify = NULL;
    if (!T.thread) return;
    PostThreadMessageW(T.tid, WM_QUIT, 0, 0);
    WaitForSingleObject(T.thread, 3000);
    CloseHandle(T.thread);
    T.thread = NULL;
    EnterCriticalSection(&T.cs);
    for (int i = 0; i < T.n; ++i) if (T.s[i].icon) DestroyIcon(T.s[i].icon);
    T.n = 0;
    LeaveCriticalSection(&T.cs);
}

/* Copia de los iconos visibles (cada icono es una copia: quien llama la destruye). */
int Tray_Snapshot(TrayItem *out, int max)
{
    if (!T.csInit) return 0;
    int k = 0;
    EnterCriticalSection(&T.cs);
    for (int i = 0; i < T.n; ) {
        if (!IsWindow(T.s[i].hwnd)) {               /* la app se cerró sin borrar su icono */
            if (T.s[i].icon) DestroyIcon(T.s[i].icon);
            MoveMemory(&T.s[i], &T.s[i + 1], (SIZE_T)(T.n - i - 1) * sizeof(Slot));
            --T.n;
            continue;
        }
        if (!T.s[i].hidden && T.s[i].icon && k < max) {
            out[k].hwnd = T.s[i].hwnd;
            out[k].uid = T.s[i].uid;
            out[k].icon = CopyIcon(T.s[i].icon);
            lstrcpynW(out[k].tip, T.s[i].tip, 128);
            ++k;
        }
        ++i;
    }
    LeaveCriticalSection(&T.cs);
    return k;
}

/* Un clic como los de la barra de tareas: botón abajo, arriba y la "selección" o el menú
 * contextual, con el formato de la versión que pidió el icono. */
void Tray_Click(HWND hwnd, UINT uid, BOOL right)
{
    UINT cb = 0, ver = 0;
    BOOL found = FALSE;
    if (!T.csInit) return;
    EnterCriticalSection(&T.cs);
    for (int i = 0; i < T.n; ++i)
        if (T.s[i].hwnd == hwnd && T.s[i].uid == uid) { cb = T.s[i].cb; ver = T.s[i].ver; found = TRUE; break; }
    LeaveCriticalSection(&T.cs);
    if (!found || !cb || !IsWindow(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);                  /* para que su menú o su ventana se pongan delante */
    POINT pt;
    GetCursorPos(&pt);
    const UINT seq[4] = { WM_MOUSEMOVE, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? WM_RBUTTONUP : WM_LBUTTONUP,
                          right ? WM_CONTEXTMENU : NIN_SELECT };
    const int count = ver >= 3 ? 4 : 3;
    for (int k = 0; k < count; ++k) {
        const WPARAM wp = ver >= 4 ? MAKEWPARAM(pt.x, pt.y) : uid;
        const LPARAM lp = ver >= 4 ? MAKELPARAM(seq[k], uid) : seq[k];
        SendNotifyMessageW(hwnd, cb, wp, lp);
    }
}
