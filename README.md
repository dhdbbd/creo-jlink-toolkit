# creo-jlink-toolkit

![Creo](https://img.shields.io/badge/Creo-9.0.8-blue)
![J-Link](https://img.shields.io/badge/J--Link-HTTP%E6%9C%8D%E5%8A%A1-007ec6)
![Pro/TOOLKIT](https://img.shields.io/badge/Pro/TOOLKIT-C-3e8f3e)
![formats](https://img.shields.io/badge/%E6%8E%A5%E5%8F%A3-STEP%C2%B7IGES%C2%B7DWG-8a2be2)
![license](https://img.shields.io/badge/license-MIT-red)

Creo 有两套官方自动化接口：**J-Link**（Java）和 **Pro/TOOLKIT**（C）。两套都是本地 API——只能在装了 Creo 的机器上调，而且都得写代码。本项目把这两个接口做成两个产物：**一个 HTTP 服务，一个 job 文件**。

> 一个常驻、一个跑批。会发 HTTP 就能驱动 Creo，批量活写成文本文件丢给无头 Creo 跑完就走。

## 这个项目解决什么问题

Creo 的两套接口都只解决了「能调」，不管「好用」：J-Link 要配 Java 环境建工程，Pro/TOOLKIT 要配 C 编译器写回调，而且两头都是 raw SDK。本项目各包了一层：

- **J-Link 包成 HTTP 常驻服务**：任何语言、任何工具（包括 AI Agent）都能直接调 Creo——不用装 JDK、不用建 Java 工程
- **Pro/TOOLKIT 包成 job 文件驱动的离线作业**：写几行文本作业，无头 Creo 启动后自动逐条执行，批量活一次跑完
- **全程不碰 Creo 界面、不学 PTC 的 SDK**——调 HTTP 或写文本文件就行

## J-Link：Java 侧的常驻服务

J-Link 是 PTC 的 Java 接口，把 Creo 内部对象（Session、Model、Parameter、Assembly…）包成 Java 类，能自己拉起一个可靠的 Creo 会话。本项目把它包成 HTTP 服务，常驻 `:8080`，每种能力对应一个端点，当前 21 个：

- **通信**：连接已开的 Creo（`/connect`）或让服务自己拉起无头会话（`/start`）、断开、健康检查
- **查询**：列出会话里的所有模型、当前模型详情、读/写参数、材料、BOM、递归 BOM 树
- **输出**：STEP / IGES 导出（可挂导出配置）、当前视图 JPEG 截图

## C Pro/TOOLKIT：C 侧的离线作业

Pro/TOOLKIT 是 PTC 的 C 接口，能力面最全（干涉检查、装配打平、工程图、进程内读文件），但要写 C、配编译器，冷启动一次 10-15 秒。本项目把常用能力**编成 DLL（已编好，不需要编译器）**，做成 job 文件驱动：一行一个作业，无头 Creo 启动后依次执行，全部跑完一起交结果。

- **适合**：批量、定时、机房里跑的活——几百个模型批量导 STEP、全装配干涉检查、批量出 DWG
- **配合**：一次拉起、全部跑完再退；多类作业可以写进同一个 job 文件
- **独占**：有的能力只有进程内才做得了——参数删除/精确改类型、装配打平、读透明加密（DLP）下的文件

## 两者的区别

| 方面 | J-Link（HTTP :8080） | C Pro/TOOLKIT（job 驱动） |
|---|---|---|
| 调用方式 | 交互调用，HTTP 实时请求 | 离线作业，job 文件一行一条 |
| 运行形态 | 常驻服务，随时待命 | 一次性进程，跑完退出 |
| 响应速度 | 会话已在，秒级返回 | 冷启动 10-15 秒，之后逐件很快 |
| 交互 | 实时，随时查 | 跑完看日志和产物 |
| 精细控制 | 参数删不掉、改不了类型 | 能删参数、精确控制类型 |
| 加密环境 | 读不了 DLP 加密文件 | 进程内读，绕过透明加密 |
| 典型产出 | 参数、BOM、STEP、JPEG | 工程图 DWG/DXF、打平 STEP、干涉报告 |

一句话：**能用 J-Link 就用 J-Link**，C 侧留给 J-Link 干不了的活。两条通道同一时刻只能用一条——服务连着会话时不要再起同步作业。

## 能力对照

常见需求一览，✅ 表示该侧原生支持，— 表示该侧不支持。

| 能力 | J-Link 侧（HTTP） | C 同步侧（job） |
|---|---|---|
| STEP / IGES 导出（AP203/214/242） | ✅ | ✅ |
| 模型查询（会话列表 / 当前模型） | ✅ | — |
| 参数读 / 写 | ✅ | ✅ |
| BOM / 递归 BOM 树 | ✅ | — |
| JPEG 截图 | ✅ | — |
| 工程图 DWG / DXF | — | ✅ |
| 干涉检查 + 逐对体积 | — | ✅ |
| 装配打平（FLATTEN / FLATASM） | — | ✅ |
| 公用名称读写（Common Name） | — | ✅ |
| 参数删除 / 类型变更（PARAMDEL/PARAMSET） | — | ✅ |
| 参数表导出（含来源/指定/访问列） | — | ✅ |
| 模型改名（版本链跟随） | ✅ | ✅ |
| 宏命令触发（QUICKDRAW 快速出图） | — | ✅ |
| 配置选项读写（GETOPT/SETOPT） | — | ✅ |
| 透明加密（DLP）环境读文件 | — | ✅ |

## 架构

两条通道各自管着自己的 Creo 会话，同一时刻只用一条，避免两边抢同一个模型。

```text
          你的脚本 / AI Agent
 (curl · Python · Node · 会发 HTTP 就行)
                │
                │ HTTP :8080
                ▼
      ┌──────────────────┐          job 文件 + 拉起脚本
      │   jlink 常驻服务   │          │
      │   (Java + J-Link) │          ▼
      └────────┬─────────┘   ┌──────────────────────┐
               │             │  c-toolkit 同步引擎    │
               │ J-Link      │  sync_export.dll      │
               │ 异步连接     │  (Creo 进程内加载)      │
               ▼             └──────────┬────────────┘
      ┌────────────────────────────────────────────┐
      │          Creo Parametric 9 (x64)            │
      │          无头会话 · nmsd + xtop              │
      └────────────────────────────────────────────┘
```

## 连上 Creo 之后能做什么

`/connect`（或 `/start`）之后，整个模型会话归你调了。

比如要做 PDM 对接：`/models` 看会话里有什么 → `/bomtree` 拉整棵装配树 → `/param` 回写属性 → C 侧 `SETCOMMON` 补公用名称，一条链路全是脚本。

比如要出图交付：`/export` 挂上 AP242 配置导 STEP，不用开界面；要工程图就走 C 侧 `EXPORT2D`，DWG/DXF 一次一批。

再比如批量质检：`INTERF` 作业全装配干涉检查，逐对给体积；配合 `PARAMD` 导出参数表核对。DLP 加密机器上照样能做——同步引擎是进程内读文件，绕过透明加密。

## 安装

不用自己动手。把压缩包丢给 AI，让它按 `docs/INSTALL.md` 装，装完跑通四项自检就行——见下面「怎么让 AI 装」。

## 打包有什么

| 内容 | 说明 | 状态 |
|---|---|---|
| `jlink/service/` | Java 服务源码 + 编好的 class | ✅ 解压即可跑 |
| `jlink/spawn_session.ps1` | 拉起无头 Creo 会话（nmsd + xtop） | ✅ 已带 |
| `jlink/native/pfcasyncmt.dll` | J-Link 原生桥 | ✅ 已带（版权归 PTC） |
| `jlink/runtime/` | Temurin JRE 11 | ❌ 自备（Adoptium 官网） |
| `jlink/lib/pfcasync.jar + otk.jar` | PTC 的 Java 库 | ❌ 自备（本机 Creo 目录） |
| `c-toolkit/*.dll + src/` | 同步引擎 64 位 DLL + C 源码 | ✅ 不用编译器 |
| `profiles/` | AP242 等 4 份 `.dep_step` 导出配置 | ✅ |
| `scripts/` | 批量导出、无头拉起、日志清理 | ✅ |
| `work/` `work-asmflat/` `samples/` | 会话目录 + 作业文件模板 | ✅ |
| `docs/INSTALL.md` | 12 节安装手册，每节带验证（给 AI 的执行脚本） | ✅ |

> 想让没装 PTC 组件的机器跑起来：`pfcasync.jar` / `otk.jar` 必须从装了 Creo 的机器上拷（`<Creo>\Common Files\text\java\`），且版本要和你的 Creo 一致；JRE 从 Adoptium 官网下 11 即可。`pfcasyncmt.dll` 已随包，但版权归 PTC，请遵守其许可。

## 运行环境

- Windows 10 / 11 x64；Creo Parametric 9（9.0.8 实测，其他 9.x 理论可用，`otk.jar` 需版本匹配）
- J-Link 侧：Java 11+（包里不带）+ `pfcasync.jar` / `otk.jar`（从 Creo 装机拷，版本必须一致）
- C 侧：**不需要编译器**——DLL 已编好（MSVC 2015+ 编出，兼容 Creo 9）；只有要改 C 源码时才需要 VS2015+ 和 Pro/TOOLKIT 头文件

## 怎么让 AI 装

新用户可以不碰终端。把 Release 里的 zip 解压后，整个文件夹丢给会执行任务的 AI（Claude Code、Codex CLI 等），说一句：

```text
解压 creo-jlink-toolkit-v1.4.0.zip 到 D:\creo-jlink-toolkit。
按 docs/INSTALL.md 一节一节装，Creo 装在 <你的 Creo 路径>，授权文件在 <license 路径>。
每节末尾的验证过了再往下走，装完跑通四项自检：
/health、/connect、/models、日志里的 SYNC_EXPORT_DONE。
```

AI 会自己读手册：跑 `configure.ps1` 写路径 → 注册 DLL 和服务 → 起服务连会话 → 四项自检 → 把四段输出贴给你。**四项全过才算复刻成功**；有不过的它会停下来修，不带病往下走。

## 使用

你不用学 API、不用写代码——把需求告诉 AI，它拼对应的 HTTP 调用或 job 作业；你自己 curl 也行。

常见动作：

- 「把这个模型导成 AP242」→ `/export`（或 `EXPORT` 作业）
- 「列一下 BOM 树，把这个参数改成 45 钢」→ `/bomtree` + `/param`
- 「查一下这两个件干不干涉」→ `INTERF` 作业
- 「这一百个装配全部打平导出」→ `FLATTEN` 作业批量跑

AI 负责拼调用、跑作业、把结果给你；你要的只是说清需求。

## License

MIT（仅限本仓库自有代码，见 [LICENSE](LICENSE)）。PTC 的 jar / DLL / JRE 不在本仓库分发，使用须遵守 PTC 许可协议。
