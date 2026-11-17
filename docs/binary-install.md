# PortTop — Linux binary bundle

English | 简体中文

This archive contains a native, dynamically linked Linux executable, its MIT license and `BUILD-INFO.json`. Check that file for the source commit, architecture, build distribution, libc and shared-library dependencies. Install compatible ncursesw/terminfo runtime libraries before use; this is not a static binary for every Linux distribution.

Verify the adjacent checksum before extracting:

```sh
sha256sum -c porttop-VERSION-linux-ARCH.tar.gz.sha256
tar -xzf porttop-VERSION-linux-ARCH.tar.gz
cd porttop-VERSION-linux-ARCH
install -Dm755 bin/porttop "$HOME/.local/bin/porttop"
export PATH="$HOME/.local/bin:$PATH"
porttop
```

Replace VERSION and ARCH with the archive's version and architecture. Install a later bundle the same way to upgrade. This binary-only installation creates no update helper or manifest. To remove it, delete `$HOME/.local/bin/porttop`. If you already use the source installer, upgrade through that installer instead of overwriting its tracked files.

此归档包含动态链接的 Linux 程序、MIT 许可证与构建信息。请在 `BUILD-INFO.json` 中确认源码提交、架构、构建系统和共享库依赖；需要兼容的 ncursesw/terminfo 运行库。将命令中的 VERSION、ARCH 替换为文件名中的实际值，先校验、再解压安装。升级时重复安装新版；此方式不创建升级助手或安装记录，卸载只需删除对应二进制。已使用源码安装脚本的用户应沿用该脚本升级。

Project, usage and source installation: https://github.com/cossoledad/PortTop
