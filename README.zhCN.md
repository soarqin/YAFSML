# YAFSML

[README in English](README.md)

YAFSML（Yet Another FromSoftware Mod Loader）是面向 Windows 的 FromSoftware 游戏模组加载器，提供独立
启动器、代理 DLL、按声明顺序生效的散文件覆盖，以及兼容 ModEngine 的外部
DLL 加载。

## 支持的游戏

| 游戏 | 启动目标 | 状态 |
| --- | --- | --- |
| Elden Ring | `eldenring` | 稳定支持 |
| Elden Ring Nightreign | `nightreign` | 稳定支持 |
| Sekiro: Shadows Die Twice | `sekiro` | 稳定支持 |
| Dark Souls III | `darksouls3` | 实验性适配器，启用 Arxan 中和 |

Elden Ring 仍是主要目标。使用 `--launch-target nightreign`、
`--launch-target sekiro` 或 `--launch-target darksouls3` 选择其他游戏。未指定
`--launch-target` 时，启动器读取 `YAFSML.ini` 顶层的 `game=...`。未配置该值时
默认启动 Elden Ring；显式启动目标始终优先。

## 安装

### 独立启动器

1. 将 `YAFSML.exe`、`YAFSML.dll` 和 `YAFSML.ini` 解压到任意目录。
2. 修改 `YAFSML.ini`，启用所需的外部 DLL 或模组。
3. 运行 `YAFSML.exe`。

启动器会依次尝试当前目录、显式路径和 Steam 库定位游戏。启动游戏后，启动器
会先保持主进程挂起，注入加载器 DLL；除非指定 `--suspend`，否则随后恢复游戏。

### 代理 DLL

1. 将 `YAFSML.dll` 和 `YAFSML.ini` 放入游戏目录。
2. 将 `YAFSML.dll` 重命名为 `dxgi.dll`、`dinput8.dll` 或 `winhttp.dll`。
3. 在不加载 Easy Anti-Cheat 的情况下启动游戏。

直接运行 `eldenring.exe` 时，可在游戏可执行文件旁创建 `steam_appid.txt`，写入
`1245620`。其他启动目标会自动使用对应的 Steam App ID。

## 配置

完整模板位于 `src/YAFSML.ini`，发布包中的文件名为
`YAFSML.ini`。布尔值支持 `true`、`yes`、`on` 和 `1`；其他值均视为 false。

### 顶层选项

以下选项位于所有 section 之外：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `game` | `eldenring` | 未指定 `--launch-target` 时选择独立启动器的游戏。可用值包括 `eldenring`、`nightreign`、`sekiro`、`darksouls3` 及其别名。 |

### `[patch]`

该 section 包含通用补丁设置。加载器仅对当前游戏应用受支持的设置。Dark Souls III
仍为实验性适配。项目包含 dearxan v0.5.3 的精简 C11 实现；其 before-main
调度对齐 me3 `6563ebb`，Dark Souls III 会强制启用 Arxan 中和。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `skip_intro` | `1` | 跳过开场 Logo。 |
| `prevent_regulation_save_write` | `1` | 阻止原始、修改后或过大的 `regulation.bin` 数据写入存档。 |
| `patch_mem` | `1` | 使用 mimalloc 替换 Dantelion 分配器。按照 me3 的行为，Nightreign 不支持该补丁。 |
| `patch_mem_heap_size` | `0` | mimalloc 专用堆大小，单位为 MB；`0` 在当前游戏支持 `patch_mem` 时使用对应默认值。 |
| `boot_boost` | `1` | 缓存解密后的 BHD 标头，减少归档启动时间。 |
| `disable_arxan` | `0` | 在 Arxan 入口桩完成后执行中和。Dark Souls III 始终启用此选项。 |
| `replace_save_filename` | 留空 | 替换存档文件名；以点号开头时仅替换扩展名。 |
| `replace_seamless_coop_save_filename` | 留空 | 替换 Seamless Co-op 使用的额外存档文件名。 |
| `enable_ime` | `0` | 为需要非拉丁文字输入的模组保持 IME 启用。 |

### `[tweak]`

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `cpu_affinity` | `0` | 选择游戏进程的 CPU 亲和性策略：`0` 保持现有亲和性，`1` 使用除第一个逻辑核外的所有逻辑核，`2` 使用能效核，`3` 使用性能核，`4` 使用除第一个逻辑核外的性能核。四款游戏都会在游戏构造完标题菜单后异步应用策略 `1` 至 `4`；不支持的处理器组布局或空掩码会保持现有亲和性。在 Intel Ultra 系列 CPU 上运行《艾尔登法环》1.16.2 或更高版本时，不要使用 `2`、`3` 或 `4`。 |

驱动该行为的渲染就绪触发器属于可选能力。如果无法安装对应 Hook，加载器会保持现有
CPU 亲和性，并继续执行运行时初始化、VFS、Logo、属性、regulation 和外部 DLL
能力。卸载时会等待亲和性工作线程完成。该触发器与 param 读取完成的时机是两回事，
后者才是 `data_ready` DLL 等待的点。

### `[log]`

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `console` | `0` | 打开用于记录日志的调试控制台。 |
| `log_file` | `0` | 配置项存在时，将日志写入配置文件旁的 `log/YAFSML.log`。 |
| `log_level` | `warn` | 最低日志级别：`trace`、`debug`、`info`、`warn`、`error` 或 `off`。 |

### DLL 与模组

`[dll]` section 用于在游戏启动时加载外部 DLL。路径可以是相对于配置文件的路径，
也可以是绝对路径。未设置条件时，为保持向后兼容，DLL 会在 `SteamAPI_Init` 之后加载。
配置值可以使用 `name=path_to_file.dll|conditions...` 追加由竖线分隔的条件：

- `early`：在 `SteamAPI_Init` 之前加载 DLL。
- `data_ready`：在游戏读取完全部 param 后加载 DLL。
- `delay,500`：等待 500 ms 后加载 DLL。
- `after,abc`：在 `[dll]` 中名称为 `abc` 的条目之后加载，而不是根据 DLL 路径判断。

`early`、`data_ready` 与 `delay` 都用于选择加载时机，因此互斥；重复设置时后者被记录
并忽略。`data_ready` 与 `delay` 的 DLL 共用一个后台线程按配置顺序依次加载，不会因启动
DLL 卡住一帧；同一个线程也保证 `after` 依赖在跨这两种条件时仍然先加载依赖项。与既有的
`delay` 行为一致，同一线程上的等待是累加的。

只有依赖关系无法由原有顺序保证时才会调整顺序；其他条目保持配置顺序。如果 `early`
DLL 依赖普通条目，该依赖项也会提前加载；依赖 `data_ready` 或 `delay` 条目的普通 DLL 会
被推迟到同一个后台线程上加载。检测到循环依赖时记录错误，并保持原有顺序不变。项目自带的
扩展 DLL 已不再随此仓库发布。

`[mod]` section 用于声明包含散文件覆盖的目录。路径可以是相对路径或绝对路径；多个
模组包含同名文件时，后声明的模组覆盖先声明的模组。

### ModEngine2 TOML 兼容

未找到 `YAFSML.ini` 时，加载器会查找对应游戏的 ModEngine2 文件：
`config_eldenring.toml`、`config_nightreign.toml`、`config_sekiro.toml` 或 `config_darksouls3.toml`。
启动器的 `-c` 选项或 `YAFSML_CONFIG` 环境变量可指定其他配置路径。

## 启动器选项

```text
-t, --launch-target <game>  选择 eldenring、nightreign、sekiro 或 darksouls3。
-p, --game-path <path>      指定游戏可执行文件或游戏目录。
-c, --config <path>         指定配置文件或配置目录。
-d, --modloader-dll <path> 指定要注入的加载器 DLL。
    --modengine-dll <path> --modloader-dll 的兼容别名。
-s, --suspend               注入后保持游戏挂起。
```

`--launch-target` 会覆盖 `YAFSML.ini` 顶层的 `game=...` 设置。

## 更新记录

详见 [CHANGELOG.md](CHANGELOG.md)。

## 鸣谢

- [ModEngine](https://github.com/soulsmods/ModEngine2)：原始 Souls 游戏模组加载器。
- [minhook](https://github.com/TsudaKageyu/minhook)：函数 Hook。
- [klib](https://github.com/attractivechaos/klib)：哈希表。
- [inih](https://github.com/benhoyt/inih)：INI 解析。
- [toml-c](https://github.com/arp242/toml-c)：ModEngine2 TOML 兼容。
- [wingetopt](https://github.com/alex85k/wingetopt)：命令行解析。
- [mimalloc](https://github.com/microsoft/mimalloc)：加载器分配器。
- [dearxan](https://github.com/tremwil/dearxan)：移植为 C11 的 Arxan 分析与中和实现。
- [Zydis](https://github.com/zyantific/zydis)：dearxan 移植使用的 x86/x86-64 反汇编库。
- [libofdf](https://github.com/Jan200101/libofdf)：Steam 库定位。
- [LZMA SDK](https://7-zip.org/sdk.html)：公有领域压缩 SDK。
