# RambosPlayer 打包发布指南

## 前提

- 已完成 Release 编译：`cmake --build build --config Release`
- `release/flv.min.js` 和 `release/mpegts.min.js` 已就位
- 当前分支代码已准备好发版

---

## 完整发布流程

### Step 1 — 确定版本号

每次发布前确认版本号。版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)：

| 变动类型 | 版本号示例 | 说明 |
|----------|-----------|------|
| 不兼容 API 变更 | 2.0.0 | 主版本递增 |
| 向下兼容的新功能 | 1.1.0 | 次版本递增 |
| 向下兼容的缺陷修复 | 1.0.1 | 补丁版本递增 |

**版本号由用户确认，Claude 不自行决定。**

### Step 2 — 更新 version.md

打开 `release/version.md`，按模板追加一条新记录：

```markdown
| x.x.x | YYYY-MM-DD | Release Title | 关键变更摘要 |
```

- **版本**：Step 1 确定的版本号
- **日期**：发布当天日期
- **Release Title**：GitHub Releases 的标题（简短，如 "低延迟 MPEG-TS 推流"）
- **概要**：关键变更的一行摘要

### Step 3 — 提交 version.md + 打 tag

```powershell
git add release/version.md
git commit -m "release: vx.x.x Release Title"
git tag vx.x.x
git push origin main --tags
```

### Step 4 — 打包

```powershell
cd release
.\package.ps1 -Version "x.x.x"
```

脚本完成后验证产物：

| 产物 | 说明 |
|------|------|
| `release/dist/` | 展开目录，可直接双击 `RambosPlayer.exe` 验证 |
| `release/RambosPlayer-vx.x.x.zip` | 最终分发包 |

### Step 5 — 创建 GitHub Release

```powershell
gh release create vx.x.x "./release/RambosPlayer-vx.x.x.zip" --title "Release Title" --notes "Release notes 正文"
```

或通过 GitHub 网页操作：

1. 打开仓库主页 → **Releases → Create a new release**
2. **Tag**：选择或输入 `vx.x.x`
3. **Release title**：同 Step 2 的 Release Title
4. **Release notes**：详细记录本次新增功能、修复、已知问题
5. **Assets**：拖入 `release/RambosPlayer-vx.x.x.zip`
6. **Publish release**

---

## 快速打包（不发版时）

仅需要打包验证，不创建 Release：

```powershell
cd release
.\package.ps1                    # 默认 1.0.0
.\package.ps1 -Version "1.2.0"  # 指定版本号
```

---

## 注意事项

- `release/dist/` 和 `release/*.zip` 已加入 `.gitignore`，不会被提交到仓库
- 目标机器若未安装 Visual C++ 运行时，需附上 [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- `release/version.md` 仅记录已发布的版本，打包验证阶段不更新
- 打包验证通过后才能更新 version.md 和创建 Release
