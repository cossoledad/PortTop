# 开发指南

[English](../CONTRIBUTING.md) | **简体中文** · [返回首页](../README.zh-CN.md)

PortTop 使用 C11 / ncursesw，当前面向 Linux。欢迎提交 issue 和 pull request；反馈问题时请提供版本、发行版、内核、终端、locale 和最小复现，并隐去命令行或截图中的敏感信息。

## 构建运行

```sh
sudo apt install build-essential pkg-config libncursesw5-dev python3 git
make -j2
./build/porttop
```

使用 `CC=clang` 切换编译器，`BUILD_DIR=build/clang` 隔离构建目录。产物统一放在已忽略的 `build/` 中。Python 3 仅供测试与打包使用，运行 PortTop 不依赖 Python。

## 验证

```sh
make test           # C 核心检查、真实 socket、进程树与 PTY 交互验收
make test-install   # 隔离 HOME 与本地 Git remote 的安装、升级、失败保留和卸载
make sanitize       # ASan / UBSan 下运行核心与集成检查，使用独立 build/asan
make test-package   # 构建归档、校验、解压，并运行归档中的程序
```

测试仅终止自行创建的 fixture；IPv6 不可用时跳过对应 socket 用例。安装测试不访问 GitHub 或修改真实用户安装，日志为 `build/install-test.log`。打包诊断为 `build/package-test.log`。Sanitizer 不替换普通构建产物。

行为变更运行 `make test`；安装或打包变更再运行对应检查。UI 变更还应观察 80 × 24 与 120 × 36 的真实终端效果。PR 中说明验证范围与未验证事项。

## 实现约定

- 进程身份由 PID 与启动时间共同确定；终止前保留完整预检、自身祖先保护和显式确认。
- 子树是用户预览过的有限集合，不使用进程组 kill，不将“发送成功”当作“退出成功”。
- 一帧先合成 ncurses 虚拟屏幕，再调用一次 `doupdate()`；避免刷新时清空物理屏幕。
- 历史按进程身份隔离，保留缺失样本；普通用户范围同时用于 TUI、文本与 JSON。

实现职责见 [架构说明](architecture.md)，已有验收与证据边界见 [验证记录](validation.md)。CI、二进制归档与后续发布准备见 [分发指南](distribution.md)。

后续方向包括端口分组与服务标记、完整 Unicode、namespace 支持，以及大规模进程场景的性能测量。
