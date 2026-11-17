# 安装、升级与卸载

[English](install.en.md) | **简体中文** · [返回首页](../README.zh-CN.md)

推荐从源码克隆后使用 `install.sh`。仅运行安装步骤不需要 sudo，默认安装到 `~/.local/bin`。编译依赖包括 C11 编译器、Make、ncursesw；安装脚本还使用 Bash 和 coreutils。Git 仅用于从 Git 仓库升级。

## Ubuntu 用户安装

```sh
sudo apt install git build-essential pkg-config libncursesw5-dev
git clone https://github.com/cossoledad/PortTop.git
cd PortTop
./install.sh
```

生成：

| 文件 | 用途 |
| --- | --- |
| `~/.local/bin/porttop` | 程序 |
| `~/.local/bin/porttop-update` | 绑定该源码目录和安装前缀的升级助手 |
| `~/.local/share/porttop/install.manifest` | 版本化安装记录，保存两个可执行文件的哈希 |

如果 shell 找不到命令，执行 `export PATH="$HOME/.local/bin:$PATH"`，并把同一行加入 `~/.bashrc` 或 `~/.zshrc`。安装器只打印提示，不编辑 shell 配置。可直接运行 `~/.local/bin/porttop`。

需要查看其他用户的进程时，执行 `sudo "$HOME/.local/bin/porttop"`；sudo 的 PATH 可能没有用户目录，故这里使用完整路径。

## 升级

```sh
porttop-update
# 或者：
cd /path/to/PortTop
./install.sh --upgrade
```

助手依赖原源码目录，保留其位置。更新当前分支的 Git upstream；工作区必须干净，更新必须为 fast-forward。工作区有未提交或未跟踪文件、没有 upstream、网络失败或分支发生分叉时，返回明确错误并保留已安装程序。自行处理本地改动或分支后再试；安装器不执行 reset、clean 或自动合并。

更新源码后重新执行新版安装脚本，在源码的 `build/install.*` 中进行独立构建，避免误用旧的 ASan 对象。验证构建产物能执行 `--version` 后，暂存到目标 bin 目录，再替换完整文件。构建失败不会替换已安装程序；Git 源码可能已经更新，修复上游后可以再次升级。替换以单个文件为单位，不宣称多个文件之间具有事务性或断电回滚能力。

下载源码压缩包的用户，下载新版并再次运行 `./install.sh` 可覆盖升级；`--upgrade` 需要 Git checkout。从新的源码位置重新安装会重写升级助手，使其指向新位置。不会自动检查或后台下载更新。

## 自定义目录和系统安装

```sh
./install.sh --prefix "$HOME/tools/porttop"
# 对应命令放在 ~/tools/porttop/bin，可手动加入 PATH

sudo ./install.sh --system
# 默认 /usr/local/bin，只有显式 --system 才允许安装脚本以 root 身份运行
```

自定义 prefix 必须为非根目录的绝对路径，支持空格；不要放在源码目录内，否则已安装文件会使 Git 工作区变脏。系统安装后以 `sudo /usr/local/bin/porttop-update` 升级。系统模式会以 root 运行构建与 Git 更新；如只需个人使用，采用用户安装。

`make install` / `make uninstall` 保留为只处理二进制的打包入口，默认 `PREFIX=$HOME/.local`，支持 `PREFIX` 和 `DESTDIR`。这条入口不创建升级助手和安装记录，使用它安装时也使用对应的 Make 目标卸载。

## 卸载

```sh
/path/to/PortTop/install.sh --uninstall
# 自定义目录：
/path/to/PortTop/install.sh --prefix "$HOME/tools/porttop" --uninstall
# 系统安装：
sudo /path/to/PortTop/install.sh --system --uninstall
```

先检查安装记录和两个可执行文件的哈希，再删除这些文件及记录；不删除源码、其他用户程序或 shell 配置。文件被手动替换或安装记录丢失时拒绝自动删除，输出对应路径供用户处理。运行中的旧进程继续使用原可执行映像，重新启动后才使用新版本。

## 故障与验证

- 缺少编译器或 ncurses 头文件：安装上述 Ubuntu 依赖，再执行安装脚本。安装器保留编译输出。
- `porttop-update` 指向已移动的源码目录：在新的源码目录重新运行安装脚本。
- 想运行当前工作区尚未提交的修改：直接 `./install.sh` 构建安装当前内容；只有 `--upgrade` 要求工作区干净。
- `TERM` 不存在或为 `dumb`：交互界面需要正确的终端类型；批处理可用 `porttop --once` 或 `--json`。

`make test-install` 使用 `build/` 下的隔离 HOME、带空格的 checkout、独立安装前缀和本地 bare Git remote，验收安装、真实快进升级、构建失败保留、工作区修改拒绝及卸载。不会访问 GitHub 或改动真实用户安装，诊断保存在 `build/install-test.log`。
