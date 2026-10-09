#pragma once
// 开机自启动任务注册 (Task Scheduler COM API)。
//
// 不使用 schtasks /TR: 其命令行解析会把含空格的路径在空格处拆散
// (Execute 只剩 "C:\Program"), 登录启动时报"文件找不到" (0x80070002)。
// 这里用任务 XML 直接注册: <Command> 与 <Arguments> 是独立元素,
// 不经过任何命令行拆分。
#include <windows.h>
#include <objbase.h>
#include <string>
#include <taskschd.h>

#ifndef TH_TASK_CREATE_ONLY
#define TH_TASK_CREATE_ONLY 0x1
#endif
#ifndef TH_TASK_CREATE_OR_UPDATE
#define TH_TASK_CREATE_OR_UPDATE 0x2
#endif

static const wchar_t* TASK_NAME = L"VeloxDisk";
// 旧命名 (OpenCache / CacheBoost 时代) 残留的自启动任务由 deleteLegacyTasks()
// 在注册/删除时一并清理, 避免升级后新旧任务并存。

// XML 转义: 安装路径含 & < > " ' 时, 不转义会生成非法 XML,
// RegisterTask 直接失败 (0x8007000B 等)。
static std::wstring xmlEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&apos;"; break;
        default: o += c;
        }
    }
    return o;
}

// 任务 XML: 登录触发器 + Administrator + 最高权限 + Exec 动作。
static std::wstring buildTaskXml(const std::wstring& exePath, const wchar_t* args) {
    const std::wstring cmd = xmlEscape(exePath);
    // 注意两点:
    // 1) 不写 <UserId>: 省略时任务以"创建任务的当前用户"身份运行 (InteractiveToken
    //    + HighestAvailable 在管理员登录时自动提升)。若硬编码 Administrator,
    //    在内置 Administrator 账户被禁用的系统 (Win10/11 默认) 上登录启动
    //    会直接失败 (logon failure), 开机自启静默失效。
    // 2) ExecutionTimeLimit 必须是 PT0S (无限制): Task Scheduler 默认 72 小时,
    //    GUI 是常驻托盘程序, 机器连续运行超过 3 天会被计划任务管理器杀掉。
    std::wstring x =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger>\r\n"
        L"      <Enabled>true</Enabled>\r\n"
        L"    </LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>" + cmd + L"</Command>\r\n";
    if (args && *args) x += L"      <Arguments>" + xmlEscape(args) + L"</Arguments>\r\n";
    x += L"    </Exec>\r\n"
         L"  </Actions>\r\n"
         L"</Task>";
    return x;
}

// 注册开机自启动任务 (替换已存在的同名任务)。
// exePath: 可执行文件路径(不含引号); args: 附加参数, 如 "--tray", 可为 nullptr。
// 清理旧版本（OpenCache / CacheBoost 时代）遗留的计划任务。
static void deleteLegacyTasks(ITaskFolder* root) {
    static const wchar_t* const kLegacyNames[] = { L"OpenCache", L"CacheBoost" };
    for (const wchar_t* legacy : kLegacyNames) {
        BSTR lb = SysAllocString(legacy);
        if (lb) { root->DeleteTask(lb, 0); SysFreeString(lb); }
    }
}

static bool registerAutoTask(const std::wstring& exePath, const wchar_t* args) {
    bool ok = false, didInit = false;
    ITaskService* svc = nullptr;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) didInit = true;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITaskService, reinterpret_cast<void**>(&svc)))) {
        VARIANT e1, e2, e3, e4, userId, pwd, sddl;
        VariantInit(&e1); VariantInit(&e2); VariantInit(&e3); VariantInit(&e4);
        VariantInit(&userId); VariantInit(&pwd); VariantInit(&sddl);
        if (SUCCEEDED(svc->Connect(e1, e2, e3, e4))) {
            ITaskFolder* root = nullptr;
            if (SUCCEEDED(svc->GetFolder(L"\\", &root))) {
                std::wstring xml = buildTaskXml(exePath, args);
                BSTR xb = SysAllocString(xml.c_str());
                BSTR nameB = SysAllocString(TASK_NAME);
                IRegisteredTask* rt = nullptr;
                if (xb && nameB)
                    ok = SUCCEEDED(root->RegisterTask(nameB, xb, TH_TASK_CREATE_OR_UPDATE,
                                                     userId, pwd, TASK_LOGON_NONE, sddl, &rt));
                if (!ok) {
                    // 覆盖已存在的任务可能返回 0x800700B7: 先删除再注册。
                    IRegisteredTask* rt2 = nullptr;
                    root->DeleteTask(nameB, 0);
                    ok = SUCCEEDED(root->RegisterTask(nameB, xb, TH_TASK_CREATE_OR_UPDATE,
                                                     userId, pwd, TASK_LOGON_NONE, sddl, &rt2));
                    if (rt2) rt2->Release();
                }
                if (xb) SysFreeString(xb);
                if (nameB) SysFreeString(nameB);
                if (rt) rt->Release();
                // 升级场景：顺手清掉旧版本遗留任务。
                if (ok) deleteLegacyTasks(root);
                root->Release();
            }
        }
        if (svc) svc->Release();
    }
    if (didInit) CoUninitialize();
    return ok;
}

static bool deleteAutoTask() {
    bool ok = false, didInit = false;
    ITaskService* svc = nullptr;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) didInit = true;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITaskService, reinterpret_cast<void**>(&svc)))) {
        VARIANT e1, e2, e3, e4;
        VariantInit(&e1); VariantInit(&e2); VariantInit(&e3); VariantInit(&e4);
        if (SUCCEEDED(svc->Connect(e1, e2, e3, e4))) {
            ITaskFolder* root = nullptr;
            if (SUCCEEDED(svc->GetFolder(L"\\", &root))) {
                BSTR nameB = SysAllocString(TASK_NAME);
                if (nameB) ok = SUCCEEDED(root->DeleteTask(nameB, 0));
                if (nameB) SysFreeString(nameB);
                // 卸载时一并清掉旧版本遗留任务。
                deleteLegacyTasks(root);
                root->Release();
            }
        }
        if (svc) svc->Release();
    }
    if (didInit) CoUninitialize();
    return ok;
}
