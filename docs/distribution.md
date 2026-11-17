# CI and distribution

CI runs on pull requests, pushes to `main` and version tags (`v*`), or manual dispatch. It has read-only repository permissions; it does not publish releases.

| Job | Coverage |
| --- | --- |
| Build and test | Ubuntu 22.04 / 24.04 × GCC / Clang; C core, real sockets/process trees and PTY interaction |
| Sanitizers | Clang on Ubuntu 24.04; ASan / UBSan, including integration tests |
| Install and package | Ubuntu 22.04 x86_64; isolated install/upgrade/uninstall and archive acceptance |

Actions are pinned to commit SHAs; Dependabot checks them monthly. Failed jobs upload available terminal/installer/package diagnostics for seven days. The package job uploads a native bundle and SHA-256 checksum for fourteen days. Download them from the workflow run's **Artifacts** section; these are CI build artifacts, not published releases.

## Build a bundle locally

```sh
sudo apt install build-essential pkg-config libncursesw5-dev python3 binutils
make test-package
make package
# Optional: make package DIST_DIR="/absolute/output path"
```

The output is `build/dist/porttop-VERSION-linux-ARCH.tar.gz` plus `.tar.gz.sha256`. Version comes from `PT_VERSION` in `src/porttop.h`. Packaging builds fresh optimized objects, strips the executable, verifies its version and includes:

- `bin/porttop`
- `LICENSE` and bilingual binary-installation instructions
- `BUILD-INFO.json`: version, native architecture, source commit/dirty state, build distribution, compiler, libc, dynamic dependencies and binary SHA-256

The acceptance test verifies the checksum, safe archive paths, executable identity and JSON behavior after extraction. It also checks that inherited sanitizer flags do not leak into the bundle. The test uses a temporary output directory; run `make package` to retain a bundle.

## Compatibility and release boundary

Bundles are dynamically linked to the build system's libc and ncursesw/terminfo. The CI package job uses Ubuntu 22.04 as its build baseline; that alone does not establish compatibility with every newer distribution. Inspect `BUILD-INFO.json` and smoke-test on each intended target. Native aarch64 packaging is recognized by the script but is not covered by the current CI matrix. There are no cross-compiled, static, deb or rpm packages yet.

Archive ownership, modes, ordering and timestamps are normalized. Builds are not claimed reproducible across compilers, distributions or source states. Checksums detect corruption; they are not release signatures.

For a future release: update `PT_VERSION`, run all checks on the intended source commit, review the resulting archive and build information, then tag and publish through a separately reviewed release process. A `v*` tag runs CI and creates artifacts only. No release token, publishing permission or automatic release workflow is configured.

## 中文摘要

CI 覆盖 Ubuntu 22.04 / 24.04 的 GCC、Clang，以及独立的 ASan / UBSan、真实安装升级和归档验收。成功的打包任务保留 Linux 归档与 SHA-256 十四天，失败日志保留七天；在 Actions 运行页面下载。

本地使用 `make test-package` 验证，`make package` 生成 `build/dist/` 下的实际归档。包中附有许可证、中英文安装说明和构建清单。当前产物动态依赖 libc、ncursesw/terminfo，仅 CI 的 x86_64 目标受覆盖；其他架构、发行版和包管理器格式需单独验证。工作流只构建与测试，不自动发布 GitHub Release；后续可在此基础上补充发布流程。
