**简体中文** · [English（英文）](README.md)

# 随身听公开展示素材

| 文件 | 尺寸 | 用途 |
| --- | --- | --- |
| `player-native.png`（播放页） | 240 × 320 | 实际播放器界面的无损导出 |
| `library-native.png`（节目库） | 240 × 320 | 实际节目库界面的无损导出 |
| `play-cover-3x4.png`（竖版封面） | 1200 × 1600 | 玩法网站作品封面 |
| `library-showcase-3x4.png`（节目库展示） | 1200 × 1600 | 玩法网站的节目库配图 |
| `github-showcase.png`（横版展示） | 1600 × 900 | 代码托管仓库的总览配图 |

两张原始画面由真正的 `firmware/main/podcast_ui.c`（设备界面程序）在电脑上
运行 LVGL 9.5.0（界面绘制库）导出，使用与设备相同的 240 × 320 像素、
RGB565（设备像素格式）及 40 行分块绘图缓冲。**这是电脑导出，不是真机截图或照片。**
节目、单集标题、日期、播放位置和电量均为虚构示例；没有使用个人收听记录、
家庭服务配置、凭据或私人屏幕截图。

声波图形由 `firmware/tools/generate_podcast_covers.py`（原创图形生成工具）
直接绘制，不读取任何原图或网络内容。预览通过生产程序的尺寸、CRC（循环校验）、
SHA（内容校验）和素材登记路径，载入三份 52 × 52 像素的 PDC1（封面传输格式）
示例。新增电脑预览入口不会编入设备，也不假称生产下载已经成功。
这验证了示例素材下的界面画面，不能证明真机传图、音频播放或实际联网已经通过。

`render.py`（展示图生成脚本）把 PPM（原始画面格式）无损编码为 PNG（图片格式），
并创建三份原创 SVG（矢量图）排版；`render.mjs`（矢量图渲染脚本）用
`@resvg/resvg-js` 2.6.2（矢量图渲染库）输出最终图片。
界面画面完整嵌入矢量图中，按两倍整数比例放大；没有用图像模型重绘界面文字。
没有使用设备外壳合成图、官方品牌参考、节目发布者封面、第三方照片或二维码。
原创展示图形按工程 MIT（宽松开源许可）发布；Noto Sans SC（思源黑体简体字库）
保留其 SIL Open Font License 1.1（开放字体许可），原文位于
`firmware/assets/fonts/LICENSE-NotoSansSC.txt`（字体许可文件）。

准备好工程锁定的固件依赖后，在工程根目录执行以下命令复现：

```bash
python3 firmware/tools/generate_podcast_covers.py
cmake -S firmware/tools/podcast_preview -B firmware/tools/podcast_preview/build
cmake --build firmware/tools/podcast_preview/build --target preview -j 4
cd assets/publication
../../firmware/tools/podcast_preview/build/preview --publication
cd ../..
python3 assets/publication/render.py
npm install --prefix /tmp/passport-publication-render --no-audit --no-fund @resvg/resvg-js@2.6.2
NODE_PATH=/tmp/passport-publication-render/node_modules node assets/publication/render.mjs
```

预览导出时检查实际字体字形、控件边界及封面的原始像素。
两张原始画面与三张最终展示图已逐张视觉检查；本材料不宣称最新公开固件已经
完成真机验收。`provenance.json`（来源校验清单）记录源码与输出图片的校验值，
两份说明保持相同的证据边界。
