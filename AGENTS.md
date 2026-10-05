# bun-termux-loader 工程约定（AGENTS.md）

## 外部仓库克隆与缓存的位置约定

**规则**：克隆和缓存外部仓库（如本仓 bun-termux-loader 被下游消费时）**只允许放在仓库内的临时 tmp 目录，或环境变量指定的缓存目录**；消费方（如 opencode-termux）一律使用**最新**仓库版本。上游仓库（本仓）持续保持在 GitHub master 上的最新最佳状态；本地缓存每次使用前应自动拉取并同步到最新（`git fetch && git reset --hard origin/master` 或重新 clone）。

**Why**：opencode-termux 的 `Makefile:831` 曾优先解析到旧本地副本 `~/bun-termux-loader`（15a4ced，缺 readlink 修复），而活跃修复版在 `~/develop/bun-termux-loader`（8da3340+），导致 wrapper-native 实际用到缺陷版本。处置决定：**不改 Makefile 解析顺序**，以本约定替代——缓存放仓内 tmp / env 缓存目录、只用最新仓库版本、缓存自动同步最新，从根上消除"旧本地副本被静默选中"的问题。

**How to apply**：
- 构建脚本需要消费本仓时：clone 到 `${repo}/tmp/`（仓内临时目录，加入 .gitignore）或 `$BTL_CACHE_DIR` 等 env 指定目录，**不要**写到 `$HOME` 固定路径。
- 使用前同步：`git -C <cache>/bun-termux-loader fetch origin && git -C <cache>/bun-termux-loader reset --hard origin/master`。
- 消费方 opencode-termux 侧的同款约定见其文档（由 A 组维护，此处不重复展开）。

## 其他

- 本仓 GitHub master 即"最新最佳"版本；修复与特性合并后即视为对外可用状态。
- 提交信息使用中文 Conventional Commits；涉及行为变更须附验证证据（红/绿测试或上游源码依据）。
