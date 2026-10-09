#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include "../engine/engine.h"
#include "../common/util.h"
#include "../common/config.h"
#include "../uimmi/uimmi.h"
#include "taskreg.h"

using namespace vd;

static Engine* g_eng = nullptr;
static HANDLE g_mutex = nullptr;
static HINSTANCE g_hInst = nullptr;
static HWND g_hwnd = nullptr;
static HWND g_tab = nullptr;
static HWND g_taskView = nullptr;
static HWND g_rdView = nullptr;
static HWND g_diagEdit = nullptr;
static HWND g_status = nullptr;
static HWND g_btnTask[12] = {};
static HWND g_btnDisk[4] = {};
static HWND g_btnDiag[5] = {};
static std::string g_lastMsg = "引擎已启动";
static std::string g_selTask;
static std::string g_selDisk;
static int g_timerId = 0;
static bool g_modal = false;

enum {
    IDT_ADD = 100, IDT_REMOVE, IDT_PAUSE, IDT_RESUME, IDT_FREEZE, IDT_UNFREEZE,
    IDT_FLUSH, IDT_CLEAR, IDT_L2VERIFY, IDT_PERF, IDT_FLUSHALL, IDT_CLEARALL,
    IDR_ADD = 200, IDR_REMOVE, IDR_READ, IDR_WRITE,
    IDD_DIAG = 300, IDD_STATS, IDD_NFO, IDD_VER, IDD_CLR,
    IDA_NAME = 400, IDA_OBJS, IDA_L1, IDA_L2, IDA_BS, IDA_L2DIR, IDA_L2NAME,
    IDA_STRAT, IDA_MODE, IDA_DEFER, IDA_DEFERWRITE, IDA_RELEASE,
    IDA_IND, IDA_L1W, IDA_L2W, IDA_L1TO2,
    IDA_BUSYINT, IDA_IGNORE, IDA_L2RESET, IDA_L2SKIP,
    IDA_STANDBY, IDA_SKIPFLUSH, IDA_NUMA,
    IDA_PF, IDA_PFBOOT, IDA_PFLOCK, IDA_PFL2,
    IDA_OK, IDA_CANCEL,
    IDR2_NAME = 500, IDR2_CAP, IDR2_BLK, IDR2_OK, IDR2_CANCEL,
    IDRD_OFF = 600, IDRD_LEN, IDRD_EDIT, IDRD_READ, IDRD_CANCEL,
    IDRW_OFF = 700, IDRW_HEX, IDRW_OK, IDRW_CANCEL,
    IDT_SHOW = 900, IDT_AUTOON, IDT_AUTOTRAY, IDT_AUTOOFF, IDT_QUIT,
};

#define WM_TRAYICON (WM_USER + 1)
#ifndef WM_TASKBARCREATED
#define WM_TASKBARCREATED 0x02A0
#endif
static NOTIFYICONDATAW g_nid = {};
static int g_trayRetries = 0;
static int g_autoMode = 0;
static bool g_quitting = false;
static bool g_startTray = false;

static std::wstring W(const std::string& utf8) { return toWide(utf8); }

static HWND mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text, style, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, g_hInst, nullptr);
}
static HWND mkEdit(HWND p, int x, int y, int w, int h, int id, DWORD extra = 0) {
    return mk(p, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | extra,
              x, y, w, h, id);
}
static HWND mkBtn(HWND p, int x, int y, int w, int h, int id, const wchar_t* text) {
    return mk(p, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, h, id);
}
static HWND mkCheck(HWND p, int x, int y, int w, int h, int id, const wchar_t* text) {

    return mk(p, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, h, id);
}
static HWND mkLabel(HWND p, int x, int y, int w, int h, const wchar_t* text) {
    return mk(p, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, 0);
}
static HWND mkCombo(HWND p, int x, int y, int w, int h, int id,
                    const wchar_t* const* items, int sel = 0) {
    HWND c = mk(p, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                x, y, w, h, id);
    for (int i = 0; items[i]; ++i) {
        int r = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
        if (sel >= 0 && r == sel) SendMessageW(c, CB_SETCURSEL, r, 0);
    }
    return c;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::string normName(const std::string& utf8) {
    std::wstring w = toWide(utf8);
    for (wchar_t& c : w) {
        if (c == 0x3000) c = L' ';
        else if (c >= 0xFF01 && c <= 0xFF5E) c = (wchar_t)(c - 0xFEE0);
    }
    return fromWide(w);
}

static void setTxt(HWND h, const std::string& utf8) {
    SetWindowTextW(h, W(utf8).c_str());
}
static std::string getTxt(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::string();
    std::wstring w((size_t)n, 0);
    GetWindowTextW(h, &w[0], n + 1);
    return fromWide(w);
}
static bool getCheck(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void setCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static int comboSel(HWND h) { return (int)SendMessageW(h, CB_GETCURSEL, 0, 0); }

static UINT g_dpi = 96;
static HFONT g_font = nullptr;

static inline int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static HFONT makeFontFor(UINT dpi) {
    LOGFONTW lf = {};
    lf.lfHeight = MulDiv(-12, (int)dpi, 96);
    wcsncpy_s(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Microsoft YaHei", _TRUNCATE);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    return CreateFontIndirectW(&lf);
}
static int CALLBACK fontCb(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    wchar_t c[32] = {0};
    GetClassNameW(h, c, 32);
    if (wcscmp(c, L"SysListView32") == 0) EnumChildWindows(h, fontCb, (LPARAM)lp);
    return 1;
}
static void applyFontTo(HWND root, HFONT f) {
    SendMessageW(root, WM_SETFONT, (WPARAM)f, TRUE);
    EnumChildWindows(root, fontCb, (LPARAM)f);
}

static HWND g_zoomRoot = nullptr;
static double g_zoomRatio = 1.0;
struct ZoomItem { HWND h; int l, t, r, b; };
static std::vector<ZoomItem> g_zoomList;
static int CALLBACK zoomCb(HWND h, LPARAM) {
    RECT r;
    if (GetWindowRect(h, &r)) {
        ScreenToClient(g_zoomRoot, (POINT*)&r.left);
        ScreenToClient(g_zoomRoot, (POINT*)&r.right);
        g_zoomList.push_back({h, (int)std::lround(r.left * g_zoomRatio),
                             (int)std::lround(r.top * g_zoomRatio),
                             (int)std::lround(r.right * g_zoomRatio),
                             (int)std::lround(r.bottom * g_zoomRatio)});
    }
    return 1;
}
static void zoomChildren(HWND root, double ratio) {
    if (ratio <= 0.0) return;
    g_zoomRoot = root;
    g_zoomRatio = ratio;
    g_zoomList.clear();
    EnumChildWindows(root, zoomCb, 0);
    for (const auto& z : g_zoomList)
        SetWindowPos(z.h, nullptr, z.l, z.t, z.r - z.l, z.b - z.t, SWP_NOZORDER);
}

static void finishDialog(HWND h, int designW, int designH) {
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)g_dpi);
    if (g_dpi != 96) {
        SetWindowPos(h, nullptr, 0, 0, S(designW), S(designH), SWP_NOMOVE);
        zoomChildren(h, (double)g_dpi / 96.0);
    }
    applyFontTo(h, g_font);
}

// 把对话框放到主窗口中央, 并钳制在当前显示器工作区内(避免顶部/底部甩出屏幕)
static void centerDlgOverOwner(HWND h) {
    RECT wr = {0}, wo = {0};
    if (!GetWindowRect(h, &wr) || !GetWindowRect(g_hwnd, &wo)) return;
    int w = wr.right - wr.left, dh = wr.bottom - wr.top;
    int x = (wo.left + wo.right) / 2 - w / 2;
    int y = (wo.top + wo.bottom) / 2 - dh / 2;
    RECT wa = {0};
    HMONITOR hm = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {0};
    mi.cbSize = sizeof(mi);
    if (hm && GetMonitorInfoW(hm, &mi))
        wa = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, (void*)&wa, 0);
    if (w > wa.right - wa.left - 16)
        x = wa.left + 8;
    else
        x = max(wa.left + 8, min(x, wa.right - w - 8));
    if (dh > wa.bottom - wa.top - 16)
        y = wa.top + 8;
    else
        y = max(wa.top + 8, min(y, wa.bottom - dh - 8));
    SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void msgErr(HWND owner, const std::string& msg) {
    MessageBoxW(owner, W(msg).c_str(), L"VeloxDisk", MB_OK | MB_ICONWARNING);
}

static bool validName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

static std::vector<std::string> splitObjs(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ';' || c == '\n' || c == '\r' || c == ',') {
            std::string t = trim(cur);
            if (!t.empty()) v.push_back(t);
            cur.clear();
        } else cur += c;
    }
    std::string t = trim(cur);
    if (!t.empty()) v.push_back(t);
    return v;
}

static void hexDump(const std::vector<uint8_t>& v, std::string& out) {
    out.clear();
    for (size_t off = 0; off < v.size(); off += 16) {
        char hex[16 * 3 + 1] = {0};
        int h = 0;
        std::string ascii;
        for (size_t i = off; i < off + 16 && i < v.size(); ++i) {
            std::snprintf(hex + h, 3, "%02X ", v[i]);
            h += 3;
            ascii += (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '.';
        }
        char line[320];
        std::snprintf(line, sizeof(line), "%08zX  %-48s %s\n", off, hex, ascii.c_str());
        out += line;
    }
    if (out.empty()) out = "(empty)\n";
}

static bool hexToBytes(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    std::string c;
    for (char ch : s) if (!isspace((unsigned char)ch)) c += ch;
    if (c.empty() || c.size() % 2) return false;
    if (c.size() > 16 * 1024 * 1024) return false;
    auto val = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < c.size(); i += 2) {
        int hi = val(c[i]), lo = val(c[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

static void placeRow(HWND* btns, int n, int x0, int y0, int w, int h, int gap) {
    for (int i = 0; i < n; ++i)
        SetWindowPos(btns[i], nullptr, x0 + i * (w + gap), y0, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
}

static const int kTwidths[8] = {120, 70, 240, 150, 150, 70, 80, 110};
static const int kDwidths[6] = {160, 120, 120, 120, 100, 90};
static void scaleCols(HWND lv, const int* base, int n) {
    for (int i = 0; i < n; ++i)
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, i, S(base[i]));
}
static void lvInit(HWND lv, const std::vector<std::wstring>& cols, const int* widths) {
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (size_t n = 0; n < cols.size(); ++n) {
        col.pszText = (LPWSTR)cols[n].c_str();
        col.cx = widths[n];
        ListView_InsertColumn(lv, (int)n, &col);
    }
}
static void lvSet(HWND lv, int row, int col, const std::wstring& s) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = col;
    it.pszText = (LPWSTR)s.c_str();
    ListView_SetItem(lv, &it);
}
static std::string selName(HWND lv) {
    int idx = ListView_GetNextItem(lv, -1, LVIS_SELECTED);
    if (idx < 0) return "";
    wchar_t b[512] = {0};
    ListView_GetItemText(lv, idx, 0, b, 512);
    return fromWide(b);
}
// (Re)apply the ListView selection to the single row whose text equals `name`,
// clearing every other row's selection. Used only when the selection has
// genuinely drifted (e.g. a task was removed or its row index shifted), so the
// steady-state refresh performs no selection churn at all.
static void applySelByName(HWND lv, const std::string& name) {
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; ++i)
        ListView_SetItemState(lv, i, 0, LVIS_SELECTED); // clear all selection
    if (name.empty()) return;
    std::wstring w = W(name);
    for (int i = 0; i < n; ++i) {
        wchar_t b[512] = {0};
        ListView_GetItemText(lv, i, 0, b, 512);
        if (w == b) {
            ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED,
                                 LVIS_SELECTED | LVIS_FOCUSED);
            return;
        }
    }
}

// Update the task list IN PLACE instead of deleting and rebuilding it every
// tick. The old rebuild cleared the row selection on the first tick after the
// user picked a task, which made it impossible to operate on that task.
// Editing the existing rows in place leaves the selection intact; we only
// re-anchor it when it has genuinely drifted (e.g. a task was removed).
static void refreshTasks() {
    if (!g_taskView) return;
    std::string keep = selName(g_taskView); // currently selected row, by name
    std::vector<std::shared_ptr<Task>> tasks = g_eng->tasks();

    int have = ListView_GetItemCount(g_taskView);
    int want = static_cast<int>(tasks.size());
    // Shrink from the tail if tasks were removed.
    while (have > want) { ListView_DeleteItem(g_taskView, have - 1); --have; }
    // Append rows for newly added tasks.
    for (int i = have; i < want; ++i) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = (LPWSTR)W(tasks[i]->name).c_str();
        ListView_InsertItem(g_taskView, &it);
    }

    for (size_t i = 0; i < tasks.size(); ++i) {
        const Task& t = *tasks[i];
        const Stats& s = t.stats;
        int r = static_cast<int>(i);
        lvSet(g_taskView, r, 0, W(t.name));
        lvSet(g_taskView, r, 1, W(t.paused ? "已暂停" : (t.frozen ? "已冻结" : "活动")));
        std::string objs;
        for (size_t k = 0; k < t.objects.size() && k < 2; ++k)
            objs += (k ? ", " : "") + t.objects[k];
        if (t.objects.size() > 2) objs += " …+" + std::to_string(t.objects.size() - 2);
        lvSet(g_taskView, r, 2, W(objs));
        lvSet(g_taskView, r, 3, W(fmtSize((long long)t.cache.l1UsedBytes()) + " / "
                                          + fmtSize((long long)t.opts.l1Bytes)));
        lvSet(g_taskView, r, 4, W(fmtSize((long long)t.cache.l2UsedBytes()) + " / "
                                          + fmtSize((long long)t.opts.l2Bytes)));
        lvSet(g_taskView, r, 5, W(std::to_string((long long)s.dirtyBlocks.load())));
        long long rt = s.readTotal.load(), rc = s.bytesReadCache.load();
        std::string hr = rt > 0 ? std::to_string((int)(100.0 * (double)rc / (double)rt)) + "%" : "-";
        lvSet(g_taskView, r, 6, W(hr));
        lvSet(g_taskView, r, 7, W(fmtSize(rt)));
    }

    // Correct the selection only if it drifted (task removed / index shifted).
    // In the normal steady state this is a no-op, so the user's selection is
    // never disturbed.
    if (selName(g_taskView) != keep) applySelByName(g_taskView, keep);
    g_selTask = selName(g_taskView);
}

static void refreshDisks() {
    if (!g_rdView) return;
    std::string keep = selName(g_rdView); // currently selected row, by name
    std::vector<std::pair<std::string, const RamDisk*>> rds = g_eng->ramDisks();

    int have = ListView_GetItemCount(g_rdView);
    int want = static_cast<int>(rds.size());
    while (have > want) { ListView_DeleteItem(g_rdView, have - 1); --have; }
    for (int i = have; i < want; ++i) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = (LPWSTR)W(rds[i].first).c_str();
        ListView_InsertItem(g_rdView, &it);
    }

    for (size_t i = 0; i < rds.size(); ++i) {
        const RamDisk* d = rds[i].second;
        int r = static_cast<int>(i);
        lvSet(g_rdView, r, 0, W(rds[i].first));
        lvSet(g_rdView, r, 1, W(fmtSize((long long)d->capacityBlocks() * d->blockBytes())));
        lvSet(g_rdView, r, 2, W(fmtSize((long long)d->usedBytes())));
        lvSet(g_rdView, r, 3, W(fmtSize((long long)d->committedBytes())));
        lvSet(g_rdView, r, 4, W(fmtSize((long long)d->blockBytes())));
        lvSet(g_rdView, r, 5, W(std::to_string((long long)d->freeBlocks())));
    }

    if (selName(g_rdView) != keep) applySelByName(g_rdView, keep);
    g_selDisk = selName(g_rdView);
}

static void updateStatus() {
    if (!g_status) return;
    std::string base = "引擎: 运行中 (uptime " + std::to_string((long long)(g_eng->uptimeMs() / 1000))
                    + "s) | 任务 " + std::to_string(g_eng->tasks().size())
                    + " | RAM 盘 " + std::to_string(g_eng->ramDiskCount());
    if (!g_lastMsg.empty()) base += " | " + g_lastMsg;
    setTxt(g_status, base);
}

static void refreshAll() {
    refreshTasks();
    refreshDisks();
    updateStatus();
}

static void addTray() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"VeloxDisk 加速控制中心");
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) Shell_NotifyIconW(NIM_ADD, &g_nid);
    // Win8+ 需要版本协商: ADD -> SETVERSION -> ADD (按 MSDN 推荐顺序)
    NOTIFYICONDATAW nv = {};
    nv.cbSize = sizeof(nv);
    nv.hWnd = g_hwnd;
    nv.uID = 1;
    nv.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nv);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void removeTray() {
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}
static void showWindowFromTray() {
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static std::wstring exePath() {
    wchar_t b[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, b, MAX_PATH);
    return b;
}

// 依次迁移旧版本 (OpenCache / CacheBoost) 的注册表状态到 Software\VeloxDisk。
static void migrateOldRegistry() {
    static const wchar_t* const kOldKeys[] = { L"Software\\OpenCache", L"Software\\CacheBoost" };
    for (const wchar_t* oldPath : kOldKeys) {
        HKEY oldK;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, oldPath, 0, KEY_READ, &oldK) != ERROR_SUCCESS)
            continue;
        DWORD d = 0, sz = sizeof(d);
        bool hasOld = RegQueryValueExW(oldK, L"AutoMode", nullptr, nullptr, (BYTE*)&d, &sz) == ERROR_SUCCESS;
        RegCloseKey(oldK);
        if (hasOld) {
            HKEY newK;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VeloxDisk", 0, 0, 0,
                                KEY_READ | KEY_WRITE, nullptr, &newK, nullptr) == ERROR_SUCCESS) {
                DWORD nv = 0, nsz = sizeof(nv);
                bool hasNew = RegQueryValueExW(newK, L"AutoMode", nullptr, nullptr, (BYTE*)&nv, &nsz) == ERROR_SUCCESS;
                if (!hasNew)
                    RegSetValueExW(newK, L"AutoMode", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
                RegCloseKey(newK);
            }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, oldPath);
    }
}

static void regSetInt(const wchar_t* name, int v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VeloxDisk", 0, 0, 0,
                        KEY_READ | KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD d = (DWORD)v;
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
}
static int regGetInt(const wchar_t* name, int def) {
    HKEY k;
    int r = def;
    DWORD d = (DWORD)def, sz = sizeof(d);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\VeloxDisk", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)&d, &sz) == ERROR_SUCCESS) r = (int)d;
        RegCloseKey(k);
    }
    return r;
}
// 任务的创建/删除见 taskreg.h (Task Scheduler COM API)。
// 注意: 不能用 schtasks /TR 注册——其命令行解析会把含空格的路径在
// 空格处拆散 (Execute 只剩 "C:\Program"), 登录启动时报"文件找不到"。

static bool autoTaskExists() {
    std::wstring cl = L"schtasks.exe /Query /TN VeloxDisk";
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, nullptr, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}
static void setAutoMode(int mode) {
    bool ok;
    if (mode == 0) {
        if (!autoTaskExists()) {

            g_autoMode = 0;
            regSetInt(L"AutoMode", 0);
            g_lastMsg = "已取消开机启动";
            updateStatus();
            return;
        }
        ok = deleteAutoTask();
    } else {

        ok = registerAutoTask(exePath(), mode == 2 ? L"--tray" : nullptr);
    }
    if (ok) {
        g_autoMode = mode;
        regSetInt(L"AutoMode", mode);
        g_lastMsg = mode == 0 ? "已取消开机启动"
                              : mode == 1 ? "开机启动已启用(登录时以最高权限启动)"
                                          : "开机启动到托盘已启用(登录时以最高权限启动)";
    } else {
        g_lastMsg = "开机启动操作失败";
    }
    updateStatus();
}
static void trayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDT_SHOW, L"显示主窗口");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_autoMode == 1 ? MF_CHECKED : 0), IDT_AUTOON, L"开机启动");
    AppendMenuW(m, MF_STRING | (g_autoMode == 2 ? MF_CHECKED : 0), IDT_AUTOTRAY, L"开机启动到托盘");
    AppendMenuW(m, MF_STRING | (g_autoMode == 0 ? MF_CHECKED : 0), IDT_AUTOOFF, L"取消开机启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDT_QUIT, L"退出");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

static void layoutMain() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int Ww = rc.right, Hh = rc.bottom;
    SetWindowPos(g_status, nullptr, 0, Hh - S(24), Ww, S(24), SWP_NOZORDER);
    SetWindowPos(g_tab, nullptr, S(6), S(6), Ww - S(12), Hh - S(36), SWP_NOZORDER);
    int cur = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
    if (cur < 0) cur = 0;

    RECT full;
    GetClientRect(g_tab, &full);
    RECT ti = {};
    SendMessageW(g_tab, TCM_GETITEMRECT, 0, (LPARAM)&ti);
    int hdr = ti.bottom - ti.top;
    if (hdr <= 8 || hdr > 64) hdr = 28;

    int x = full.left + S(8) + S(6), y = full.top + hdr + S(6) + S(6);
    int w = full.right - full.left - S(16), h = full.bottom - full.top - hdr - S(10);

    for (int i = 0; i < 12; ++i) if (g_btnTask[i]) ShowWindow(g_btnTask[i], SW_HIDE);
    for (int i = 0; i < 4; ++i) if (g_btnDisk[i]) ShowWindow(g_btnDisk[i], SW_HIDE);
    for (int i = 0; i < 5; ++i) if (g_btnDiag[i]) ShowWindow(g_btnDiag[i], SW_HIDE);
    ShowWindow(g_taskView, SW_HIDE);
    ShowWindow(g_rdView, SW_HIDE);
    ShowWindow(g_diagEdit, SW_HIDE);

    if (cur == 0) {
        int listH = h - S(100);
        SetWindowPos(g_taskView, nullptr, x, y, w, listH, SWP_NOZORDER | SWP_SHOWWINDOW);
        placeRow(g_btnTask, 6, x, y + h - S(92), S(118), S(28), S(6));
        placeRow(g_btnTask + 6, 6, x, y + h - S(58), S(118), S(28), S(6));
    } else if (cur == 1) {
        int listH = h - S(92);
        SetWindowPos(g_rdView, nullptr, x, y, w, listH, SWP_NOZORDER | SWP_SHOWWINDOW);
        placeRow(g_btnDisk, 4, x, y + h - S(78), S(130), S(28), S(6));
    } else {
        placeRow(g_btnDiag, 5, x, y, S(130), S(28), S(6));
        SetWindowPos(g_diagEdit, nullptr, x, y + S(36), w, h - S(44), SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

static void actTask(const char* okMsg, bool (Engine::*fn)(const std::string&),
                    bool needSel = true) {
    std::string name = needSel ? selName(g_taskView) : "";
    if (needSel && name.empty()) { msgErr(g_hwnd, "请先在任务列表选择一个任务"); return; }
    if ((g_eng->*fn)(name)) {
        g_lastMsg = std::string(okMsg) + (name.empty() ? "" : " " + name);
    } else {
        g_lastMsg = std::string("操作失败: ") + name;
    }
    updateStatus();
}

static const size_t kBsList[] = {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288};

static bool doTaskAdd(HWND h) {
    HWND nameH = GetDlgItem(h, IDA_NAME);
    std::string rawName = nameH ? getTxt(nameH) : std::string("<null>");
    std::string name = trim(normName(rawName));
    if (!validName(name)) {
        msgErr(h, "任务名只能含字母/数字/._-，长度 1..64");
        return false;
    }
    std::vector<std::string> objs = splitObjs(getTxt(GetDlgItem(h, IDA_OBJS)));
    if (objs.empty()) { msgErr(h, "至少填写一个对象(文件/目录)"); return false; }
    if (objs.size() > 16) { msgErr(h, "对象最多 16 个"); return false; }

    long long l1 = Config::parseSize(getTxt(GetDlgItem(h, IDA_L1)), 0);
    long long l2 = Config::parseSize(getTxt(GetDlgItem(h, IDA_L2)), 0);
    if (l1 <= 0) { msgErr(h, "L1 大小必须大于 0"); return false; }
    if (l2 < 0) { msgErr(h, "L2 大小非法(可为 0 表示关闭)"); return false; }

    TieredCache::Options o;
    o.l1Bytes = (size_t)l1;
    o.l2Bytes = (size_t)l2;
    int bs = comboSel(GetDlgItem(h, IDA_BS));
    o.blockSize = kBsList[bs >= 0 && bs < (int)(sizeof(kBsList) / sizeof(kBsList[0])) ? bs : 4];
    static const TieredCache::Strategy kStrat[] = {
        TieredCache::Strategy::ReadWrite, TieredCache::Strategy::ReadOnly,
        TieredCache::Strategy::WriteOnly};

    int cs = comboSel(GetDlgItem(h, IDA_STRAT));
    o.strategy = kStrat[cs >= 0 && cs < 3 ? cs : 0];
    int mc = comboSel(GetDlgItem(h, IDA_MODE));
    o.writeMode = (TieredCache::WriteMode)(mc >= 0 && mc <= 4 ? mc : 0);

    std::string l2dir = trim(getTxt(GetDlgItem(h, IDA_L2DIR)));
    if (l2 > 0 && l2dir.empty()) {
        wchar_t ld[4096] = {0};
        GetEnvironmentVariableW(L"LOCALAPPDATA", ld, 4096);
        l2dir = fromWide(ld) + "\\VeloxDisk\\l2";
    }
    o.l2Dir = l2dir;
    std::string l2name = trim(getTxt(GetDlgItem(h, IDA_L2NAME)));
    o.l2BaseName = l2name.empty() ? name : l2name;

    auto num = [&](int id, long long lo, long long hi, long long def) -> long long {
        std::string s = getTxt(GetDlgItem(h, id));
        long long v = s.empty() ? def : strtoll(s.c_str(), nullptr, 10);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return v;
    };
    o.deferSeconds = (int)num(IDA_DEFER, 0, 3600, 10);
    o.l1WritePercent = (int)num(IDA_L1W, 0, 100, 50);
    o.l2WritePercent = (int)num(IDA_L2W, 0, 100, 50);
    o.l2BusyCollectIntervalS = (int)num(IDA_BUSYINT, 0, 250, 30);

    o.deferWrite = getCheck(GetDlgItem(h, IDA_DEFERWRITE));
    o.releaseAfterWrite = getCheck(GetDlgItem(h, IDA_RELEASE));
    o.independentRwSpace = getCheck(GetDlgItem(h, IDA_IND));
    o.ignoreBusy = getCheck(GetDlgItem(h, IDA_IGNORE));
    o.l1ToL2 = getCheck(GetDlgItem(h, IDA_L1TO2));
    o.flushOnStandby = getCheck(GetDlgItem(h, IDA_STANDBY));
    o.skipFlushOnShutdown = getCheck(GetDlgItem(h, IDA_SKIPFLUSH));
    o.numaAware = getCheck(GetDlgItem(h, IDA_NUMA));
    o.l2ResetOnBoot = getCheck(GetDlgItem(h, IDA_L2RESET));
    o.l2SkipVerifyOnCrash = getCheck(GetDlgItem(h, IDA_L2SKIP));
    o.prefetchLast = getCheck(GetDlgItem(h, IDA_PF));
    o.prefetchAtBoot = getCheck(GetDlgItem(h, IDA_PFBOOT));
    o.lockPrefetchContent = getCheck(GetDlgItem(h, IDA_PFLOCK));
    o.prefetchFromL2 = getCheck(GetDlgItem(h, IDA_PFL2));

    if (g_eng->addTask(name, objs, o)) {
        g_selTask = name;
        g_lastMsg = "任务已添加: " + name;
        return true;
    }
    msgErr(h, "添加失败: 任务名已存在或参数无效");
    return false;
}

static void openTaskDialog() {
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"VD.Dlg", W("添加加速任务").c_str(),
                             WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                             CW_USEDEFAULT, CW_USEDEFAULT, 680, 664,
                             g_hwnd, nullptr, g_hInst, nullptr);
    g_modal = true;
    EnableWindow(g_hwnd, FALSE);
    int x1 = 12, x2 = 352, fw = 220;
    auto L1 = [&](int y, const char* t) { mkLabel(h, x1, y, 130, 20, W(t).c_str()); };
    auto L2 = [&](int y, const char* t) { mkLabel(h, x2, y, 130, 20, W(t).c_str()); };
    auto E1 = [&](int y, int id, const char* def) { mkEdit(h, x1 + 136, y, 104, 24, id); setTxt(GetDlgItem(h, id), def); };
    auto E2 = [&](int y, int id, const char* def) { mkEdit(h, x2 + 136, y, 104, 24, id); setTxt(GetDlgItem(h, id), def); };
    auto C1 = [&](int y, int id) { mkCheck(h, x1, y, 200, 22, id, L""); };
    auto C2 = [&](int y, int id) { mkCheck(h, x2, y, 200, 22, id, L""); };
    auto G = [&](int y, int gh, const char* t) {
        mk(h, L"BUTTON", W(t).c_str(), WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 8, y, 656, gh, 0);
    };

    int y = 10;
    mkLabel(h, x1, y, 80, 20, W("任务名").c_str());
    mkEdit(h, x1 + 82, y, 460, 24, IDA_NAME);
    y += 28;
    mkLabel(h, x1, y, 80, 20, W("对象(多个用 ; 分隔, 最多16)").c_str());
    mkEdit(h, x1 + 8, y + 20, 628, 58, IDA_OBJS, ES_MULTILINE | ES_AUTOVSCROLL);
    y += 88;

    G(y, 84, "容量与粒度"); y += 24;
    L1(y, "L1 大小");  E1(y, IDA_L1, "256MB");
    L2(y, "L2 大小(0=关闭)"); E2(y, IDA_L2, "1GB");
    y += 28;
    L1(y, "块粒度");
    {
        static const wchar_t* bs[] = {L"4KB", L"8KB", L"16KB", L"32KB", L"64KB", L"128KB", L"256KB", L"512KB", nullptr};

        mkCombo(h, x1 + 136, y, 190, 200, IDA_BS, bs);
        SendMessageW(GetDlgItem(h, IDA_BS), CB_SETCURSEL, 4, 0);
    }
    L2(y, "策略");
    {
        static const wchar_t* st[] = {L"读写 ReadWrite", L"只读 ReadOnly", L"只写 WriteOnly", nullptr};
        mkCombo(h, x2 + 136, y, 170, 200, IDA_STRAT, st);
    }
    y += 28;
    L1(y, "写模式");
    {
        static const wchar_t* md[] = {L"Original(0)", L"Smart(1)", L"Idle(2)", L"Buffer(3)", L"Balanced(4)", nullptr};
        mkCombo(h, x1 + 136, y, 190, 200, IDA_MODE, md);
        SendMessageW(GetDlgItem(h, IDA_MODE), CB_SETCURSEL, 1, 0);
    }
    L2(y, "延迟写回(秒)"); E2(y, IDA_DEFER, "10");
    y += 28;
    L1(y, "L2 目录"); E1(y, IDA_L2DIR, "");
    L2(y, "L2 文件基名"); E2(y, IDA_L2NAME, "");

    y += 28;
    G(y, 116, "写策略"); y += 24;
    C1(y, IDA_DEFERWRITE); SetWindowTextW(GetDlgItem(h, IDA_DEFERWRITE), W("延迟写开关(关=立即写源)").c_str());
    C2(y, IDA_RELEASE); SetWindowTextW(GetDlgItem(h, IDA_RELEASE), W("写后释放(转 swap 预留)").c_str());
    y += 26;
    C1(y, IDA_IND); SetWindowTextW(GetDlgItem(h, IDA_IND), W("读写独立空间").c_str());
    C2(y, IDA_IGNORE); SetWindowTextW(GetDlgItem(h, IDA_IGNORE), W("忽略系统繁忙").c_str());
    y += 26;
    L1(y, "L1 写区占比%"); E1(y, IDA_L1W, "50");
    L2(y, "L2 写区占比%"); E2(y, IDA_L2W, "50");
    y += 26;
    C1(y, IDA_L1TO2); SetWindowTextW(GetDlgItem(h, IDA_L1TO2), W("L1 写池满时下移 L2").c_str());
    C2(y, IDA_STANDBY); SetWindowTextW(GetDlgItem(h, IDA_STANDBY), W("系统空闲时刷盘").c_str());

    y += 26;
    G(y, 64, "L2 与恢复"); y += 24;
    L1(y, "L2 忙时采集间隔(秒)"); E1(y, IDA_BUSYINT, "30");
    C2(y, IDA_SKIPFLUSH); SetWindowTextW(GetDlgItem(h, IDA_SKIPFLUSH), W("关机不刷写(有丢数据风险)").c_str());
    y += 26;
    C1(y, IDA_L2RESET); SetWindowTextW(GetDlgItem(h, IDA_L2RESET), W("启动时重置 L2").c_str());
    C2(y, IDA_L2SKIP); SetWindowTextW(GetDlgItem(h, IDA_L2SKIP), W("崩溃后跳过校验").c_str());

    y += 26;
    G(y, 84, "预取与内存"); y += 24;
    C1(y, IDA_PF); SetWindowTextW(GetDlgItem(h, IDA_PF), W("预取上次加速内容").c_str());
    C2(y, IDA_PFBOOT); SetWindowTextW(GetDlgItem(h, IDA_PFBOOT), W("开机时预取").c_str());
    y += 26;
    C1(y, IDA_PFLOCK); SetWindowTextW(GetDlgItem(h, IDA_PFLOCK), W("锁定预取内容(不淘汰)").c_str());
    C2(y, IDA_PFL2); SetWindowTextW(GetDlgItem(h, IDA_PFL2), W("从 L2 预取").c_str());
    y += 26;
    C1(y, IDA_NUMA); SetWindowTextW(GetDlgItem(h, IDA_NUMA), W("NUMA 感知").c_str());
    y += 28;

    setCheck(GetDlgItem(h, IDA_DEFERWRITE), true);
    setCheck(GetDlgItem(h, IDA_STANDBY), true);
    setCheck(GetDlgItem(h, IDA_NUMA), true);

    mkBtn(h, 240, y + 6, 100, 30, IDA_OK, W("确定").c_str());
    mkBtn(h, 350, y + 6, 100, 30, IDA_CANCEL, W("取消").c_str());
    finishDialog(h, 680, 664);
    centerDlgOverOwner(h);
}

static void onTaskRemove() {
    std::string name = selName(g_taskView);
    if (name.empty()) { msgErr(g_hwnd, "请先在任务列表选择一个任务"); return; }
    if (MessageBoxW(g_hwnd, W("确定移除任务 " + name + " ?").c_str(), L"VeloxDisk", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (g_eng->removeTask(name)) {
        g_selTask.clear();
        g_lastMsg = "任务已移除: " + name;
    } else g_lastMsg = "移除失败: " + name;
    refreshAll();
}

static void onTaskL2Verify() {
    std::string name = selName(g_taskView);
    if (name.empty()) { msgErr(g_hwnd, "请先在任务列表选择一个任务"); return; }
    int bad = g_eng->verifyL2Task(name);
    if (bad < 0) g_lastMsg = "L2 校验失败: 任务不存在或无 L2 层";
    else g_lastMsg = "L2 校验完成: " + name + " 丢弃坏块 " + std::to_string(bad);
    updateStatus();
}

static void onTaskPerf() {
    std::string name = selName(g_taskView);
    if (name.empty()) { msgErr(g_hwnd, "请先在任务列表选择一个任务"); return; }
    std::string r = g_eng->perfReport(name);
    if (r.empty()) { g_lastMsg = "性能统计失败"; updateStatus(); return; }
    setTxt(g_diagEdit, r);
    SendMessageW(g_tab, TCM_SETCURSEL, 2, 0);
    layoutMain();
    g_lastMsg = "性能: " + name;
    updateStatus();
}

static void onFlushAll() {
    size_t n = 0;
    for (const auto& t : g_eng->tasks()) if (g_eng->flushTask(t->name)) ++n;
    g_lastMsg = "全部刷盘: " + std::to_string(n) + " 个任务";
    updateStatus();
}
static void onClearAll() {
    size_t n = 0;
    for (const auto& t : g_eng->tasks()) if (g_eng->clearTask(t->name)) ++n;
    g_lastMsg = "全部清空: " + std::to_string(n) + " 个任务";
    refreshAll();
}

static const RamDisk* findDisk(const std::string& name) {
    for (const auto& kv : g_eng->ramDisks())
        if (kv.first == name) return kv.second;
    return nullptr;
}

static void doRamAddDlg() {
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"VD.Dlg", W("添加 RAM 盘").c_str(),
                             WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                             CW_USEDEFAULT, CW_USEDEFAULT, 420, 240,
                             g_hwnd, nullptr, g_hInst, nullptr);
    g_modal = true;
    EnableWindow(g_hwnd, FALSE);
    int x = 16, y = 16, fw = 280;
    mkLabel(h, x, y, 80, 20, W("名称").c_str());
    mkEdit(h, x + 84, y, fw, 24, IDR2_NAME);
    y += 32;
    mkLabel(h, x, y, 80, 20, W("容量").c_str());
    mkEdit(h, x + 84, y, fw, 24, IDR2_CAP);
    setTxt(GetDlgItem(h, IDR2_CAP), "1GB");
    y += 32;
    mkLabel(h, x, y, 80, 20, W("块大小").c_str());
    {
        static const wchar_t* bs[] = {L"4KB", L"8KB", L"16KB", L"32KB", L"64KB", L"128KB", L"256KB", L"512KB", nullptr};
        mkCombo(h, x + 84, y, fw, 200, IDR2_BLK, bs);
        SendMessageW(GetDlgItem(h, IDR2_BLK), CB_SETCURSEL, 4, 0);
    }
    y += 36;
    mkBtn(h, 100, y, 100, 30, IDR2_OK, W("确定").c_str());
    mkBtn(h, 210, y, 100, 30, IDR2_CANCEL, W("取消").c_str());
    finishDialog(h, 420, 240);
    centerDlgOverOwner(h);
}

static bool doRamAdd(HWND h) {
    std::string name = trim(normName(getTxt(GetDlgItem(h, IDR2_NAME))));
    if (!validName(name)) { msgErr(h, "名称只能含字母/数字/._-，长度 1..64"); return false; }
    long long cap = Config::parseSize(getTxt(GetDlgItem(h, IDR2_CAP)), 0);
    if (cap <= 0) { msgErr(h, "容量必须大于 0"); return false; }
    int bs = comboSel(GetDlgItem(h, IDR2_BLK));
    if (g_eng->addRamDisk(name, (size_t)cap,
                         kBsList[bs >= 0 && bs < 8 ? bs : 4])) {
        g_selDisk = name;
        g_lastMsg = "RAM 盘已添加: " + name;
        return true;
    }
    msgErr(h, "添加失败: 名称已存在或内存预留失败");
    return false;
}

static void onDiskRemove() {
    std::string name = selName(g_rdView);
    if (name.empty()) { msgErr(g_hwnd, "请先在列表选择一个 RAM 盘"); return; }
    if (MessageBoxW(g_hwnd, W("确定移除 RAM 盘 " + name + " ? 数据将丢失").c_str(),
                    L"VeloxDisk", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    if (g_eng->removeRamDisk(name)) {
        g_selDisk.clear();
        g_lastMsg = "RAM 盘已移除: " + name;
    } else g_lastMsg = "移除失败: " + name;
    refreshAll();
}

static void openReadDlg() {
    std::string name = selName(g_rdView);
    if (name.empty()) { msgErr(g_hwnd, "请先在列表选择一个 RAM 盘"); return; }
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"VD.Dlg",
                             W(("读取 RAM 盘 " + name).c_str()).c_str(),
                             WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                             CW_USEDEFAULT, CW_USEDEFAULT, 560, 460,
                             g_hwnd, nullptr, g_hInst, nullptr);
    g_modal = true;
    EnableWindow(g_hwnd, FALSE);
    int x = 16, y = 16;
    mkLabel(h, x, y, 80, 20, W("偏移(字节)").c_str());
    mkEdit(h, x + 84, y, 160, 24, IDRD_OFF);
    setTxt(GetDlgItem(h, IDRD_OFF), "0");
    mkLabel(h, x + 260, y, 60, 20, W("长度(1..4096)").c_str());
    mkEdit(h, x + 324, y, 80, 24, IDRD_LEN);
    setTxt(GetDlgItem(h, IDRD_LEN), "4096");
    y += 34;
    mkEdit(h, x, y, 512, 300, IDRD_EDIT, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY);
    setTxt(GetDlgItem(h, IDRD_EDIT), "(点击 读取 填充内容)\n");
    y += 310;
    mkBtn(h, 260, y, 100, 30, IDRD_READ, W("读取").c_str());
    mkBtn(h, 370, y, 100, 30, IDRD_CANCEL, W("关闭").c_str());
    finishDialog(h, 560, 460);
    centerDlgOverOwner(h);
}

static void doRamRead(HWND h) {
    std::string name = g_selDisk.empty() ? selName(g_rdView) : g_selDisk;
    if (name.empty()) { msgErr(h, "未选择 RAM 盘"); return; }
    const RamDisk* d = findDisk(name);
    if (!d) { msgErr(h, "RAM 盘不存在"); return; }
    uint64_t off = strtoull(getTxt(GetDlgItem(h, IDRD_OFF)).c_str(), nullptr, 10);
    long long len = strtoll(getTxt(GetDlgItem(h, IDRD_LEN)).c_str(), nullptr, 10);
    if (len < 1 || len > 4096) { msgErr(h, "长度必须在 1..4096"); return; }
    if (off % d->blockBytes()) { msgErr(h, "偏移必须块对齐"); return; }
    std::vector<uint8_t> buf((size_t)len);
    if (!g_eng->ramDiskReadPartial(name, off, buf.data(), (size_t)len)) {
        msgErr(h, "读取失败(偏移可能越界)");
        return;
    }
    std::string dump;
    hexDump(buf, dump);
    setTxt(GetDlgItem(h, IDRD_EDIT), dump);
}

static void openWriteDlg() {
    std::string name = selName(g_rdView);
    if (name.empty()) { msgErr(g_hwnd, "请先在列表选择一个 RAM 盘"); return; }
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"VD.Dlg",
                             W(("写入 RAM 盘 " + name).c_str()).c_str(),
                             WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                             CW_USEDEFAULT, CW_USEDEFAULT, 560, 360,
                             g_hwnd, nullptr, g_hInst, nullptr);
    g_modal = true;
    EnableWindow(g_hwnd, FALSE);
    int x = 16, y = 16;
    mkLabel(h, x, y, 80, 20, W("偏移(字节)").c_str());
    mkEdit(h, x + 84, y, 160, 24, IDRW_OFF);
    setTxt(GetDlgItem(h, IDRW_OFF), "0");
    y += 32;
    mkLabel(h, x, y, 80, 20, W("内容(hex)").c_str());
    mkEdit(h, x, y + 22, 512, 130, IDRW_HEX, ES_MULTILINE | ES_AUTOVSCROLL);
    y += 168;
    mkBtn(h, 260, y, 100, 30, IDRW_OK, W("写入").c_str());
    mkBtn(h, 370, y, 100, 30, IDRW_CANCEL, W("关闭").c_str());
    finishDialog(h, 560, 360);
    centerDlgOverOwner(h);
}

static bool doRamWrite(HWND h) {
    std::string name = g_selDisk.empty() ? selName(g_rdView) : g_selDisk;
    if (name.empty()) { msgErr(h, "未选择 RAM 盘"); return false; }
    const RamDisk* d = findDisk(name);
    if (!d) { msgErr(h, "RAM 盘不存在"); return false; }
    uint64_t off = strtoull(getTxt(GetDlgItem(h, IDRW_OFF)).c_str(), nullptr, 10);
    std::vector<uint8_t> b;
    if (!hexToBytes(getTxt(GetDlgItem(h, IDRW_HEX)), b)) {
        msgErr(h, "hex 内容非法(需成对的十六进制字符)");
        return false;
    }
    size_t bb = d->blockBytes();
    if (off % bb) { msgErr(h, "偏移必须块对齐"); return false; }
    if (b.size() == 0 || b.size() % bb) {
        msgErr(h, "长度必须是块大小的整数倍(" + fmtSize((long long)bb) + ")");
        return false;
    }
    if (g_eng->ramDiskWrite(name, off, b.data(), b.size())) {
        g_lastMsg = "RAM 盘写入完成: " + name + " " + fmtSize((long long)b.size());
        return true;
    }
    msgErr(h, "写入失败(偏移可能越界或盘已满)");
    return false;
}

static void onDiag() { setTxt(g_diagEdit, g_eng->diagReport()); }

static void onStats() {
    std::string name = selName(g_taskView);
    if (name.empty()) { msgErr(g_hwnd, "请先在任务页选择一个任务"); return; }
    std::string r = g_eng->perfReport(name);
    if (r.empty()) { g_lastMsg = "统计失败"; updateStatus(); return; }
    setTxt(g_diagEdit, r);
    SendMessageW(g_tab, TCM_SETCURSEL, 2, 0);
    layoutMain();
    g_lastMsg = "统计: " + name;
    updateStatus();
}

static void onNfo() {
    wchar_t fn[MAX_PATH] = {0};
    wchar_t ld[4096] = {0};
    GetEnvironmentVariableW(L"LOCALAPPDATA", ld, 4096);
    std::wstring def(ld, wcslen(ld));
    def += L"\\VeloxDisk\\diag.nfo";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    static const wchar_t kFilter[] = L"NFO 文件 (*.nfo)\0*.nfo\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    std::wstring title = W("导出 NFO");
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = def.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (writeFile(fromWide(fn), g_eng->nfoText()))
        g_lastMsg = "NFO 已导出";
    else
        g_lastMsg = "NFO 导出失败: " + fromWide(fn);
    updateStatus();
}

static void onVer() {
    Uimmi::Info u = Uimmi::detect();
    std::string t = "VeloxDisk 1.0.0 (user-mode build)\n";
    t += "uptime=" + std::to_string((long long)(g_eng->uptimeMs() / 1000)) + "s\n";
    t += "os=" + u.describe() + "\n";
    t += "systemIdle=" + std::string(Engine::systemIdle() ? "yes" : "no") + "\n";
    t += "UIMMI 支持=" + std::string(u.supportedOs ? "是" : "否") + "\n";
    setTxt(g_diagEdit, t);
}

static LRESULT CALLBACK dlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDA_OK:
            if (doTaskAdd(h)) DestroyWindow(h);
            return 0;
        case IDA_CANCEL:
        case IDR2_CANCEL:
        case IDRD_CANCEL:
        case IDRW_CANCEL:
            DestroyWindow(h);
            return 0;
        case IDR2_OK:
            if (doRamAdd(h)) DestroyWindow(h);
            return 0;
        case IDRD_READ:
            doRamRead(h);
            return 0;
        case IDRW_OK:
            if (doRamWrite(h)) DestroyWindow(h);
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        // 静态标签: 背景与对话框灰色面一致, 文字用系统文字色
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_3DFACE));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
    }
    case WM_CTLCOLOREDIT: {
        // 编辑框: 标准白色背景 + 系统文字色(暗色主题下自动适配)
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DPICHANGED: {

        const RECT* sr = (const RECT*)lp;
        UINT oldDpi = (UINT)(UINT_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (oldDpi == 0) oldDpi = 96;
        UINT nd = LOWORD((UINT)wp);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)nd);
        SetWindowPos(h, nullptr, sr->left, sr->top, sr->right - sr->left,
                     sr->bottom - sr->top, SWP_NOZORDER);
        if (nd != oldDpi) zoomChildren(h, (double)nd / (double)oldDpi);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_modal) {
            g_modal = false;
            EnableWindow(g_hwnd, TRUE);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void stopEngine() {
    if (g_eng) {
        g_eng->stop();
        g_eng = nullptr;
    }
}

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == g_timerId) {
            refreshAll();
            // 开机登录瞬间任务与 explorer 同秒启动: 通知区域可能尚未建立,
            // 首个 NIM_ADD 会丢失。启动后 60 秒内每秒重注册一次
            // (NIM_ADD 对已存在的 (hWnd,uID) 等价于刷新, 幂等)。
            if (g_trayRetries < 60) {
                g_trayRetries++;
                Shell_NotifyIconW(NIM_ADD, &g_nid);
            }
        }
        return 0;
    case WM_SIZE:
        // Re-lay-out children on every resize; without this the listview
        // keeps covering the button rows after the window is resized.
        layoutMain();
        return 0;
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->hwndFrom == g_tab && nm->code == TCN_SELCHANGE) {
            layoutMain();
            return 0;
        }
        if (nm->hwndFrom == g_taskView && nm->code == LVN_ITEMCHANGED) {
            g_selTask = selName(g_taskView);
            return 0;
        }
        if (nm->hwndFrom == g_rdView && nm->code == LVN_ITEMCHANGED) {
            g_selDisk = selName(g_rdView);
            return 0;
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDT_ADD: openTaskDialog(); return 0;
        case IDT_REMOVE: onTaskRemove(); return 0;
        case IDT_PAUSE: actTask("任务已暂停", &Engine::pauseTask); return 0;
        case IDT_RESUME: actTask("任务已恢复", &Engine::resumeTask); return 0;
        case IDT_FREEZE: actTask("任务已冻结(读缓存保留, 写直通)", &Engine::freezeTask); return 0;
        case IDT_UNFREEZE: actTask("任务已解冻", &Engine::unfreezeTask); return 0;
        case IDT_FLUSH: actTask("任务已刷盘", &Engine::flushTask); return 0;
        case IDT_CLEAR: actTask("任务已清空", &Engine::clearTask); return 0;
        case IDT_L2VERIFY: onTaskL2Verify(); return 0;
        case IDT_PERF: onTaskPerf(); return 0;
        case IDT_FLUSHALL: onFlushAll(); return 0;
        case IDT_CLEARALL: onClearAll(); return 0;
        case IDR_ADD: doRamAddDlg(); return 0;
        case IDR_REMOVE: onDiskRemove(); return 0;
        case IDR_READ: openReadDlg(); return 0;
        case IDR_WRITE: openWriteDlg(); return 0;
        case IDD_DIAG: onDiag(); return 0;
        case IDD_STATS: onStats(); return 0;
        case IDD_NFO: onNfo(); return 0;
        case IDD_VER: onVer(); return 0;
        case IDD_CLR: setTxt(g_diagEdit, ""); return 0;
        case IDT_SHOW: showWindowFromTray(); return 0;
        case IDT_AUTOON: setAutoMode(1); return 0;
        case IDT_AUTOTRAY: setAutoMode(2); return 0;
        case IDT_AUTOOFF: setAutoMode(0); return 0;
        case IDT_QUIT:
            g_quitting = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;
    }
    case WM_DPICHANGED: {

        const RECT* sr = (const RECT*)lp;
        UINT nd = LOWORD((UINT)wp);
        g_dpi = nd;
        if (g_font) DeleteObject(g_font);
        g_font = makeFontFor(nd);
        applyFontTo(g_hwnd, g_font);
        SetWindowPos(hwnd, nullptr, sr->left, sr->top, sr->right - sr->left,
                     sr->bottom - sr->top, SWP_NOZORDER);
        layoutMain();
        scaleCols(g_taskView, kTwidths, 8);
        scaleCols(g_rdView, kDwidths, 6);
        return 0;
    }
    case WM_TRAYICON: {

        static bool ctxSent = false;
        int ev = LOWORD(lp);
        if (ev == WM_RBUTTONDOWN) {
            ctxSent = false;
        } else if (ev == WM_RBUTTONUP) {
            // Shell sends WM_RBUTTONDOWN/WM_RBUTTONUP *before* WM_CONTEXTMENU,
            // so show the menu on the button-up and mark it sent; the
            // subsequent WM_CONTEXTMENU is then skipped, avoiding a double
            // popup on a single right-click.
            if (!ctxSent) { ctxSent = true; trayMenu(); }
        } else if (ev == WM_CONTEXTMENU) {
            // Fallback for shells that deliver only WM_CONTEXTMENU.
            if (!ctxSent) { ctxSent = true; trayMenu(); }
        } else if (ev == WM_LBUTTONDBLCLK) {
            trayMenu();
        } else if (ev == WM_LBUTTONUP) {
            showWindowFromTray();
        }
        return 0;
    }
    case WM_TASKBARCREATED:
        addTray();
        g_trayRetries = 0;
        return 0;
    case WM_CLOSE:
        if (g_quitting) {
            KillTimer(hwnd, g_timerId);
            stopEngine();
            DestroyWindow(hwnd);
        } else {
            ShowWindow(hwnd, SW_HIDE);
            g_lastMsg = "已最小化到托盘(右键托盘图标管理)";
            updateStatus();
        }
        return 0;
    case WM_DESTROY:
        removeTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::string defaultCfg() {
    wchar_t buf[4096] = {0};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 4096);
    std::string base = fromWide(buf);
    std::string p = base + "\\VeloxDisk\\config.ini";
    // 旧版本目录回退（新到旧）: CacheBoost 时代 → OpenCache 时代
    copyFileIfAbsent(base + "\\CacheBoost\\config.ini", p);
    copyFileIfAbsent(base + "\\OpenCache\\config.ini", p);
    return p;
}

int APIENTRY wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hi;
    if (GetCommandLineW() && std::wstring(GetCommandLineW()).find(L"--tray") != std::wstring::npos)
        g_startTray = true;
    migrateOldRegistry();
    g_autoMode = regGetInt(L"AutoMode", 0);

    {
        std::wstring mf = exePath() + L".manifest";
        if (GetFileAttributesW(mf.c_str()) == INVALID_FILE_ATTRIBUTES) {
            writeFile(fromWide(mf),
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
                "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
                "    <security>\n"
                "      <requestedPrivileges>\n"
                "        <requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"false\"/>\n"
                "      </requestedPrivileges>\n"
                "    </security>\n"
                "  </trustInfo>\n"
                "</assembly>\n");
        }
    }

    {
        typedef BOOL (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
        SetCtxFn setCtx = (SetCtxFn)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (setCtx)
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else
            SetProcessDPIAware();
    }
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\VeloxDisk.Serve");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, W("已有 VeloxDisk 引擎在运行(vd.exe serve 或另一个 GUI 实例)。\n请先停止它再启动 GUI。").c_str(),
                    L"VeloxDisk", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VD.Main";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = dlgProc;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"VD.Dlg";
    RegisterClassExW(&wc);

    g_eng = new Engine();
    std::string cfg = defaultCfg();
    if (!g_eng->start(cfg)) {
        MessageBoxW(nullptr, W("引擎启动失败, 配置: " + cfg).c_str(),
                    L"VeloxDisk", MB_OK | MB_ICONERROR);
        delete g_eng;
        g_eng = nullptr;
        return 1;
    }

    g_hwnd = CreateWindowExW(0, L"VD.Main", W("VeloxDisk 加速控制中心").c_str(),
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1120, 720, nullptr, nullptr, hi, nullptr);
    g_dpi = GetDpiForWindow(g_hwnd);
    g_font = makeFontFor(g_dpi);
    g_tab = mk(g_hwnd, L"SysTabControl32", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 6, 6, 100, 100, 0);
    {
        std::vector<std::wstring> names = {W("任务"), W("RAM 盘"), W("诊断")};
        TCITEMW ti = {};
        ti.mask = TCIF_TEXT;
        for (int i = 0; i < 3; ++i) {
            ti.pszText = (LPWSTR)names[i].c_str();
            TabCtrl_InsertItem(g_tab, i, &ti);
        }
    }

    std::vector<std::wstring> tcols = {W("名称"), W("状态"), W("对象"),
                                       W("L1 已用/总"), W("L2 已用/总"),
                                       W("脏块"), W("读命中"), W("读取总量")};

    g_taskView = mk(g_hwnd, L"SysListView32", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                    0, 0, 100, 100, 0);
    lvInit(g_taskView, tcols, kTwidths);

    std::vector<std::wstring> dcols = {W("名称"), W("容量"), W("已用"),
                                       W("已提交"), W("块大小"), W("空闲块")};
    g_rdView = mk(g_hwnd, L"SysListView32", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                  0, 0, 100, 100, 0);
    lvInit(g_rdView, dcols, kDwidths);

    g_diagEdit = mk(g_hwnd, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                         ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0, 0, 100, 100, 0);

    g_status = mk(g_hwnd, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY, 0, 0, 100, 24, 0);

    std::vector<std::wstring> tbtns = {
        W("添加任务"), W("移除"), W("暂停"), W("恢复"),
        W("冻结"), W("解冻"),
        W("刷盘"), W("清空"), W("L2 校验"), W("性能"),
        W("全部刷盘"), W("全部清空")};
    for (int i = 0; i < 12; ++i) g_btnTask[i] = mkBtn(g_hwnd, 0, 0, 118, 28, IDT_ADD + i, tbtns[i].c_str());
    std::vector<std::wstring> dbtns = {W("添加 RAM 盘"), W("移除"), W("读取"), W("写入")};
    for (int i = 0; i < 4; ++i) g_btnDisk[i] = mkBtn(g_hwnd, 0, 0, 130, 28, IDR_ADD + i, dbtns[i].c_str());
    std::vector<std::wstring> ibtns = {W("完整诊断"), W("任务统计"), W("NFO 导出"),
                                       W("版本信息"), W("清空输出")};
    for (int i = 0; i < 5; ++i) g_btnDiag[i] = mkBtn(g_hwnd, 0, 0, 130, 28, IDD_DIAG + i, ibtns[i].c_str());

    applyFontTo(g_hwnd, g_font);
    layoutMain();
    refreshAll();
    addTray();
    if (g_startTray) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_lastMsg = "在托盘运行(右键托盘图标管理)";
        updateStatus();
    } else {
        ShowWindow(g_hwnd, nShow);
    }
    g_timerId = (int)SetTimer(g_hwnd, 1, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
