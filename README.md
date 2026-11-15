# PortTop

**Ports first. Processes in context.**

面向本地微服务开发的 Linux 终端进程监视器，使用 C11 和 ncursesw 编写。先找到端口，再查看占用它的进程、启动参数和启动链，在确认影响范围后终止进程或服务子树。布局与键盘操作参考 [btop](https://github.com/aristocratos/btop)，实现独立。

当前版本 **0.2.0**。默认展示 TCP 监听端口和未连接的 UDP socket，按本地端口升序排列。同一端口的不同协议、地址及进程拥有者分别展示。

## 编译运行

需要 Linux、C11 编译器、Make 和 ncursesw 开发包。测试额外需要 Python 3；运行不调用 Python、lsof 或 ss。

```sh
sudo apt install build-essential pkg-config libncurses5-dev libncursesw5-dev
make
./build/porttop
```

终端至少 80 × 24，建议 120 × 36。使用 terminfo 和 ncurses 适配终端，支持 SSH 和 WSL；需要正确设置 `TERM`。`NO_COLOR=1` 或 `--no-color` 关闭颜色，选中项仍有反色提示。

```sh
./build/porttop --port 18080       # 精确查看本地端口
./build/porttop --filter python    # 按命令、PID、用户、地址等筛选
./build/porttop --all              # 包含已连接的 TCP/UDP socket
./build/porttop --sort cpu         # CPU 从高到低，需等待两次采样
./build/porttop --once             # 文本快照
./build/porttop --json             # JSON 快照
./build/porttop --help
```

标准输入或输出不是终端时，自动输出一次文本快照。`--json` 的 stdout 只输出 JSON，进程不可见时 `process` 为 `null`。

可选安装：`sudo make install`，卸载：`sudo make uninstall`；支持 `PREFIX` 和 `DESTDIR`。

## 操作

| 按键 | 功能 |
| --- | --- |
| ↑ / ↓，`j` 向下 | 选择端口及进程；小写 `k` 用于终止 |
| PgUp / PgDn，Home / End | 翻页、跳转首尾 |
| `/` | 实时筛选；Enter 应用，Esc 取消编辑 |
| Esc | 清除已应用的筛选（命令行 `--port` 仍生效） |
| `s` / `r` | 切换端口、PID、CPU、RSS 排序 / 反转排序 |
| `a` | 切换默认服务视图与全部 socket |
| Space / `R` | 暂停采样 / 立即刷新 |
| `[` / `]` | 滚动查看长启动命令 |
| `k` | 打开 SIGTERM 终止弹窗，`t` 保留为别名 |
| `K` | 打开 SIGKILL 强制终止弹窗 |
| `?` / `q` | 帮助 / 退出 |

上方显示系统 CPU、内存及采集可见性；中间显示端口列表；下方显示进程的 PID、PPID、用户、CPU、RSS、工作目录、可执行文件和命令。启动链最多包含拥有者及 11 级祖先进程，在受保护的祖先或有终端的会话首进程处停止。

## 脚本启动多个服务 / 服务被自动拉起

例如 `launcher → supervisor → server:18080`，只杀 server 可能会触发 supervisor 重启它。在端口行按 `k` 后：

1. 默认目标是端口拥有者，范围是 **Process only**，弹窗默认焦点为 **Cancel**。
2. 在弹窗中按 ↑ 逐级选择父进程，按 ↓ 返回子进程。选择父进程时默认切换到 **Subtree**，可按 `s` 切换为仅此进程。
3. 检查目标命令及受影响的进程和可见端口；`[` / `]` 可滚动长命令。列表包含不监听端口的后代；PgUp / PgDn 滚动预览。选 supervisor 会影响它管理的其他服务，选 launcher 则会影响其整个子树。
4. Tab 或左右键选择 **Terminate**，Enter 执行；也可按 `y` 确认。Esc、`n`、`q` 或在 Cancel 上按 Enter 均取消。

确认期间冻结快照、目标身份和弹窗位置。每个目标使用 pidfd 和启动时间验证，拒绝 PID 1、自身和自身祖先；所有目标通过预检后才开始发送。子树按父进程在前的顺序逐个发送，不使用进程组 kill。报告区分成功发送、已退出及失败；发送成功不等于退出成功。

子树是预览时捕获的**有限集合**，不是原子服务停止事务。新派生进程、已经脱离父子关系的进程，以及树外的 systemd 等监督者不在集合内。无法仅凭通用 `/proc` 信息暂停某个 supervisor 的单项重启策略；如需保留其余受管服务，应通过该 supervisor 自身提供的控制接口操作。

## 刷新稳定性

主界面与弹窗先绘制到 ncurses 虚拟屏幕，再通过一次 `doupdate()` 提交差异，不在采样时反复清空物理屏幕。固定分区尺寸和数字列宽；有数据、输入或尺寸变化才更新。按 socket inode、PID、启动时间保留选中对象，并尽量保留它所在的屏幕行，减少前面端口增减时的跳动。窗口大小改变时重新布局，弹窗会保持居中；尺寸过小时暂停可见确认操作。

## 权限与数据边界

普通用户即可运行。其他用户的 `/proc/<pid>/fd` 可能不可读，此时仍保留端口行并显示未知归属及权限统计。需要更多进程可见性时，可自行使用 `sudo ./build/porttop`；容器和 namespace 隔离仍可能限制可见范围。

- 扫描当前网络 namespace 的 TCP/TCP6/UDP/UDP6。暂不聚合其他网络 namespace，不包含 Unix socket。
- `/proc` 快照非原子；短命进程和正在关闭的 socket 可能暂时显示未知归属。TIME-WAIT 等 socket 本来也可能没有进程拥有者。子树预览仅包含可读的进程，遇到不可读记录会提示；扫描安全上限为 16,384 个进程，超出则拒绝构造计划。
- 进程 CPU 以一个逻辑核为 100%，多线程可以超过 100%；第一帧为 0。RSS 在多个端口行重复展示，不能相加。系统 CPU 基于 `/proc/stat`，系统内存使用 Total − Available。
- 命令来源于 `/proc/<pid>/cmdline`，按空格连接 argv，**不是可以无损重新执行的 shell 命令**。最多读取 16 KiB，达到上限明确标记截断；弹窗与详情区均可用 `[` / `]` 滚动读取。脚本原始文本和已经被 exec 替换的历史命令无法恢复。
- 当前用户文本与 JSON 使用 ASCII 安全文本：控制字符和非 ASCII 字节替换为 `?`；完整 Unicode 文本展示待支持。
- 内核或编译环境不支持 pidfd 时仅监视，操作失败显示原因。建议 Linux 5.3+。没有自动提权或自动终止父进程的行为。

## 开发与验证

```sh
make test        # 核心检查 + 真实 socket、进程树、PTY 交互验收
make sanitize    # ASan / UBSan 下运行相同检查（会清理普通构建）
make clean
make             # 恢复普通构建
```

测试仅终止其自行创建的进程。覆盖重启服务的 supervisor、多服务 launcher、无端口后代、兄弟服务隔离、子树身份预检及刷新稳定性。IPv6 不可用时跳过 IPv6 socket 创建；当前 Ubuntu / WSL2 环境已实际覆盖 IPv6。

详见 [架构说明](docs/architecture.md) 和 [验证记录](docs/validation.md)。后续方向：可折叠端口分组与服务标记、完整 Unicode、主题与鼠标操作、namespace 支持，以及大规模进程场景的采集性能测量。

MIT License，见 [LICENSE](LICENSE)。
