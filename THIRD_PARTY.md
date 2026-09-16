# 第三方组件

| 组件 | 固定版本 | 用途 | 许可及源码 |
|---|---|---|---|
| 7-Zip（完整 x64 7z.dll） | 26.03 | ZIP/RAR/7z 解压和解密 | LGPL 及部分 BSD、unRAR 限制；完整许可随包提供；https://www.7-zip.org/ |
| SQLite | 3.53.4 | 标签、设置、进度、明文密码存储 | 公有领域；https://sqlite.org/copyright.html |
| libwebp | 1.6.0 | WebP 解码 | BSD 风格许可及专利授权；https://chromium.googlesource.com/webm/libwebp/ |

7-Zip DLL 未修改，动态加载，完整许可见 `licenses/7-Zip.txt`；其 LGPL 对应完整源码的固定下载地址为 https://www.7-zip.org/a/7z2603-src.7z 。SQLite 与 libwebp 静态链接。固定下载地址与 SHA-256 记录在依赖清单中。

RAR 兼容测试使用 SharpCompress 仓库中的公开测试样本，只读取其中的图片，不执行样本中的其他文件。样本下载地址固定到提交，记录在 `test-output/fixtures/sources.json`；许可为 MIT，仅用于测试，不随便携包发布。

“参考”目录中的 ComicsViewer 仅用于功能与操作参考，不修改、复制其代码或将其二进制文件打入新程序。
