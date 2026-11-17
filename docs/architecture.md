# 架构与实现边界

入口为 `README.md`。当前只有一套 C11 / Linux 实现，运行时依赖 libc、ncursesw / terminfo，不调用外部命令。

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | 参数校验、TTY 检测、批处理或交互入口 |
| `src/porttop.h` | Socket / Process / Row / Snapshot / SignalPlan 契约 |
| `src/proc.c` | Linux socket/owner 采集、排序筛选、文本/JSON 输出、单目标 pidfd 信号 |
| `src/history.c` | 系统与进程采样历史环，按 PID + start ticks 隔离、保留缺口和淘汰旧记录 |
| `src/actions.c` | 自身祖先保护、有限子树预览、所有目标预检和逐个发送 |
| `src/text_view.c` / `src/text_view.h` | 可变长度只读文本、无损换行索引、滚动边界和 resize 字节锚点 |
| `src/ui.c` | ncurses 屏幕、事件循环、选中身份、居中终止弹窗、终端生命周期 |
| `tests/core.c` | stat 解析、CPU 差分、pidfd 及子树身份安全 |
| `tests/integration.py` | 实际进程/socket、PTY、重启链与列表稳定性验收 |
| `tests/vt.py` | 测试用增量 UTF-8 VT 屏幕模型，解析 ncurses 差分输出 |
| `tests/fixture.py` | 多协议、共享 FD、fork 测试服务 |
| `install.sh` / `tests/install.py` | 用户安装、显式快进升级、卸载与隔离验收 |
| `tests/load_fixture.py` | 可控 RSS 增长和 CPU 负载，用于实际图表变化验收 |
| `tests/supervisor_fixture.py` | launcher、重启 supervisor、服务、无端口 worker 和兄弟服务 |

## 采集与展示

读取当前 namespace 的 `/proc/net/{tcp,tcp6,udp,udp6}`，默认仅 TCP LISTEN 和 UDP UNCONN，忽略本地端口 0。按 socket inode 排序，单次遍历可见 `/proc/<pid>/fd`，通过二分查找匹配。相同进程重复 FD 去重；fork 继承者分别记为 owner。

Process 记录 PID、PPID、PGID、session、tty、状态、start ticks、CPU ticks、RSS、cmdline、cwd、exe 和用户信息。枚举 FD 前后验证启动时间，丢弃身份改变的进程。未找到 owner 的 socket 保留行。每个 owner/socket 关系映射为 Row，仅保存 snapshot 内索引；排序筛选生成索引投影，不改原始数据。

CPU 使用前后快照的进程 ticks 差值 / `_SC_CLK_TCK` / 实际单调时钟间隔。系统 CPU 从总 ticks 和 idle+iowait 差分计算。批处理不做两次采样，JSON 明示 `cpu_sampled: false`。RSS、端口和进程的统计不可混合求和。

`denied_processes` 是打不开 FD 目录的进程数；`unknown_sockets` 是没有匹配进程的 socket 数；`unavailable_tables` 是无法打开的网络表数量。统计作用于完整采集结果，不受筛选影响；每个 FD 及附加元数据读失败不单独计数。

## 用户范围与历史曲线

默认视图判定统一在 `rows_select`：viewer 为有效 UID 0 时保留所有原始行，否则必须有已识别 owner 且其 UID 等于 viewer。该投影用于交互、文本与 JSON，`--all` 不改变它。进程 UID 取 `/proc/<pid>` 目录属主，属于归属过滤而非启动历史追踪。普通用户的祖先选择也在跨 UID 处停止。root 不绕过 namespace、LSM 或 proc 挂载的实际限制。

Snapshot 仍保留完整可采集数据，诊断计数基于它；JSON 的 `scope`、`viewer_uid`、`system` 明示视图和系统容量。系统 CPU/内存统计不按 UID 过滤。此规则是默认展示范围，不是操作授权；实际信号仍经过内核 pidfd 权限检查。

`/proc/stat` 解析总 CPU 和每个 `cpuN`；总量只累加 user/nice/system/idle/iowait/irq/softirq/steal，避免重复计入 guest。CPU 差分必须满足总 ticks 递增且 idle 差值在合理范围，否则标为无效；每核按 ID 匹配前一帧以支持核上下线。进程 CPU 也标记是否存在有效前帧，避免把缺失采样当作测得 0。

History 为 240 点环，每次成功采样追加一帧系统 CPU、内存、核数、总内存和单调时间。每个当前用户范围内的 socket owner 按 PID + start ticks 存独立 CPU/RSS 序列，与主环共享 slot。每帧先清空该 slot 的进程存在标记，再填当前数据；退出、缺失和 PID 复用不会拼接曲线。240 帧未出现的进程历史被淘汰。同一进程的多个端口复用同一条曲线，切换过滤或选择可立即读取已有历史。

界面左右两图分别为系统 CPU 与系统内存，固定 0–100% 刻度。UTF-8 模式每个 Braille 单元绘制两次采样，系统区域渐层填充、轮廓高亮，进程轨迹连接相邻有效点；缺失点不插值。加入淡色 0/50/100% 刻度和可见范围首尾样本的单调时间跨度。混合单元采用进程颜色，其余系统区域保持原色。用户文本仍为 ASCII。locale 不支持宽字符时保留原密度字符及 `*` 虚线作为回退。默认进程曲线采用独立刻度，CPU 上限从 100% 起、内存从 32 MiB 起按 2 倍扩展至覆盖保留历史峰值，并在图例显示。按 g 切换统一刻度：进程 CPU 图值 = top 风格 CPU% / 该历史帧的核数，内存图值 = RSS / 该帧系统总内存。独立刻度让小服务的实际变化可见，统一刻度用于比较系统贡献。标题始终保留进程原始 CPU% 和 RSS MiB。每个逻辑核另有实时仪表，按 c 翻页，不为适配核数而动态增加面板高度。

历史只在成功采样时推进；暂停、弹窗与采集失败不追加。窗口宽度只改变读取环的范围，不重置历史。横轴每列两次采样，非均匀墙钟时间轴；没有跨重启持久化。图高在固定的 24 行压缩模式和 32 行以上完整模式间切换，压缩模式省略 EXE 等详情以保留列表空间。

## ncurses 与稳定刷新

事件循环在数据更新、用户输入和窗口变化时绘制。`erase()` 只清理 stdscr 的虚拟内容，随后 `wnoutrefresh(stdscr)`；如有弹窗，绘制并暂存独立 WINDOW；最后只执行一次 `doupdate()`。不在常规采样时使用 `clear()`、`clearok()` 或直接输出清屏 ANSI 序列。差分刷新依据 [ncurses 刷新契约](https://invisible-island.net/ncurses/man/curs_refresh.3x.html)。暂停且没有输入或尺寸变化时，不绘制也不输出。

面板使用圆角边框和语义配色（CPU 绿、内存橄榄色、端口暖色、详情蓝），支持 256 色、基础色与无颜色回退。顶部 CPU 面板内部固定分为图表和核网格，内存另有面板；中部端口表和底部详情均有完整边框。表格按宽度调整地址、命令和 USER 列；详情高度压缩为完整 9 行或紧凑 6 行，释放列表空间。命令列表省略首个可执行路径，下方摘要固定预览首段；Enter/i 的独立详情使用原 cmdline 自动换行。弹窗和帮助遮罩只改变绘制样式，不改变确认协议。

分区布局固定，不随命令长度、端口数和核数改变。刷新按 inode + PID + start ticks 恢复选中身份；根据刷新前选中行相对滚动窗口的位置修正 offset，在边界允许时保留屏幕行。CPU/RSS 排序仍会根据真实数值重排，选中目标消失时选邻近行；这与绘制闪烁是不同边界。

输入由 ncurses keypad / getch 处理。内核 WINCH 信号触发实际尺寸检查和 resizeterm；ncurses 合成的 KEY_RESIZE 仅请求重绘，不能再次无条件触发 resizeterm，否则形成事件反馈循环。弹窗存活至关闭或尺寸变化，不因每次采样反复创建。

保存 termios；ncurses 进入 alternate screen / cbreak / noecho。INT/TERM/HUP 和正常退出 endwin 并还原 termios；TSTP 保存程序模式、恢复终端、停止，继续后 reset_prog_mode 并重新布局。SIGKILL 等不可捕获退出无法恢复。

## 固定详情视口与鼠标

Enter / i 打开 `InfoDialog`：首选 110 × 28 字符，实际尺寸为首选与终端减边距的较小值。尺寸不依赖数据长度，终端变大不会超过首选尺寸。当前进程、端口和父链先复制为独立文本，冻结采样，父进程终止确认可作为它的父弹窗；关闭详情后恢复原视图与按钮状态，不重建已审查的 SignalPlan。

TextView 负责格式化文本、以字节区间索引显示行，换行不丢弃空格和长单词。用户数据已按既有规则转为 ASCII，因此字节宽度等于终端单元宽度。终端变窄时重新换行，保留视口顶部字节附近的锚点；滚动位置始终限制在 `0..max(0,total-visible)`。控件只显示切片，不裁剪或修改底层 CMD，16 KiB 截断来自采集层并明确标记。

仅在详情或终止弹窗打开时启用 BUTTON4/5 的鼠标报告；所有 KEY_MOUSE 都先消费，再按屏幕坐标命中窗口。详情内滚轮每次移动三行，窗外忽略；Up/Down、PgUp/PgDn 和 Home/End 走相同滚动边界。鼠标接口依据 [ncurses 鼠标事件契约](https://invisible-mirror.net/ncurses/man/curs_mouse.3x.html)。关闭最后一个弹窗和退出时禁用报告，挂起/继续重新恢复鼠标状态。全屏尺寸小于 80 × 24 时保留已有快照并只允许关闭，不处理不可见滚动或确认操作。

详情是只读模式，k/K/y 不会触发终止；进程退出也不会替换该视口身份。关闭后主采集再检查实际存活状态。原方括号分段滚动已移除，终止确认窗以 i 打开当前候选目标的完整详情，其预览列表支持滚轮及翻页。

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

## 用户安装与升级

`install.sh` 默认前缀 `$HOME/.local`，拒绝未显式指定 --system 的 root 调用；不自动提权或修改 shell 配置。使用独立的 `build/install.*` 构建目录，在 staging 文件具备可执行版本输出后替换安装文件。升级助手通过 Bash 安全引用原始源码路径及前缀，支持含空格的目录。

`--upgrade` 要求是源码根 Git checkout、工作区干净、有 upstream；仅执行 `git pull --ff-only`，然后重新执行更新后的脚本。构建失败保留原安装文件，不回滚已快进的源码。安装清单只保存格式标签和两个文件哈希，不 source 或执行清单内容；卸载前核对两个文件，任一变化时不删除。每个二进制以同目录 rename 替换，不宣称整个安装具有多文件事务性。

Makefile 的 `BUILD_DIR` 支持独立构建，`PREFIX` 默认也改为用户目录；底层 Make 安装目标仍用于二进制打包，脚本承担升级助手和清单生命周期。详见 [安装文档](install.md)。

## 后续扩展边界

尚无线程、后台守护程序、持久化配置、容器聚合和进程组操作。主采集仍扫描可见进程及 FD；子树预览额外扫描进程，有限集合不构成原子进程树快照。用户文本目前先转换为 ASCII 安全文本，ncursesw 本身并不自动补足应用的 Unicode 数据模型。需要提高容量时先测量采集、计划构造和终端输出，再考虑 inet_diag、缓存或异步采集。
