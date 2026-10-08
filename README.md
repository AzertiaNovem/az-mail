# AZ Mail

团队内部使用的网页邮箱，界面和操作方式接近 Gmail。所有邮件经 [Resend](https://resend.com) 收发，附件和原始邮件存放在 Cloudflare R2（也可以用本地磁盘）。后端是一个 C++20 可执行文件（Boost.Beast + SQLite），前端是 React 单页应用，两者分开部署。

## 功能

* **收件箱与会话**：按会话（thread）聚合，收件箱 / 已加星标 / 已定时 / 已发送 / 草稿 / 全部邮件 / 垃圾邮件 / 已删除；标签；归档、星标、已读未读、移入垃圾箱、永久删除等批量操作；Gmail 风格的翻页。
* **写信**：富文本编辑器，多个写信窗口；自动保存草稿（冲突检测）；回复 / 全部回复 / 转发；附件（单个 25 MiB）与粘贴插图；签名（写信时插入编辑器，可删除或更换，按所见内容发送）；**撤销发送**（0–30 秒）；**定时发送**（可改期、取消）。
* **团队地址**：成员邮箱 + 别名（如 `support@`），别名来信分发给所有成员；授权成员可以用别名身份回复，其他成员能在同一会话中看到回复。
* **送达状态**：已发送 / 已送达 / 投递延迟 / 退信 / 被标记垃圾邮件 / 失败 / 已抑制，失败可重试。
* **搜索**：中文全文检索（SQLite FTS5 trigram，2 字词也能搜），支持 `from:`、`to:`、`subject:`、`label:`、`has:attachment`、`in:`、`is:`、`before:`/`after:`、`newer_than:`、`larger:`、排除词和 `OR`。
* **安全**：邮件正文在无脚本的沙箱 iframe 中显示，默认拦截外部图片；DMARC 失败和冒充本域发件人的邮件进入垃圾邮件并显示警告；附件经签名链接下载；登录限流；会话可远程注销。
* **实时更新**：WebSocket 推送新邮件和状态变化。
* **管理后台**：用户、别名、域名（含 Resend DNS 状态）、Webhook 事件、无法投递的邮件、发件队列、后台任务、统计。管理员只能看到元数据，看不到邮件正文。
* 界面为简体中文；不依赖任何境外 CDN（字体、图标均自托管）。

## 截图

> 截图待补充：`docs/screenshots/inbox.png`、`docs/screenshots/compose.png`、`docs/screenshots/admin.png`。

## 快速开始（本地开发）

需要：macOS 或 Linux，Python ≥ 3.10，CMake ≥ 3.24 + C++20 编译器 + Boost ≥ 1.83 / OpenSSL 3 / SQLite（macOS 用 Homebrew），Node.js ≥ 22 + pnpm。

```bash
scripts/dev.sh            # 首次运行会编译后端、安装前端依赖、写入演示数据
```

一条命令启动：

* 模拟 Resend + 模拟 R2（`tools/mock_resend`，不会真的发信）：`http://127.0.0.1:8787`（R2 `:8788`）；
* 后端：`http://127.0.0.1:8080`；
* 前端（Vite）：**http://localhost:5173**。

首次启动会创建域名 `azmail.test`、管理员 `admin@azmail.test`、用户 张三 / 李四 / 王五、别名 `support@azmail.test` 以及一批中文演示邮件；**演示密码只打印一次**，同时保存在 `data/dev/credentials.txt`。按 Ctrl-C 停止所有进程。

常用选项：`--reset`（清空 `data/dev` 重新开始）、`--local`（本地磁盘存储代替模拟 R2）、`--no-frontend`、`--no-seed`。发往 `bounce@` / `fail@` / `delay@` / `complain@` / `suppress@` 开头的外部地址可以看到对应的送达状态；`http://127.0.0.1:8787/_mock/state` 查看模拟服务器状态。

## 测试

```bash
# 后端单元测试（ASan/UBSan）
cmake --preset mac-debug -S backend && cmake --build backend/build/mac-debug -j && ctest --test-dir backend/build/mac-debug --output-on-failure
# 前端
cd frontend && pnpm install && pnpm build && pnpm test && pnpm lint
# 模拟服务器自检（Svix / AWS SigV4 测试向量 + 主要流程）
python3 -m tools.mock_resend.selftest
# 端到端测试（36 个场景，随机端口启动模拟服务器和真实后端）
python3 tests/e2e/run.py            # -k s03 只跑部分；--list 列出场景；--keep 保留日志
python3 tests/e2e/test_harness.py   # 测试框架自身的单元测试
AZMAIL_BIN=backend/build/mac-debug/azmail python3 tests/e2e/test_deploy.py   # 部署文件检查（Nginx、配置示例、systemd、前端部署脚本）
```

## 目录结构

```
backend/              C++20 后端（CMake 工程）：src/{app,http,ws,api,repo,mail,jobs,resend,net,storage,core,db}
  tests/unit/         Catch2 单元测试
frontend/             React 19 + TypeScript + Vite 前端
tools/mock_resend/    模拟 Resend API + 最小 R2（S3）服务，供开发和 E2E 使用
tools/resend_probe.py 用真实 Resend 账号验证接口行为的探测脚本（手动运行）
tests/e2e/            端到端测试（Python 标准库）：run.py、lib/、scenarios/s01–s36、test_deploy.py
scripts/              dev.sh、seed_dev.py、build_backend.sh、build_frontend.sh、deploy_frontend.sh
deploy/               systemd 单元、配置示例、Nginx 配置（含代理头片段 nginx-azmail-proxy.conf）、Ubuntu 24.04 构建容器
docs/                 设计、接口、契约与部署文档
```

## 文档

* [docs/DEPLOY.md](docs/DEPLOY.md) — 部署指南：服务器要求、构建、配置、Resend 与 R2 设置、备份、升级、故障排查
* [docs/DESIGN.md](docs/DESIGN.md) — 设计规格（数据库结构、后端接口、前端、测试计划）
* [docs/API.md](docs/API.md) — REST / WebSocket 接口格式
* [docs/CONTRACTS.md](docs/CONTRACTS.md) — 模块分工、路由与后台任务对照
* [tools/mock_resend/README.md](tools/mock_resend/README.md) — 模拟服务器行为与控制接口
* [tests/e2e/README.md](tests/e2e/README.md) — 端到端测试说明
