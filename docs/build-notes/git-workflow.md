# Git 工作流：在上游仓库之上开发自己的功能

本仓库是 [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk) 的 fork，
你在 `my/features` 分支上开发自己的功能，同时跟踪上游更新。

---

## 1. 当前的 remote 与分支布局

```
origin    https://github.com/albbzmkh/hermes-gadget-sdk.git      你的 fork（可推送）
upstream  https://github.com/Adolanium/hermes-gadget-sdk.git     原作者（只读）
```

分支：

| 分支 | 跟踪 | 用途 |
|---|---|---|
| `main` | `upstream/main` | 纯镜像，**永远不改** |
| `my/features` | `origin/my/features` | **你所有工作在这里** |

> 为什么 `main` 指向 upstream 而不指向自己的 fork：这样 `git pull` 在 `main` 上
> 就是拉上游，不会误推。所有改动都在 `my/features`，`main` 始终保持上游原样。

---

## 2. 日常开发

```bash
cd /Users/capsule/Documents/hermes-gadget-sdk
git checkout my/features        # 确认在功能分支上

# 写代码…

git add -A
git commit -m "说明这次改了什么、为什么"
git push                        # 推到你的 fork
```

---

## 3. 同步上游更新

### 看看上游有什么新东西

```bash
git fetch upstream --tags

# 上游 main 比你的分支多了多少提交
git log --oneline my/features..upstream/main | wc -l

# 新 tag
git tag --sort=-creatordate | head -5
```

### 建议：跟 tag，不跟 main

上游节奏很快（近 30 天 685 个提交，其中 260 个涉及 `firmware/`）。
`main` 上常有半成品；tag 是作者认为「可以发布」的状态。

```bash
# 推荐：合并某个 tag
git checkout my/features
git merge upstream/v0.6.0

# 或者跟最新的 main（更激进）
git merge upstream/main
```

### 冲突处理

```bash
git merge upstream/v0.6.0
# 有冲突时：
#   <<<<<<< HEAD        ← 你的版本
#   =======
#   >>>>>>> upstream/...  ← 上游版本
git status                 # 看哪些文件冲突
# 编辑解决后：
git add <冲突文件>
git commit
git push
```

**用 merge 不用 rebase**：你的提交会累积到几十上百个，rebase 要为每个提交解决一次冲突；
merge 只解决一次。

### 觉得合并失败想重来

```bash
git merge --abort
```

---

## 4. 怎么改才不容易冲突

上游近 30 天 260 个 `firmware/` 提交，绝大多数是**新增文件**（新板子 profile、新 port、新 panel），
不是大改已有文件。所以：

| 改动方式 | 冲突风险 | 说明 |
|---|---|---|
| 新增 `esp32/main/port_xxx.cpp` | 🟢 几乎为零 | 上游不认识这个文件 |
| 新增 `esp32/main/panel_xxx.hpp` | 🟢 几乎为零 | 同上 |
| 新增 `esp32/boards/<新板子>/` | 🟢 几乎为零 | 同上 |
| 新增 `core/src/app_myfeature.cpp` | 🟢 几乎为零 | 照 `app_timer.cpp` / `app_camera.cpp` 的模式 |
| 给 `core/include/hg/*.hpp` 加新接口 | 🟡 低 | 头文件偶尔变动 |
| 改 `esp32/main/board.cpp`（板子注册表） | 🟠 中 | 每上新板子都会动 |
| 改 `core/src/app.cpp` / `esp32/main/main.cpp` | 🟠 中 | 上游改得频繁 |
| 改 `core/src/canvas.cpp` / `font*.cpp` | 🔴 高 | 渲染核心，改得最狠 |

### 核心原则

**能新增文件就不要改老文件。** 必须 hook 时，用编译宏包住，冲突范围只有几行：

```cpp
#ifdef HG_MY_FEATURE
    my_feature_tick();
#endif
```

---

## 5. 冲突了怎么办

### 先看冲突规模

```bash
git diff --name-only --diff-filter=U     # 冲突文件列表
git diff                                  # 冲突内容
```

### 三种处理策略

**① 只有几行冲突** → 手工改。

**② 某个文件整体想用上游版本**：

```bash
git checkout --theirs <文件>    # 用 upstream 的
git add <文件>
```

**③ 某个文件整体想保留自己的**：

```bash
git checkout --ours <文件>      # 用 my/features 的
git add <文件>
```

⚠️ **②③ 要极其小心**：`--theirs` 会让你丢失在该文件里的所有自定义修改。
只在确认这个文件你没有实质改动时用。

### 改完验证

```bash
cd firmware/esp32
rm -rf .pio/build/m5stack-sticks3
~/.platformio/penv/bin/pio run -e m5stack-sticks3
```

**合并上游后必须重新编译验证**，因为上游可能改了 API 但你那段代码没跟上。

---

## 6. 常用命令速查

```bash
# 当前状态
git status
git log --oneline -10
git branch -vv

# 上游对比
git fetch upstream
git log --oneline my/features..upstream/main     # 上游领先的提交
git log --oneline upstream/main..my/features     # 你领先的提交
git diff upstream/main --stat                    # 改了哪些文件

# 撤销
git checkout -- <文件>            # 丢弃工作区改动
git reset --soft HEAD~1           # 撤销上次 commit（保留改动）
git merge --abort                 # 放弃正在进行的 merge

# 临时存一下
git stash && git stash pop
```

---

## 7. 推送故障排查

### `could not read Username for 'https://github.com'`

凭据失效：

```bash
gh auth login                    # 或 gh auth refresh
gh auth setup-git
```

### `refusing to allow an OAuth App to create or update workflow`

token 缺 `workflow` 权限（首次推送整个仓库历史时需要，因为含有 `.github/workflows/`）：

```bash
gh auth refresh --hostname github.com --scopes repo,workflow
```

### 国内网络下 `gh auth refresh` 报 `TLS handshake timeout` / `unexpected EOF`

设备码接口 (`github.com/login/device/code`) 不稳定。多试几次，或换个时间：

```bash
for i in 1 2 3 4 5 6; do
  gh auth refresh --hostname github.com --scopes repo,workflow && break
  echo "重试 $i"
done
```

### 推送大仓库超时

首次推送 1000+ 提交的历史会很慢。加大限制：

```bash
git config --local http.postBuffer 524288000
git config --local http.lowSpeedLimit 0
git config --local http.lowSpeedTime 999999
```

---

## 8. 当前已提交的内容

`my/features` 分支基于 `v0.5.0-113-ge053748`，第一个提交是：

```
d756ce4  Enable CJK font for sticks3/aipi-lite; add local build notes and workspace
```

包含：

| 文件 | 改动 |
|---|---|
| `firmware/esp32/platformio.ini` | `[env:m5stack-sticks3]` 与 `[env:aipi-lite]` 各加 `-DHG_CJK_FONT=ON` |
| `docs/build-notes/local-build-log.md` | 新增：本地构建全过程实录 |
| `firmware/esp32/.gitignore` | 新增：忽略 `.pio` / `.vscode` 中间文件 |
| `hermes-gadget.code-workspace` | 新增：多根工作区，让 PlatformIO 插件在仓库根也能激活 |

**源码零改动**，只动了构建配置。

分支地址：https://github.com/albbzmkh/hermes-gadget-sdk/tree/my/features

---

## 9. 要不要给上游提 PR

如果你做的功能具有通用价值（比如支持了一块新板子、修了一个上游 bug），
可以只把那部分改动挑出来提 PR：

```bash
git checkout -b upstream/cjk-patch upstream/main
git cherry-pick <只包含那个改动的 commit>
git push origin upstream/cjk-patch
# 然后在 GitHub 上向 Adolanium/hermes-gadget-sdk 提 PR
```

提 PR 前请先读 [CONTRIBUTING.md](../../CONTRIBUTING.md)——
这个工程对文档、验证、许可证、贡献者署名都有明确要求。

私人功能（不适合上游的）就留在 `my/features` 里，不用提。
