# creo-jlink-toolkit

把 Creo Parametric 变成 **HTTP 可调用的自动化通道** —— 不开 Creo 界面，用脚本批量干活：导 STEP、抓 BOM、读写参数、出图、查干涉。

两条通道：

- **jlink/** —— J-Link 异步通道包装成常驻 HTTP 服务（`:8080`），任何会发 HTTP 的语言（Python / Node / C# / curl）都能调
- **c-toolkit/** —— 同步 Pro/TOOLKIT 引擎，跑在 Creo 进程内，干异步通道干不了的"重活"

> 本机在 Creo 9.0.8 x64 实测；其他 Creo 9.x 大概率可用，`otk.jar` 与 Creo 版本需匹配（见自备清单）。

## 两条通道怎么选

- **要交互**（在线预览、参数面板、PDM 实时对接）→ jlink/，常驻会话，单次操作秒级
- **要批量**（几十上百个模型一次导出/检查）→ c-toolkit/，写作业文件跑无头批处理

同一时刻只用一条：服务连着会话时，不要再起同步会话。

## 功能清单

<table>
<tr>
<th width="50%">jlink/ —— 异步 HTTP 服务（:8080）</th>
<th width="50%">c-toolkit/ —— 同步作业引擎（job 文件驱动）</th>
</tr>
<tr>
<td valign="top">

**连接**
- `POST /connect` 连接已运行的 Creo 会话
- `POST /start` 自动拉起无头 Creo（nmsd + xtop）
- `GET /health`、`GET /session` 健康检查 / 会话信息

**模型**
- 列出会话模型、当前模型详情
- 打开 / 保存 / 擦除 / 显示 / 重命名

**参数**
- 读全部参数、读单个、写参数（支持类型）
- 材料属性 + 材料库清单

**结构**
- `GET /bom` 一级装配结构
- `GET /bomtree` 递归 BOM 树（可限深度、取公用名称）

**输出**
- `POST /export` STEP / IGES（可挂 .dep_step 导出配置）
- `POST /image` 当前视图 JPEG（可设分辨率 / DPI）

**其他**
- `POST /dir` 切换工作目录

</td>
<td valign="top">

**导出**
- `EXPORT` 3D 导出：STEP（AP203/214/242）、IGES，可挂 profile
- `EXPORT2D` 工程图 → DWG / DXF

**检查**
- `INTERF` 干涉检查：零件 / 子装配 / 整装，可只出干涉对、逐对算体积

**装配**
- `FLATTEN` 装配打平：导入 STEP → 下层零件按总装坐标装到顶层 → 再导出
- `FLATASM` 打平指定子装配一层，保留总装其余结构

**参数**
- `PARAMSET` / `PARAMDEL` 精确类型控制（异步通道删不了、改不了类型的）
- `PARAMD` 导出「工具→参数」全表（含来源/指定/访问列）

**PDM 字段**
- `SETCOMMON` / `READCOMMON` 公用名称读写
- `RENAME` 模型改名（版本链跟随）

**命令与配置**
- `QUICKDRAW` 触发 PiCreo 快速出图，等 PDF 落地
- `MACRO` 执行任意已注册命令宏
- `GETOPT` / `SETOPT` 配置选项读写

**特殊**
- `COUNT` / `DUMPNEW` / `DUMPPID` 进程内读文件——**能绕过本机透明加密（DLP）**

</td>
</tr>
</table>

## 能干什么（工作实例）

| 场景 | 做法 |
|---|---|
| PDM/PLM 对接 | `/bomtree` 拉装配树 → 回写公用名称（`SETCOMMON`）、改参数 |
| 批量导 AP242 | `scripts/export_ap242.ps1 -Model A,B,C` 一次三个模型，桌面收 `.stp` |
| 给下游出打平 STEP | `FLATTEN` 作业：导入 STEP 装配 → 打平 → 输出（3D 打印 / 轻量化预览） |
| 模型质检 | `INTERF` 干涉检查 + `PARAMD` 导出参数表核对 |
| 在线预览 | `/models` 列模型 → `/image` 出 JPEG 给网页用 |
| 参数类型修正 | `PARAMDEL` + `PARAMSET` 把数值参数改成字符型 |
| 图纸批量出 DWG | `EXPORT2D` 逐张图纸转 DWG/DXF |
| 有透明加密（DLP）的机器 | 同步通道进程内读文件，校验不受加密干扰 |

## 发布包里有什么

从 [Releases](https://github.com/dhdbbd/creo-jlink-toolkit/releases) 下载 `creo-jlink-toolkit-v1.4.0.zip`：

| 内容 | 说明 |
|---|---|
| `jlink/` | Java 服务**源码 + 编好的 class**、`pfcasyncmt.dll`、启动脚本、拉起无头会话的 `spawn_session.ps1` |
| `c-toolkit/` | **编好的 64 位 DLL**（`sync_export.dll` 主引擎、`flatasm.dll` 装配打平）+ C 源码 |
| `profiles/` | 4 份 `.dep_step` 导出配置（AP242 默认 / 仅零件 / 含面组隐藏 / factory） |
| `scripts/` | 批量导出、无头会话拉起、日志清理（PowerShell） |
| `work/` `work-asmflat/` `samples/` | Pro/TOOLKIT 会话目录、作业文件模板 |
| `docs/INSTALL.md` | **12 节安装手册，每节带验证**——这是给执行安装的 AI 看的操作脚本 |

## 需要自备（不在包里）

| 缺什么 | 从哪来 | 放哪 |
|---|---|---|
| JRE 11+ | [Adoptium Temurin 11](https://adoptium.net/temurin/releases/?version=11) | 解压到 `jlink/runtime/` |
| `pfcasync.jar` + `otk.jar` | 本机 Creo 安装目录 `<Creo>\Common Files\text\java\` | 复制到 `jlink/lib/` |
| Creo 本体 + 授权 | 正版安装、license 文件 | 路径填进 `configure.ps1` 参数 |

## 怎么用：直接丢给 AI

1. 下载 Release 里的 zip，解压到**非中文路径**（如 `D:\creo-jlink-toolkit`）
2. 把整个文件夹丢给你的 AI，说一句：

   > 按 docs/INSTALL.md 安装，装完跑通四项自检

3. 手册 12 节每节末尾有验证，验证不过 AI 会停下来，不会带病往下走
4. 装完 AI 回传四段输出：`/health`、`/connect`、`/models`、日志里的 `SYNC_EXPORT_DONE` —— **四项都有，才算复刻成功**

## 重要约束

- **两条通道不同时用**：异步服务连着会话时起同步会话，两边抢同一个模型
- 失败的 `/connect` **不要重试**——会话被污染只能杀掉重建（INSTALL.md 第 10 节有失败对照表）
- 无头会话拉起后**等 150 秒再连**，早连会让会话进入永久 Busy
- 本工具不改你的模型文件；重命名/保存只对明确指定的副本做

## License

仓库代码（Java/C 源码、脚本、配置）按你的项目需要自行处理；**PTC 的 jar / DLL / JRE 不在包里也不在本仓库，受 PTC 商业许可约束，请勿再分发**。
