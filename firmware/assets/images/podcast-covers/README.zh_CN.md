<p align="right"><strong>简体中文</strong> · <a href="README.md">English（英文）</a></p>

# 原创中性播客图形

公开源码使用程序绘制的原创声波图形，按工程的 MIT（宽松开源许可）发布；
不复制任何节目发布者的封面。`manifest.json`（素材清单）声明
`artwork_policy: original-neutral`（原创中性策略），`artworks`（节目原图）为空，
在 `neutral_tiles`（中性图形）中记录 RGB565（设备像素格式）的校验值。
原来源编号、112 × 112 与 48 × 48 像素的图像接口、对齐且只读的像素，以及
386,048 字节的原常驻闪存预算均保持兼容。

在工程根目录执行，不访问网络，也不依赖第三方图像库：

```bash
python3 firmware/tools/generate_podcast_covers.py
```

此命令重新生成 `firmware/main/podcast_covers.c`（设备图形代码）、素材清单，
以及 `assets/publication/neutral-covers/neutral-<id>-<size>.png`（原创图形预览）。
不读取或覆盖私人保留的节目原图。历史 `originals/`（原图目录）和
`<id>-<size>.png`（节目图）可留在本机供个人使用，但不进入公开源码与发布包。

当前实际界面显示由后台传来的 52 × 52 像素动态封面。这里的内置中性图形
保留兼容接口，不能据此宣称动态封面传输或真机显示已通过。
公开展示页使用原创示例图形与虚构的节目、单集名称。
