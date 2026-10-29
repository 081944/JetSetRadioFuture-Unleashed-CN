# 中文分支维护与上游同步

## 分支与版本

- `origin`：`081944/JetSetRadioFuture-Unleashed-CN`。
- `upstream`：`JuMatt/JetSetRadioFuture-Unleashed`，只拉取，不向原项目直接推送。
- `main`：原版分支，跟随上游；中文改动提交到默认分支 `cn`。
- `sync/upstream-<版本>`：从 `cn` 建立的临时整合分支。
- `v0.5.9-cn.1`：首次中文版本的源码基线。已有发布历史不做 rebase 或强制推送。

## 同步流程

先确保工作区干净。以下示例仅合并源码，不构建或发布 APK：

```powershell
$ErrorActionPreference = 'Stop'
git fetch upstream
if ($LASTEXITCODE -ne 0) { throw 'Fetch failed' }
git switch main
if ($LASTEXITCODE -ne 0) { throw 'Switch failed' }
git merge --ff-only upstream/main
if ($LASTEXITCODE -ne 0) { throw 'Upstream branch diverged; inspect before proceeding' }
git push origin main
if ($LASTEXITCODE -ne 0) { throw 'Push failed' }
git switch cn
if ($LASTEXITCODE -ne 0) { throw 'Switch failed' }
git switch -c sync/upstream-0.5.10
if ($LASTEXITCODE -ne 0) { throw 'Sync branch creation failed' }
git merge main
if ($LASTEXITCODE -ne 0) { throw 'Resolve merge conflicts and review each resolution' }
```

复核中文入口、CMake、字体补丁和 SDL 输入映射；重新执行适配生成、实际生成 C 检查和 Android 构建。提交到 fork 的同步分支，建立面向 `cn` 的 PR。完成设备测试后再合并并打版本标签，未测试的同步分支不作为默认版本。

不要在 `cn` 上直接点自动同步后立即发布；源码无冲突不代表静态重编译结果和中文资源兼容。

## 必须复测

- 中文 ISO 识别、完整资源提取及再次启动。
- 标题、开场对话、中文字体页、切场景、存档读档。
- 实体手柄四种设置组合、扳机压力、切换立即生效。
- 肩键互换和 ABXY 布局在退出、重启后的恢复。
- 关闭中文构建选项时保持上游美版检查行为。

底层检查不能替代设备测试；编译器报告的未解析调用及未实现指令仍需关注实际执行路径。

## 上传边界

标准 fork 保留上游已有内容和历史。我们新增的 Git 提交只包含源码、脚本、测试、文档和版本识别用元数据。

本地 `game_files/`、`port/recomp/`、`build*/`、镜像、XBE、字体、提取文本、生成的 `cn_profile.h`、构建产物及签名密钥均不进入 Git。不要使用 `git add -f` 绕过忽略规则。

提交前运行 `python tools/cn_compat/audit_publish.py --staged`；推送前运行 `python tools/cn_compat/audit_publish.py`，检查从首次适配基线以后所有新增 Git blob，包括已经删除但仍在提交历史中的文件。该检查用于阻止误传文件，人工仍需审阅 diff。

## 发布 APK

明确准备发布时，完成本地构建和测试后，通过 Release 上传指定 APK 及 SHA256 文件，禁止上传整个构建目录或日志目录。自动检查不上传任何构建目录。

延续当前签名身份并递增 Android `versionCode`。已有私钥和密码文件需由维护者自行离线备份，不提交到 Git，不作为 Release 附件。当前设备上的测试包与本次发布包使用同一身份。

游戏派生的兼容配置和静态重编译代码会编入本地构建的 APK；“Git 只提交源码”不表示 APK 内不存在这些派生数据。保留原项目和第三方组件的许可说明。
