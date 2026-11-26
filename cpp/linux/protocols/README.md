# Wayland 协议定义

- `ext-image-copy-capture-v1.xml`、`ext-image-capture-source-v1.xml` 和它的链接依赖 `ext-foreign-toplevel-list-v1.xml` 来自 [wayland-protocols 1.45](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/1.45/staging)，许可证保留在 XML 内。
- `wlr-screencopy-unstable-v1.xml` 来自 [wlr-protocols](https://github.com/swaywm/wlr-protocols/blob/master/unstable/wlr-screencopy-unstable-v1.xml) 已归档仓库，许可证保留在 XML 内。

构建时使用 `wayland-scanner` 生成客户端代码。协议是否可用由运行时合成器公布的接口和访问策略决定，并不由这些 XML 文件决定。
