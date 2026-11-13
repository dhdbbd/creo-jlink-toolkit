# creo-jlink-toolkit

把 **Creo Parametric 9** 变成一条 HTTP 可调用的自动化通道 —— 不开 Creo 界面，用脚本/程序批量驱动它干活：导 STEP、抓 BOM、读写参数、出图、检查干涉。

适合的场景：PDM/PLM 系统对接、模型批量质检、图纸/中间格式批量产出、把 Creo 当成 CAD 后端嵌进你自己的工具链。

> Windows x64 · Creo Parametric 9.0.8 实测 · 需要 Creo 有效授权
> **不含任何 PTC 专有组件**（jar / JRE），使用前从你本机的 Creo 安装目录自备，见 [docs/INSTALL.md](docs/INSTALL.md)

---

## 它解决什么问题

Creo 官方自动化接口有三条路，各有痛点：

| 路线 | 痛点 |
|---|---|
| Pro/TOOLKIT（C） | 要写 C、配编译器、懂回调；改一行重编一遍 |
| J-Link（Java） | 异步连接没法直接拉起内核，进程管理是坑 |
| VB/COM | 只能驱动 GUI 界面，无头批处理做不到 |

本项目把两条官方接口组合成**两个互补的引擎**，坑都趟过了，开箱即用：

- **jlink/** —— J-Link 异步通道包装成常驻 HTTP 服务。你的任何语言（Python / Node / C# / curl）都能调
- **c-toolkit/** —— 同步 Pro/TOOLKIT 引擎，跑在 Creo 进程内，干异步通道干不了的"重活"

**两条通道同一时刻只用一条**：服务连着会话时，不要再起同步会话。

## 两条通道对比

| | jlink/（异步 HTTP 服务） | c-toolkit/（同步引擎） |
|---|---|---|
| **原理** | Java 服务 `:8080`，经 J-Link 异步连接操控独立 Creo 进程 | DLL 经 protk.dat 加载进 Creo 进程，读 job 文件逐条执行 |
| **调用方式** | 任何能发 HTTP 的语言，实时请求-响应 | 写 `export_job.txt` 作业文件 → 拉起无头 Creo 跑完退出 |
| **适合** | 交互式应用：在线预览、参数面板、实时查询 | 批处理：几十上百个模型一次性导出/检查 |
| **速度** | 常驻会话，单次操作秒级 | 每次冷启动 ~2.5 分钟，之后逐件很快 |
| **能力边界** | 参数读写、BOM、打开/保存/重命名、导出、截图 | 进程内精确操作：干涉检查、装配打平、DWG/DXF、读加密文件 |

## 功能清单

### jlink/ —— HTTP API（`:8080`）

| 类别 | 端点 | 说明 |
|---|---|---|
| 连接 | `POST /connect` | 连接已运行的 Creo 会话 |
| | `POST /start` | 自动拉起新的无头 Creo 进程（nmsd + xtop） |
| | `GET /health` `GET /session` | 健康检查、会话信息 |
| 模型 | `GET /models` `GET /model/current` | 列出会话中所有模型、当前模型详情 |
| | `POST /model/open` `/model/save` `/model/erase` `/model/display` | 打开/保存/擦除/显示 |
| | `POST /model/rename` | 重命名（只动明确指定的副本） |
| 参数 | `GET /params` `GET /param?name=X` | 读全部参数 / 读单个 |
| | `POST /param` | 写参数（支持类型） |
| | `GET /material` | 材料属性 + 材料库清单 |
| 结构 | `GET /bom` | 一级装配结构（组件清单） |
| | `GET /bomtree` | 递归 BOM 树（可指定深度、取公用名称） |
| 输出 | `POST /export` | STEP / IGES，可挂 `.dep_step` 导出配置 |
| | `POST /image` | 当前视图 JPEG（可设分辨率/DPI） |
| 其他 | `POST /dir` | 切换工作目录 |

**示例**：起服务、连上、读个参数、导个 STEP——

```powershell
wscript.exe .\jlink\start_service_hidden.vbs
curl.exe -X POST http://localhost:8080/start          # 拉起无头 Creo
curl.exe http://localhost:8080/models                 # 列出模型
curl.exe -X POST http://localhost:8080/export -d "{\"model\":\"MY_ASM\",\"type\":\"step\"}"
```

### c-toolkit/ —— 同步作业引擎

作业文件驱动（`work/export_job.txt`，一行一条），由 `scripts/spawn_sync_export.ps1` 拉起无头 Creo 执行，日志尾部 `SYNC_EXPORT_DONE ok=<n> fail=<m>` 判定成败。

| 作业 | 用途 |
|---|---|
| `EXPORT` | 3D 导出（STEP AP203/214/242、IGES…），可挂导出配置 |
| `EXPORT2D` | 工程图 → DWG / DXF |
| `INTERF` | **干涉检查**（整体/子装配/零件级，可只出干涉对、可逐对算体积） |
| `FLATTEN` | **装配打平**：导入 STEP 装配 → 下层零件按总装坐标直接装到顶层 → 输出打平后的 STEP（给轻量化/3D 打印/下游系统用） |
| `FLATASM` | 打平指定子装配一层（保留总装其余结构） |
| `PARAMSET` / `PARAMDEL` | 参数精确类型控制（异步通道删不了、改不了类型的，这里能做） |
| `PARAMD` | 导出「工具→参数」全表（含来源/指定/访问列） |
| `SETCOMMON` / `READCOMMON` | 公用名称（Common Name）写/读 —— PDM 对接字段 |
| `RENAME` | 模型改名（版本链一起改） |
| `QUICKDRAW` | 触发 PiCreo「快速出图」宏，等 PDF 产物落地 |
| `MACRO` | 执行任意已注册命令宏 |
| `GETOPT` / `SETOPT` | 读/写 Creo 配置选项 |
| `COUNT` / `DUMPNEW` / `DUMPPID` | 进程内读文件（**能绕过本机透明加密**，DLP 环境下同步通道还能正常工作） |

**示例**：批量把装配导成 AP242 STEP——

```powershell
.\scripts\export_ap242.ps1 -Model ASM001,ASM002,ASM003 -ModelDir D:\models
# 作业文件自动生成 → 无头 Creo 逐个导出 → 桌面上收 .stp
```

### profiles/ —— 导出配置

预置 4 份 `.dep_step`（Creo 导出 profile）：

- `ap242.dep_step` —— AP242 默认配置
- `ap242_protoonly.dep_step` —— AP242，只导零件不导装配结构
- `ap242_quilts_hidden.dep_step` —— AP242，含面组和隐藏图元
- `factory.dep_step` —— 工厂格式

用 `scripts/make_profile.ps1` 从这些底版派生自己的配置（协议版本/面组/隐藏图元可调）。

## 仓库结构

```
creo-jlink-toolkit/
├── jlink/              异步 HTTP 服务
│   ├── run_server.bat            启动脚本（改 CREO_HOME / license 后运行）
│   ├── start_service_hidden.vbs  后台静默启动
│   ├── spawn_session.ps1         供服务拉起无头 Creo（nmsd + xtop）
│   ├── lib/                      ← 你要放的 pfcasync.jar + otk.jar（自备）
│   ├── runtime/                  ← 你要放的 JRE 11+（自备，可选）
│   ├── native/                   pfcasyncmt.dll（J-Link 原生桥）
│   └── service/src/              Java 源码（HTTP 服务 + Creo 连接管理）
├── c-toolkit/          同步作业引擎
│   ├── sync_export.dll           主引擎（编好的 64 位 DLL，Creo 进程加载）
│   ├── flatasm.dll               装配打平专用引擎
│   └── src/                      C 源码
├── work/               同步引擎会话目录（protk.dat / export_job.txt / 日志）
├── work-asmflat/       FLATASM 专用会话目录
├── profiles/           .dep_step 导出配置
├── scripts/            批量导出、拉起会话、日志清理
├── samples/            作业文件模板
└── docs/INSTALL.md     完整安装手册（12 节，每节带验证）
```

## 快速开始（概览）

完整步骤（含逐节验证、失败对照表）见 **[docs/INSTALL.md](docs/INSTALL.md)**，这里只列主线：

```powershell
# 0) 自备两样东西（详见 INSTALL.md 第 0-2 节）
#    - JRE 11+ → 放 jlink/runtime/ 或装系统级
#    - 你本机 Creo 的 pfcasync.jar + otk.jar → 放 jlink/lib/

# 1) 一次性配置路径（把 Creo 位置写进所有脚本）
powershell -ExecutionPolicy Bypass -File .\configure.ps1 -CreoHome 'C:\Program Files\PTC\Creo 9.0.8.0'

# 2) 起异步服务
wscript.exe .\jlink\start_service_hidden.vbs
curl.exe http://localhost:8080/health

# 3) 或者跑同步批处理
powershell -ExecutionPolicy Bypass -File .\scripts\export_ap242.ps1 -Model YOUR_ASM
```

## 重要约束

- **两条通道不同时用**：异步服务连着会话时起同步会话，两边会抢同一个模型
- 失败的 `/connect` **不要重试**——会话被污染只能杀掉重建（详见 INSTALL.md 第 10 节失败对照表）
- 无头会话拉起后**等 150 秒再连**，早连会让会话进入永久 Busy
- 本工具不改你的模型文件；重命名/保存只对明确指定的副本做

## License

仓库代码（`jlink/service/src`、`c-toolkit/src`、脚本、配置）按你的项目需要自行处理；**PTC 的 jar / DLL / JRE 不在本仓库内，受 PTC 商业许可约束，请勿再分发**。
