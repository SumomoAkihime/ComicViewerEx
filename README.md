# ComicViewerEx

中文 Windows 原生漫画阅读器，支持直接阅读压缩包、保存常用密码、给压缩包打标签。采用 C++20、Win32、Direct2D/WIC，不需要安装 .NET、浏览器或 7-Zip。

## 下载

- [下载最新便携版](https://github.com/SumomoAkihime/ComicViewerEx/releases/latest)：选择 `ComicViewerEx-1.0.0-win-x64.zip`，解压即可运行。
- [v1.0.0 发布说明与完整源码](https://github.com/SumomoAkihime/ComicViewerEx/releases/tag/v1.0.0)：`ComicViewerEx-1.0.0-source.zip` 附固定版本依赖，`SHA256.json` 提供两个 ZIP 的校验值。
- GitHub 自动生成的 `Source code` 压缩包仅包含仓库文件，构建前需要执行依赖准备脚本。

![ComicViewerEx 阅读界面](docs/images/reading.png)

上图使用程序生成的测试图片展示布局，不附带漫画内容。

## 运行

解压 `ComicViewerEx-1.0.0-win-x64.zip`，运行 `ComicViewerEx.exe`。请完整保留同目录的 `7z.dll` 和 `licenses`。

支持 Windows 10/11 x64。设置、标签、阅读进度和**明文密码**保存在程序旁的 `data/library.db`；备份时退出程序并复制整个 `data` 目录。目录必须可写。同一便携目录只运行一个实例。

## 阅读

- 打开 ZIP/CBZ、RAR/CBR、7z 或单张图片，也可以直接拖入。菜单“阅读图片目录”递归阅读目录中的图片。
- JPEG、PNG、WebP、BMP，以及 GIF/TIFF 的静态首帧。按完整条目名称自然排序，例如 `page2` 在 `page10` 前。
- 单页、双页、宽图左右分页；双页遇到横图时单独显示。菜单可切换从右向左阅读。
- 适合页面、适合宽度、适合高度、原始比例、缩放、拖动、全屏、跳转页码。每本书单独记忆阅读位置和模式，启动恢复上次阅读。
- 加密包先尝试该书上次成功密码和常用密码表；仍不能打开时输入密码。勾选保存后，验证成功才写入密码表。菜单“常用密码表”支持删除和修改；修改需通过当前加密漫画验证。

| 操作 | 快捷键 |
|---|---|
| 打开漫画 / 图片目录 / 浏览漫画目录 | Ctrl+O / Ctrl+D / Ctrl+L |
| 上一页 / 下一页 | ←、PgUp / →、PgDn、空格、滚轮 |
| 单页 / 双页 / 宽图分页 | 1 / 2 / 3 |
| 适合页面 / 宽度 / 高度 / 原始比例 | F / W / H / 0 |
| 放大 / 缩小 | + / −、Ctrl+滚轮 |
| 跳转 / 全屏 | Ctrl+G / F11、双击阅读区 |
| 取消读取 / 退出全屏 | Esc |
| 刷新目录 | F5 |

单字母与翻页快捷键在阅读区获得焦点时生效，避免干扰左侧列表选择。全屏仍可使用快捷键。

## 标签

1. 用“目录”选择存放漫画压缩包的位置。
2. 点击“新建标签”；在文件列表中选择一个或多个文件，在标签列表中选择标签（Ctrl 多选）。
3. 点击“添加标签”或“移除标签”。
4. 点击“应用筛选”，按所选标签过滤文件；可切换“全部匹配 / 任一匹配”、勾选“包含子目录”。“清除筛选”恢复目录列表。

右键标签可重命名或删除。标签绑定磁盘卷和文件标识，同卷改名/移动后刷新目录可继续识别；复制或跨卷移动不会自动继承。无法取得稳定文件标识时使用完整路径。不会改写漫画压缩包。

## 资源边界

- 图片内存缓存预算 96 MiB，优先当前页和相邻页，不预载整本漫画或整目录封面。
- 固实 RAR/7z 可能需要从固实块起点重新解码。经过的图片逐张写入上限 512 MiB 的临时缓存；关闭书籍即清理，异常退出遗留文件下次启动清理。可按 Esc 取消。
- 大于 600 万像素的图片按比例缩小解码，状态栏会提示；“原始比例”仍按原始尺寸布局，但细节受解码分辨率限制。大于 1 亿像素或单张图片数据超出内存预算时明确报错。
- 首版不支持嵌套压缩包、分卷、动画播放、图像增强、文件整理和旧 ComicsViewer 数据导入。

## 构建与验证

构建环境：Visual Studio 2022 C++ Build Tools（含 Windows SDK 和 CMake）、Python 3.12+。首次从精简源码准备依赖时，需要本机 7-Zip 展开源码；源码完整包已附必要依赖。

从仓库获取源码：

```powershell
git clone https://github.com/SumomoAkihime/ComicViewerEx.git
cd ComicViewerEx
```

在项目根目录执行：

```powershell
# 源码包已带依赖时可跳过；联网准备固定依赖并校验 SHA-256
python -X utf8 tools/fetch_dependencies.py

# 准备自生成图片与固定来源的 RAR 测试样本
python -X utf8 tests/prepare_fixtures.py

# Release 编译和核心测试
./tools/build.ps1

# 在隔离的 test-output 子目录运行图形界面验收，产生截图与性能 JSON
python -X utf8 tests/gui_acceptance.py

# 生成便携包、源码包和校验清单
python -X utf8 tools/package.py
```

仅构建不执行测试：`./tools/build.ps1 -SkipTests`。程序位于 `build/Release/ComicViewerEx.exe`。测试生成的数据不进入发布包；不读取“参考”目录的密码。

依赖版本和 SHA-256 见 `third_party/dependencies.lock.json`。实际验证环境、性能口径和未覆盖项目见 `验收报告.md`。

## 性能与兼容性

Windows 11 x64、Ryzen AI 9 HX 370、24 GiB 内存、本地 SSD，Release 构建；阅读样本为 200 页、每页 2000×3000 的 PNG 压缩包。

| 指标 | 本机实测 |
|---|---|
| 启动至可操作 | 首次 467 ms；重复启动 320–344 ms |
| 空闲私有内存 | 37.15 MiB |
| 阅读私有内存 | 162.09 MiB |
| 已缓存翻页 | 48.52–64.25 ms |

核心测试 4/4、界面验收及便携包独立运行检查通过。首次启动未清除系统文件缓存，不能视作严格冷启动；Windows 10 尚未实机验证。固实包首次解码及超大图片不套用以上翻页数据。详见[验收报告](验收报告.md)。

## 项目与反馈

`src` 为程序源码，`tests` 为核心与界面测试，`tools` 为依赖准备、构建和打包脚本。仓库不包含本地数据库、测试输出及参考程序；发布包包含完整 `7z.dll` 和第三方许可，详见[第三方组件说明](THIRD_PARTY.md)。

可通过 [Issues](https://github.com/SumomoAkihime/ComicViewerEx/issues) 反馈问题，请注明系统版本、程序版本、文件格式与复现步骤。不要上传个人密码或 `data/library.db`。
