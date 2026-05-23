# RambosPlayer 打包发布指南

## 前提

- 已完成 Release 编译：`cmake --build build --config Release`
- `release/flv.min.js` 已存在（已随脚本一起提交，无需额外操作）

---

## 打包步骤

在项目根目录的终端中执行：

```powershell
cd release
.\package.ps1                    # 版本默认 1.0.0
.\package.ps1 -Version "1.2.0"  # 指定版本号
```

脚本完成后输出两个产物：

| 产物 | 说明 |
|------|------|
| `release/dist/` | 展开目录，可直接双击 `RambosPlayer.exe` 验证 |
| `release/RambosPlayer-vX.X.X.zip` | 最终分发包，发给客户或上传 GitHub |

---

## 发布到 GitHub Releases

打包验证无误后，在 GitHub 网页发布：

1. 打开仓库主页：`https://github.com/johnjiamzhong-project/RambosPlayer`
2. 右侧点击 **Releases → Create a new release**
3. 填写 **Tag**（如 `v1.0.0`）和 **Release title**
4. 在 **Assets** 区域拖入 `release/RambosPlayer-vX.X.X.zip`
5. 点击 **Publish release**

客户可在 Releases 页面直接下载 zip，解压后双击 `RambosPlayer.exe` 即可运行。

---

## 注意事项

- `release/dist/` 和 `release/*.zip` 已加入 `.gitignore`，不会被提交到仓库
- 目标机器若未安装 Visual C++ 运行时，需附上 [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)
