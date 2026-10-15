# YAFSML

[README in English](README.md)

YAFSML（Yet Another FromSoftware Mod Loader）是面向 Windows 的 FromSoftware 游戏模组加载器，提供独立
启动器、代理 DLL、按声明顺序生效的散文件覆盖，以及兼容 ModEngine 的外部
DLL 加载。

## 支持的游戏

| 游戏 | 启动目标 | 状态 |
| --- | --- | --- |
| Elden Ring | `eldenring` | 稳定支持 |
| Armored Core VI: Fires of Rubicon | `armoredcore6`（`ac6`） | 稳定支持 |
| Elden Ring Nightreign | `nightreign` | 稳定支持 |
| Sekiro: Shadows Die Twice | `sekiro` | 稳定支持 |
| Dark Souls III | `darksouls3` | 实验性适配器，启用 Arxan 中和 |

Elden Ring 仍是主要目标。使用 `--launch-target armoredcore6`、`--launch-target nightreign`、
`--launch-target sekiro` 或 `--launch-target darksouls3` 选择其他游戏。未指定
`--launch-target` 时，启动器读取 `YAFSML.ini` 顶层的 `game=...`。未配置该值时
默认启动 Elden Ring；显式启动目标始终优先。

## 安装

### 独立启动器

1. 将 `YAFSML.exe`、`YAFSML.dll` 和 `YAFSML.ini` 解压到任意目录。
2. 修改 `YAFSML.ini`，启用所需的外部 DLL 或模组。
3. 运行 `YAFSML.exe`。

启动器使用 `--game-path` 指定的游戏；未指定时，在启动器所在目录或 Steam 库中定位
游戏。启动游戏后，启动器会先保持主进程挂起，注入加载器 DLL；除非指定 `--suspend`，
否则随后恢复游戏。

错误会以消息框显示，并写入标准错误输出；从命令提示符启动时也会输出到该控制台。
如果游戏目录中已放置 YAFSML 代理 DLL，启动器会拒绝启动；请移除该代理 DLL，或改为
直接启动游戏。

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
| `game` | `eldenring` | 未指定 `--launch-target` 时选择独立启动器的游戏。可用值包括 `eldenring`、`armoredcore6`、`nightreign`、`sekiro`、`darksouls3` 及其别名。 |

### `[patch]`

该 section 包含通用补丁设置。加载器仅对当前游戏应用受支持的设置。Dark Souls III 仍为实验性适配。项目包含 dearxan v0.5.7 的精简 C11 实现；其 before-main 调度基于 me3，已审阅的同步基线见[仓库参考](docs/me3-repo.md)。Dark Souls III 会强制启用 Arxan 中和。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `skip_intro` | `1` | 跳过开场 Logo。 |
| `prevent_regulation_save_write` | `1` | 阻止 mod 提供的 regulation 文件（`regulation.bin`；Dark Souls III 为 `Data0.bdt`）写入存档；使用游戏原始文件时保持原有的存档写入机制。 |
| `patch_mem` | `1` | 使用 mimalloc 替换 Dantelion 分配器。按照 me3 的行为，Armored Core VI 和 Nightreign 不支持该补丁。 |
| `patch_mem_dedicated_heap` | `1` | 预留独立的高地址 mimalloc arena，而不使用常规 mimalloc heap。 |
| `patch_mem_heap_size` | `0` | mimalloc 专用堆大小，单位为 MB；`0` 在当前游戏支持 `patch_mem` 时使用对应默认值。 |
| `boot_boost` | `1` | 缓存解密后的 BHD 标头，减少归档启动时间。 |
| `disable_arxan` | `0` | 在 Arxan 入口桩完成后执行中和。Dark Souls III 始终启用此选项。 |
| `replace_save_filename` | 留空 | 替换存档文件名；以点号开头时仅替换扩展名。替换存档名期间，若存档路径经过游戏存档目录下的符号链接或 junction，加载器会拒绝访问，而不是重定向。 |
| `replace_seamless_coop_save_filename` | 留空 | 替换 Seamless Co-op 使用的额外存档文件名。 |
| `enable_ime` | `0` | 在受支持的游戏中保持 IME 启用。Elden Ring、Armored Core VI、Nightreign 和 Dark Souls III 还允许软件键盘输入非拉丁文字；Sekiro 仅保持 IME 启用。修改后需重启游戏。 |

#### 专用堆诊断

启用 `patch_mem_dedicated_heap=1` 后，加载器会在尽可能高的地址预留 arena，
再交给 mimalloc 管理。以下环境变量可用于开发诊断：

- `YAFSML_HEAP_MAPPING_NAME`：创建命名的页文件映射，供其他进程打开。
- `YAFSML_HEAP_MAPPING_FILE`：使用文件作为 arena 的后备文件。加载器会创建
  该文件，或将已有文件截断到配置的堆大小；默认大小为数 GiB。

可以同时设置两个环境变量，以创建具名的文件映射。YAFSML 会将相关映射句柄
保留到进程退出。创建映射失败时，加载器会记录警告，并改用私有高地址内存；
如果私有内存预留也失败，则按既有行为回退到常规 mimalloc heap。

### `[tweak]`

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `cpu_affinity` | `0` | 选择游戏进程的 CPU Set 策略：`0` 保持现有 CPU Set 设置，`1` 使用除第一个逻辑 CPU Set 外的所有 CPU Set，`2` 使用能效 CPU Set，`3` 使用性能 CPU Set，`4` 使用除第一个性能 CPU Set 外的所有性能 CPU Set。五款游戏都会在游戏构造完标题菜单后异步应用策略 `1` 至 `4`；不支持的 CPU Set 布局或空选择会保持现有设置。在 Intel Ultra 系列 CPU 上运行《艾尔登法环》1.16.2 或更高版本时，不要使用 `2`、`3` 或 `4`。 |

驱动该行为的渲染就绪触发器属于可选能力。如果无法安装对应 Hook，加载器会保持现有
CPU Set 设置，并继续执行运行时初始化、VFS、Logo、属性、regulation 和外部 DLL
能力。卸载时会等待 CPU Set 工作线程完成。该触发器与 param 读取完成的时机是两回事，
后者才是 `data_ready` DLL 等待的点。

### `[log]`

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `console` | `0` | 打开用于记录日志的调试控制台。 |
| `log_file` | `0` | 启用后，将日志写入加载器 DLL 旁的 `log/YAFSML.log`。 |
| `log_level` | `warn` | 最低日志级别：`trace`、`debug`、`info`、`warn`、`error` 或 `off`。 |

### DLL 与模组

`[dll]` 节用于在游戏启动时加载外部 DLL。路径可以相对于加载器 DLL（通常为
`YAFSML.dll`），也可以是绝对路径。移动配置文件不会改变路径基准。未设置条件时，
为保持向后兼容，DLL 会在 `SteamAPI_Init` 之后加载。
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
被推迟到同一个后台线程上加载。检测到循环依赖时记录错误，并保持原有顺序不变；提前加载和
推迟到后台线程的规则仍然生效。ModEngine 扩展的 `modengine_ext_init` 返回 false 时会记录
警告，其后的 DLL 仍会继续加载。项目自带的扩展 DLL 已不再随此仓库发布。

`[mod]` 节用于声明覆盖游戏文件的目录或 ZIP 文件。路径可以相对于加载器 DLL，也可以是绝对路径。目标为目录时扫描散文件，目标为 `.zip` 文件时读取压缩包。多个模组包含同名文件时，后声明的模组覆盖先声明的模组；目录和 ZIP 文件混合配置时遵循相同规则。

ModEngine2 TOML 的 DLL、mod 路径、日志和 BootBoost 的 `cache` 目录使用同一基准。
磁盘路径支持 `/`、`\`、`.`、`..` 及混合分隔符，并在文件系统允许的范围内支持超过
260 个字符的路径。以单个分隔符开头的路径使用加载器 DLL 所在盘符或 UNC 共享的根目录。
不支持 `C:mod` 这类盘符相对路径；应使用 `mod` 或 `C:\mod`。原先依赖配置文件所在
目录的配置需要调整相对路径。

```ini
[mod]
loose=mod
archive=abc.zip
selected=abc.zip:def
```

`abc.zip` 使用 ZIP 根目录。`abc.zip:def` 和 `abc.zip:/def` 均使用 ZIP 内的 `/def` 作为模组根目录：`def/parts/example.bin` 会覆盖游戏的 `parts/example.bin`。ModEngine2 TOML 的 `mods` 条目也支持这种路径写法。ZIP 支持未压缩存储和 Deflate 压缩，包括 ZIP64，不支持加密条目。

VFS 通过 miniz 的 zlib 兼容数据流按需读取 ZIP 条目，不创建临时解压文件。每次打开拥有独立的文件位置和固定大小的解码缓冲；向后定位时从条目起点重新解码。ZIP 模组保持只读，存档写入仍使用已有的替换存档映射。ZIP 条目不支持内存映射（`CreateFileMapping`）。重叠读取在调用线程完成解码，然后通过事件、IOCP 或 APC 发送完成通知。

ZIP 子路径支持正斜杠 `/`、反斜杠 `\` 和多层目录，也可混用两种分隔符。`abc.zip:def\subdir\mod`、`abc.zip:\def\subdir\mod`、`abc.zip:/def/subdir/mod` 和 `abc.zip:/def\subdir\mod` 均将 ZIP 内的 `/def/subdir/mod` 作为模组根目录。INI 中的反斜杠无需转义；TOML 可使用单引号字符串 `path = 'abc.zip:\def\subdir\mod'`，使用双引号字符串时须将反斜杠写成 `\\`。

### ModEngine2 TOML 兼容

未找到 `YAFSML.ini` 时，加载器会查找对应游戏的 ModEngine2 文件：
`config_eldenring.toml`、`config_armoredcore6.toml`、`config_nightreign.toml`、`config_sekiro.toml` 或 `config_darksouls3.toml`。
启动器的 `-c` 选项或 `YAFSML_CONFIG` 环境变量可指定其他配置文件或目录；指定目录时使用
其中的 `YAFSML.ini`，不存在时使用 ModEngine2 文件。相对路径的 `-c` 与 `YAFSML_CONFIG`
均以 `YAFSML.dll` 所在目录为基准。与 ModEngine2 一致，`mods` 条目只需提供 `path`，`enabled`
默认为 true。

## 启动器选项

```text
-t, --launch-target <game>  选择 eldenring、armoredcore6、nightreign、sekiro 或 darksouls3。
-p, --game-path <path>      指定游戏可执行文件或游戏目录。
-c, --config <path>         指定配置文件或配置目录。
-d, --modloader-dll <path> 指定要注入的加载器 DLL。
    --modengine-dll <path> --modloader-dll 的兼容别名。
-s, --suspend               注入后保持游戏挂起。
```

`--launch-target` 会覆盖 `YAFSML.ini` 顶层的 `game=...` 设置。未指定该选项时，
`--game-path` 指向已知游戏的可执行文件即选择该游戏；已指定时，指向其他游戏的可执行
文件会报错。

`--modloader-dll` 的相对路径以启动器所在目录为基准，指定的 DLL 必须存在。启动器与
DLL 从同一份配置读取 `game=...`：`--config` 或继承的 `YAFSML_CONFIG` 的相对路径以 DLL
所在目录为基准，两者都未指定时使用 DLL 所在目录的 `YAFSML.ini`。通过这两种方式指定的
配置会以绝对路径传给 DLL。

相对路径的 `--game-path` 先按当前目录查找，再按启动器所在目录查找；都找不到游戏时，
启动器报错退出，不会转而在 Steam 库中查找。未指定 `--game-path` 时，启动器先查找自身
所在目录，再按 Steam 客户端的规则读取 `libraryfolders.vdf` 与应用清单，在 Steam 库中
查找。

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
- [miniz](https://github.com/richgel999/miniz)：ZIP 模组包与 BootBoost 缓存压缩。
- [xxHash](https://github.com/Cyan4973/xxHash)：BootBoost 缓存键。
- [dearxan](https://github.com/tremwil/dearxan)：移植为 C11 的 Arxan 分析与中和实现。
- [Zydis](https://github.com/zyantific/zydis)：dearxan 移植使用的 x86/x86-64 反汇编库。
- [libofdf](https://github.com/Jan200101/libofdf)：Steam VDF 读取器的接口。
