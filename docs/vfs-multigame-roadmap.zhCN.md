# VFS 与多游戏支持规划

## 1. 文档目的

本文记录 YAFSML 从「Elden Ring 专用加载器」演进为支持 Elden Ring、Elden Ring Nightreign、Sekiro: Shadows Die Twice 与 Dark Souls III 的 FromSoftware 模组加载器的设计、完成状态与后续计划。

项目已直接重命名为 YAFSML（Yet Another FromSoftware Mod Loader）。二进制和配置文件使用 `YAFSML` 名称，不保留旧名称兼容入口。

me3 对照仓库、分支与同步提交以 [`docs/me3-repo.md`](me3-repo.md) 为准，重点参考实际实现，而不是 README、历史博客或尚未兑现的配置字段。缓存字典采用该文档记录的性能分支实现。

## 2. 已确认的目标与边界

### 2.1 支持游戏

| 游戏 | Steam App ID | 可执行文件 | 首期支持等级 |
| --- | ---: | --- | --- |
| Elden Ring | `1245620` | `Game\\eldenring.exe` | 稳定支持，现有行为必须回归 |
| Elden Ring Nightreign | `2622380` | `Game\\nightreign.exe` | 稳定支持 |
| Sekiro: Shadows Die Twice | `814380` | `sekiro.exe` | 稳定支持 |
| Dark Souls III | `374320` | `Game\\DarkSoulsIII.exe` | 实验性支持 |

Dark Souls III 已完成阶段 9，并继续保持实验性支持。项目现已加入 dearxan v0.5.3 的 C11 实现；Dark Souls III 会强制启用 Arxan 中和。支持等级仍根据长期运行、回到标题、切图和调试器附加等稳定性验证决定。

### 2.2 VFS 的完成标准

「完整 VFS」指达到当前 me3 的实际资源覆盖能力，不指实现任意来源、任意写入语义的通用用户态文件系统。

完成后必须覆盖以下读取入口：

- 启动时递归扫描多个 package，并建立冻结的路径索引。
- Windows `CreateFileA`、`CreateFileW` 与 `CreateFile2`。
- Dantelion disk device 的 `open_file`。
- `DlFileOperator::SetPath` 的三个入口。
- `DLEncryptedBinderLightUtility::MakeEblObject`。
- 已挂载 BND4 device 的直接 `open_file`。
- `MountEbl` 后新出现的 BND4 device。
- Wwise 音频打开入口，包括 `.bnk` 与 `.wem`。
- 独立存档的 `.sl2` 与 `.bak`。

第一版不以实现虚拟 HANDLE、目录枚举合并、非磁盘 provider、热重载或自定义流为门槛。这些能力在 me3 当前实现中也不是完整 VFS 的必要组成部分。

### 2.3 覆盖优先级

多个 package 对同一虚拟路径提供文件时，**后声明的 package 覆盖先声明的 package**。当前 VFS index 已按此规则替换已有条目。

优先级在 INI、ModEngine2 TOML 兼容输入和未来配置模型中保持一致；发行说明仍需明确该行为。

### 2.4 写入策略

模组资源层默认只读，但不能以「禁止所有写入」替代正确的路由规则。

| 请求对象 | 读取 | 写入、创建、删除 |
| --- | --- | --- |
| package 中的普通资源覆盖 | 重定向到 package 文件 | 不重定向，交给原始游戏路径 |
| 已直接指向 package 物理路径的文件 | 不干预 | 不干预 |
| 显式注册的设置文件映射 | 重定向 | 重定向 |
| 独立存档映射 | 重定向 | 重定向 |

设置文件不能通过扩展名或目录名称猜测。适配器或扩展必须精确注册可写虚拟路径，避免将 `regulation.bin`、BND、贴图、Wwise 资源等误判为可写文件。

`CreateDirectoryA/W/ExW` 与 `DeleteFileA/W` 仅服务于显式可写映射。普通 package 资源不允许被这些 Hook 重定向后修改或删除。

### 2.5 不纳入本计划的内容

- Proton、Linux 和 macOS 支持。
- dearxan v0.5.3 的 C11 实现与 Arxan 中和；before-main 调度采用 me3 `6563ebb` 的语义。
- Sentry、联网遥测、自动更新器、安装器与网站基础设施。
- me3 的 Rust closure Hook 框架和 rkyv 共享内存 RPC。
- me3 `.me3` Profile 格式兼容。
- me3 schema 中声明但当前没有执行路径的 `finalizer`。
- 通用虚拟 HANDLE、目录枚举合并、非磁盘 provider 与热重载。
- 原生 minidump；当前 me3 也没有可靠的游戏原生崩溃转储实现。

## 3. 当前状态与差距

当前项目包含可复用的 Windows 注入、proxy DLL、PE 扫描、RTTI、FD4 step、Steam 安装目录查找、INI/TOML 解析、MinHook、外部 ModEngine 兼容 DLL 加载与线程安全文件缓存。

当前核心架构已完成多游戏拆分，但产品支持范围仍不完整：

- launcher 已由 Game Registry 驱动，并支持 Elden Ring、Nightreign、Sekiro 与实验性的 Dark Souls III 目标。
- 四款游戏均具有独立 adapter；Dark Souls III 保持实验性，并强制启用 Arxan 中和。
- 配置使用顶层 `game=...` 选择启动目标，并将加载器设置分为 `[patch]`、`[tweak]`、`[log]`、`[dll]` 和 `[mod]` section。
- VFS 已改为启动时扫描、last-wins 索引、分域缓存、显式可写映射和统一 Win32/Dantelion 路由。
- 无模组时跳过普通资源查询；存档文件名替换仍可按需独立安装文件 Hook。
- 文件覆盖已集中到 VFS index、Win32、Dantelion 和 Wwise 路由；`mod.c` 保留兼容 wrapper。
- `src/modloader` 不再向外部 DLL 提供自定义扩展 ABI；游戏上下文、生命周期和 VFS 服务只在 loader 内部使用。
- 项目自带扩展 DLL 已拆出；`src/extdlls` 不再参与当前仓库构建。

### 3.1 阶段 7 当前进度

本轮已撤销自定义扩展 API 方案。me3 没有对应的扩展 API V2 入口，本项目不再通过非标准入口向扩展暴露 Host 服务，避免破坏与其他 mod loader 的兼容性。

当前保留内容：

- `modengine_ext_init` 的兼容加载和逆序卸载路径。
- 供内部 Host 功能使用的生命周期状态，不属于扩展 ABI。

四款游戏共用的白闪修复已移入内部 Host capability，并在 `PRE_ENTRY_SAFE` 阶段 Hook `RegisterClassExW`，将窗口类背景刷替换为黑色。Logo 策略已成为显式 game trait；Elden Ring 与 Nightreign 在 `AFTER_RUNTIME_INIT` 阶段重定向 FD4 `TitleStep::STEP_BeginLogo`，不再修改版本相关的代码字节。DLRF 已具备运行时类、方法 resolver 和 invoker 地址解析，以及属性初始化引用扫描；真正的离线属性调用仍需游戏现场验证。Sekiro 与 Dark Souls III 使用 SPRJ Logo 和旧式 regulation 保护路径；Nightreign 不支持 allocator patch，其他三款游戏使用各自的 allocator 策略。Dark Souls III 已完成阶段 9 现场验收。

## 4. 强制前置兼容性修正

在引入 Game Registry、VFS 或 Sekiro/DS3 适配器之前，先修复现有移植不完整造成的 ABI 和 Wwise 不一致。以下项目必须与 me3 基准提交完全一致，不能保留兼容旧声明的分支。

### 4.1 FD4 step ABI

me3 的 FD4 step 函数类型是：

```c
typedef struct fd4_time_s {
    uintptr_t vtable;
    float time;
} fd4_time_t;

typedef void (__cdecl *fd4_step_fn_t)(
    void *this_ptr,
    fd4_time_t *time);
```

当前 `src/modloader/patches/eldenring.c` 将 `CSRegulationStep::STEP_Idle` 声明为单参数函数。该声明必须删除并统一使用 `src/process/fd4_step.h` 定义的双参数类型。

`regulation_step_idle_hooked()` 及未来所有 `CSFileStep::STEP_Init`、`SprjFileStep::STEP_Init` Hook 必须原样转发 `this_ptr` 和 `time`：

```c
static void __cdecl regulation_step_idle_hooked(
    void *this_ptr,
    fd4_time_t *time)
{
    old_regulation_step_idle(this_ptr, time);
    /* 原有 raw regulation 清理逻辑 */
}
```

验收要求：

- `fd4_time_t` 的字段偏移与大小具有静态断言。
- trampoline 测试证明两个参数均被原样转发。
- Elden Ring regulation guard 回归测试通过。

### 4.2 Dantelion allocator ABI

`src/modloader/dl_allocator.h` 的 `dl_allocator_vtable_t` 必须与 me3 的 `DlAllocatorVtable` 布局完全一致。

重点变化：

- `dl_heap_direction_t` 只作为 `capability()` 的第三个参数保留。
- `allocate_aligned(self, size, alignment)` 删除 `direction` 参数。
- `reallocate_aligned(self, ptr, size, alignment)` 删除 `direction` 参数。
- `back_allocate_aligned(self, size, alignment)` 删除 `direction` 参数。
- `back_reallocate_aligned(self, ptr, size, alignment)` 删除 `direction` 参数。
- `block_size()`、`is_valid_block()` 与 `block_of()` 的指针参数类型按 me3 定义统一。
- `mimalloc_allocator.c` 的 front/back 分配函数、vtable 槽位顺序和对齐策略必须同步为 me3 实现。

验收要求：

- 对 vtable 槽位、结构大小和关键字段偏移增加静态断言。
- `tests/smoke_mimalloc_allocator.c` 使用新函数签名。
- 分配、对齐分配、重分配、back 分配、释放和 `block_size(NULL)` 通过测试。
- Elden Ring `patch_mem` 回归验证通过后，才能继续多游戏 allocator 工作。

### 4.3 Wwise `AkOpenMode` 与路径规则

`READ_EBL` 必须从当前的 `9` 修正为 me3 使用的 `10`：

```c
typedef enum ak_open_mode_e {
    AK_OPEN_MODE_READ = 0,
    AK_OPEN_MODE_WRITE = 1,
    AK_OPEN_MODE_WRITE_OVERWRITE = 2,
    AK_OPEN_MODE_READ_WRITE = 3,
    AK_OPEN_MODE_READ_EBL = 10,
} ak_open_mode_t;
```

Wwise 查询行为同步为：

1. 重复移除 `sd:/` 和 `sd_dlc02:/` 前缀。
2. 非 `.wem` 输入依次查询 `sd/`、`sd/enus/`、`sd/ja/`。
3. `.wem` 输入先查询 `wem/<ID>.wem`，再查询 `wem/<ID 前两位>/<ID>.wem`。
4. 命中覆盖文件后，调用原函数时强制使用普通 `READ`，不使用 `READ_EBL`。

验收要求：

- 对所有枚举值增加静态断言。
- 为前缀剥离、语言目录、DLC 前缀与两种 WEM 目录写纯函数测试。
- Elden Ring `.bnk`、普通 `.wem`、两位目录 `.wem` 与 DLC 音频回归通过。

## 5. 目标架构

### 5.1 目录结构

建议将核心按「通用基础设施、FromSoftware 运行时、VFS、游戏适配器」拆分。目录名称可在实施中按现有 CMake 风格微调，但依赖方向必须保持单向。

```text
src/
  common/
  process/
  steam/
  launcher/
  modloader/
    core/
    game/
      game.h
      registry.c
      context.c
      eldenring/
      sekiro/
      darksouls3/
    from/
      abi_msvc2012.*
      abi_msvc2015.*
      dl_allocator.*
      dl_device.*
      dlrf.*
      wwise.*
    vfs/
      mapping.*
      path.*
      cache.*
      win32_hooks.*
      save_mapping.*
      writable_mapping.*
      from_device_hooks.*
      boot_boost.*
```

`process/` 继续提供 PE、signature scan、RTTI、singleton、FD4 step 与反修饰名称等分析能力。游戏 adapter 只能消费这些服务，不能把游戏特有签名回写到通用模块。

### 5.2 游戏描述符与上下文

不要使用 `game < sekiro` 一类按枚举排序的特性判断。每种差异都由显式 traits 表示。

```c
typedef enum ml_game_id_e {
    ML_GAME_UNKNOWN = 0,
    ML_GAME_ELDEN_RING,
    ML_GAME_NIGHTREIGN,
    ML_GAME_SEKIRO,
    ML_GAME_DARK_SOULS_3,
} ml_game_id_t;

typedef enum ml_stl_abi_e {
    ML_STL_ABI_MSVC2012,
    ML_STL_ABI_MSVC2015,
} ml_stl_abi_t;

typedef enum ml_support_level_e {
    ML_SUPPORT_STABLE,
    ML_SUPPORT_EXPERIMENTAL,
} ml_support_level_t;

typedef struct ml_game_descriptor_s {
    ml_game_id_t id;
    const char *key;
    const wchar_t *title;
    uint32_t steam_app_id;
    const wchar_t *const *exe_relpaths;
    size_t exe_relpath_count;
    const wchar_t *save_root_name;
    const wchar_t *ini_section;
    const wchar_t *modengine_config_name;
    ml_stl_abi_t stl_abi;
    ml_support_level_t support_level;
    const wchar_t *file_step_name;
    const char *control_api_class;
    size_t ebl_bhd_holder_offset;
} ml_game_descriptor_t;
```

每个进程只能存在一个 `ml_game_context_t`。launcher 选择的游戏只作为预期值；注入 DLL 必须根据当前 EXE 名称、PE version resource 和必要签名再次校验。launcher、核心和 ext DLL 必须获得同一个经验证的 `game_id`。

### 5.3 生命周期

统一使用六个阶段：

| 阶段 | 目的 | 典型功能 |
| --- | --- | --- |
| `PRE_ENTRY_SAFE` | 进程入口前的安全初始化 | proxy、配置、VFS index、Win32 Hook、黑屏闪烁修复 |
| `BEFORE_MAIN` | 游戏必要初始化或 Arxan 调度后 | system allocator、early native |
| `AFTER_RUNTIME_INIT` | 游戏运行时静态对象可分析后 | RTTI、FD4、DLRF、普通 native、allocator、资产 Hook |
| `AFTER_PROPERTIES_READY` | 游戏属性表可写后 | 离线属性和用户 property override |
| `AFTER_RENDER_READY` | 标题或菜单构造完成后 | 异步应用 CPU 亲和性 |
| `AFTER_DATA_READY` | 游戏 param 全部读取并后处理完成后 | `data_ready` native |

`AFTER_RENDER_READY` 只保证游戏可以渲染，不保证 param 已读完：Elden Ring 的 `TitleStep::STEP_InitMenu` 只构造菜单 job，Dark Souls III 的 `TitleFlowStep::STEP_Init` 只创建 title flow task，而 `CSSystemStep` / `SprjSystemStep` 的 boot phase 并不等待 param 任务。依赖 param 完整性的功能必须使用 `AFTER_DATA_READY`。

现有 Elden Ring 使用 `SteamAPI_Init` 作为 `AFTER_RUNTIME_INIT` 触发点。该机制可保留，但不是唯一合法触发方式。每个 adapter 必须声明可靠触发点及其 fallback；未触发时日志必须明确报告，不得静默等待。

功能安装结果统一记录为：

- `APPLIED`
- `SKIPPED_DISABLED`
- `UNSUPPORTED`
- `SIGNATURE_NOT_FOUND`
- `HOOK_FAILED`
- `DEFERRED_NOT_REACHED`

零个 Hook 被安装时不得记录为 `APPLIED`。

### 5.4 分析服务

后续多游戏功能将使用三个独立、按需初始化的分析服务：

- RTTI class/vtable map。
- initialized FD4 step table。
- DLRF RuntimeClass registry。

RTTI 服务需要支持同名类的多个 vtable，不能只保留最小对象偏移的 vtable。DLRF 服务用于 `SprjAutoControlAPI` 或 `CSAutoControlAPI` 的 `SetGameProperty` 查找。

分析服务失败只禁用依赖它的功能。例如 DLRF 失败不能阻止 Win32 VFS、package 扫描或不依赖 DLRF 的 native DLL 加载。

## 6. VFS 设计

### 6.1 路径命名空间与规范化

VFS core 使用 UTF-16 动态字符串，不使用 `MAX_PATH` 固定数组。所有目录扫描和查询必须经过同一套规范化逻辑：

- 统一分隔符。
- 折叠 `.`。
- 拒绝从 package 根目录逃逸的 `..`。
- 使用不区分大小写的比较键。
- 处理尾部 NUL，使 NUL 终止和非终止输入命中同一缓存键。
- 区分游戏虚拟路径、游戏根目录物理路径、package 物理路径与 fake UID。
- 扫描目录时检测 symlink/junction 环，避免无限递归。

适配器负责把游戏特有格式转换为 core 虚拟路径，例如 `data0:/...`、`sd:/...` 或根目录物理路径。VFS core 不应包含 Elden Ring、Nightreign、Sekiro 或 DS3 的路径字面量。

### 6.2 扫描、索引与优先级

启动时递归扫描 `[mod]`、TOML mods 或未来 package 列表中的目录。每个文件生成标准虚拟路径键和物理路径条目。

索引规则：

1. 按配置声明顺序扫描 package。
2. 同一键再次出现时，由后声明 package 的条目替换已有条目。
3. 扫描完成后冻结条目存储；缓存只保存稳定条目的指针或索引。
4. 记录覆盖来源，便于日志和未来冲突诊断。

需要保留现有相对路径语义：相对路径以配置文件目录为根；以 `\\` 开头的路径按配置所在盘根解释。迁移期间旧 `mods_file_search()` 与 `mods_file_search_prefixed()` 先实现为 VFS wrapper，待 Elden Ring parity 测试通过后再删除旧目录遍历。

### 6.3 分域缓存与 generation

不能将不同查询语义共享同一个缓存。至少拆分：

- 宽字符磁盘或 fake UID 路径到物理路径。
- ANSI 磁盘或 fake UID 路径到物理路径。
- 虚拟资源路径到物理路径。
- 虚拟资源路径到 fake UID。
- Wwise 输入路径到物理路径。

每个域同时缓存命中和未命中。缓存开启与失效遵循：

1. FileStep 初始化完成前，缓存禁用，查询实时计算。
2. FileStep 初始化完成后启用缓存。
3. FileStep 再次初始化时递增 generation 并清空所有缓存。
4. 插入缓存前重新检查 generation，防止跨 reset 的查询把旧设备状态写入新 generation。

所有缓存使用 `SRWLOCK` 或等价同步机制。返回的路径必须在 VFS 生命周期内稳定，不能在 shared lock 释放后失效。

### 6.4 Fake UID

Dantelion 内部对象不应长期直接持有任意 package 的绝对路径。对需要引擎内部重写的路径，VFS 使用稳定 fake UID：

```text
\\me3??<entry-index>
```

UID 字符串在条目建立时生成，并在整个 VFS 生命周期内保持稳定。查询缓存 reset 只清理查询键，不释放 UID；UID 始终解析回同一冻结条目。该行为采用项目记录的 me3 性能分支方案，避免缓存命中时复制字符串。

### 6.5 Win32 Hook

第一版安装：

- `CreateFileA`
- `CreateFileW`
- `CreateFile2`
- `CreateDirectoryA`
- `CreateDirectoryW`
- `CreateDirectoryExW`
- `DeleteFileA`
- `DeleteFileW`
- `CopyFileA`、`CopyFileW`、`CopyFileExA`、`CopyFileExW` 与 `CopyFile2`
- `MoveFileExW` 与 `ReplaceFileW`

这些 Hook 只调用 VFS 路由器；不得在每个 Hook 内重复实现路径搜索、缓存和读写策略。

路由时需要检查：

- 目标是否已是 package 的物理路径；若是，直接调用原 API。
- 请求是否是读取、写入、创建或删除。
- 命中条目是否标记为可写映射。
- 是否处于 VFS 后端访问的 TLS recursion guard 中。

普通资源包只在读取时重定向。对于写入请求，若没有显式可写映射，则调用原始 Windows API 并保持原路径。

### 6.6 存档与设置映射

独立存档是可写映射的首个实现：

- 按游戏存档根目录识别 `.sl2`。
- 同步映射 `.bak`。
- 指定替代存档不存在时，复制当前基础存档。
- 映射创建或复制失败时不得回退到主存档。

设置映射通过显式 API 注册精确虚拟路径。注册项包含读写权限和替代物理路径，不与普通 package asset index 混淆。

### 6.7 Dantelion device、BND 与 EBL

在对应 FileStep 初始化中执行：

1. 定位并验证 `DlDeviceManager`。
2. 读取并展开 virtual roots。
3. Hook disk device 的 `open_file`。
4. Hook `DlFileOperator::SetPath`、`SetPath2` 与 `SetPath3`。
5. Hook `MakeEblObject`；有 loose override 时使游戏回退到 disk device。
6. Hook 已挂载 BND4 devices 的唯一 `open_file` 实现。
7. Hook `MountEbl`；每次成功挂载后继续 Hook 新出现的 BND4 device。

hook 必须避免重复安装。`DlDeviceManager` 的结构和 critical section 使用必须按游戏 ABI 验证，不能仅依赖固定地址或没有布局检查的 singleton。

### 6.8 Wwise

Wwise locator 优先通过 RTTI 类 `DLMOW::IOHookBlocking` 获取 `open_by_name`，找不到时使用版本化 signature fallback。Wwise Hook 调用 VFS 的专用 Wwise 查询域，避免重复多前缀尝试和重复分配。

### 6.9 BootBoost

BootBoost 作为独立、可关闭的性能功能实现：

1. Hook `MountEbl`。
2. 从游戏 RSA public key 获得 block 大小。
3. 用临时 BHD stub 让游戏创建 device，读取解密后 BHD 长度。
4. 使用原始加密 BHD 的稳定 hash 作为缓存键。
5. 命中缓存时校验保存长度、raw-deflate 解压结果与完整消费。
6. 使用游戏 allocator 分配 BHD 内容，并写回 EBL device holder。
7. 未命中时让游戏正常解密，之后用临时文件加原子替换写入压缩缓存。
8. 缓存损坏时删除缓存并回退到游戏正常解密。

DS3 的 BHD holder 偏移为 `0xC0`；Sekiro 与 Elden Ring 为 `0xB0`。该差异必须由游戏 traits 提供。

## 7. 多游戏 Host 功能

### 7.1 启动与注入

launcher 必须：

- 支持 `--launch-target eldenring`、`nightreign`、`sekiro`、`darksouls3` 及合理别名。
- 未指定 `--launch-target` 时读取 `YAFSML.ini` 顶层的 `game=...`；未配置时默认 Elden Ring，显式启动目标优先。
- 保留 `--modengine-dll` 作为 `--modloader-dll` 别名。
- 从显式 EXE、游戏目录、当前目录和 Steam library manifest 按固定优先级定位游戏。
- 使用游戏描述符的 App ID、工作目录和 EXE 相对路径。
- 设置 `SteamAppId`、`SteamGameId` 与 `SteamOverlayGameId`。
- 统一使用 Remote Thread 调用 `LoadLibraryW` 注入 DLL，并在注入完成前保持游戏主线程挂起。

DLL 进程内校验失败时，必须停止游戏专用 Hook，不能继续把错误 adapter 应用于目标进程。

### 7.2 Native DLL 与 ABI

加载顺序：

1. 按 `[dll]` 配置加载外部 DLL。
2. 对外部 ModEngine extension，继续调用 `modengine_ext_init`。

项目内置 ER 扩展已拆出当前仓库。当前仓库不再导出或依赖 `modloader_ext_init`；ModEngine extension 的加载、attach 和逆序 detach 行为保持兼容。

### 7.3 离线属性与 debug properties

通过 DLRF 找到：

- DS3、Sekiro：`SprjAutoControlAPI.SetGameProperty`。
- Elden Ring、Nightreign：`CSAutoControlAPI.SetGameProperty`。

默认设置 `Menu.IsEnableOnlineMode=false`。用户显式 property override 后应用，保证用户配置覆盖内部默认值。DLRF 或 property-init Hook 失败时，日志必须显示离线保护没有真正应用。

### 7.4 跳 Logo 与白闪修复

- DS3、Sekiro 使用 SPRJ title step 策略。
- Elden Ring、Nightreign 使用 FD4 `TitleStep::STEP_BeginLogo` 策略。
- 四款游戏都 Hook `RegisterClassExW`，将窗口类背景刷改为黑色，避免启动白闪。

### 7.5 Mimalloc allocator patch

除 Nightreign 外的三款游戏均实现：

1. 在 `BEFORE_MAIN` 替换 system allocator getter。
2. 在 `AFTER_RUNTIME_INIT` 查找 `CSMemoryImp` 或 `NS_SPRJ::CSMemoryImp`。
3. 按游戏签名找到 allocator table 首尾，并全部替换为 mimalloc allocator。
4. Hook `CSMemoryImp::init/deinit` 为 no-op。
5. 应用游戏特定补丁。

差异：

| 游戏 | 特定逻辑 |
| --- | --- |
| DS3 | debug allocator getter；allocator table 尾部 `0x78` 策略 |
| Sekiro | 统计调用目标选取 debug allocator getter；allocator table 尾部 `0x70` 策略 |
| Elden Ring | `CSGraphicsImp` 首槽 no-op；Elden Ring table 尾部策略 |

任何 system allocator Hook 失败都必须阻止 heap allocator table 修改，避免混合 allocator 释放。

### 7.6 Regulation 保护

- 仅当 VFS 中存在由 mod 提供的 `regulation.bin` 覆盖，且 `prevent_regulation_save_write` 已启用时，才安装保护 Hook；未覆盖时保持游戏原有的存档写入机制。
- Elden Ring、Nightreign：Hook `CSRegulationStep::STEP_Idle`，调用原函数后将 raw regulation 长度设为 `0`，但保留由游戏管理的 buffer，避免已排队的存档任务读取已释放内存。
- DS3、Sekiro：仅在存在 mod 提供的 `regulation.bin` 覆盖时实现并验证旧式「阻止写 regulation 到存档」Hook。

旧式 DS3/Sekiro 实现不得复制 me3 当前疑似错误的 `(s?-u)` 正则。必须使用正确模式、检查至少安装一个 Hook，并在零匹配时报告失败。

### 7.7 DS3 loose param

仅 Dark Souls III：若检测到下列任一 loose parambnd 覆盖，则应用 `Game.Debug.EnableRegulationFile=false`：

```text
data1:/param/gameparam/gameparam.parambnd.dcx
data1:/param/gameparam/gameparam_dlc1.parambnd.dcx
data1:/param/gameparam/gameparam_dlc2.parambnd.dcx
```

### 7.8 CPU 亲和性

`cpu_affinity` 不再在进程早期立即应用。策略 `1` 至 `4` 在各 adapter 报告 `AFTER_RENDER_READY` 后由独立 worker 异步应用，策略 `0` 保持现有亲和性。

| 游戏 | 触发 step | 时机 |
| --- | --- | --- |
| Elden Ring | `TitleStep::STEP_InitMenu` | 原函数返回后 |
| Nightreign | `TitleStep::STEP_InitMenu` | 原函数返回后 |
| Sekiro | `TitleFlowStep::STEP_Init` | 原函数返回后 |
| Dark Souls III | `TitleFlowStep::STEP_Init` | 原函数返回后 |

`ML_RENDER_READY_FILE_STEP_AFTER_ORIGINAL`（与资产 FileStep Hook 共用安装路径）保留为回退策略，当前四款游戏都不使用。

触发 Hook 属于可选能力。安装失败时记录 warning 并保持现有 CPU 亲和性，不阻断 `AFTER_RUNTIME_INIT`、VFS、Logo、properties、regulation 或外部 DLL。卸载时先禁用 Hook，等待正在执行的调用结束，再移除 Hook；CPU worker 也会在卸载期间等待完成。

### 7.9 Param Data Ready

依赖 param 完整性的 MOD 需要一个「param 已全部读完」的入口，而不是渲染就绪。四款游戏都由 `ParamStep` 负责读取 param，并都以 `CSRegulationManager` 报告空闲作为完成判据，但实现分成两代，因此需要两条解析策略。

| 游戏 | 策略 | 锚点 |
| --- | --- | --- |
| Elden Ring | `ML_DATA_READY_FD4_NAMED_STEP` | `ParamStep::STEP_Wait`，首次进入即 param 就绪 |
| Nightreign | `ML_DATA_READY_FD4_NAMED_STEP` | 同上 |
| Sekiro | `ML_DATA_READY_SPRJ_PARAM_STEP` | 由 `NS_SPRJ::ParamStep` 的 RTTI vtable 推导步骤表 |
| Dark Souls III | `ML_DATA_READY_SPRJ_PARAM_STEP` | 同上 |

Elden Ring 与 Nightreign 的 `ParamStep` 使用具名 FD4 step 模板（`STEP_Load`、`STEP_LoadWait`、`STEP_Wait`、`STEP_Unload`）。`STEP_LoadWait` 会等到 regulation manager 完成并跑完 post-load 修补后才推进，因此 `STEP_Wait` 首次被调用时 param 已经完整；该步骤名在 `.rdata` 名表内，`fd4_step_find()` 即可解析，无需新增签名。该步骤此后每帧运行，Hook 必须先检查一次性 latch 再触碰 lifecycle 注册表。

Sekiro 与 Dark Souls III 的 `ParamStep` 是旧的 `NS_SPRJ::Step<T>`，步骤表不带名字，只能从构造函数推导：`rtti_find_vtable()` 取得类 vtable，`sprj_step_find_from_vtable()`（`src/process/sprj_step.c`）在 `.text` 内找到 `lea rax,[rip+vtable]; mov [this], rax`，再回溯同一构造函数中的 `lea rax,[rip+table]; mov [this+table_offset], rax` 与 `mov [this+index_offset], rcx`。步骤索引偏移与表地址来自同一次匹配，形状不符即整体失败。等待步骤在 param 读完时把步骤索引写为负值结束步骤机，Hook 因此比较调用前后的索引，只在该次转换上推进阶段。

安装失败时该阶段 fail closed：`AFTER_DATA_READY` 不再被推进，并记录 error。不允许退回 `AFTER_RENDER_READY` 触发——那会在 param 可能仍在加载时告知所有消费者数据已就绪，而这正是这些消费者唯一需要依赖的保证。代价是配置为 `data_ready` 的 DLL 在该版本上不会加载，因此 error 必须写明这一后果。

`[dll]` 的 `data_ready` 条件与 `delay` 共用同一个 deferred worker 线程，避免在报告数据就绪的游戏 step 线程上执行 `LoadLibrary`。共用而非各起一个线程是为了保证 `after`：两个并发 worker 会让 `delay` 条目与 `data_ready` 条目之间的相对顺序未定义，跨这两个阶段的 `after` 依赖就可能早于依赖项加载。单个 worker 按数组顺序（`extdlls_prepare()` 已把依赖项排在前面）顺序处理全部 deferred 条目，该顺序即为全序，因此已经 deferred 的条目无论依赖谁都不需要改阶段；只有仍在主线程阶段的条目需要被推入 worker。

代价是 deferred 条目之间存在队头阻塞——这本来就是既有语义（`delay` 的等待历来是累加的）。`AFTER_DATA_READY` 未到达时，worker 会在第一个 `data_ready` 条目上同时等待数据就绪与取消事件；卸载会先置取消事件再 join，因此不会挂住。

## 8. 分阶段实施计划

### 阶段 0：测试基线与行为冻结

目标：在移动现有代码前，为 Elden Ring 现有对外行为建立可回归基线。

工作项：

- 为配置默认值、INI 相对路径、TOML fallback、mods/dlls 顺序写 golden tests。
- 为存档名称替换、archive 路径转换、Wwise 候选路径写纯函数测试。
- 为 ext DLL 的 LoadLibrary、init、uninit、ModEngine attach/detach 顺序写 fixture DLL 测试。
- 记录当前 Elden Ring 功能安装日志和游戏版本信息。

完成条件：现有 Elden Ring 行为可由自动测试和人工回归共同描述；后续重构不允许无意改变未列出的行为。

### 阶段 1：ABI 与 Wwise 前置修正

目标：完成第 4 节的 FD4、allocator 与 Wwise 修正。

完成条件：

- 所有新增静态断言和 smoke test 通过。
- Elden Ring regulation、patch_mem、Wwise 资产覆盖均完成现场回归。
- 代码不再保留旧 ABI 或 `READ_EBL = 9` 兼容分支。

### 阶段 2：Game Registry 与 Elden Ring adapter 包装

目标：先分离架构，不改变 Elden Ring hook 逻辑。

工作项：

- 引入 `ml_game_descriptor_t`、registry 与 process 内 game context。
- launcher 将 App ID、EXE 路径与 Steam 环境变量改为 descriptor 驱动。
- injected DLL 通过 descriptor 探测当前进程。
- 将现有 `eldenring_install/uninstall` 机械包装为 Elden Ring adapter。
- 将真正通用的 IME 留在 core；Wwise、FD4、Dantelion 移到 adapter capability。

完成条件：不传 `--launch-target` 时仍默认 Elden Ring；旧 `eldenring.exe`、Steam 查找与配置文件的运行结果不变。

### 阶段 3：分析服务与生命周期

目标：为多游戏 Host 功能提供可验证的共同基础。

工作项：

- RTTI 支持多个 vtable。
- 增加 DLRF RuntimeClass 扫描。
- 明确并实现 MSVC 2012/2015 string/vector ABI。
- 将 Hook 注册封装为可返回状态、可阶段回滚的服务。
- 将 `SteamAPI_Init` 后置初始化改为 adapter 可配置的 runtime-ready trigger，并记录 fallback 状态。
- 记录 signature 匹配数量、选择规则、地址与失败原因。

完成条件：RTTI、FD4、DLRF 任一失败只降级相关功能；不阻断 VFS index、Win32 文件重定向或独立 native DLL 加载。

### 阶段 4：冻结式 VFS index

目标：替代逐查询目录遍历，完成统一路径、last-wins、分域缓存和 generation。

工作项：

- 创建 VFS path、mapping、entry storage、cache 模块。
- 递归扫描 package，处理 Unicode、长路径和循环链接。
- 将当前 `mod.c + filecache.c` 改为 legacy wrapper。
- 完成读写路由和 TLS recursion guard。
- 加入 fake UID 编解码。

完成条件：在不安装新的 Dantelion Hook 前，Elden Ring 的现有 `mods_file_search*` 调用通过 VFS 返回与预期一致的结果；相同路径的最后 package 生效。

### 阶段 5：Win32、archive 与 Wwise 迁移

目标：让所有当前 Elden Ring 路径入口访问统一 VFS。

迁移顺序：

1. `CreateFile*`。
2. Elden Ring archive resolver。
3. Wwise resolver。
4. 独立存档和显式可写设置映射。
5. `CreateDirectory*` 与 `DeleteFile*`。

每一步保留旧实现的对照模式，在测试或诊断构建中记录新旧解析结果差异。确认 parity 后删除旧路径专用查找。

完成条件：普通资源只读、存档可写、设置映射可写的路由测试通过；Elden Ring 无 package 时不安装不必要的高频资源 Hook。

### 阶段 6：Dantelion device、BND/EBL 与 BootBoost

目标：完成 me3 级别的 archive 覆盖。

工作项：

- 实现 `DlDeviceManager` 布局验证和锁。
- 实现 virtual root 展开。
- Hook disk device、SetPath、MakeEblObject、BND4 direct open 与动态 MountEbl。
- 实现 BHD holder 的游戏特定布局。
- 实现 BootBoost 缓存、校验、损坏回退和原子写入。

完成条件：Elden Ring 基础 BND、DLC EBL、Wwise、重复 FileStep 初始化和 BootBoost 缓存命中全部通过现场验证。

### 阶段 7：Host 功能泛化与扩展兼容

目标：将 Logo、离线属性、allocator、regulation 和 native 生命周期从 ER 专用逻辑中分离，并保持外部 ModEngine DLL 兼容性。

完成条件：ModEngine extension 行为不退化；不再导出或依赖项目自定义扩展 ABI，且跨游戏加载不会把 ER 专用逻辑应用到其他游戏。

完成状态：已完成。项目自定义扩展 ABI、对应 fixture 和初始化分支已移除；项目自带扩展 DLL 已从当前仓库拆出；ModEngine extension 的加载、attach、逆序 detach 行为保持兼容。白闪、Logo、离线属性、allocator 和 regulation 已拆分为由 game trait 选择的内部 Host capability；FD4 与 SPRJ Logo、FD4 与旧式 SPRJ regulation 保护均具备独立安装路径，Elden Ring、Sekiro 与 Dark Souls III 具有各自的 allocator 策略，Nightreign 明确标记为不支持该补丁。

### 阶段 8：Sekiro 稳定支持

目标：完成第一个非 Elden Ring 的稳定 adapter。

工作项：

- 新增 launcher discovery、存档根、SPRJ FileStep、SPRJ control API、Logo 和 allocator 策略。
- 使用 MSVC 2015 ABI 与 `0xB0` EBL holder。
- 完成 VFS、Wwise、BootBoost、独立存档、离线属性、Logo、白闪、mimalloc 与 regulation 保护。

完成条件：连续启动、进入标题、读档、返回标题和退出至少 10 次，无重复 Hook、缓存污染、存档误写或退出崩溃。

以下两段保留阶段 8 的开发批次记录，其中测试数量和 fake UID 语义是当时状态；
当前实现以第 10.2 节为准。

开发状态：进行中。首批改动已启用 Sekiro adapter，不再由通用入口直接拒绝非 Elden Ring 进程；launcher registry 已覆盖 App ID、可执行文件、存档根目录、MSVC 2015 ABI、SPRJ FileStep、control API、`0xB0` EBL holder、Logo、allocator 与 regulation traits。Sekiro 现使用 `SteamAPI_Init` 触发 `AFTER_RUNTIME_INIT`，并按 descriptor 安装 `SprjFileStep::STEP_Init` Dantelion asset Hook；INI 游戏段和 ModEngine2 TOML fallback 也改为按当前进程 descriptor 选择。第二批改动增加了 Sekiro 独立存档映射和通用 Win32 VFS Hook；`.sl2` 首次映射会复制当前基础存档，`.bak` 使用同一替代名称，映射或复制失败时拒绝访问主存档。第三批改动增加了 Win32 Hook 批量安装与逐位置失败回滚测试、writable registry 并发压力测试，以及 Sekiro runtime-ready 和 asset capability 状态日志。第四批改动实现了 FileStep 前禁用缓存、FileStep 后递增 generation 并清空缓存、跨 reset 插入检查，以及带 generation 和 entry index 的 fake UID；旧 UID 在下一次 FileStep 初始化后失效。第五批改动移除了 Wwise 候选路径构造中的 `MAX_PATH` 限制，改用动态 UTF-16 字符串，并增加超过 260 个字符的 WEM 路径测试。第六批改动将通用 Win32 Hook 的 ANSI 路径转换改为按输入长度动态分配，`CreateFileA`、`DeleteFileA` 与 `CreateDirectoryA` 不再因固定缓冲区跳过长路径路由，并增加约 700 个字符的转换测试。第七批改动将 Sekiro 独立存档根目录、规范化源路径和替代目标路径改为动态 UTF-16 字符串；环境变量和绝对路径长度查询会在缓冲区需求变化时重试，映射查询使用加锁快照，避免初始化或卸载期间读取已释放配置。第八批改动将 `CopyFileW` 纳入 Win32 VFS Hook 批处理，对复制源和目标同时执行独立存档映射，并在任一端映射失败时拒绝复制，避免存档轮换绕过替代文件。第九批改动将同一双端路由扩展到 `CopyFileExW` 和 `CopyFile2`，保留进度回调、取消标志、复制参数和 HRESULT 语义。第十批改动增加了 `CopyFileA` 与 `CopyFileExA` 动态路径转换和双端映射，并拒绝 Windows 设备名、控制字符、通配符及尾随空格或句点等无效替代存档名。第十一批改动使现有 `.bak` 与主 `.sl2` 一样在首次映射时复制到替代文件，并增加初始化、查询和卸载并发压力测试。第十二批改动移除了 VFS 规范化的 512 段固定上限，路径段回退栈改为按输入长度动态分配，并增加 600 段路径及连续 `..` 折叠测试。第十三批改动将 BootBoost 缓存目录和 hash 文件名改为动态 UTF-16 路径，移除缓存入口的 `MAX_PATH` 拼接限制，并增加约 600 个字符的缓存目录测试。第十四批改动将虚拟资源、宽字符磁盘、ANSI 磁盘和 Wwise 查询拆分为独立命中与未命中缓存；所有域共享 generation 和 reset 锁，但不会再混用查询键空间。第十五批改动增加了虚拟资源路径到 fake UID 的独立命中与未命中缓存；返回值始终复制给调用方，reset 会释放旧 UID 并使下一代重新编码，并发压力测试同时覆盖查询、UID 编码和 generation 重置。第十六批改动在独立存档路由前逐级检查存档根以下的现有路径组件，拒绝 symlink、junction 和其他 reparse point；测试在系统允许创建非特权目录符号链接时验证该逃逸路径被拒绝。第十七批改动修复了 BootBoost 原始 mount 失败时未释放设备管理器锁、重复 disk open 仅首次应用替代路径，以及 Dantelion 展开绝对路径未剥离游戏根导致 VFS 与 fake UID 未命中的问题；新增带目录边界检查的 prefixed domain lookup 和 prefixed UID API。第十八批改动将 `MoveFileExW` 与 `ReplaceFileW` 纳入独立存档路由，分别映射移动源与目标，以及替换目标、替换源和可选备份路径；Hook 事务测试扩展为覆盖 15 个安装位置。自动构建、registry、VFS、Hook 事务、generation 并发、存档映射、Wwise 长路径和 ANSI 长路径测试已覆盖这些非现场行为。

自动开发状态：已完成。第十九批补齐 asset、Wwise、IME 和 runtime-ready Hook 的显式卸载，成功卸载后重置 trampoline、目标地址、`INIT_ONCE`、动态 vtable 记录、日志门和 adapter 图像状态；卸载失败会保留状态、输出目标与 MinHook 状态，并允许后续清理重试。Hook 批处理会报告安装失败后的不完整回滚，跳过 NULL 目标，对重复动态目标只卸载一次，并继续尝试剩余目标。新增 Win32 Hook 宿主测试，覆盖 `CreateFileW`、`CreateFile2`、只读 package 路由、writable 路由、存档映射失败、recursion bypass、直接 package 路径读写、复制、移动、目录和删除行为。Debug 全量构建和 29 项 CTest 已通过。

阶段 8 验收状态：已完成。Sekiro 的 signature、Wwise、BootBoost、独立存档与备份轮换、离线属性、Logo、白闪、mimalloc、regulation protection 和连续启动流程均已完成现场验收；因此后续工作得以进入阶段 9。

### 阶段 9：Dark Souls III 实验性支持

目标：完成 Dark Souls III 的 me3 host 功能；后续加入 dearxan v0.5.3 的 C11 实现。

工作项：

- 新增 MSVC 2012 string/vector ABI。
- 使用 `0xC0` EBL holder。
- 实现 DS3 allocator getter、`0x78` table 策略、SPRJ FileStep 和 control API。
- 实现 loose param property override。
- 完成 VFS、Wwise、BootBoost、独立存档、离线属性、Logo、白闪、mimalloc 和旧式 regulation 保护。

完成条件：所有功能都有明确状态日志，且至少一次实际 Hook 被验证安装。发布内容标注「实验性」；当前版本会强制启用 Arxan 中和。

自动开发状态：已完成。新增独立 Dark Souls III adapter，以 `SteamAPI_Init` 触发 `AFTER_RUNTIME_INIT`，接入 Win32 VFS、独立存档、Wwise、SPRJ FileStep、Dantelion/BND/EBL、`0xC0` BHD holder、BootBoost、离线属性、SPRJ Logo、白闪、DS3 debug allocator getter、`0x78` allocator table 和旧式 regulation 保护。Dantelion string/vector/device 路径已按 descriptor 支持 MSVC 2012 与 MSVC 2015 ABI；检测到三种 loose parambnd 任一覆盖时，会在属性初始化后应用 `Game.Debug.EnableRegulationFile=false`。当前启动日志明确标注实验性支持和强制 Arxan 中和，并报告 adapter、文件路由、runtime-ready 和 deferred capability 状态。

阶段 9 验收状态：已完成（2026-07-18）。依据 Cinders 与 Dark Souls III 现场日志，已确认 loose param、SPRJ FileStep、Win32 文件路由、Dantelion/BND/EBL、BootBoost、独立存档、离线属性、mimalloc allocator 和 regulation protection 的请求状态与实际结果；其中 `CSMemoryImp::deinit` 的 Hook 失败按 me3 语义作为可选项跳过，最终 heap allocator capability 仍为 `APPLIED`。Wwise 在当前 Cinders 内容中没有匹配的 `.bnk`/`.wem` 条目，因此正确报告为 `NOT_REQUESTED`。`skip_intro` 已按 me3 的 after-runtime 登记顺序修正。dearxan C11 实现加入后，自动测试增至 43 项；Dark Souls III 仍保持实验性支持。

### 阶段 10：稳定化、发布与文档

目标：在 YAFSML 重命名完成后，统一发布准备和文档。

工作项：

- 整理每游戏功能矩阵、配置说明、已知限制和支持等级。
- 更新 dist 打包规则，避免 DS3/Sekiro 默认携带并加载 ER 专用 extension。
- 检查 launcher、DLL、extension 的游戏元数据一致性。
- 统一 YAFSML 项目名、二进制名、配置文件名、发布包和文档。

阶段 10 当前状态：进行中。重命名已完成：项目、Windows 版本资源、`YAFSML.exe`、`YAFSML.dll`、`YAFSML.ini`、环境变量、远程初始化导出、dist 归档和 GitHub Release 工作流均已直接切换到 YAFSML，不保留旧名称兼容入口。启动器已支持配置文件顶层 `game=...`，并按显式 `--launch-target`、配置值、Elden Ring 默认值的顺序选择游戏。Nightreign 已加入稳定支持；四款游戏的 CPU 亲和性均延迟至 render-ready step 后异步应用。Debug 构建、43 项 CTest 和 dist 内容检查已通过。剩余内容是四游戏统一发布验证、GitHub Release 实际运行复核、功能矩阵和已知限制整理；Dark Souls III 继续保留实验性标记。

## 9. 测试计划

### 9.1 不运行游戏即可完成

| 范围 | 用例 |
| --- | --- |
| Launcher/Game Registry | 四游戏 alias、App ID、EXE、工作目录、Steam 环境变量、显式路径和 Steam fallback |
| Config | 默认值、game section 隔离、INI/TOML 顺序、UTF-8、相对路径、错误输入 |
| VFS path | 分隔符、大小写、`.`、`..`、尾部 NUL、Unicode、长路径、symlink/junction 环 |
| VFS mapping | last-wins、命中/未命中缓存、generation、fake UID、可写映射、recursion guard |
| Win32 Hook 宿主 | CreateFile、CreateFile2、目录创建、删除、读写路由和直接 package 路径绕过 |
| Binary analysis | synthetic PE 上的 section、scanner、RTTI、多 vtable、FD4 与 DLRF |
| ABI | FD4 双参数 trampoline、Dantelion vtable 布局、mimalloc 分配和对齐 |
| Wwise | `sd:/`、`sd_dlc02:/`、语言目录、WEM 两种目录、枚举值 |
| Ext DLL | 内置 DLL 的直接初始化、共享扫描和 Hook 代码、ModEngine 加载/卸载、非 ER 游戏跳过 ER 专用扩展 |
| Hook lifecycle | 第 N 个 Hook 失败的回滚、卸载幂等、禁止 NULL unhook |

测试用例不得提交游戏二进制。可提供本地只读 EXE 验证目标：开发者指定合法安装路径后，仅检查版本、signature 命中数量和结构识别结果。

### 9.2 每游戏现场验收

每款游戏的每个受支持版本至少测试：

- 标准 Steam 路径与显式 `--game-path`。
- launcher 注入和 proxy DLL 启动方式。
- loose 文件、param、EMEVD、模型、贴图、`regulation.bin`。
- BND/EBL 内文件与 DLC archive。
- Wwise `.bnk` 和 `.wem`。
- BootBoost 首次生成、再次命中、缓存损坏回退。
- 独立 `.sl2`/`.bak` 与原始主存档 hash 不变。
- 默认离线属性实际调用成功。
- 显示/跳过 Logo 与白闪修复。
- early/normal native DLL 生命周期。
- mimalloc 压力测试。
- 连续 10 次启动、进标题、读档、退出。

Dark Souls III 额外测试：

- MSVC 2012 容器布局。
- loose parambnd property override。
- 验证强制启用 dearxan 后的启动、回到标题、切图和调试器附加稳定性。

## 10. 发布门槛与风险控制

### 10.1 当前配置与文档同步

当前配置模板位于 `src/YAFSML.ini`，发布包会将其复制为
`YAFSML.ini`。独立启动器在未指定 `--launch-target` 时读取顶层 `game=...`，
未配置时默认 Elden Ring；显式启动目标优先。加载器的通用补丁位于 `[patch]`，
CPU 亲和性策略位于 `[tweak]`，并在四款游戏各自的 render-ready step 后异步应用；日志设置位于 `[log]`，外部 DLL 和模组目录分别位于
`[dll]` 和 `[mod]`。Dark Souls III adapter 会安装实验性的共享 Host 与游戏适配 Hook，
并强制启用 Arxan 中和，实际能力以状态日志和目标版本现场验证为准。全部选项、外部 DLL
顺序和启动器参数已同步到仓库根目录的中英文 README。

ModEngine2 TOML fallback 仍按 Game Registry 选择 `config_eldenring.toml`、`config_nightreign.toml`、`config_sekiro.toml` 或 `config_darksouls3.toml`。

### 10.2 当前实现状态修正

前文阶段记录中的历史批次说明保留作为开发轨迹；当前代码和测试结果以本节及
README 为准。当前 Debug 构建已通过 43 项 CTest。VFS fake UID 采用稳定的
`\\me3??<entry-index>` 字符串，UID 在 VFS 生命周期内保持有效；缓存命中不复制
字符串。无模组时跳过普通 VFS 查询，但配置的存档文件名替换仍安装所需的文件 Hook。

### 10.3 发布门槛

每个发布支持的游戏版本必须满足：

- launcher、DLL 和 ext DLL 的 `game_id` 一致。
- 关键 signature 记录匹配数量和地址。
- 零个 Hook 不得报告成功。
- 默认不写入主存档。
- 默认不进入官方在线模式，且日志能确认 property override 已执行。
- VFS 读取不会把写请求误导向普通 package 资源。
- FileStep 重跑不会使用旧 generation 缓存。
- 任何分析服务失败只影响其依赖功能。
- DS3 在 dearxan 与长期稳定性验证完成前保持实验性。

### 10.4 主要风险

| 风险 | 控制措施 |
| --- | --- |
| 游戏更新使 signature 或布局失效 | 记录版本、匹配数量与 feature status；不匹配时禁用相关功能 |
| 迁移改变 ER 模组优先级 | 将 last-wins 作为明确行为变更；使用冲突 fixture 和发布说明验证 |
| VFS 误重定向写入 | 资源层默认只读；仅精确注册存档和设置可写映射；真实 Win32 Hook 宿主测试 |
| 缓存与初始化竞态 | 缓存分域、generation、冻结条目和锁；FileStep 前不缓存 |
| allocator 混用或 ABI 偏差 | 先完成第 4 节强制 ABI 修正；system allocator 失败时禁止 heap patch |
| Dantelion 内部布局随游戏变化 | 每游戏显式 traits、布局验证、版本化 signature 与现场测试 |
| DS3 Arxan 恢复或阻止 Hook | 使用 dearxan 状态日志、自动分析测试和长期稳定性验证监测 |
| 从 me3 复用源码的许可 | me3 提供 MIT/Apache-2.0 双许可；选择 MIT 路径时保留原版权和许可声明 |

## 11. 参考实现与关键文件

本计划的主要本地参考：

- me3 VFS mapping：`E:\\Projects\\me3\\crates\\mod-host-assets\\src\\mapping.rs`
- me3 Win32 文件 Hook：`E:\\Projects\\me3\\crates\\mod-host\\src\\filesystem.rs`
- me3 Dantelion/EBL/BootBoost Hook：`E:\\Projects\\me3\\crates\\mod-host\\src\\asset_hooks.rs`
- me3 device manager：`E:\\Projects\\me3\\crates\\mod-host-assets\\src\\dl_device.rs`
- me3 EBL 布局：`E:\\Projects\\me3\\crates\\mod-host-assets\\src\\ebl.rs`
- me3 Wwise：`E:\\Projects\\me3\\crates\\mod-host-assets\\src\\wwise.rs`
- me3 游戏元数据：`E:\\Projects\\me3\\crates\\mod-protocol\\src\\game.rs`
- me3 host 生命周期：`E:\\Projects\\me3\\crates\\mod-host\\src\\lib.rs`
- me3 allocator：`E:\\Projects\\me3\\crates\\mod-host-types\\src\\alloc.rs`
- 当前 mod 搜索：`src/modloader/mod.c`
- 当前 filecache：`src/modloader/filecache.c`
- 当前 Elden Ring Hook：`src/modloader/patches/eldenring.c`
- 当前 Wwise Hook：`src/modloader/patches/common.c`
- 当前 FD4 step：`src/process/fd4_step.c`
- 当前 allocator：`src/modloader/dl_allocator.h`

## 12. 后续工作顺序

后续按以下顺序推进：

1. 完成四游戏的统一发布验证：标准 Steam 路径、显式 `--game-path`、launcher 注入、proxy DLL、native DLL 生命周期和各游戏受支持资源类型。
2. 完善发布包与功能矩阵：检查 dist 规则、游戏元数据、配置说明、已知限制和实验性支持标记。
3. 完成 YAFSML 重命名后的发布元数据和文档复核。

任何阶段完成前，不提前在下一个阶段复制游戏特定 signature 或 patch。每阶段必须先满足对应自动测试与 Elden Ring/Nightreign/Sekiro/DS3 的适用验收条件，再进入下一阶段。
