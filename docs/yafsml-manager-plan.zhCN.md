# YAFSML Manager 实施计划

## 目标

新增独立的 Windows 原生 Win32 C11 管理器 `YAFSML.Manager.exe`，用于管理 YAFSML 的配置、启动、快捷方式、更新和 Windows Defender 排除项。

管理器默认使用自身所在目录，也允许选择另一个包含 `YAFSML.exe` 与 `YAFSML.dll` 的目录，并记住该目录。

首发界面使用 English。所有可见文本通过资源 ID 获取，保留后续增加简体中文等语言的扩展点。

## 配置编辑器

界面按 INI section 分组，覆盖：

- 顶层 `game`。
- `[patch]` 的所有固定配置项。
- `[tweak]` 的 `cpu_affinity`。
- `[log]` 的 `console`、`log_file`、`log_level`。
- `[dll]` 的有序动态项：名称、路径、加载阶段、延迟和 `after` 依赖。
- `[mod]` 的有序动态项：名称和路径。

保存使用当前内置 schema：

- 规范化输出 UTF-8 无 BOM、CRLF 格式。
- 输出全部已知固定项，并按固定 section 顺序排列。
- 删除注释、未知键和未知 section；打开文件时显示将被删除的未知项数量。
- 动态项保留用户排序。
- 校验布尔值、整数范围、枚举值、重复名称、缺失依赖、自依赖和依赖环。
- 所选游戏不适用的选项仍显示但禁用，并说明原因；保存时写入有效默认值。
- 使用临时文件、`.bak` 备份和原子替换。

未来 YAFSML CLI 提供配置 schema 后，将通过独立 provider 接口接入，不改变表单层。

## 启动与快捷方式

启动现有 launcher：

```text
YAFSML.exe --config "<absolute-config-path>"
```

快捷方式使用 `IShellLinkW`，设置目标、参数、工作目录和 YAFSML 图标。

保存前检查未保存修改，提供 Save、Discard、Cancel。

## 更新

loader 与 manager 使用两个独立版本体系：

- loader：标签 `vX.Y.Z`，资产 `YAFSML-vX.Y.Z.zip`。
- manager：标签 `manager-vX.Y.Z`，资产 `YAFSML-Manager-vX.Y.Z.zip`。

每次启动管理器后检查更新；发现新版本先询问，确认后才下载。更新器下载到临时目录，校验 SHA-256 和 ZIP 内部路径，只替换对应程序文件，不覆盖用户 INI、mod、cache、log 和其他文件。文件被占用时要求关闭相关进程并重试，替换失败时回滚。管理器自身更新由辅助进程等待主程序退出后完成并重新启动。

## Defender

启动时只检查两个精确文件路径：`YAFSML.exe` 和 `YAFSML.dll`。缺少排除项时询问用户；确认后通过 UAC 执行最小权限操作，只添加缺失的文件排除项，不添加整个目录。提供重新检查和移除这两个精确排除项的入口。Defender 不可用或被策略禁用时显示状态，不阻止其他功能。

## 构建与测试

- 新增原生 Win32 C11 CMake target。
- 使用 Win32、COM、Shell、WinHTTP、BCrypt、`inih` 和仓库已有 `miniz`。
- manager 版本独立定义，不修改 `YAFSML_VERSION`。
- 增加独立 manager 发布工作流和 SHA-256 资产。
- 增加配置解析、规范化保存、快捷方式、ZIP 安全、更新回滚和路径校验测试。
- Defender 的真实添加、移除和 UAC 流程只做人工验收，不在自动测试中修改系统设置。

## 分阶段实现

1. 创建 manager 目标、资源和计划文档。
2. 实现配置模型、schema 校验、规范化读写和启动。
3. 实现 Win32 section 面板、动态 DLL/mod 编辑器、多语言资源入口和快捷方式。
4. 实现 GitHub Release 检查、下载、SHA-256、ZIP 校验、事务替换和自更新辅助进程。
5. 实现 Defender 精确文件排除检查、UAC 辅助操作和撤销入口。
6. 更新发行配置，运行构建和测试。
