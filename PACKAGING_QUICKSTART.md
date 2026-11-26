# jietuba-linux 快速打包指南

## ✅ 当前状态

正在本地构建测试包...

## 📦 完成后的操作

### 1. 验证安装成功

```bash
# 检查程序是否安装
which jietuba-linux

# 运行程序
jietuba-linux

# 检查包信息
pacman -Qi jietuba-linux-git
```

### 2. 生成 .SRCINFO（准备发布到 AUR）

```bash
cd /home/wy/Projects/jietuba-linux
makepkg --printsrcinfo > .SRCINFO
```

### 3. 发布到 AUR（如果需要）

```bash
# Step 1: 克隆 AUR 仓库
git clone ssh://aur@aur.archlinux.org/jietuba-linux-git.git aur-repo
cd aur-repo

# Step 2: 复制文件
cp /home/wy/Projects/jietuba-linux/PKGBUILD .
cp /home/wy/Projects/jietuba-linux/.SRCINFO .

# Step 3: 提交
git add PKGBUILD .SRCINFO
git commit -m "Initial release: jietuba-linux-git

Linux port of jietuba screenshot tool
- X11 and Wayland support
- PP-OCR high-accuracy text recognition
- GIF/MP4 recording
- Pin to screen functionality"

# Step 4: 推送到 AUR
git push origin master
```

### 4. 分发包文件（可选）

如果不发布到 AUR，可以分发生成的包：

```bash
# 包文件位置
ls -lh /home/wy/Projects/jietuba-linux/jietuba-linux-git-*.pkg.tar.zst

# 上传到 GitHub Release
# 或通过其他方式分发
```

## 🎯 推荐流程

**个人使用/测试阶段：**
- ✅ 使用本地 makepkg 安装
- ✅ 更新代码后重新 makepkg

**公开发布阶段：**
- 📦 发布到 AUR
- 🌍 用户通过 yay/paru 安装
- 🔄 只需维护 PKGBUILD，用户自动获取最新代码

## ⏱️ 预计时间

- 首次编译：5-10 分钟
- 后续编译：2-5 分钟（有缓存）

## 🆘 如果编译失败

查看完整日志：
```bash
cd /home/wy/Projects/jietuba-linux
makepkg -si 2>&1 | tee build.log
```

常见问题：
1. 依赖缺失 → 安装提示的包
2. 编译错误 → 检查 cpp/build/ 目录
3. Rust 错误 → 检查 rust_libs/ 编译
