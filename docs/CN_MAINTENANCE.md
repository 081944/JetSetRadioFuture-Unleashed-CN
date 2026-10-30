# 中文分支维护与上游同步

## 分支与版本

- `origin`：`081944/JetSetRadioFuture-Unleashed-CN`。
- `upstream`：`JuMatt/JetSetRadioFuture-Unleashed`，只拉取，不向原项目直接推送。
- `main`：原版分支，跟随上游；中文改动提交到默认分支 `cn`。
- `sync/upstream-<版本>`：从 `cn` 建立的临时整合分支。
- `v0.5.9-cn.1`：首次中文版本的源码基线。已有发布历史不做 rebase 或强制推送。
- `v0.5.10-cn.1`：合并上游 0.5.10 后的发布基线，`versionCode` 4。
- `v0.5.10-cn.2`：当前发布基线，几何命令包快路径与性能浮层，内嵌文字补齐到 534 处，`versionCode` 5。

## 0.5.10 整合验证

`sync/upstream-0.5.10` 从已发布的 `cn` 建立，合并上游 `1c7e21f31b8899c810f77a89bec4f90e47a3a00f`，无文本冲突。上游本次修改五个文件，重点是 GPU 中断竞争及卡死修复。

发布包使用 `0.5.10-cn.1` / versionCode 4。先上传 Release 草稿并覆盖安装到 AYN Thor，用户已确认正常运行，随后将源码合入 `cn` 并公开同一份 APK，不重新打包。不得用构建通过替代运行测试。

已验证安装文件与 Release 附件 SHA256 一致：`4c1651c3c01c87c5dcf7f6959055f72c6a39a2c1967eaade06be0fb7e561034b`。发布源码标签 `v0.5.10-cn.1` 保持指向实际构建的提交，之后记录验证结果的文档提交不改变 APK。

已核对 XBE 哈希、生成器及运行时模板、手写函数声明与入口钩子均未变化，因此复用原生成 C，重新编译更新后的引擎。字体及转换的本地回归、实际生成字体 C 的全部 65,536 个输入、手柄四种组合检查通过。

原生和 APK 构建脚本现使用一致的 `-Version`，打包时检查版本匹配。可用 `-OutputDir` 将不同版本存入不同的本地构建目录；签名身份默认始终保存在原 `build-cn-apk/`，不随输出目录改变，也不提交到仓库。

## 0.5.10-cn.2 发布

`0.5.10-cn.2` / versionCode 5 在 0.5.10 基线上加了几何命令包快路径（`src/kernel/nv2a_pb_scan.c`、`src/nv2a_gl/nv2a_gl.c`）、可选性能浮层，并把内嵌文字从 409 处补齐到 534 处。因为文本属于数据改动，`port/recomp/gen` 生成的 C 与上一版逐字节相同，只有 `cn_codegen.sha256` 指向新的 `target_sha256`：`d523d10e60e759d1a7f71d27aeb4ea4d64c0cf1837a7c4cb5fd55292c2fc04d3`。

打包此版本需要 SDK 的 `build-tools;36.0.0` 与 `platforms;android-36`（`build_apk.ps1` 硬编码这两个路径），以及 PowerShell 7：Windows PowerShell 5.1 的默认执行策略会拒绝运行脚本。

验证记录：汉化单测 10 项通过；`verify_codegen` 覆盖 1,545 个映射编码与全部 65,536 个输入；Android 构建通过；APK 覆盖安装到骁龙 865 设备后日志版本为 `0.5.10-cn.2`，实测 40–60 帧浮动（该平台低于上游建议的骁龙 8 Gen 1）。目标设备为骁龙 8 Gen 2 及以上。

不要把另一台机器的 `build*/` 目录复制过来复用：CMake 缓存里记录的是绝对路径，跨机器会直接报 `CMakeCache.txt ... different than the directory`，必须清空重建。

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
