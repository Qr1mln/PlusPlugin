// version.dll 代理（ghost 层）：导出的转发外壳 + 加载器引导入口。
// 目标进程：由本 DLL 同目录 version.ini 的 target= 指定；
// 交付的应用是 Flutter + ANGLE(D3D11) 宿主，例如 TTVoice.exe (x64)。
//
// 转发模型：
//   应用 -> 我们的转发桩(stubs.asm) -> jmp qword ptr [g_pfn_X] -> 真身 version.dll!X
//   真身 DLL 从 %SystemRoot%\System32 按绝对路径加载，
//   所以既不会递归，也没有版本耦合。
//
// 模块划分（本文件只是入口）：
//   log.h/.cpp         proxy.log + 插件日志扇出
//   config.h/.cpp      version.ini（Win32 profile API + 旧格式兼容）
//   bootstrap.h/.cpp   窗口发现、插件启动
//
// 设计说明：真身 DLL 必须在 DllMain 内解析，因为应用可能在主窗口出现之前
// 就调用这些 API；真身 DLL 的依赖链要保持极简（只用 kernel32），
// 这样早期加载才不会干扰图形栈。
//
// framework 职责（本模块即 framework，见 pplugin.h）：
//   1. 把每个 version.dll 导出转发到真身（stubs.asm）。
//   2. 只提供插件框架。代理【不做渲染】：无 Present hook、无 D3D/DXGI 设备、
//      无 overlay 窗口。每个插件自持窗口/UI，并通过 Host API 访问宿主。
//      插件从不自己装 hook 或建设备，因此它们始终可卸载。
//   3. 从 %APPDATA%\TTVoice\plugins 加载/卸载插件。插件只通过 Host 登记回调，
//      所以加载器可以撤销整套回调后再 FreeLibrary。

#include <windows.h>        // Win32 API：GetSystemDirectoryW / LoadLibraryW / CreateThread 等
#include <cstdio>           // swprintf_s：拼真身 DLL 的绝对路径

#include "bootstrap.h"      // PayloadThread 声明
#include "log.h"            // Log() 日志
#include "proxy_exports.h"  // PROXY_EXPORT_LIST：17 个待转发的导出清单

#pragma comment(lib, "user32.lib")  // 链接 user32（bootstrap 用到窗口相关 API）

extern "C" {  // 用 C 链接，避免 C++ 名字修饰，保证符号名干净
#define DEF_PTR(name) void* g_pfn_##name = nullptr;  // 为每个导出生成一个函数指针，初值为空
PROXY_EXPORT_LIST(DEF_PTR)  // 宏展开：g_pfn_GetFileVersionInfoW ... 共 17 个
#undef DEF_PTR              // 用完即撤，避免影响之后的宏定义
}

namespace {  // 匿名命名空间：下列符号仅在本编译单元内可见

int g_resolveMiss = -1;           // 未解析到的导出数量；-1 表示“尚未解析”
HMODULE g_sysVersion = nullptr;  // 真身(System32) version.dll 的模块句柄（保留，防止被卸载后 g_pfn_* 悬空）

// 职责 1：定位并加载真身 version.dll —— 只管拿到模块句柄。
HMODULE LoadSysVersionDll() {
    wchar_t sysdir[MAX_PATH] = {};                         // 存放 System32 目录路径的缓冲区
    const UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);  // 取 C:\Windows\System32
    if (n == 0 || n >= MAX_PATH) {                         // 取失败，或路径超出缓冲区
        return nullptr;                                    // 定位失败
    }
    wchar_t realPath[MAX_PATH] = {};                        // 拼出真身 DLL 的绝对路径
    swprintf_s(realPath, MAX_PATH, L"%s\\version.dll", sysdir);
    return LoadLibraryW(realPath);                          // 按绝对路径加载真身，失败返回 nullptr（防递归）
}

// 职责 2：把真身的导出地址绑进 g_pfn_* —— 只填表，返回未解析到的数量。
int BindExportsToSys(HMODULE sys) {
    int missing = 0;  // 统计有多少个导出没找到
#define RESOLVE(name)                                                     \
    g_pfn_##name = reinterpret_cast<void*>(GetProcAddress(sys, #name));  \
    if (g_pfn_##name == nullptr) ++missing;
    // 上面：从真身逐个取地址填进对应 g_pfn_*；取不到则 missing 加一
    PROXY_EXPORT_LIST(RESOLVE)   // 对 17 个导出各执行一次 RESOLVE
#undef RESOLVE                   // 用完撤销该宏
    return missing;              // 返回缺失计数（0 表示全部解析成功）
}

// 编排：加载真身 → 绑定导出（自身不承担任何一件事情的细节）。
int ResolveSysVersion() {
    g_sysVersion = LoadSysVersionDll();      // 加载真身并保留句柄
    if (g_sysVersion == nullptr) {           // 加载失败
        return -1;                           // 返回 -1 表示“无法解析”
    }
    return BindExportsToSys(g_sysVersion);   // 绑定导出，返回缺失计数
}

}  // namespace

// 供外部查询“未解析导出数”，PayloadThread 启动日志会用（跨 TU：bootstrap.cpp 调用）。
int ResolveMissCount() {
    return g_resolveMiss;  // 返回缓存的缺失计数
}

// DLL 入口：加载器在进程附加/分离时调用。
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {       // 只在“进程附加”（首次加载）时做事
        DisableThreadLibraryCalls(hModule);   // 禁用线程通知，省掉无谓开销（本 DLL 用不到）
        g_resolveMiss = ResolveSysVersion();  // 解析真身导出，把所有转发指针填好

        // 此处不再做任何早期 hook：代理不渲染。宿主能力（含 scale）由 framework
        // 持有，各插件自持窗口/UI。

        // 所有插件/宿主工作都推迟到工作线程，绝不在 loader lock 下执行。
        // 这里刻意用 CreateThread 而非 std::thread：在 loader lock 下启动 C++
        // 线程（CRT 初始化、内存分配）可能死锁。
        HANDLE th = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);  // 启动引导线程
        if (th != nullptr) {   // 创建成功
            CloseHandle(th);   // 关闭句柄（线程自身会继续运行）
        }
    }
    return TRUE;  // 返回 TRUE 表示初始化成功
}