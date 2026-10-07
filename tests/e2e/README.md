# 端到端测试（tests/e2e）

只依赖 Python 标准库（≥ 3.10）。`run.py` 在临时目录里用随机端口启动模拟 Resend/R2（`tools/mock_resend`）和**真实的** `azmail` 二进制，通过命令行完成初始化（`migrate`、`add-domain`、`create-user --admin`，密码走标准输入），然后按顺序执行 `scenarios/` 中的场景（DESIGN §6 的 01–34 和附录 A 的 35）。所有场景共享一个环境；每个场景开始前模拟服务器会恢复默认配置（数据保留）。

```bash
python3 tests/e2e/run.py                        # 全部场景（R2 存储，模拟 S3）+ 本地存储冒烟测试
python3 tests/e2e/run.py -k s03 -k undo         # 名称或标题包含 s03 或 undo 的场景（可用逗号）
python3 tests/e2e/run.py --blob-backend local   # 全部场景使用本地存储
python3 tests/e2e/run.py --list                 # 列出场景，[smoke] 为本地冒烟子集
python3 tests/e2e/run.py --keep -v              # 保留临时目录（数据库、日志、credentials.txt）
python3 tests/e2e/run.py --azmail-bin path/to/azmail   # 默认 $AZMAIL_BIN 或 backend/build/mac-debug/azmail
```

退出码：0 全部通过（允许跳过），1 有失败，2 环境启动失败或用法错误。失败时打印断言位置和后端日志末尾（`--log-lines`）；模拟服务器退出时也会打印它的日志。

后端环境变量（见 `lib/env.py`）：`AZMAIL_UNDO_SEND_SECONDS=0`（需要撤销窗口的场景单独设为 5 秒）、`AZMAIL_SCHEDULE_MIN_LEAD_SEC=5`、`AZMAIL_POLL_INTERVAL_SEC=3600`（只有 `/api/admin/sync` 触发轮询）、`AZMAIL_RECONCILE_INTERVAL_SEC=15`、`RESEND_TIMEOUT_SEC=4`、`AZMAIL_ALLOW_INSECURE_HTTP=1`，R2 指向模拟 S3（SigV4 按 S3 规则严格校验）。

## 目录

| 文件 | 内容 |
|---|---|
| `run.py` | 发现、筛选、执行、超时（SIGALRM）、汇总；场景结束后执行清理并在需要时用默认配置重启后端 |
| `lib/env.py` | `Env`（进程、端口、密钥、CLI、重启/SIGKILL）、`Ctx`（传给场景：`api`、`admin`、`mock`、`team()`、`ws()`、`mark()`、`defer()`、`signer()`） |
| `lib/api.py` | urllib REST 客户端（带 token，不跟随重定向，`expect=` 失败抛出 `ApiError`）；`raw_request` 手写请求（只发请求头测 413） |
| `lib/ws.py` | 最小 RFC 6455 客户端（掩码、分片、ping/pong、关闭码） |
| `lib/mock.py` | 模拟服务器控制接口客户端 |
| `lib/flows.py` | 测试团队（alice/bob/carol/dave + support@）、写信/发送/上传、查找会话、等待状态 |
| `lib/signing.py` | 后端签名链接算法的 Python 复刻（构造“已过期但签名正确”的链接） |
| `lib/wait.py` | `eventually` / `wait_for` / `never` |
| `lib/proc.py` | 子进程（日志文件、停止、SIGKILL）、命令行调用、空闲端口 |
| `scenarios/sNN_*.py` | 每个文件一个场景：`TITLE`、可选 `SMOKE` / `TIMEOUT`、`run(ctx)` |
| `test_mock.py` | 运行模拟服务器自检 |
| `test_harness.py` | 测试框架自身的单元测试（不需要后端） |

新增场景：复制一个现有场景，使用 `ctx.uniq()` 生成唯一主题，用 `ctx.mark()` 取得模拟服务器游标后只检查新产生的请求，需要临时改动的设置用 `ctx.defer()` 恢复。
