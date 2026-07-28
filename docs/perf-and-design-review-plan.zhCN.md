# YAFSML 性能与设计缺陷审查计划（第二轮）

本文件记录第二轮系统性审查的结论与执行计划。第一轮（`docs/defect-fix-and-refactor-plan.zhCN.md`,
提交 `9dd15c8`）聚焦内存安全与死代码；本轮聚焦**热路径性能**与**结构性设计缺陷**,
覆盖第一轮之后新增的 `src/dearxan/`（Arxan 中和,约 4000 行）、Nightreign 适配与
第一轮明确延后的项。

## 基线

`cmake-build-debug`（VS 2026,Debug）全量构建通过,43 个 smoke 测试全绿。

```
cmake --build cmake-build-debug --config Debug
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

## 执行原则

- 每个阶段结束跑一次完整构建 + ctest,绿了才进下一阶段;阶段之间可单独回退。
- 性能修复不改变可观测行为。凡是需要证明「行为等价」的改动,证明写在代码注释里。
- 遵循 `AGENTS.md`:C11-only、主 DLL 用 `ml_mem_*`、`wchar_t` 路径、匹配既有风格。
- 仅在 `CHANGELOG.md` 的 `#### Unreleased` 段补记,不改 `YAFSML_VERSION`,不发版。

---

## 缺陷清单

记号:**N** = mod 文件总数（大型 mod 常见 1e4–1e5）;**T** = 游戏 `.text` 字节数
（Elden Ring 约 4e7–6e7）。

### P0 — 每次文件打开都付的代价（游戏运行期）

游戏每次 `CreateFileW` 都会走 `create_file_w_hooked` → `route_wide`,
Dantelion 每次资源打开都会走 `disk_open_file_hooked` / `ebl_open_file_hooked` →
`make_override_path`。以下四项都在这两条路径上。

| # | 位置 | 缺陷 | 代价 |
|---|------|------|------|
| 1 | `vfs.c:576` `vfs_is_package_path` | 对全表做 `CompareStringOrdinal` 线性扫描;**且恒为假** | 每次打开 O(N) 次全 Unicode 比较 |
| 2 | `vfs.c:583` `vfs_route_writable_path` | 无界负缓存 + 每个首见路径取**独占**锁 | 内存随进程访问过的路径数无界增长;首访问全线程串行 |
| 3 | `save_mapping.c:249` `ml_save_mapping_route` | 未做扩展名前置过滤就 `strdup(save_root)` + 两次快照 + `canonicalize_path` | 每次打开 1 次堆分配 + 2 次 `GetFullPathNameW`（进程当前目录锁） |
| 4 | `asset_hooks.c:423` `log_override_once` | 独占锁下对已记录路径线性扫描 | 覆盖 M 个文件时总计 O(M²),且串行化资源加载 |

**第 1 项的恒假证明**:`entries[i].path` 由 `vfs_join(root, ...)` 生成,`root` 来自
`vfs_add_package` 且经 `PathIsDirectoryW` 校验,故必然以盘符或 UNC 前缀开头。
`vfs_route_read_path` 在生产中只经 `vfs_route_read_path_prefixed` 调用,其入参是
`vfs_strip_path_prefix` 的返回值 —— 该函数只在下一字符为 `\0` / `\` / `/` 时返回非空,
故返回值必然以反斜杠开头或为空串。绝对路径与相对路径的序数比较不可能相等。
测试中直接调用 `vfs_route_read_path` 时传入的同样是相对路径。因此**删除该调用在生产
与测试中均行为等价**;重入由 `vfs_recursion_depth` 与 `vfs_uid_to_path` 快路径覆盖。

**第 2 项的前置过滤等价性**:路径要匹配 `writable_entries[i].key`,其归一化形式必须与
key 全等,从而归一化后的**末段**也必须相等。`vfs_normalize_path` 逐字符 `towlower`,
不改动末段内容 —— 唯一例外是末段为 `.` / `..`（会被折叠到父目录）或路径以分隔符结尾。
故前置过滤按「原始路径末段与已注册 key 末段大小写无关相等」判断,并对末段为
`.` / `..` 及以分隔符结尾的路径回退到慢路径,即为精确等价。

**第 3 项的前置过滤等价性**:函数返回非 `false` 的全部情形要求 `raw_ext` 属于
{已映射扩展名} ∪ {已失败扩展名} ∪ {`.bak`}（`mapping_failed` 置位时另需放行 `.sl2`
并保守跳过快路径）。逐分支核对见代码注释。

### P0 — Wwise 音频流

| # | 位置 | 缺陷 | 代价 |
|---|------|------|------|
| 5 | `common.c:81` `ak_file_location_resolver_open` | 每次打开最多 11 次 `ml_mem_alloc`/`free` 拼路径 | 音频流式加载期持续的分配抖动 |

### P0 — 一次性初始化占用热路径锁

| # | 位置 | 缺陷 |
|---|------|------|
| 6 | `asset_hooks.c:542,638` `hook_file_operator` | 每次 `disk_open_file` 返回都取**独占** SRW 锁,只为读一个「已尝试过」布尔量 |

### P1 — 启动延迟

| # | 位置 | 缺陷 | 代价 |
|---|------|------|------|
| 7 | `dearxan/vm.c:13-103` | VM 内存是**单链表 + 线性查找**;每次分叉深拷贝整条链 | 每次内存访问 O(块数);每次 cmov 分叉 O(块数) 次 `malloc`。Arxan 分析单候选上限 1e6 步 |
| 8 | `asset_sig.c:16` `ml_asset_sig_match_mount_ebl` | 无首字节门控,第二个模式对每个字节都调 `memcmp(…, 46)` | `find_mount_ebl` 扫全 `.text`:约 T 次函数调用 + T 次 `memcmp` |
| 9 | `singleton.c:596-597` | `.text` 扫两遍,`singleton_candidate_at` 对每个 `0xBA` 重复计算两次（纯函数,同参同果） | 约 2T 字节遍历,候选解析工作量翻倍 |
### P2 — 设计缺陷 / 死代码

| # | 位置 | 缺陷 |
|---|------|------|
| 10 | `extdll.c:196-258` | 整段 Kahn 拓扑排序的结果在 `:264` 被 `order[i] = i` 覆盖 —— 60 行纯粹白算,仅剩环检测作用,而 `:292` 已有独立环检测 |
| 11 | `analysis.c:216` `clone_analysis_state`、`:837` `find_visited` | 死代码（已被 `clone_analysis_state_with_vm` / `find_visited_map` 取代） |
| 12 | `scanner.c:88` | `sig_scan_without_mask` 中的 `next:` 标签无人跳转 |
| 13 | `disabler.c:449,584` | 两处 `while (…) Sleep(0)` 忙等。`Sleep(0)` 只让给同优先级就绪线程,等待期可跑满一核,而被等待的 Arxan 分析耗时以秒计 |
| 14 | `config.c:82-88` | `config_full_path_alloc` 可能返回 NULL,随后 `PathRemoveFileSpecW(NULL)` 崩溃 |
| 15 | `config.c:105` | `MultiByteToWideChar` 返回值未检查,失败时把未初始化栈缓冲当作 mod 路径传下去 |
| 16 | `asset_hooks.c:886` | `ml_asset_hooks_install_game_data_ready` 中外层 `HMODULE module` 被内层同名变量遮蔽,外层在该分支未使用 |

### 记录但不改动

- `vfs.c` 的 4 个按域划分的查找缓存存放**完全相同**的值（域只作命名空间隔离）,
  代价是 4 倍缓存内存与 4 倍冷未命中归一化。收敛为单一缓存会改变
  `vfs_reset_lookup_caches` / 域隔离的语义边界,属产品决策,不在本轮范围。
- `dearxan_analyze_all_stubs` 的 `worker_count = 核数/4`（上限 8）偏保守:8 核只用 2 线程。
  提高并发度会同比放大 VM 峰值内存,应在第 7 项落地并可用真实游戏跑
  `benchmark_dearxan_analysis` 后再定,不在本轮盲调。
- `patch.c:191-223` 逐 stub 打补丁失败时不回滚,镜像半补丁状态。回滚 Arxan 补丁的风险
  高于半补丁 + 报错,维持现状。
- `log.c` 每行 `fflush`。日志等级门控在 `log.h:40`,INFO 下热路径不进函数,现状可接受。
- `singleton.c:635` `singleton_try_finish_fd4` 在 `added` 为假时重跑整张 partial 表。
  初审判为 P1,复核后撤销:`singleton_find` 只有两个调用点
  （`asset_hooks.c` 的 `install_post_hooks` 与 `regulation.c`）,均为一次性启动路径,
  重试次数有界;而该重试是「等待游戏发布 reflection 名字」的隐式机制,改成一次性
  有丢失 FD4 单例查找的风险。改为补注释说明取舍。

---

## 阶段计划

### 阶段 1 — Win32 文件路由热路径（缺陷 1、2、3）

- `vfs.c`:删除 `vfs_is_package_path` 及其调用,注释记录恒假证明。
- `vfs.c`:`vfs_route_writable_path` 增加两级快路径 —— 空注册表直接返回;
  末段前置过滤（不分配、不写缓存）。仅在末段命中时走原有归一化 + 缓存逻辑。
- `save_mapping.c`:`ml_save_mapping_route` 在共享锁下先做扩展名前置过滤,
  不命中直接返回 `false`,不分配、不 `canonicalize_path`。

**守护**:`smoke_vfs`、`smoke_vfs_writable_mt`、`smoke_vfs_generation_mt`、
`smoke_save_mapping`、`smoke_save_mapping_mt`、`smoke_win32_hooks`、`smoke_mod_routing`。
其中 `smoke_vfs:176` 已断言重复 `vfs_route_writable_path` 零分配,`smoke_save_mapping`
覆盖 `.sl2` + `.co2` 双扩展。

### 阶段 2 — Dantelion 资源热路径（缺陷 4、5、6）

- `asset_hooks.c`:`log_override_once` 的线性表改 `khash` 宽字符串集合;
  查重走共享锁,仅插入取独占锁。
- `asset_hooks.c`:`hook_file_operator` 前置 `InterlockedCompareExchange` 读「已尝试」标志,
  未尝试时才取独占锁并在锁内复查。
- `common.c` / `wwise_path.c`:Wwise 解析器改用栈缓冲拼接候选路径,消除每次打开的
  堆分配;超长路径回退到堆。

**守护**:`smoke_asset_hooks`、`smoke_wwise_path`、`smoke_dl_device`。

### 阶段 3 — 启动延迟（缺陷 7、8、9）

- `dearxan/vm.c`:内存块从单链表改为开放寻址哈希表（键为块索引,与 `analysis.c`
  的 `visited_map` 同构）。查找 O(1);`dearxan_vm_clone` 变为一次分配 + 一次 `memcpy`。
- `asset_sig.c`:`ml_asset_sig_match_mount_ebl` 顶部加首字节门控
  （模式一必要条件 `p[0] == 0x48`,模式二必要条件 `p[0] == 0x53`）。
- `singleton.c`:两遍扫描合并为一遍,`singleton_candidate_at` 每个 `0xBA` 只算一次,
  按 `candidate.kind` 分派。

**守护**:`smoke_dearxan_vm`（含 clone 语义）、`smoke_dearxan_analysis`、
`smoke_dearxan_abi`、`smoke_asset_hooks`、`smoke_scanner`。

### 阶段 4 — 设计缺陷与死代码（缺陷 10–16）

- `extdll.c`:删除被覆盖的 Kahn 排序段,保留单一排序算法及其环检测。
- `analysis.c`:删除 `clone_analysis_state`、`find_visited`。
- `scanner.c`:移除 `sig_scan_without_mask` 中不可达的 `next:` 标签。
- `disabler.c`:两处忙等改为命名手动重置事件等待;事件创建失败时回退到原循环。
- `config.c`:补 `config_full_path_alloc` NULL 检查与 `MultiByteToWideChar` 返回值检查。
- `asset_hooks.c`:消除遮蔽变量。

**守护**:`smoke_extdll_config`、`smoke_dearxan_scheduler`、`smoke_dearxan_analysis`、
`smoke_scanner`、`smoke_log`。

### 收尾

- `CHANGELOG.md` 的 `#### Unreleased` 段补记条目。
- 全量构建 + ctest 最终确认。

---

## 执行结果与偏差记录

全部阶段已执行完毕。`cmake-build-debug`（VS 2026,Debug）全量构建零警告零错误,
43 个 smoke 测试连跑 3 轮全绿;多线程与 scheduler 用例单独连跑 8 轮全绿。
Release 构建（LTO + 静态 CRT + strip）通过,`dumpbin /exports` 确认 `YAFSML.dll`
仍导出 107 个符号,`WinHttpOpen` / `DXGID3D10CreateDevice` / `DirectInput8Create`
均在位。

**阶段 1（缺陷 1、2、3,全部完成）**
- 删除 `vfs_is_package_path`,恒假证明写进 `vfs_route_read_path` 上方注释。
- `vfs_route_writable_path` 增加空注册表快路径与 `vfs_last_segment` 末段前置过滤;
  等价性与「追加式表 + 生产者同线程回查」的并发论证写进注释。
- `ml_save_mapping_route` 在共享锁下先做扩展名前置过滤,逐分支等价性写进注释。

**阶段 2（缺陷 4、5、6,全部完成）**
- `log_override_once` 改 `khash(wstr)` 集合,查重走共享锁;等价性依据「入参恒为 VFS
  interned 指针」。
- `hook_file_operator` 增加 `set_path_hook_settled` 原子快路径,卸载时复位。
- Wwise 解析器改 `wwise_join3` + 栈缓冲。`wwise_join_path` / `wwise_wem_candidates`
  被 `wwise_join3` / `wwise_wem_bucket` 取代（前者改造后仅测试使用,按「不留生产死代码」
  一并移除）,`smoke_wwise_path` 相应改写并补了容量不足、NULL 入参、超长路径用例。

**阶段 3（缺陷 7、8、9,完成）**
- `dearxan/vm.c` 内存块改开放寻址哈希表:`index_plus_one == 0` 表示空槽（使块索引 0
  无需额外标志位）,`grow_blocks` 先扩容再插入以保证返回指针属于最终表,
  `dearxan_vm_clone` 变为一次 `malloc` + 一次 `memcpy`。
  `smoke_dearxan_vm` 新增 `test_memory_table_growth`:300 个块跨多次扩容后全部回读、
  扩容后 clone 的独立性、向 clone 追加新块不影响源、以及从未触碰内存的 VM 的 clone。
- `asset_sig.c` 加首字节门控（`0x48` / `0x53`）。
- `singleton.c` 两遍 `.text` 扫描合并为 `singleton_scan` 一遍,按 `candidate.kind`
  分派到 `singleton_record_derived` / `singleton_record_fd4`。
**阶段 4（缺陷 10–16,全部完成）**
- `extdll.c` 删除被覆盖的 Kahn 排序段及随之孤立的 `selected` / `indegree` /
  `extdll_has_unique_dependency`;缺失依赖的警告日志保留在单独一轮里。
  `smoke_extdll_config` 已覆盖环、重复依赖、缺失依赖三种情形。
- `analysis.c` 删除 `clone_analysis_state`、`find_visited`。
- `scanner.c` 移除 `sig_scan_without_mask` 中不可达的 `next:` 标签。
- `disabler.c` 两处忙等改为 `wait_for_state` + 命名手动重置事件
  （`..._DRAINED_<pid>` / `..._NEUTERED_<pid>`）;事件创建失败时回退到 `Sleep(1)`,
  且每轮都重读状态,缺事件或漏信号都不会挂死。顺带补上互斥量等待失败路径上的
  事件句柄泄漏。
- `config.c` 补 `config_full_path_alloc` NULL 检查与 mod 路径转换失败检查。
- `asset_hooks.c` 消除遮蔽的 `HMODULE module`。

## 无法用冒烟测试覆盖的项

以下需要真实游戏手动验证:

1. mod 资源覆盖仍生效（阶段 1 删除 `vfs_is_package_path`、阶段 2 改 Wwise 拼接）。
2. `replace_save_filename` / `replace_seamless_coop_save_filename` 重定向仍生效,
   含 `.sl2` / `.sl2.bak` / `.co2` / `.co2.bak` 四种形态（阶段 1 扩展名前置过滤）。
3. Wwise 音频覆盖仍生效（阶段 2 栈缓冲拼接）。
4. BootBoost 与 MountEbl 仍命中（阶段 3 首字节门控）。
5. `disable_arxan` 在 Elden Ring / Nightreign 上仍成功中和,并对比启动耗时
   （阶段 3 VM 哈希表）。可用 `benchmark_dearxan_analysis <game.exe>` 量化。
