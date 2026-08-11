#include "locale.h"

static const wchar_t *const english[MGR_STR_COUNT] = {
    L"YAFSML Manager", L"Open", L"Save", L"Save As", L"Launch", L"Shortcut",
    L"Update YAFSML", L"Update Manager", L"Add exclusion", L"Remove exclusion", L"Checking Defender...", L"Defender unavailable", L"Browse",
    L"General", L"Game:", L"[patch]", L"[tweak]", L"[log]", L"[dll]", L"[mod]",
    L"Add", L"Remove", L"Up", L"Down", L"Name:", L"Path:", L"Stage:", L"Delay:", L"After:",
    L"YAFSML directory:", L"Game directory:", L"Override game path", L"Language:",
    L"0 - Keep current CPU Set assignment",
    L"1 - All logical CPU Sets except the first",
    L"2 - All efficient CPU Sets",
    L"3 - All performance CPU Sets",
    L"4 - All performance CPU Sets except the first"
};

static const wchar_t *const simplified_chinese[MGR_STR_COUNT] = {
    L"YAFSML 管理器", L"打开", L"保存", L"另存为", L"启动", L"快捷方式",
    L"更新 YAFSML", L"更新管理器", L"添加排除项", L"移除排除项", L"正在检查 Defender…", L"Defender 不可用", L"浏览",
    L"常规", L"游戏：", L"[patch]", L"[tweak]", L"[log]", L"[dll]", L"[mod]",
    L"添加", L"移除", L"上移", L"下移", L"名称：", L"路径：", L"阶段：", L"延迟：", L"依赖：",
    L"YAFSML 目录：", L"游戏目录：", L"覆盖游戏启动路径", L"界面语言：",
    L"0 - 保持当前 CPU Set 分配",
    L"1 - 除第一个以外的所有逻辑 CPU Set",
    L"2 - 所有能效 CPU Set",
    L"3 - 所有性能 CPU Set",
    L"4 - 除第一个以外的所有性能 CPU Set"
};

const wchar_t *manager_text(manager_language_t language, manager_string_id_t id) {
    if (id < 0 || id >= MGR_STR_COUNT) return L"";
    return (language == MANAGER_LANGUAGE_SIMPLIFIED_CHINESE ? simplified_chinese : english)[id];
}
