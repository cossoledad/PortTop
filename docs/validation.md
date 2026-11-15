# 0.2.0 验证记录

验证日期：2026-11-15（工作区环境日期）。

环境：Ubuntu 26.04.1 LTS、x86_64、WSL2，内核 `6.18.58.1-microsoft-standard-WSL2`；GCC 15.2.0；pkg-config 返回 ncursesw `6.6.20251231`。程序和测试以普通用户运行。

## 自动验收

`make test` 通过；默认严格编译选项下无告警。`make sanitize` 在 AddressSanitizer、UndefinedBehaviorSanitizer 和 leak detection 下运行相同检查，无报告。最后重新清理并恢复普通构建及测试。Clang 对所有 C 文件执行严格告警的 syntax-only 检查通过；`git diff --check` 通过。

- **基础采集**：真实 IPv4/IPv6 TCP/UDP、同一 socket 的 fork 拥有者、重复 FD 去重、PPID/CWD/命令、控制序列清理、端口和文本筛选、排序、JSON 和参数校验。真实 TCP 连接验证默认与 `--all` 的状态差异。忙循环和真实监听 socket 验证 CPU 差分采样。
- **身份与子树安全**：括号和空格进程名解析；拒绝 PID 1、自身和自身祖先；拒绝启动时间不匹配及已退出目标。构造真实父子子树，篡改后代的预览启动时间，验证整个预检失败且父子均存活，再恢复身份并终止两者、回收退出状态。
- **交互**：在 130 × 36 PTY 中打开小写 `k` 的居中终止弹窗；默认 Cancel 上 Enter 取消；确认 TERM / KILL；命令滚动；窗口缩小至 60 × 16 时不可执行隐藏确认，恢复后继续操作。退出、外部 SIGTERM、Ctrl-C、SIGTSTP/SIGCONT 均检查 termios 恢复。
- **重启监督链**：实际启动 `launcher → supervisor → service + idle worker` 和独立兄弟服务。先仅终止 service，确认 supervisor 重新启动新 PID；再选择 supervisor 的 Subtree，确认包含无端口 worker，原端口持续关闭，launcher 和兄弟服务存活；最后显式选择 launcher 子树，确认其剩余服务均退出。
- **刷新稳定性**：暂停且无变化时，在排空已有输出后的 0.4 秒窗口内无终端输出；正常 100ms 采样的 0.4 秒窗口内无全屏清除序列；确认弹窗冻结期间屏幕不变。创建 25 个监听端口，滚动选中第 21 个，关闭前面的 5 个，验证选中 socket 保留且所在屏幕行不变。

终端断言基于解析后的 VT 屏幕，而不是要求 ncurses 在差分输出中重复发送整个标题。失败时保存 `build/pty-failure.log`。测试只终止自行创建的进程。

## 当前 18080 端口的只读验证

实际读取到 `mise dev → occccad-dev → occccad-control → occccad-server`。对应 control 源码存在 managed API 退出后的 restart 分支，支持“只杀端口进程可能重启”的判断。

在 tmux 的真实 ncurses 屏幕中打开 18080 的终止弹窗并选择 control 父节点预览：当时可见 7 个受影响进程、6 个 socket owner 行，包含 API、jobs、geometry 和前端等。随后取消弹窗并退出。该数字仅是当时快照，18080 的实际服务没有用于终止测试；终止行为由上述隔离监督链验收。

## 证据边界

PTY 检查和本机 tmux 视觉检查不代表所有终端模拟器的视觉验收，也不是长时间帧率或零闪烁承诺。重排排序值、选中目标退出或尺寸变化仍会造成必要的列表/布局变化。

子树为预览捕获的有限进程集合，不是原子停止操作。尚未覆盖树外 systemd 拉起、脱离父子关系后代、fork 密集竞争、多 namespace、权限失败矩阵、无 pidfd 内核或大规模容量。若发送阶段发生错误，可能出现已发送部分目标的结果，程序报告计数，不宣称事务回滚。

未做性能基准和长期稳定性测试。Unicode 用户文本仍转换为 ASCII，cmdline 最多读取 16 KiB。IPv6 不可用环境的跳过路径尚未在本机执行；本机实际覆盖了 IPv6。
