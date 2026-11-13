# creo-jlink-toolkit

Creo Parametric 9 的自动化工具集：把 Creo 变成一条 HTTP 可调用的自动化通道。

两条通道：

- `jlink/` — Java 异步服务，HTTP `:8080`。负责参数读写、BOM、打开/保存/重命名、STEP/IGES、JPEG。
- `c-toolkit/` — C Pro/TOOLKIT 同步作业引擎。负责干涉检查、装配打平、DWG/DXF、进程内读回校验。

两条通道同一时刻只用一条：服务连着会话时，不要再起同步会话。

> **本仓库不含 PTC 专有运行时。** 使用前需自备：
> - JRE 11+（如 Adoptium Temurin 11），放到 `jlink/runtime/`，或修改 `run_server.bat` 指向系统 Java；
> - `pfcasync.jar`、`otk.jar`（来自你本机的 Creo 安装目录 `Common Files\text\java\`），放到 `jlink/lib/`。
>
> 需要 Creo 有效授权（PTC_D_LICENSE_FILE）。请遵守 PTC 许可协议，本仓库不分发任何 PTC 组件。

---

## 0. 确认环境

```powershell
# Creo 装在哪
Get-ChildItem 'C:\Program Files\PTC' -Directory -Filter 'Creo*' -ErrorAction SilentlyContinue |
  Select-Object -ExpandProperty FullName

# 确认是完整 loadpoint
Test-Path '<Creo 目录>\Common Files\x86e_win64\obj\xtop.exe'
```

要求：

- Windows 10 / 11 x64
- Creo Parametric 9.0.8 x64，授权有效，能手动启动到界面
- **JRE 11 或更高**：J-Link 服务是 Java 进程。包里 `jlink\runtime\` 已带 Temurin 11，不用另装
- **不需要 C 编译器**：C 侧是编好的 64 位 DLL，由 Creo 进程加载

**验证**：`xtop.exe` 存在，`jlink\runtime\bin\java.exe -version` 输出 11.x。

---

## 1. 解压并配置路径

把 zip 解压到 `D:\creo-jlink-toolkit`（目录名随意，但别放在中文路径下）。

```text
D:\creo-jlink-toolkit\
├── README.md               # 本文
├── configure.ps1           # 一次性的路径配置脚本
├── jlink\
│   ├── run_server.bat
│   ├── start_service_hidden.vbs
│   ├── spawn_session.ps1
│   ├── lib\                # pfcasync.jar · otk.jar
│   ├── native\             # pfcasyncmt.dll
│   ├── runtime\            # Temurin 11
│   └── service\            # 源码 + 编译好的 class
├── c-toolkit\
│   ├── sync_export.dll
│   ├── flatasm.dll
│   └── src\
├── work\                   # 同步作业工作目录（protk.dat / config.pro / 日志）
├── work-asmflat\           # FLATASM 会话目录
├── profiles\               # .dep_step 导出配置
├── scripts\                # 拉起、连接、批处理
├── samples\
└── out\                    # 导出输出
```

运行配置脚本，把路径写进所有脚本和注册文件：

```powershell
cd D:\creo-jlink-toolkit
powershell -ExecutionPolicy Bypass -File .\configure.ps1 -CreoHome '<Creo 目录>'
```

授权有两种给法：

```powershell
# 方式一：显式传
.\configure.ps1 -CreoHome '<Creo 目录>' -LicenseFile 'C:\Licenses\PTC_D_LICENSE.dat'

# 方式二：先设环境变量，脚本自动读取
$env:PTC_D_LICENSE_FILE = '<授权文件或 license server>'
.\configure.ps1 -CreoHome '<Creo 目录>'
```

**验证**：

```powershell
Select-String -Path .\jlink\run_server.bat -Pattern 'CREO_HOME='
Select-String -Path .\work\protk.dat -Pattern 'EXEC_FILE'
```

不应再有 `__CREO_HOME__`、`__LICENSE_FILE__` 这样的占位符。

---

## 2. PTC 运行时（包里已带，建议核对）

`pfcasync.jar` 和 `otk.jar` 在 `jlink\lib\`，是从源机器复制来的。

`otk.jar` 属于 Creo 的可选组件 **API Toolkits → Object TOOLKIT Java**，默认安装没有。如果目标机器的
Creo 版本和源机器一致（9.0.8.0），直接用包里的；不一致就用目标机器自己的：

```powershell
$creo = '<Creo 目录>'
Get-ChildItem "$creo\Common Files\text\java" -Filter *.jar
```

- 有 `otk.jar`：复制过来覆盖 `jlink\lib\otk.jar`。
- 没有：说明这台机器没装 API 工具包。要么重跑同版本安装器勾上，要么继续用包里的。

`otk.jar` 和 Creo 的版本、datecode、架构（`x86e_win64`）必须匹配。混用会在启动时报
`UnsatisfiedLinkError` 或 `NoClassDefFoundError: com/ptc/otk`。

**验证**：`Get-ChildItem .\jlink\lib` 能看到两个 jar，大小都大于 0。

---

## 3. 注册同步 DLL

`configure.ps1` 已经按实际路径写好了两个注册文件：

- `work\protk.dat` → `c-toolkit\sync_export.dll`
- `work-asmflat\protk.dat` → `c-toolkit\flatasm.dll`

**验证**：

```powershell
Get-Content .\work\protk.dat
```

`EXEC_FILE` 指向的 DLL 存在；`STARTUP dll`、`ALLOW_STOP` 这些字段不要动。

---

## 4. 注册到 Creo 的 config.pro

Creo 只读它认识的那份 `config.pro`。先找：

```powershell
$env:HOMEDRIVE
Get-ChildItem "$env:HOMEDRIVE\config.pro" -ErrorAction SilentlyContinue
Get-ChildItem "$env:USERPROFILE\config.pro" -ErrorAction SilentlyContinue
```

把这一行追加到**已经存在**的那份（都没有就新建在 `$env:HOMEDRIVE\config.pro`）：

```text
toolkit_registry_file D:\creo-jlink-toolkit\work\protk.dat
```

包内的 `work\config.pro` 只对同步会话生效，是给无头会话用的；用户级 `config.pro` 不写这一行，
GUI 里不会注册这个 DLL。

注意有的环境用户级配置在映射盘（`HOMEDRIVE=Z:`），写到 `Z:\config.pro`。

**验证**：

```powershell
Select-String -Path "$env:HOMEDRIVE\config.pro" -Pattern 'toolkit_registry_file'
```

命中一次，路径正确。

---

## 5. 环境变量

不用手工设。`spawn_session.ps1` 和 `spawn_sync_export.ps1` 会在自己的进程里设置：

`CREOPMA_FEATURE_NAME`、`PRO_DIRECTORY`、`PRO_MACHINE_TYPE`、`PRO_COMM_MSG_EXE`、`CREOAPP_DIRECTORY`、`PTC_D_LICENSE_FILE`

`CREOPMA_FEATURE_NAME` 默认写的是 `creopma`。如果连接时报授权相关错误，把它改成你机器上许可的
feature name（可以参考原来那台机器的设置）。

脚本还会在拉起 `xtop` 前清掉 `BTK_*` 环境变量，这个不要删。

---

## 6. 拉起无头会话

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\spawn_headless.ps1
```

这个脚本会起 `nmsd` + `xtop`，输出 `READY`。

**输出 READY 后必须等 150 秒再连接。** 早连会让会话进入永久 Busy，只能杀掉重建。

**验证**：

```powershell
Get-Process xtop -ErrorAction SilentlyContinue | Select-Object Id, StartTime
Get-NetTCPConnection -LocalPort 1239 -State Listen -ErrorAction SilentlyContinue
```

---

## 7. 启动服务并连接

```powershell
wscript.exe .\jlink\start_service_hidden.vbs
Start-Sleep -Seconds 8
curl.exe http://localhost:8080/health
curl.exe -X POST http://localhost:8080/connect
```

**验证**：`/connect` 返回 `{"connected":true,...}`，耗时约 2.6 秒。

返回 `XToolkitBusy` 不要重试，直接看第 10 节。

---

## 8. 自检（四项全过才算装好）

| # | 命令 | 期望 |
|---|---|---|
| 1 | `curl.exe http://localhost:8080/health` | `creoHome` 有值 |
| 2 | `curl.exe -X POST http://localhost:8080/connect` | `connected:true` |
| 3 | `curl.exe http://localhost:8080/models` | 返回数组（空数组也算通） |
| 4 | 跑一次同步作业（第 9 节） | 日志出现 `SYNC_EXPORT_DONE` |

---

## 9. 跑第一个同步作业

先断开异步服务，避免两个会话抢同一个模型：

```powershell
curl.exe -X POST http://localhost:8080/disconnect
```

复制模板，改成你的模型：

```powershell
Copy-Item .\samples\export_job.txt .\work\export_job.txt -Force
notepad .\work\export_job.txt
```

跑：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\spawn_sync_export.ps1
```

**验证**：`work\logs\` 里出现 `SYNC_EXPORT_DONE`，`out\` 里有 `.stp`。

> 磁盘上的 `.stp` 如果开头是 `40 21 28 15`，说明本机有透明加密（DLP），文件本身是好的，
> 用 Creo 或公司授权程序打开即可。

---

## 10. 失败对照

| 现象 | 处置 |
|---|---|
| `/connect` 报 `XToolkitBusy` | 会话被污染。杀掉服务 JVM、`nmsd`、`xtop`，重新第 6 步，等满 150 秒再连。不要重试 |
| `/connect` 报 `XToolkitNotFound` | 会话没起。回第 6 步 |
| `/connect` 报 `XToolkitAmbiguous` | 有多个 Creo 会话（用户开着 GUI）。请用户关掉其它会话 |
| 连上了但 `/params` 报 `NotFound` | 模型不在搜索路径。先 `POST /dir` 到模型目录 |
| 同步作业挂起 | 缺 `EventProcess` 泵或多客户端并发。断开服务再跑写操作 |
| 同步 job 没反应 | `protk.dat` 路径不对、没注册、DLL 位数不对（必须 64 位） |
| 启动报 `NoClassDefFoundError: com/ptc/otk` | `otk.jar` 缺失或没进 classpath，回第 2 节 |
| 启动报 `UnsatisfiedLinkError` | `otk.jar`/`pfcasync.jar` 与 Creo 版本不匹配 |
| 导出协议不对 / 大小不变 | 没显式传 `profile`，沿用了上次会话配置 |

---

## 11. 不要做的事

- 不要重试失败的 `/connect`。
- 不要在异步服务连着会话时起同步会话。
- 不要修改用户的模型文件；重命名/保存只对明确指定的副本做。
- 不要把 `pfcasync.jar`、`otk.jar` 提交到 Git 或二次分发。
- 不要把本手册之外的路径写死；不同机器环境不一样。

---

## 12. 装完回给用户

按顺序贴四段输出，不要只说「装好了」：

1. `/health` 的返回
2. `/connect` 的返回
3. `/models` 的返回
4. 同步作业日志里含 `SYNC_EXPORT_DONE` 的那一行

四项都有，才算复刻成功。
