# OMV「插件管理器」插件开发交接文档

> 目标：仿 OpenWrt 包管理器（luci-app-package-manager / 旧名 luci-app-opkg），
> 为 OpenMediaVault 8.x 开发原生插件 `openmediavault-pluginmgr`（显示名"插件管理器"），
> 提供 WebUI 上传/安装本地 .deb、已安装包列表、搜索仓库、安装/卸载、检查更新。
> 源码托管 GitHub，编译发布全部走 GitHub Actions 云端完成。

## 0. 已确认的环境事实（NAS 侧，勿重复调查）

- NAS：OMV 8.5.9（Synchrony）/ Debian 13 trixie / x86_64，主机名 Satellite。
- 已装插件：openmediavault-apt 8.0.2、omvextrasorg 8.0.3、compose、ftp、md、cterm、sharerootfs。
- OMV 原生插件管理器（PluginMgmt RPC）**没有**上传 deb 安装功能，无现成插件补齐此洞 → 本插件的存在意义。
- NAS 本机无 git/gh，**不要**在 NAS 上开发；本仓库全部在本地工作区完成。
- 开发调试若需 OMV 环境：建议 Docker 起 OMV（如 `openmediavault/omv` 类镜像或 omv-extras installScript 装进 Debian 13 容器/VM），不要把未验证 deb 直接装生产 NAS。

## 1. 功能规格（参考 OpenWrt，全部标准功能）

单页 tabsPage，四个标签：

| 标签 | 功能 | 对应 OpenWrt 行为 |
|---|---|---|
| Installed | 已安装包表格（名称/版本/架构/大小/描述），行按钮 Remove、标记；顶部 Filter 防抖搜索 | `opkg list-installed` |
| Available | 仓库内可搜包表格，行按钮 Install；支持按名称/描述过滤 | `opkg list` |
| Updates | 可升级包（旧 » 新版本对比），行按钮 Upgrade；顶部 Update lists…（apt update，后台任务） | `opkg list-upgradable` |
| Local deb | 上传/指定本地 .deb 安装（见 §3 上传方案） | Upload Package… → `/tmp/upload.ipk` → install |

借鉴的交互细节（OpenWrt 源码已验证的做法）：
- 安装/卸载前弹确认框，显示 apt 模拟输出（`apt-get -s install <pkg>` 的 NEW/REMOVED 列表），**若将移除 `openmediavault*` 则红色警告并需二次确认**（NAS 红线）。
- 操作走 OMV 后台任务（RPC `task` 参数），前端 taskDialog 轮询进度，完成后展示原始 stdout/stderr/退出码。
- 全局文件锁（`flock /var/lock/pluginmgr.lock`），并发操作返回 "Failed to acquire lock"。
- 页面顶部常驻警告横幅："包操作可能破坏系统，请确认已备份配置"。
- 只读权限用户（非 admin 角色）隐藏全部操作按钮（permissions.role: admin）。

## 2. OMV 插件架构（官方文档 + 源码依据）

插件 = Debian 包，命名 `openmediavault-<name>`，铺文件到固定路径，各层自动发现：

```
openmediavault-pluginmgr/
├── debian/
│   ├── control          # Package: openmediavault-pluginmgr; Depends: openmediavault (>=8.0)
│   ├── changelog
│   ├── rules            # 极简，dh $@（纯声明式插件无需编译产物）
│   ├── compat / source/format
│   └── postinst         # omv-mkworkbench all; 无需重启 engined（见 §4）
├── usr/share/openmediavault/
│   ├── engined/rpc/pluginmgr.inc        # PHP RPC 服务（核心后端）
│   └── workbench/
│       ├── component.d/*.yaml           # 4 个页面组件（tabsPage + datatablePage×3 + formPage）
│       ├── navigation.d/system.pluginmgr.yaml
│       └── route.d/*.yaml
└── usr/share/doc/openmediavault-pluginmgr/copyright
```

关键参考文件（本地已装插件即活模板，`dpkg -L openmediavault-apt` 可列全）：
- `/usr/share/openmediavault/engined/rpc/aptmgmt.inc` —— PHP RPC 类模板：
  `class PluginMgr extends \OMV\Rpc\ServiceAbstract`，`getName()` 返回 "PluginMgr"，
  `initialize()` 里 `registerMethod(...)` 注册全部方法。
- `/usr/share/openmediavault/workbench/component.d/omv-system-updatemgmt-settings-sources-*.yaml` —— 组件/路由/导航 YAML 实例。
- 官方核心 RPC 模板（GitHub）：
  `deb/openmediavault/usr/share/openmediavault/engined/rpc/{apt.inc,pluginmgmt.inc,certificatemgmt.inc,exec.inc,folderbrowser.inc}`

### RPC 方法设计（PluginMgr 服务）

| 方法 | 实现 | 备注 |
|---|---|---|
| `getInstalledList(params)` | `dpkg-query -W -f='...'` 解析，支持 start/limit/sortfield/sortdir + 搜索 | 仿 Config.get*List 分页签名 |
| `getAvailableList(params)` | `apt-cache search --names-only` / `dumpavail` 解析 | 量大，建议 dumpavail 缓存 |
| `getUpgradableList()` | `apt list --upgradable` 或 apt-cache policy 对比 | |
| `install(path_or_pkg)` | 后台任务：`apt-get install -y <pkg>` 或 `apt-get install -y /path/x.deb` | task=1 |
| `remove(uuid/name)` | 后台任务：`apt-get remove -y` | 先查 openmediavault* 保护 |
| `simulate(pkg, action)` | `apt-get -s install/remove` 返回 NEW/REMOVED 数组 | 供确认弹窗 |
| `updateLists()` | 后台任务：`apt-get update` | task=1 |
| `getDebInfo(path)` | `dpkg-deb -f` 读元数据 | 安装前展示 |

后台任务用 OMV 标准机制：`\OMV\Engine\Rpc\...` 里返回 `["id" => $taskid]`，参考 `exec.inc` / `filebrowser.inc` 里 `execute` 类任务的写法（OpenWrt 是阻塞式弹窗，OMV 有原生 task 轮询，直接用 OMV 的）。

## 3. 本地 .deb 上传方案（重要设计决策）

OMV Workbench 是**纯声明式**（官方文档明确"no JavaScript or TypeScript knowledge needed"），
`fileInput` 表单字段类型只读文本内容（见 form-field-config.type.ts 注释：accept/trim/rows，用于证书/密钥类文本），**不能传二进制 .deb**。

两条可行路线（推荐 A）：

- **A. 共享文件夹选择（OMV 原生风格）**：Local deb 页用 `folderBrowser` 字段让用户浏览 NAS 上的 .deb 路径（用户先把 deb 放进共享文件夹，FileBrowser Quantum 也可当"上传站"用），RPC 校验扩展名+`dpkg-deb -f` 后 `apt-get install -y <path>`。零前端 hack，纯 YAML 即可实现。
- B. multipart 上传端点：OMV 核心无通用 upload RPC（证书上传走的是 fileInput 文本），要做真"浏览器选文件→上传"需研究 nginx 端点 + PHP 收文件，复杂且偏离声明式哲学。v2 再说。

安全校验（A 方案必做）：路径必须 realpath 后位于已注册共享文件夹挂载点内；拒绝含 `..`；文件属主/可读检查；大小上限提示。

## 4. 构建与生效机制

- 前端清单合并：`omv-mkworkbench all`（postinst 里调用；官方文档 development/plugins.html "Build configuration"）。
- PHP RPC 新装后需 omv-engined 重新加载：postinst 里 `monit restart omv-engined` 或依赖 dpkg 触发；调试期改 PHP 后手动重启即可。
- 本地快速调试（免打包）：文件直接拷到对应路径 + `omv-mkworkbench all` + 浏览器强刷。
- 打包：`dpkg-buildpackage -us -uc -b` 产出 `openmediavault-pluginmgr_x.y_all.deb`（架构无关，用 all）。

## 5. GitHub 仓库与 CI（云端编译发布，用户硬性要求）

- 仓库名建议 `openmediavault-pluginmgr`，公开，MIT/GPLv3（OMV 生态惯例 GPLv3，**建议 GPLv3**）。
- `.github/workflows/build.yml`：
  - 触发：push tag `v*` → 构建 + 创建 Release 附 deb；push main → 仅构建传 artifact（验证用）。
  - job：`runs-on: ubuntu-latest`（或 `container: debian:trixie` 更贴近目标基座），
    `apt-get install -y dpkg-dev build-essential debhelper`，
    `dpkg-buildpackage -us -uc -b`，
    `actions/upload-artifact` + `softprops/action-gh-release`（`GITHUB_TOKEN` 即可发 Release，无需 PAT）。
  - 可选加 lint job：`php -l` 所有 .inc、`yamllint` 所有 workbench yaml。
- 版本策略：`debian/changelog` 顶部版本 = tag 版本（CI 里可校验一致）。

## 6. 安装方式（插件装好后）

```bash
# 方式一：CLI（用户偏好）
apt install ./openmediavault-pluginmgr_1.0.0_all.deb
# 方式二：WebUI 走本插件自身（自举）不可行，需先有第一次 CLI 安装
# 卸载
apt purge openmediavault-pluginmgr
```

## 7. 参考资料链接汇总

官方文档：
- Plugin Development（workbench YAML 全规范）: https://docs.openmediavault.org/en/stable/development/plugins.html
- How does it work（RPC→config.xml→dirty→salt 链路）: https://docs.openmediavault.org/en/stable/development/howitworks.html
- Software & Update Management（插件安装机制）: https://docs.openmediavault.org/en/stable/various/apt.html
- Internal Tools（omv-salt/omv-confdbadm/omv-rpc）: https://docs.openmediavault.org/en/stable/development/tools/index.html

OMV 源码（活模板）：
- 主仓库: https://github.com/openmediavault/openmediavault （`deb/` 下每个插件目录都是模板；重点看 `deb/openmediavault-lvm2/`、`deb/openmediavault-apt/`）
- 表单字段类型定义: `deb/openmediavault/workbench/src/app/core/components/intuition/models/form-field-config.type.ts`
- 表单页类型定义: 同目录 `form-page-config.type.ts`（request.task、taskDialog、confirmationDialogConfig 均在此）
- 现成 workbench 清单: `deb/openmediavault/usr/share/openmediavault/workbench/`
- DeepWiki 插件创建指南: https://deepwiki.com/openmediavault/openmediavault/6.1-creating-plugins
- 旧版模板仓库（结构参考）: https://github.com/DavidB-OMV/OMV3-Plugin-Template

OpenWrt 参考源码（功能蓝本）:
- 仓库: https://github.com/openwrt/luci （master 路径 `applications/luci-app-package-manager/`，22.03 为 `applications/luci-app-opkg/`）
- 视图: `.../htdocs/luci-static/resources/view/opkg/packages.js`（1251 行，全部交互逻辑）
- 后端: `.../root/usr/libexec/opkg-call`（锁、命令白名单）、`.../root/usr/share/rpcd/acl.d/*.json`（ACL 白名单思想）
- 可借鉴清单见 §1；其上传实现（cgi-io → /tmp → install → 清理）对应本文 §3 A 方案

## 8. 红线提醒（NAS 生产机）

1. 本插件装在生产 NAS Satellite 上之前，先在 VM/容器 OMV 里完整测试。
2. 插件的 install/remove RPC 必须内置 openmediavault* 保护（模拟输出含 REMOVED openmediavault 即拒绝，除非 force 参数 + 双重确认）。
3. 永远不要走"上传任意 shell 脚本执行"类扩展。
4. 首次安装本 deb 用 CLI，装完 WebUI 出现 System → Plugin Manager 入口即成功。
