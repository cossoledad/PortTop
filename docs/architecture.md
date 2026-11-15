# 架构与实现边界

入口为 `README.md`。当前只有一套 C11 / Linux 实现，运行时依赖 libc、ncursesw / terminfo，不调用外部命令。

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | 参数校验、TTY 检测、批处理或交互入口 |
| `src/porttop.h` | Socket / Process / Row / Snapshot / SignalPlan 契约 |
| `src/proc.c` | Linux socket/owner 采集、排序筛选、文本/JSON 输出、单目标 pidfd 信号 |
| `src/actions.c` | 自身祖先保护、有限子树预览、所有目标预检和逐个发送 |
| `src/ui.c` | ncurses 屏幕、事件循环、选中身份、居中终止弹窗、终端生命周期 |
| `tests/core.c` | stat 解析、CPU 差分、pidfd 及子树身份安全 |
| `tests/integration.py` | 实际进程/socket、PTY、重启链与列表稳定性验收 |
| `tests/vt.py` | 测试用 ASCII VT 屏幕模型，解析 ncurses 差分输出 |
| `tests/fixture.py` | 多协议、共享 FD、fork 测试服务 |
| `tests/supervisor_fixture.py` | launcher、重启 supervisor、服务、无端口 worker 和兄弟服务 |

## 采集与展示

读取当前 namespace 的 `/proc/net/{tcp,tcp6,udp,udp6}`，默认仅 TCP LISTEN 和 UDP UNCONN，忽略本地端口 0。按 socket inode 排序，单次遍历可见 `/proc/<pid>/fd`，通过二分查找匹配。相同进程重复 FD 去重；fork 继承者分别记为 owner。

Process 记录 PID、PPID、PGID、session、tty、状态、start ticks、CPU ticks、RSS、cmdline、cwd、exe 和用户信息。枚举 FD 前后验证启动时间，丢弃身份改变的进程。未找到 owner 的 socket 保留行。每个 owner/socket 关系映射为 Row，仅保存 snapshot 内索引；排序筛选生成索引投影，不改原始数据。

CPU 使用前后快照的进程 ticks 差值 / `_SC_CLK_TCK` / 实际单调时钟间隔。系统 CPU 从总 ticks 和 idle+iowait 差分计算。批处理不做两次采样，JSON 明示 `cpu_sampled: false`。RSS、端口和进程的统计不可混合求和。

`denied_processes` 是打不开 FD 目录的进程数；`unknown_sockets` 是没有匹配进程的 socket 数；`unavailable_tables` 是无法打开的网络表数量。统计作用于完整采集结果，不受筛选影响；每个 FD 及附加元数据读失败不单独计数。

## ncurses 与稳定刷新

事件循环在数据更新、用户输入和窗口变化时绘制。`erase()` 只清理 stdscr 的虚拟内容，随后 `wnoutrefresh(stdscr)`；如有弹窗，绘制并暂存独立 WINDOW；最后只执行一次 `doupdate()`。不在常规采样时使用 `clear()`、`clearok()` 或直接输出清屏 ANSI 序列。差分刷新依据 [ncurses 刷新契约](https://invisible-island.net/ncurses/man/curs_refresh.3x.html)。暂停且没有输入或尺寸变化时，不绘制也不输出。

分区布局固定，不随命令长度和端口数改变。刷新按 inode + PID + start ticks 恢复选中身份；根据刷新前选中行相对滚动窗口的位置修正 offset，在边界允许时保留屏幕行。CPU/RSS 排序仍会根据真实数值重排，选中目标消失时选邻近行；这与绘制闪烁是不同边界。

输入由 ncurses keypad / getch 处理。内核 WINCH 信号触发实际尺寸检查和 resizeterm；ncurses 合成的 KEY_RESIZE 仅请求重绘，不能再次无条件触发 resizeterm，否则形成事件反馈循环。弹窗存活至关闭或尺寸变化，不因每次采样反复创建。

保存 termios；ncurses 进入 alternate screen / cbreak / noecho。INT/TERM/HUP 和正常退出 endwin 并还原 termios；TSTP 保存程序模式、恢复终端、停止，继续后 reset_prog_mode 并重新布局。SIGKILL 等不可捕获退出无法恢复。

## 启动链与终止计划

详情的启动链在采样或选择变化时读取，绘制期间不再访问 `/proc`；最多 12 个节点。停止于 PID 1、自身祖先、有 tty 的 session leader、不可读节点或疑似 PID 复用的父节点。

`k` 默认 SIGTERM，`K` 为 SIGKILL，`t` 是 TERM 别名。弹窗初始目标为端口拥有者、Process only 范围、Cancel 按钮；↑ 选择父节点时自动改为 Subtree，可按 s 切回 Process only。确认时主采集冻结，按钮默认取消，尺寸过小时禁止执行不可见的确认。

SignalPlan 保存 PID、PPID、start ticks、name 和截断至 1 KiB 的预览命令。Subtree 独立读取所有可读的进程，包含无端口后代，按父子关系和启动时间构造 root-first 顺序并去重。上限 16,384 条；权限导致的不可读记录有提示。端口影响从主快照映射，不受命令行筛选限制，但仍限于采集模式和当前 namespace。

确认后：

1. 再次拒绝 PID 1、自身和自身祖先。
2. 对所有目标先 pidfd_open，核对启动时间并以 signal 0 预检权限。目标身份变更或权限失败时，整个预检取消，不先发给其他目标。
3. 已消失的非根节点可计为 exited；根节点失效则取消。取得的每个句柄最终都会关闭。
4. 对捕获的有限集合按 root-first 顺序 pidfd_send_signal，分别计 sent / exited / failed。操作中途失败属于部分结果，不宣称回滚或原子性。

不执行负 PID / 进程组 kill，不扫描“新 PID”自动追加到已确认集合，也不按名字推测服务。父进程和子树必须由用户在预览中选择。树外监督者、已脱离父子关系的后代、预览后派生的新进程可能继续存在；通用工具不能修改 systemd 或任意 supervisor 的单项重启策略。

## 后续扩展边界

尚无线程、后台守护程序、持久化配置、容器聚合和进程组操作。主采集仍扫描可见进程及 FD；子树预览额外扫描进程，有限集合不构成原子进程树快照。用户文本目前先转换为 ASCII 安全文本，ncursesw 本身并不自动补足应用的 Unicode 数据模型。需要提高容量时先测量采集、计划构造和终端输出，再考虑 inet_diag、缓存或异步采集。
