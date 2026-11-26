# jietuba-linux AUR 打包指南

## 📦 打包方式选择

### 方案 1：发布到 AUR（推荐）
- ✅ Arch 用户可以通过 `yay -S jietuba-linux-git` 安装
- ✅ 自动处理依赖
- ✅ 自动更新通知
- ⚠️ 需要 AUR 账号

### 方案 2：本地打包
- ✅ 快速测试
- ✅ 生成 .pkg.tar.zst 包
- ⚠️ 其他用户需要手动安装

---

## 🚀 方案 1：发布到 AUR

### 步骤 1：注册 AUR 账号

1. 访问 https://aur.archlinux.org/register
2. 注册账号（如果还没有）
3. 配置 SSH 密钥

```bash
# 生成 SSH 密钥（如果还没有）
ssh-keygen -t ed25519 -C "wy56752466@gmail.com"

# 查看公钥
cat ~/.ssh/id_ed25519.pub

# 复制输出的公钥，添加到 AUR 账户设置中
# https://aur.archlinux.org/account/
```

### 步骤 2：本地测试 PKGBUILD

```bash
cd /home/wy/Projects/jietuba-linux

# 测试构建
makepkg -s

# 如果成功，会生成 .pkg.tar.zst 文件
# 可以先本地安装测试
sudo pacman -U jietuba-linux-git-*.pkg.tar.zst
```

### 步骤 3：创建 AUR 仓库

```bash
# 克隆 AUR 仓库（首次发布）
git clone ssh://aur@aur.archlinux.org/jietuba-linux-git.git aur-jietuba-linux-git
cd aur-jietuba-linux-git

# 复制 PKGBUILD
cp /home/wy/Projects/jietuba-linux/PKGBUILD .

# 生成 .SRCINFO（必需！）
makepkg --printsrcinfo > .SRCINFO

# 查看生成的文件
ls -la
# 应该看到：PKGBUILD  .SRCINFO
```

### 步骤 4：提交到 AUR

```bash
# 添加文件
git add PKGBUILD .SRCINFO

# 提交
git commit -m "Initial commit: jietuba-linux-git

- Screenshot and annotation tool
- Linux port with X11 and Wayland support
- PP-OCR for high-accuracy Chinese text recognition"

# 推送到 AUR
git push origin master
```

### 步骤 5：验证发布

访问：https://aur.archlinux.org/packages/jietuba-linux-git

应该能看到你的包！

### 步骤 6：用户安装

其他用户可以这样安装：
```bash
yay -S jietuba-linux-git
# 或
paru -S jietuba-linux-git
```

---

## 🔧 方案 2：本地打包（快速测试）

### 步骤 1：构建包

```bash
cd /home/wy/Projects/jietuba-linux

# 安装构建依赖并构建
makepkg -si
# -s: 自动安装依赖
# -i: 构建后自动安装
```

### 步骤 2：分发给他人

```bash
# 生成的包文件
ls -lh jietuba-linux-git-*.pkg.tar.zst

# 可以上传到 GitHub Release
# 或通过其他方式分发
```

### 其他人安装
```bash
# 下载 .pkg.tar.zst 文件后
sudo pacman -U jietuba-linux-git-r1.0.0-1-x86_64.pkg.tar.zst
```

---

## 🔄 更新 AUR 包

当你更新代码后，需要更新 AUR：

```bash
cd aur-jietuba-linux-git

# 1. 更新 PKGBUILD（如果有改动）
# 2. 重新生成 .SRCINFO
makepkg --printsrcinfo > .SRCINFO

# 3. 提交更新
git add PKGBUILD .SRCINFO
git commit -m "Update to latest commit

- 描述更新内容"
git push
```

**注意：** 对于 `-git` 包，用户每次运行 `yay -S jietuba-linux-git` 都会拉取最新代码编译，所以你只需要在更改依赖或 PKGBUILD 逻辑时更新 AUR。

---

## 📝 PKGBUILD 检查清单

在提交前确认：

- [x] `pkgname` 正确（jietuba-linux-git）
- [x] `url` 指向你的 GitHub 仓库
- [x] `source` 指向正确的 Git URL
- [x] `depends` 包含所有必需依赖
- [x] `optdepends` 列出可选功能
- [x] `Maintainer` 信息正确

---

## ⚠️ 常见问题

### 问题 1：SSH 连接失败
```bash
# 测试 SSH 连接
ssh -T aur@aur.archlinux.org

# 应该看到：
# Hi username! You've successfully authenticated...
```

### 问题 2：makepkg 缺少依赖
```bash
# 安装 base-devel
sudo pacman -S base-devel

# 安装构建工具
sudo pacman -S git cmake cargo
```

### 问题 3：.SRCINFO 格式错误
```bash
# 删除并重新生成
rm .SRCINFO
makepkg --printsrcinfo > .SRCINFO
```

### 问题 4：包名冲突
如果 `jietuba-linux-git` 已被占用，可以使用：
- `jietuba-linux-dev-git`
- `jietuba-wayland-git`

---

## 📊 预期结果

发布成功后：

1. **用户可以搜索到**
   ```bash
   yay -Ss jietuba
   ```

2. **一键安装**
   ```bash
   yay -S jietuba-linux-git
   ```

3. **自动更新**
   ```bash
   yay -Syu  # 会检查你的 Git 仓库更新
   ```

---

## 🎯 推荐流程

**首次发布：**
1. 本地测试 `makepkg -si`
2. 确认能正常运行
3. 发布到 AUR
4. 在另一台机器用 `yay` 测试安装

**后续更新：**
1. 修改代码并推送到 GitHub
2. 仅当 PKGBUILD 变化时更新 AUR
3. 通知用户重新安装以获取最新版本

---

需要我帮你执行哪一步吗？
