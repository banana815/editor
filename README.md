# Note Lab

使用 C11 编写的富文本笔记编辑器，面向 **Windows、macOS、Linux、Android、iOS 和浏览器**，支持笔记存储、Zotero 文献导入及纯 C 引用处理。

当前应用源码统一位于 [`c-editor/`](c-editor/README.md)。旧 Tauri 应用和专用构建流程已移除，历史版本可从 Git 历史查看。旧笔记的 HTML、JSON 和兼容格式导入继续由 C 版本提供。

## 构建与测试

需要 CMake 3.24+、C/C++ 编译工具和 Python 3.8+。首次配置会下载固定版本依赖和字体；Linux 还需要窗口系统开发包及 libcurl 7.84+，完整依赖见 [应用构建说明](c-editor/README.md) 与 [构建流程](.github/workflows/c-editor.yml)。

在仓库根目录运行：

```sh
cmake -S c-editor -B c-editor/build -DCMAKE_BUILD_TYPE=Debug
cmake --build c-editor/build --config Debug --parallel 4
ctest --test-dir c-editor/build -C Debug --output-on-failure
```

macOS/Linux 启动 `./c-editor/build/notelab`；Windows 使用对应配置目录中的 `c-editor/build/Debug/notelab.exe`。桌面打包、移动端和浏览器构建见下方文档。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| [`c-editor/src/`](c-editor/src/) 与 [`include/`](c-editor/include/) | 应用、文档编辑、存储、文献库和平台接口 |
| [`c-editor/csl/`](c-editor/csl/README.md) | 纯 C 引用处理器与测试 |
| [`c-editor/platform/`](c-editor/platform/) | Android、iOS、浏览器构建与平台连接代码 |
| [`c-editor/tests/`](c-editor/tests/) | 原生、浏览器、网络与发行包测试 |
| [`c-editor/cmake/`](c-editor/cmake/) 与 [`tools/`](c-editor/tools/) | 固定依赖、打包及数据生成工具 |
| [`c-editor/licenses/`](c-editor/licenses/) | 第三方许可证和来源记录 |
| [`.github/workflows/`](.github/workflows/) | 应用和引用处理器的跨平台检查 |
| [`docs/design/`](docs/design/README.md) | 原始图标设计素材 |

## 文档入口

- [应用功能、构建与桌面打包](c-editor/README.md)
- [项目状态、已验证结果与待办](c-editor/PROJECT_STATUS.md)
- [浏览器](c-editor/platform/web/README.md)、[Android](c-editor/platform/android/README.md)、[iOS](c-editor/platform/ios/README.md)
- [文献来源与网络行为](c-editor/platform/LIBRARY_SOURCE.md)
- [引用处理器](c-editor/csl/README.md)与[文档兼容边界](c-editor/csl/DOCUMENT.md)
- [第三方依赖与许可](c-editor/THIRD_PARTY.md)
- [开发与维护指南](AGENTS.md)

## 构建产物与进度

[C editor 工作流](https://github.com/banana815/editor/actions/workflows/c-editor.yml)生成桌面预览包、浏览器文件和未签名移动端构建；[C citation processor 工作流](https://github.com/banana815/editor/actions/workflows/csl.yml)验证引用处理器。两条流程均可在 GitHub Actions 中手动触发。

版本仍处于开发预览阶段。六平台编译和启动检查已通过；功能对齐、实体设备验证、签名与自动更新仍有待办，详见项目状态。当前功能开发保持暂停。
