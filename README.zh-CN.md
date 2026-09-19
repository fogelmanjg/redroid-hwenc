# redroid-hwenc

**语言:** [English](README.md) | [Español](README.es.md) | 中文

> 本项目的官方语言是**英语**。本文件仅为方便阅读而提供，翻译可能不完全准确或未及时更新——
> 如有疑问，请以 [README.md](README.md)（英文版）为准。

为 [redroid](https://github.com/remote-android/redroid-doc) 提供硬件视频编码支持（H.264/H.265，基于 VA-API）。

## 问题所在

BlueStacks、Nox、MuMu 在 Linux 桌面上都没有真正的替代品。[redroid](https://github.com/remote-android/redroid-doc)
是最接近的方案——Android 运行在 Docker 容器中，渲染部分有 GPU 直通加速——但屏幕串流
（scrcpy，或者任何 Android 应用内部的视频播放）依然只能依赖**纯软件编码器**
（`OMX.google.h264.encoder` / `c2.android.avc.encoder`）。`gpuMode=host` 只加速渲染
（通过 Mesa 的 OpenGL/Vulkan），从不涉及编码器。

这个需求在官方仓库里**从 2022 年就一直开着**，至今没有人解决并公开发布过：

- [remote-android/redroid-doc#126](https://github.com/remote-android/redroid-doc/issues/126) — AMD VA-API（OMX/Codec2）
- [remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172) — 同样的需求，AMD
- [remote-android/redroid-doc#168](https://github.com/remote-android/redroid-doc/issues/168) — 同样的需求，Intel
- [remote-android/redroid-doc#535](https://github.com/remote-android/redroid-doc/issues/535) — H.265 需求，2025 年 9 月的留言至今无人回应

项目维护者（`zhouziyang`）的回答一直是同一句话：VA-API 驱动已经打包进 redroid 了，但真正
调用它来编码的 Codec2/OMX 组件，得社区自己去写。四年过去了，没有人把它写完并发布出来。

## 为什么是现在

这不是不可能的事——只是"真正关心这个具体问题"和"愿意深入 AOSP/Codec2/VA-API"这两者
的交集太少见了。那些 issue 里大部分人是在求这个功能，而不是愿意亲自去写的人。这个仓库是在
公开场合尝试解决它，按难度递增分成几个阶段，并随进度记录过程（见 [DEVLOG.md](DEVLOG.md)，英文）。

## 路线图（按难度分级，不按时间）

每个阶段都假设前一个阶段已经完成。⭐ 标记的是最关键、杠杆最大的检查点。

- [x] **阶段 0 — 纯调研。** 弄清楚 redroid/scrcpy 现在是怎么选择编码器的（是通过
      `MediaCodecList` 按能力匹配，还是写死了名字？）。确认真实硬件上有哪些 VA-API 编码
      entrypoint 可用（`VAEntrypointEncSlice`）。
- [ ] **阶段 1 — 阅读与梳理。** Codec2 组件的结构（参考
      [`android_external_v4l2_codec2`](https://gitcode.com/pi-plus/android_external_v4l2_codec2)
      的代码结构，而不是它的硬件逻辑——那是 V4L2，这里用的是 VA-API）。VA-API **编码**
      （而非解码）部分的 API 接口。
- [ ] **阶段 2 — 独立的原生原型。** 一个独立程序（跑在宿主机上，不涉及 Android），
      通过 `/dev/dri/renderD*` 用 VA-API 编码 H.264。
- [ ] **阶段 3 — ⭐ 决定成败的关键验证。** gralloc buffer 导出的 `dma-buf` fd，能不能在
      和 redroid 权限相同的容器里被 VA-API 零拷贝导入？四年的 issue 里似乎没人验证过这一点。
      redroid 是以容器（而非虚拟机）方式运行的——容器边界上没有 virtio-gpu，起点比社区
      普遍假设的要好。
- [ ] **阶段 4 — Android 端的 Codec2 骨架。** 一个能被 Android 识别并列出的组件
      （`dumpsys media.c2` 里显示为 `c2.hardware.encoder.h264`），暂时不接入真实编码逻辑。
- [ ] **阶段 5 — 真正的整合。** 把阶段 3 和阶段 2 接入阶段 4 组件的回调里。这才是最终目标。
- [ ] **阶段 6（取决于阶段 0 的结果）。** 如果发现 redroid/scrcpy 的编码器选择逻辑是写死的
      而不是按能力匹配的：修补它。

哪怕最终没能走到最后一步，每个阶段单独拿出来都是一份可以发布的贡献——这四年里从来没人
把这些内容整理记录下来过。

## 目前进度

刚刚起步。进度请看 [DEVLOG.md](DEVLOG.md)（英文），按会话逐次记录。

## 参与贡献

没有 CLA，没有额外门槛——纯 Apache-2.0。如果你对这个问题感兴趣，直接提 PR 或在 issue 里
留言，比先问"能不能参与"更有价值。

## 许可证

Apache License 2.0——详见 [LICENSE](LICENSE)。特意选择和 AOSP（`frameworks/av`）相同的许可证：
如果将来这里的成果值得往上游合并，不会有法律上的阻碍。
