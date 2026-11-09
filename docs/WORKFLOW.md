# 本地分支架构与上游同步工作流指南

本文档记录了本仓库对上游项目（`valentderah/totk-pc-recomp`）的分支管理策略与日常同步维护流程。

---

## 1. 分支架构设计

本地及 Fork 仓库分为两条平行的开发主线：

```text
upstream/main (官方上游仓库主线)
  │
  ├── totk  ───【TOTK 专有分支（带本地修复）】
  │              • 包含所有与 TotK / Linux 运行环境相关的本地增强与修复
  │              • 包含自动 UMA 硬件预设 (split_draws=0, frame_queue=1, astc_bc=1, pressure=80%)
  │              • 包含 AMD APU gpu_metrics 负载读取与 hwmon 传感器 (PR #952)
  │              • 包含无边框/全屏窗口模式 (PR #945) 与纹理视图格式重解释 (PR #975)
  │              • 严格保持 TotK 专有性，不引入多游戏重编译的非 TotK 符号
  │
  └── ryza  ───【Ryza 2 / 多游戏重编译扩展分支】
                 • 基于 totk 分支的基础平台与硬件优化
                 • 包含多游戏 Target Seam 接口 (`runtime/target.h`, `targets/ryza2.c`)
                 • 包含被上游拒收的 7 个通用 Switch / 早期 SDK PR (#928, #929, #931, #932, #944, #956, #958)
                 • 包含针对光荣特库摩 Gust 引擎的音频/渲染专属优化与帧率解锁
```

---

## 2. 上游更新同步流程 (Standard Update Workflow)

当上游仓库（`upstream/main`）发布新版本或合并新提交时，严格按以下两步分别进行 Rebase 更新：

### 第一步：获取上游最新提交
```bash
git fetch upstream main
```

### 第二步：将 TOTK 分支 rebase 到 upstream/main
```bash
git checkout totk
git rebase upstream/main

# 编译验证 TotK
python3 tools/build_game.py --version v121 --target totk

# 同步推送到 fork 远端
git push origin totk --force-with-lease
```

### 第三步：将 Ryza 分支 rebase 到 totk 分支
```bash
git checkout ryza
git rebase totk

# 编译验证 Ryza 2
python3 tools/build_game.py --version v100 --target ryza2

# 同步推送到 fork 远端
git push origin ryza --force-with-lease
```

> [!TIP]
> 保持 `ryza` 在 `totk` 基础之上 rebase，既能让莱莎自动享受 TotK 线上的最新底层修复与 UMA 硬件预设，又确保了解决冲突时一次性通过，无需重复解决通用底层代码冲突。

---

## 3. 被拒收与自关闭 PR 追踪记录

为了留下清晰的技术审计轨迹，所有被上游关闭或自关闭的 PR 均已完整转录为 Fork 仓库的 Issue：

### 3.1 上游以“超出 TotK 范围”为由【被上游拒绝】的 7 个 PR（打在 `ryza` 线上）
1. **[Issue #1](https://github.com/snailium/totk-pc-recomp/issues/1)**（相关 PR 链接：[PR #928](https://github.com/valentderah/totk-pc-recomp/pull/928) - **被上游拒绝**）: `runtime: implement nvnCommandBufferDrawArraysIndirect and OP_DRAW_IND`  
   *原因*: 莱莎2图元渲染需要非索引间接绘制接口，TotK 从不调用。
2. **[Issue #2](https://github.com/snailium/totk-pc-recomp/issues/2)**（相关 PR 链接：[PR #929](https://github.com/valentderah/totk-pc-recomp/pull/929) - **被上游拒绝**）: `runtime/vk_exec: support separate shader program bindings per stage`  
   *原因*: 莱莎2独立绑定 VS/FS 着色器，TotK 仅绑定整包 Program。
3. **[Issue #3](https://github.com/snailium/totk-pc-recomp/issues/3)**（相关 PR 链接：[PR #931](https://github.com/valentderah/totk-pc-recomp/pull/931) - **被上游拒绝**）: `romfs: support directory listing and normalize repeated path separators`  
   *原因*: 莱莎2通过 `OpenDirectory` 动态遍历加载资源，TotK 仅直接读文件。
4. **[Issue #4](https://github.com/snailium/totk-pc-recomp/issues/4)**（相关 PR 链接：[PR #932](https://github.com/valentderah/totk-pc-recomp/pull/932) - **被上游拒绝**）: `boot, kernel: configure main thread attributes dynamically from main.npdm`  
   *原因*: 动态适配不同游戏的 NPDM 栈大小与主线程优先级，上游坚持对 TotK 做固定数值强校验。
5. **[Issue #5](https://github.com/snailium/totk-pc-recomp/issues/5)**（相关 PR 链接：[PR #944](https://github.com/valentderah/totk-pc-recomp/pull/944) - **被上游拒绝**）: `runtime/ov_system: implement nn::settings::detail::MakeLanguageCode`  
   *原因*: 莱莎2依赖系统接口获取语言代码，TotK 走内部独立配置。
6. **[Issue #6](https://github.com/snailium/totk-pc-recomp/issues/6)**（相关 PR 链接：[PR #956](https://github.com/valentderah/totk-pc-recomp/pull/956) - **被上游拒绝**）: `runtime/audren, audio_mixer: fix voice/channel pool indexing, splitter depth order, and aux pass-through`  
   *原因*: 修复复杂音频图中的 BGM 静音与语音丢失问题，上游认为 TotK 未受影响。
7. **[Issue #7](https://github.com/snailium/totk-pc-recomp/issues/7)**（相关 PR 链接：[PR #958](https://github.com/valentderah/totk-pc-recomp/pull/958) - **被上游拒绝**）: `kernel: yield CPU on sub-microsecond SleepThread durations to prevent busy-spin`  
   *原因*: 消除微小睡眠（`<= 1000ns`）造成的 CPU 100% 空转自旋。

### 3.2 之前做错并主动关闭的 3 个 PR（复盘反思，非被拒，永久废弃）
8. **[Issue #8](https://github.com/snailium/totk-pc-recomp/issues/8)**（相关 PR 链接：[PR #955](https://github.com/valentderah/totk-pc-recomp/pull/955)）: `runtime/nvn_api_cmd: release replayed recording copies on control memory ring wraparound`  
   *做错复盘*: 控制内存中不仅有单帧瞬态命令，更有长期常驻的静态命令缓冲区。游标回绕时强行释放会导致致命的 **Use-After-Free (UAF)**。已在代码中彻底回退。
9. **[Issue #9](https://github.com/snailium/totk-pc-recomp/issues/9)**（相关 PR 链接：[PR #959](https://github.com/valentderah/totk-pc-recomp/pull/959)）: `vk_backend, vk_memory: asynchronous memory pool finalization and retire without pipeline stall`  
   *做错复盘*: `nvnMemoryPoolFinalize` 对 CPU 具有强同步语义，异步推迟到 GPU 回放线程会导致 CPU 提前复用物理内存，造成严重的双重映射与内存踩踏。已彻底回退。
10. **[Issue #10](https://github.com/snailium/totk-pc-recomp/issues/10)**（相关 PR 链接：[PR #960](https://github.com/valentderah/totk-pc-recomp/pull/960)）: `runtime/nvn, cfg: add no_queue_finish setting to skip synchronous queue finish stalls`  
    *做错复盘*: `nvnQueueFinish` 是保障 CPU/GPU 环形缓冲区同步的硬性屏障。粗暴跳过等待会造成严重的数据竞争与画面拉丝爆炸。正确解法是通过 UMA 硬件预设（`split_draws=0` 和 `frame_queue=1`）消除提交卡顿，绝不能跳过 API 同步。

### 3.3 通用 Switch 重编译技术记录与正规修复（已合并至 `ryza` 分支）
11. **[Issue #11](https://github.com/snailium/totk-pc-recomp/issues/11)**: `runtime/nvn_api_cmd: reclaim command buffer recording copies upon reuse to fix memory leak`  
    *说明*: 记录了跨帧循环重用 `NVNcommandBuffer` 句柄导致主机内存暴涨的根本原因、Issue #8 做错复盘，以及最终通过挂钩对象生命周期重用事件（Commit `7f59e16`）安全回收旧副本的技术方案。实机验证 5000+ 次提交稳定在 1 个活跃副本（0.0 MiB），彻底根治内存泄漏。
