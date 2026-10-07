# AZ Mail 部署指南

本文面向运维人员，说明如何在 Ubuntu 24.04 上部署 AZ Mail：后端（C++ 单个可执行文件 `azmail`）、前端（静态文件）、Resend（收发邮件）和 Cloudflare R2（附件与原始邮件存储）。设计细节见 [DESIGN.md](DESIGN.md)，接口见 [API.md](API.md)。

---

## 1. 架构概览

```
                       ┌──────────────────────── 服务器（Ubuntu 24.04）────────────────────────┐
 浏览器 ── HTTPS ──▶ Nginx  mail.example.com      → /var/www/azmail（前端静态文件 + config.js）
 浏览器 ── HTTPS/WSS ▶ Nginx  mail-api.example.com → 127.0.0.1:8080  azmail serve
                       │                                   │  SQLite：/var/lib/azmail/azmail.db
                       └───────────────────────────────────│───────────────────────────────────┘
                                                           │ HTTPS（出站）
                         ┌─────────────────────────────────┼──────────────────────┐
                         ▼                                 ▼                      ▼
                  Resend API                        Resend Webhook          Cloudflare R2（S3 API）
            发信 POST /emails、收信列表          email.* 事件推送到            附件、原始 .eml
            /emails/receiving、附件下载        /api/webhooks/resend         （按 sha256 内容寻址）

 外部发件人 ── SMTP ──▶ Resend 收信（域名 MX → inbound-smtp.us-east-1.amazonaws.com）
                         └─▶ webhook email.received ─▶ azmail 拉取正文/附件 ─▶ 存入 R2 + SQLite
```

* **发信**：用户点击发送 → 撤销窗口（默认 5 秒）结束后由后台任务调用 Resend `POST /emails`（带 `Idempotency-Key`）；状态通过 webhook（及定期对账）更新。
* **收信**：Resend 收到邮件后推送 `email.received`；后端拉取邮件详情、原始 .eml 和附件，写入 R2 和数据库。webhook 不可达时，后台每 `AZMAIL_POLL_INTERVAL_SEC` 秒轮询一次收件列表兜底。
* **团队内部邮件**同样经过 Resend 投递（回环），后端按 Message-ID / `X-AzMail-Ref` 识别自己发出的邮件。
* **文件下载**：默认 `proxy` 模式——后端从 R2 取回并经 API 域名转发（中国大陆访问 `*.r2.cloudflarestorage.com` 经常很慢）；`redirect` 模式则 302 跳转到 R2 预签名链接。
* 前端和后端分开部署，一个前端构建适用于所有环境（API 地址由 `/config.js` 指定）。

## 2. 服务器要求

| 项目 | 要求 |
|---|---|
| 系统 | Ubuntu 24.04 LTS（x86_64 或 arm64） |
| 配置 | 2 vCPU / 2 GB 内存起步（scrypt 登录校验每次约 64 MB 内存，最多 4 个并发）；本地 SSD |
| 磁盘 | 数据库放本地磁盘（**不要放 NFS**）；使用 R2 时本地只需数据库 + 代理缓存（默认 1 GB） |
| 网络 | 入站 80/443；出站 HTTPS 到 `api.resend.com` 和 `<账户ID>.r2.cloudflarestorage.com` |
| 时间 | **必须开启 NTP**（webhook 签名只接受 ±5 分钟内的时间戳）：`timedatectl` 显示 `System clock synchronized: yes` |
| 位置 | 用户在中国大陆时，建议服务器放在香港 / 新加坡 / 日本等地：R2 不能建在大陆，后端到 R2 的流量跨境 |

需要的软件包：

```bash
# 运行时
sudo apt-get install -y ca-certificates nginx sqlite3 \
    libboost-json1.83.0 libboost-url1.83.0 libboost-program-options1.83.0 libssl3t64 libsqlite3-0
# TLS 证书（Let's Encrypt）
sudo apt-get install -y certbot python3-certbot-nginx
# 在服务器上编译后端时（也可以在别的 Ubuntu 24.04 机器 / 容器里编译）
sudo apt-get install -y build-essential cmake ninja-build pkg-config git python3 catch2 \
    libboost-dev libboost-json-dev libboost-url-dev libboost-program-options-dev \
    libssl-dev libsqlite3-dev
```

前端构建需要 Node.js ≥ 22 和 pnpm（可在开发机或 CI 上构建，只上传 `frontend/dist`）。

## 3. 构建

### 后端

```bash
scripts/build_backend.sh                    # linux-release 预设：编译 + 单元测试
scripts/build_backend.sh --out /tmp/azmail-release   # 另存一份 strip 过的二进制
```

产物：`backend/build/linux-release/azmail`。使用 Ubuntu 24.04 自带的 Boost 1.83、OpenSSL 3、SQLite 3.45（自带 FTS5 trigram，启动时会检查）。

用容器验证 Linux 兼容性（编译 + ASan/UBSan 单元测试 + release 构建，并生成一个最小运行镜像）：

```bash
docker build -f deploy/Dockerfile.ubuntu2404 -t azmail:ubuntu2404 .
docker build -f deploy/Dockerfile.ubuntu2404 --target build .        # 只做编译和测试
```

### 前端

```bash
scripts/build_frontend.sh                   # pnpm install --frozen-lockfile && test && lint && build
```

产物：`frontend/dist/`。`config.js` 按环境单独提供（见第 10 节），不要依赖构建里的默认值。

## 4. 安装后端

```bash
# 1. 系统用户与目录
sudo useradd --system --home-dir /var/lib/azmail --shell /usr/sbin/nologin azmail
sudo install -d -o azmail -g azmail -m 0750 /var/lib/azmail
sudo install -d -o root -g azmail -m 0750 /etc/azmail

# 2. 可执行文件
sudo install -m 0755 backend/build/linux-release/azmail /usr/local/bin/azmail

# 3. 配置（含密钥，权限 0640）
sudo install -o root -g azmail -m 0640 deploy/azmail.env.example /etc/azmail/azmail.env
sudo editor /etc/azmail/azmail.env          # 按第 5–7 节填写
openssl rand -hex 32                         # 生成 AZMAIL_SECRET

# 4. systemd
sudo install -m 0644 deploy/azmail.service /etc/systemd/system/azmail.service
sudo systemctl daemon-reload
```

命令行工具读取同一份配置：`sudo -u azmail azmail --env-file /etc/azmail/azmail.env <命令>`。下文简写为 `azm`：

```bash
alias azm='sudo -u azmail /usr/local/bin/azmail --env-file /etc/azmail/azmail.env'
```

可用命令：`serve`、`migrate`、`create-user`、`reset-password`、`add-domain`、`backup`、`reindex`、`doctor [--r2]`、`blobs-migrate --to local|r2`、`version`。

## 5. 配置参考

完整列表（每一项都有中文注释和默认值）见 [deploy/azmail.env.example](../deploy/azmail.env.example)，变量名以 `backend/src/config.hpp` 为准。必须填写的：

| 变量 | 说明 |
|---|---|
| `AZMAIL_PUBLIC_API_URL` | API 的公网地址，如 `https://mail-api.example.com`（签名下载链接用） |
| `AZMAIL_CORS_ORIGINS` | 前端地址，如 `https://mail.example.com`（逗号分隔；WebSocket 也按它校验 Origin） |
| `AZMAIL_SECRET` | 至少 32 字节随机数；更换后所有附件链接失效 |
| `AZMAIL_LOCAL_DOMAINS` | 团队域名，如 `example.com` |
| `RESEND_API_KEY` | Full access 权限的 Key（见第 6 节） |
| `RESEND_WEBHOOK_SECRET` | Webhook 签名密钥 `whsec_…` |
| `AZMAIL_BLOB_BACKEND` + `R2_*` | 文件存储（见第 7 节）；`local` 时不需要 R2 |

常用可选项：

| 变量 | 默认 | 说明 |
|---|---|---|
| `AZMAIL_LISTEN_ADDRESS` / `AZMAIL_PORT` | `127.0.0.1` / `8080` | 只监听本机，由 Nginx 对外 |
| `AZMAIL_TRUSTED_PROXIES` | `127.0.0.1,::1` | 只信任这些代理的 `X-Forwarded-For` |
| `AZMAIL_DATA_DIR` / `AZMAIL_DB_PATH` | `/var/lib/azmail` / `…/azmail.db` | 数据目录 / 数据库 |
| `AZMAIL_UNDO_SEND_SECONDS` | `5` | 新用户默认撤销发送时间（0/5/10/20/30） |
| `AZMAIL_SCHEDULE_MODE` | `resend` | `local` = 定时发送一律由本服务执行 |
| `AZMAIL_FILES_DELIVERY` | `proxy` | `redirect` = 302 跳转到 R2 预签名链接 |
| `RESEND_RATE_RPS` | `8` | Resend 限额 10 次/秒且按团队共享，留余量 |
| `AZMAIL_POLL_INTERVAL_SEC` | `120` | 收件轮询间隔（webhook 的兜底） |
| `AZMAIL_TRASH_PURGE_DAYS` / `AZMAIL_SPAM_PURGE_DAYS` | `30` | 已删除 / 垃圾邮件保留天数 |
| `AZMAIL_LOG_LEVEL` | `info` | 日志写入 journald，不含密钥、令牌、邮件正文 |

## 6. Resend 配置清单

在 [Resend 控制台](https://resend.com) 按顺序完成：

1. **添加并验证域名**（Domains → Add Domain，例如 `example.com`；区域通常为 `us-east-1`）。按页面提示在 DNS 中添加：
   * **SPF**：`send` 子域的 MX 和 TXT 记录（`v=spf1 include:amazonses.com ~all`）；
   * **DKIM**：`resend._domainkey` TXT 记录；
   * 等待状态变为 **Verified**。
2. **开启收信**（同一域名的 Receiving / Inbound）：在根域名添加 MX 记录
   `MX  @  inbound-smtp.us-east-1.amazonaws.com  优先级 10`。
   ⚠ 这会把该域名的所有来信交给 Resend；如原来有其他邮箱服务，请先迁移或使用子域名。
3. **DMARC**：添加 `_dmarc` TXT 记录，策略至少为 quarantine，例如
   `v=DMARC1; p=quarantine; rua=mailto:dmarc@example.com`。
   AZ Mail 依赖它识别冒充本域发件人的邮件（DMARC 失败 / 冒充内部发件人会进入垃圾邮件并显示警告）。
4. **Webhook**（Webhooks → Add Endpoint）：
   * URL：`https://mail-api.example.com/api/webhooks/resend`
   * 订阅**全部 `email.*` 事件**，包括 `email.received`、`email.sent`、`email.delivered`、`email.delivery_delayed`、`email.bounced`、`email.complained`、`email.failed`、`email.suppressed`、`email.scheduled`（`email.opened` / `email.clicked` 可选，只记录不改变状态）；
   * 复制 **Signing secret**（`whsec_…`）到 `RESEND_WEBHOOK_SECRET`。
5. **API Key**（API Keys → Create）：权限选 **Full access**（收信接口需要；“Sending access” 只能发信）。建议给 AZ Mail **单独建一个 Key**：10 次/秒的限额按团队共享。填入 `RESEND_API_KEY`。
6. **时间同步**：确认服务器 NTP 正常（`timedatectl`），否则 webhook 会因时间戳超出 ±5 分钟被拒绝（401）。
7. 部署完成后运行 `azm doctor`，它会检查配置、数据库能力、文件存储和 Resend Key。

如果 API 只在内网可访问、Resend 无法推送 webhook：收信靠轮询（`AZMAIL_POLL_INTERVAL_SEC`，管理后台“立即同步”可手动触发），发信状态靠定期对账，功能不受影响，只是延迟更高。

## 7. Cloudflare R2 配置

1. Cloudflare 控制台 → **R2** → Create bucket，例如 `azmail`。
   * 位置提示（location hint）选离服务器最近的区域（如 Asia-Pacific）；
   * 有数据驻留要求时可选 **欧盟管辖区（jurisdiction）**，此时 `R2_ENDPOINT=https://<账户ID>.eu.r2.cloudflarestorage.com`。
2. R2 → **Manage R2 API Tokens** → Create API token：
   * 权限 **Object Read & Write**，**只授权给这个存储桶**；
   * 记下 **Access Key ID** 和 **Secret Access Key**（只显示一次）。
3. 填写配置：
   ```ini
   AZMAIL_BLOB_BACKEND=r2
   R2_ACCOUNT_ID=<账户 ID>
   R2_ACCESS_KEY_ID=<Access Key ID>
   R2_SECRET_ACCESS_KEY=<Secret Access Key>
   R2_BUCKET=azmail
   # R2_ENDPOINT 留空 = https://<账户ID>.r2.cloudflarestorage.com
   ```
   对象键为 `azmail/blobs/<aa>/<bb>/<sha256>`；文件名、类型等元数据只保存在数据库里，存储桶**不要**设为公开、**不要**绑定自定义域名（预签名链接只在 S3 API 域名上有效）。
4. **下载方式**（`AZMAIL_FILES_DELIVERY`）：
   * `proxy`（默认，推荐给中国大陆用户）：浏览器只访问 API 域名，后端经本地缓存（`AZMAIL_FILE_CACHE_MB`）转发；
   * `redirect`：浏览器直接从 R2 下载（节省服务器带宽）。启用前运行 **`azm doctor --r2`**：它会对探测对象做预签名并检查 `response-content-type` / `response-content-disposition` 是否生效（R2 文档未写明支持）。检查不通过时服务自动退回 `proxy` 并在日志中警告。
5. **生命周期与版本**：对象内容不可变且按内容寻址，无引用的对象由后台任务 `gc.blobs` 删除。若希望误删可恢复，可在 R2 中开启对象版本 / 设置保留规则，并配合生命周期规则清理旧版本；**不要**配置“N 天后删除对象”的生命周期规则，否则会删掉仍在使用的附件。
6. 从本地存储迁移到 R2：`azm blobs-migrate --to r2`（复制 → 校验 sha256 → 切换数据库记录 → 删除本地文件），可以在线执行；完成后把 `AZMAIL_BLOB_BACKEND` 改为 `r2` 并重启。

## 8. 初始化与第一个管理员

```bash
azm migrate                                         # 建表（serve 启动时也会自动迁移）
azm add-domain --name example.com                   # 团队域名（与 Resend 中验证的一致）
read -rsp '管理员密码: ' PW; echo
printf '%s\n' "$PW" | azm create-user --email admin@example.com --name 管理员 --admin
unset PW
azm doctor                                          # 全部 OK 再启动服务
azm doctor --r2                                     # 使用 R2 时
```

密码从标准输入读取，不会出现在进程列表或 shell 历史中。其他用户、别名（如 `support@`，可指定哪些成员能以别名身份发信）和域名在网页管理后台（`/admin`）中管理。忘记密码：`printf '%s\n' "$NEW" | azm reset-password --email user@example.com`（同时注销该用户所有会话）。

## 9. 启动服务与 Nginx

```bash
sudo systemctl enable --now azmail
systemctl status azmail
journalctl -u azmail -f
curl -s http://127.0.0.1:8080/api/health          # {"status":"ok",...}
```

systemd 单元（`deploy/azmail.service`）以 `azmail` 用户运行，`ProtectSystem=strict`，只有 `/var/lib/azmail` 可写；`TimeoutStopSec=30`（后端优雅停机最多等 25 秒）。

Nginx：

```bash
# 证书（每个域名一张；配置文件里引用 /etc/letsencrypt/live/<域名>/）。
# 首次签发用 standalone（证书不存在时 nginx 无法通过 nginx -t），续期时自动停启 nginx。
sudo install -d /var/www/letsencrypt
for host in mail.example.com mail-api.example.com; do
  sudo certbot certonly --standalone -d "$host" \
       --pre-hook "systemctl stop nginx" --post-hook "systemctl start nginx"
done

sudo cp deploy/nginx-api.conf      /etc/nginx/sites-available/azmail-api.conf
sudo cp deploy/nginx-frontend.conf /etc/nginx/sites-available/azmail-web.conf
sudo editor /etc/nginx/sites-available/azmail-*.conf   # 替换域名、证书路径、CSP 中的 API 地址
sudo ln -s ../sites-available/azmail-api.conf /etc/nginx/sites-enabled/
sudo ln -s ../sites-available/azmail-web.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

Ubuntu 24.04 的 nginx 是 1.24，配置里使用 `listen 443 ssl http2;` 写法（`http2 on;` 需要 1.25.1+）。

* **API**（`nginx-api.conf`）：TLS；反代到 `127.0.0.1:8080`；`/api/ws` 带 `Upgrade`/`Connection` 头且 `proxy_read_timeout 1h`；`client_max_body_size 30m`（附件上限 25 MiB）；传递 `X-Forwarded-For` / `X-Forwarded-Proto`；JSON gzip；访问日志**不记录查询串**（签名链接的签名在查询串里）。
* **前端**（`nginx-frontend.conf`）：SPA 回退到 `index.html`；`index.html` 与 `/config.js` 设置 `Cache-Control: no-store`，`/assets/*` 一年 `immutable`；严格 CSP：
  `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob: https:; connect-src 'self' https://mail-api.example.com wss://mail-api.example.com; font-src 'self'; frame-src 'self'; object-src 'none'; base-uri 'none'`。
  `img-src` 必须包含 `https:`：邮件正文在沙箱 iframe（srcdoc）中显示，会继承页面 CSP，再由 iframe 内的 CSP 收紧。

## 10. 部署前端

```bash
sudo install -d /var/www/azmail
sudo rsync -a --delete frontend/dist/ /var/www/azmail/
echo 'window.__AZMAIL_CONFIG__ = { apiBase: "https://mail-api.example.com" };' | sudo tee /var/www/azmail/config.js
```

`config.js` 由每个环境单独维护（部署脚本里 `rsync` 后再写入，或把它排除在 `--delete` 之外）。前端不从 CDN 加载任何字体或图标（Google Fonts 在中国大陆不可用）。

## 11. 备份与恢复

**顺序很重要：先备份数据库，再备份文件**——这样数据库引用的每个文件在备份里都存在（多出来的文件无害，会被 GC 清理）。

```bash
# 1) 数据库在线备份（SQLite backup API，服务无需停止）
sudo install -d -o azmail -g azmail -m 0750 /var/backups/azmail
azm backup --to /var/backups/azmail/azmail-$(date +%F-%H%M).db
# 2a) R2：开启存储桶对象版本，或用 rclone 同步到另一个存储桶/位置
rclone sync r2:azmail r2-backup:azmail-backup
# 2b) 本地存储（AZMAIL_BLOB_BACKEND=local）
rsync -a /var/lib/azmail/blobs/ backup-host:/srv/azmail-blobs/
```

建议用 systemd timer 或 cron 每天执行，并定期把备份复制到另一台机器；保留 14–30 天。

恢复：`systemctl stop azmail` → 把备份的 `.db` 复制为 `/var/lib/azmail/azmail.db`（属主 `azmail`，删除旧的 `-wal`/`-shm` 文件）→ 恢复文件 → `azm migrate` → `systemctl start azmail` → `azm doctor`。搜索索引异常时可运行 `azm reindex`。

注意：Resend 收件只保留 30 天。服务停机超过 30 天会丢失这期间的来信（管理后台会提示轮询发现的缺口）。

## 12. 升级

```bash
scripts/build_backend.sh                                   # 新版本（含单元测试）
azm backup --to /var/backups/azmail/pre-upgrade-$(date +%F).db
sudo install -m 0755 backend/build/linux-release/azmail /usr/local/bin/azmail.new
sudo systemctl stop azmail
sudo mv /usr/local/bin/azmail.new /usr/local/bin/azmail
azm migrate && azm doctor
sudo systemctl start azmail
curl -s https://mail-api.example.com/api/health
```

前端：重新构建后 `rsync` 到 `/var/www/azmail/`（保留 `config.js`）。哈希文件名的资源可以长期缓存，`index.html` 不缓存，用户刷新即得到新版本。数据库迁移只增不减；回滚二进制前请先恢复升级前的数据库备份。停机期间到达的邮件不会丢：webhook 会重试，轮询也会补上。

## 13. 故障排查

| 现象 | 原因与处理 |
|---|---|
| 日志中 Resend 返回 **403，正文是 `error code: 1010`** | Cloudflare 拦截了没有 User-Agent 的请求——配置错误，不是暂时故障。检查 `RESEND_USER_AGENT` 不为空 |
| Resend 401 / 403 `invalid_api_key`、`restricted_api_key` | Key 错误或只有发信权限；收信接口需要 Full access |
| **Webhook 返回 401** | ① `RESEND_WEBHOOK_SECRET` 与 Resend 端点的 Signing secret 不一致；② 服务器时间偏差超过 5 分钟（检查 NTP）；③ 中间代理改动了请求体（Nginx 配置不要改写正文）。管理后台“Webhook 事件”中可以看到收到的事件 |
| Webhook 413 | 请求体超过 1 MiB，正常 Resend 事件不会这样；检查是否有其他服务往这个地址推送 |
| 发信失败“发送配额已用完” | Resend `daily_quota_exceeded` / `monthly_quota_exceeded`：不会自动重试，管理后台显示横幅；升级套餐或等配额恢复后在“发件队列”中重试 |
| 发信很慢 / 日志大量 429 | 10 次/秒限额由同一团队的所有 Key 共享；给 AZ Mail 单独的 Key，或调低 `RESEND_RATE_RPS` |
| 收不到外部来信 | 检查 MX 记录、Resend 中域名的 Receiving 已开启、webhook 地址可从公网访问；管理后台“立即同步”触发轮询；“无法投递”列表显示发给不存在地址的邮件 |
| 发往本域不存在地址被拒（422 `unknown_local_recipient`） | 预期行为：先在管理后台创建用户或别名 |
| 附件上传 413 | 单个附件上限 25 MiB、每封邮件 28 MiB；Nginx `client_max_body_size` 要 ≥ 30m |
| 附件链接 403 | 签名链接过期（默认 12 小时）或 `AZMAIL_SECRET` 已更换；刷新页面即可得到新链接 |
| R2 `SignatureDoesNotMatch` / `AccessDenied` | Access Key/Secret 错误，或令牌未授权该存储桶；`R2_ENDPOINT` 应为 S3 API 地址（不是自定义域名），区域固定为 `auto` |
| R2 `NoSuchBucket` | `R2_BUCKET` 名称错误或管辖区不对（欧盟存储桶需要 `.eu.` 地址） |
| 附件下载很慢（中国大陆） | 使用 `AZMAIL_FILES_DELIVERY=proxy`（默认）；适当增大 `AZMAIL_FILE_CACHE_MB` |
| 实时更新不工作（新邮件不自动出现） | 检查 Nginx `/api/ws` 的 Upgrade 配置和 `proxy_read_timeout`；`AZMAIL_CORS_ORIGINS` 必须包含前端地址（WebSocket 校验 Origin）。断线时前端每 60 秒轮询，功能不受影响 |
| 浏览器报 CORS 错误 | `AZMAIL_CORS_ORIGINS` 与浏览器地址栏的 scheme/域名/端口必须完全一致 |
| 启动失败：FTS5 / trigram | SQLite 版本过旧；Ubuntu 24.04 自带的 3.45 满足要求 |
| `database is locked` / 503 | 数据库放在了网络盘，或有外部进程长时间占用；数据库必须在本地磁盘 |

更多诊断：`azm doctor`、`journalctl -u azmail --since -1h`、管理后台“统计”（队列、失败数、最近一次 webhook / 轮询时间、存储用量）。

## 14. 用真实 Resend 验证假设（tools/resend_probe.py）

设计中有几项 Resend 行为在文档里没有写明（DESIGN §1 F4），测试用的模拟服务器按保守假设实现。上线前请用一个**测试域名**（已验证发信、已开启收信）运行探测脚本：

```bash
export RESEND_API_KEY=re_...                  # Full access
python3 tools/resend_probe.py --domain probe.example.com                 # 先看计划（不发信）
python3 tools/resend_probe.py --domain probe.example.com --yes --json probe-report.json
python3 tools/resend_probe.py --domain probe.example.com --yes --sending-key re_sending_only_key
```

脚本只给测试域名下的地址发少量邮件（可选 `--external` 一个外部邮箱），检查并打印：

1. `received_for` 的内容，以及一封邮件发给同域多个收件人（含抄送、密送）时是否被拆成多封（split delivery）；
2. 收件详情的 `headers` 是否包含 `in-reply-to`、`references` 和自定义 `X-*` 头（如 `X-AzMail-Ref`），名称是否小写；
3. 收件列表的排序（是否最新在前）以及 `after` / `before` 游标的方向；
4. 带附件的定时发送是否被接受；
5. 自定义 `Message-ID` 头是否生效；
6. `message_id` 何时可用（立即发送需要几秒；定时邮件在发送前是否为空）；
7. 收件的 `from` / `subject` 是否已解码（中文显示名和主题）；
8. 收信接口需要哪种 Key 权限（用 `--sending-key` 对比只能发信的 Key）；
9. 空主题是否被接受。

另外还会检查：缺少 User-Agent 时的 403 1010、`Idempotency-Key` 的重放与冲突、限流响应头、`html_format=cid`、原始邮件下载链接，以及附件 `download_url` 带 `Authorization` 头时的表现。结果与模拟服务器的假设（`tools/mock_resend/README.md`）不一致时，请记录到 issue 并相应调整模拟服务器和后端——例如定时邮件不接受附件时后端会自动改为本地定时，`headers` 中没有 `X-AzMail-Ref` 时依靠 Message-ID 识别回环邮件。

## 附：开发与测试环境

* 本地开发：`scripts/dev.sh`（模拟 Resend/R2 + 后端 + 演示数据 + Vite），见根目录 README。
* 端到端测试：`python3 tests/e2e/run.py`（随机端口启动模拟服务器和后端，默认使用模拟 R2，结束后再用本地存储跑一组冒烟测试）。
* 模拟服务器自检：`python3 -m tools.mock_resend.selftest`。
