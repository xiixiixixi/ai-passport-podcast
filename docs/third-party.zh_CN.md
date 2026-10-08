**简体中文** · [English（英文）](third-party.md)

# 第三方材料说明

应用代码按工程根目录 MIT（宽松开源许可）发布。原板级支持和模板保留 `firmware/LICENSE`（原许可）中的 FoloToy 版权。锁定的开发依赖按各自许可下载，不把生成的依赖目录打包成源码。

固件实际依赖的开发库、界面库、音频驱动、网络与基础运行库许可原文及版权声明随包保留在[固件依赖许可证索引](licenses/README.zh_CN.md)。其中还保留界面库内置西文字体、图标字形和 GCC（编译器）运行库例外原文；根目录的 MIT（宽松开源许可）不替代这些依赖条款。

内置 Noto Sans SC（思源黑体简体字库）使用 SIL Open Font License 1.1（开放字体许可），原文保留在 `firmware/assets/fonts/LICENSE-NotoSansSC.txt`。字库无损压缩不改变其许可。网页图标原 MIT 许可在 `server/static/assets/icons/LICENSE`。音频解码器保留原公共领域／CC0（放弃版权限制声明）。

公开包内置的中性封面和展示包装为本项目原创图形，按 MIT（宽松开源许可）发布；网页备用图沿用前述已保留许可的耳机图标。个人节目原图、许可不明的品牌背景和内部外壳参考图不进入公开源码或安装包。首页的设备与节目界面展示来自作者提供的图文稿，图片用途与来源见[首页图片说明](images/README.md)；设备品牌和节目封面不按工程的代码许可重新授权。

节目名称、动态封面和音频属于各自发布者，不改成工程的代码许可。后台动态封面只在使用者实际使用时从节目发布者来源获取，用于识别对应节目；个人音频缓存不进入源码或发布包。公开可听不等于可以任意转载，若另外分发这些媒体，请先确认权利或取得许可。

现代恢复接口参考固定版本 `127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566`。本工程按乐鑫接口和已公开协议独立实现启动入口，没有附带永久恢复程序或复制未明确许可的社区实现。见[固定版本接口说明](https://github.com/SHLcy/ai-passport-miniapp-installer/blob/127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566/docs/development/engineering/ble-recovery-compatibility.md)。
