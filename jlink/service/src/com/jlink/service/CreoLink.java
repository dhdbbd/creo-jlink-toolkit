package com.jlink.service;

import com.ptc.pfc.pfcAsyncConnection.*;
import com.ptc.pfc.pfcSession.*;
import com.ptc.pfc.pfcModel.*;
import com.ptc.cipjava.*;

import java.io.File;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;

/**
 * CreoLink —— J-Link 异步连接管理器（单例）。
 *
 * 架构（本机 Creo 9.0.0.0 实测验证）：
 *   ✗ 不要用 AsyncConnection_Start —— 原生层 spawn xtop 静默失败，调用会永久挂起。
 *   ✓ 正确路线：服务自己管理进程 + AsyncConnection_Connect：
 *       1) nmsd.exe -noservice（Name Server，监听 127.0.0.1:1239）
 *       2) xtop.exe -g:no_graphics -i:rpc_input（无图形内核，独立进程）
 *       3) AsyncConnection_Connect(null, null, null, retries)
 *
 * 必需的环境变量（由启动脚本设置，xtop 子进程自动继承）：
 *   PATH 含 x86e_win64\lib                —— pfcasyncmt.dll 加载
 *   PRO_COMM_MSG_EXE                      —— 通信守护
 *   PTC_D_LICENSE_FILE                    —— license 文件
 *   CREOPMA_FEATURE_NAME                  —— license 特性名，须与本机许可一致（必需！）
 *   PRO_DIRECTORY / PRO_MACHINE_TYPE / CREOAPP_DIRECTORY 等
 *
 * J-Link 只能单线程访问：所有 Creo 操作都在 synchronized(lock) 内执行。
 */
public class CreoLink {

    /** 全局锁：J-Link 单线程访问 */
    public static final Object lock = new Object();

    // ---- 路径配置（configure() 初始化，可用 CREO_HOME 环境变量覆盖） ----
    public static String creoHome  = "C:\\Program Files\\PTC\\Creo 9.0.8.0";
    public static String commonDir, objDir, libDir, nmsdExe, xtopExe;
    public static String logDir = "logs";
    public static int nmsdPort = 1239;

    private static volatile boolean libLoaded = false;
    private static AsyncConnection connection = null;
    private static String connectMode = null;   // "connect"(依附运行中会话) | "start"(本服务拉起)

    // 由本服务拉起的进程（disconnect 时负责清理）
    // 必须经 spawn_session.ps1（PowerShell Start-Process）拉起——java ProcessBuilder 直接
    // 拉起的 xtop 在 9.0.8.0 上会产生永久 XToolkitBusy 会话（实测）。进程按 PID 管理。
    private static int spawnedNmsdPid = 0;
    private static int spawnedXtopPid = 0;

    /** 从 CREO_HOME 等推导全部路径 */
    public static void configure(String home, String logs) {
        if (home != null && !home.isEmpty()) creoHome = home;
        commonDir = creoHome + "\\Common Files";
        objDir    = commonDir + "\\x86e_win64\\obj";
        libDir    = commonDir + "\\x86e_win64\\lib";
        nmsdExe   = commonDir + "\\x86e_win64\\nms\\nmsd.exe";
        xtopExe   = objDir + "\\xtop.exe";
        if (logs != null && !logs.isEmpty()) logDir = logs;
        new File(logDir).mkdirs();
    }

    /**
     * 加载 J-Link 原生库 pfcasyncmt.dll。
     * 需要 PATH / java.library.path 指向 Creo 的 x86e_win64\lib。
     */
    public static void loadNativeLibrary() throws Throwable {
        synchronized (lock) {
            if (libLoaded) return;
            try {
                System.loadLibrary("pfcasyncmt");
                libLoaded = true;
                log("原生库 pfcasyncmt 加载成功");
            } catch (UnsatisfiedLinkError e) {
                throw new RuntimeException(
                    "加载 pfcasyncmt.dll 失败：PATH 需含 " + libDir + "。原始错误: " + e.getMessage(), e);
            }
        }
    }

    // ================= 连接 =================

    /** 连接到已在运行的 Creo 会话（GUI Creo 或遗留的 xtop） */
    public static String connectRunning(int retries) throws Throwable {
        loadNativeLibrary();
        synchronized (lock) {
            if (connection != null) {
                return "已经处于连接状态 (mode=" + connectMode + ")，先 /disconnect 再重连";
            }
            long t0 = System.currentTimeMillis();
            connection = pfcAsyncConnection.AsyncConnection_Connect(
                null, null, null, retries == 0 ? null : Integer.valueOf(retries));
            connectMode = "connect";
            Session s = connection.GetSession();
            String info = safeSessionInfo(s);
            log("已连接到运行中的 Creo: " + info);
            return "连接成功(已运行的Creo): " + info + "，耗时 "
                + (System.currentTimeMillis() - t0) + " ms";
        }
    }

    /**
     * 启动（或复用）一个后台无图形 Creo 并连接。
     * 流程：先探测运行中会话 → 无则拉起 nmsd + xtop → 带重试连接。
     */
    public static String startNew() throws Throwable {
        loadNativeLibrary();
        synchronized (lock) {
            if (connection != null) {
                return "已经处于连接状态 (mode=" + connectMode + ")，先 /disconnect 再重连";
            }
            StringBuilder steps = new StringBuilder();
            long t0 = System.currentTimeMillis();

            // 注意：不要在此处做"探测现有会话"的 tryConnect——
            // 任何一次失败的连接尝试都会污染本 JVM 的 CIP 状态，此后所有连接一律 XToolkitBusy。
            // 依附已有会话请走 /connect；/start 只负责拉起自己的 nmsd + xtop。

            // 2) Name Server（xtop 启动第一步必须能连上 1239）
            ensureNmsd();
            steps.append("nmsd ok → ");

            // 3) 无图形内核 xtop
            spawnXtop();
            steps.append("xtop 已拉起 → ");

            // 4) 等 xtop 完全就绪后再连接（3 分钟，单次连接）：
            //    实测：过早/pack 多次内部重试的连接都会失败并毒化本 JVM（此后一律 XToolkitBusy）；
            //    成熟会话 + 单次连接是最稳的组合。连接失败时清理自拉进程。
            Thread.sleep(180000);
            if (spawnedXtopPid > 0 && !processAlive(spawnedXtopPid)) {
                throw new IllegalStateException(
                    "xtop 进程提前退出，诊断见 " + logDir + "\\xtop.log");
            }
            Throwable last = null;
            try {
                connection = tryConnect(null);
            } catch (Throwable t) {
                last = t;
                Thread.sleep(45000);
                try { connection = tryConnect(null); }
                catch (Throwable t2) { last = t2; }
            }
            if (connection == null) {
                cleanupSpawned();
                throw new IllegalStateException(
                    "连接失败: " + root(last) + "，诊断见 " + logDir + "\\xtop.log");
            }
            connectMode = "start";
            Session s = connection.GetSession();
            String info = safeSessionInfo(s);
            long ms = System.currentTimeMillis() - t0;
            log("已启动后台 Creo 并连接 (" + ms + " ms): " + info);
            return "已启动后台无图形 Creo 并连接，耗时 " + ms + " ms。步骤: "
                + steps + "connect ok。会话: " + info;
        }
    }

    private static AsyncConnection tryConnect(Integer retries) throws Throwable {
        return pfcAsyncConnection.AsyncConnection_Connect(null, null, null, retries);
    }

    /** 连接失败时清理自己拉起的进程，避免下次 /start 累积新实例（失败会话已被污染，必须重建） */
    private static void cleanupSpawned() {
        killPid(spawnedNmsdPid); spawnedNmsdPid = 0;
        killPid(spawnedXtopPid); spawnedXtopPid = 0;
    }

    /** 断开连接；若 xtop/nmsd 是本服务拉起的，一并清理 */
    public static String disconnect() {
        synchronized (lock) {
            if (connection == null) return "当前未连接";
            String mode = connectMode;
            String note = "";
            try { connection.EventProcess(); } catch (Throwable ignore) {}  // 处理待决事件；失败不阻塞断开
            boolean weSpawned = (spawnedXtopPid > 0);
            try {
                if (weSpawned) {
                    connection.End();                 // 优雅关闭自己拉起的 xtop
                } else {
                    connection.Disconnect(Integer.valueOf(0)); // 仅断开，不动用户的 Creo
                }
            } catch (Throwable t) {
                note = "（断开时: " + root(t) + "）";
            }
            connection = null;
            connectMode = null;
            if (weSpawned) {
                killPid(spawnedXtopPid); spawnedXtopPid = 0;
            }
            killPid(spawnedNmsdPid); spawnedNmsdPid = 0;
            log("已断开 (" + mode + ")" + note);
            return "已断开 (" + mode + ")" + note;
        }
    }

    public static boolean isConnected() { return connection != null; }
    public static String getMode() { return connectMode; }
    public static boolean ownsCreo() { return spawnedXtopPid > 0; }

    /** 在锁内执行任意会话操作 —— 所有 endpoint 都通过它访问 Creo，保证单线程 */
    public interface SessionOp<T> {
        T run(Session session) throws Throwable;
    }

    public static <T> T withSession(SessionOp<T> op) throws Throwable {
        loadNativeLibrary();
        synchronized (lock) {
            if (connection == null) {
                throw new IllegalStateException("未连接到 Creo，请先调用 /start 或 /connect");
            }
            try { connection.EventProcess(); } catch (Throwable ignore) {}  // 处理待决事件；失败不阻塞操作
            Session s = connection.GetSession();
            return op.run(s);
        }
    }

    // ================= 进程管理 =================

    /** 端口探测（Socket 连一下即关） */
    private static boolean portOpen(int port, int timeoutMs) {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", port), timeoutMs);
            return true;
        } catch (Exception e) { return false; }
    }

    /** 确保 nmsd 运行（监听 1239） */
    private static void ensureNmsd() throws Exception {
        if (portOpen(nmsdPort, 300)) {
            log("nmsd 已在 1239 端口监听（复用）");
            return;
        }
        log("1239 未监听，通过 spawn_session.ps1 拉起 nmsd + xtop ...");
        spawnViaPs1();
        for (int i = 0; i < 20; i++) {
            if (portOpen(nmsdPort, 300)) { log("nmsd 已监听 1239"); return; }
            Thread.sleep(500);
        }
        throw new IllegalStateException("nmsd 10 秒内未能监听 1239，诊断见 " + logDir + "\\nmsd.log");
    }

    /** 拉起无图形内核 xtop（经 spawn_session.ps1；若会话文件已存在且进程存活则跳过） */
    private static void spawnXtop() throws Exception {
        if (spawnedXtopPid > 0 && processAlive(spawnedXtopPid)) return;
        spawnViaPs1();
    }

    /**
     * 用 spawn_session.ps1（PowerShell Start-Process）拉起 nmsd + xtop。
     * 关键：java ProcessBuilder 直接拉起同一 xtop 会产生永久 XToolkitBusy 会话——
     * 必须复刻 spawn_headless.ps1 的成功方式。
     */
    private static void spawnViaPs1() throws Exception {
        if (spawnedXtopPid > 0 && processAlive(spawnedXtopPid) && portOpen(nmsdPort, 300)) return;
        File script = new File(new File(logDir).getParentFile().getParentFile(), "spawn_session.ps1");
        if (!script.exists()) throw new IllegalStateException("找不到 " + script.getAbsolutePath());
        File pidFile = new File(logDir, "session.pid");
        pidFile.delete();
        ProcessBuilder pb = new ProcessBuilder("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", script.getAbsolutePath(), nmsdExe, xtopExe, objDir, logDir);
        pb.redirectErrorStream(true);
        pb.redirectOutput(new File(logDir, "spawn_ps1.log"));
        Process p = pb.start();
        if (!p.waitFor(90, TimeUnit.SECONDS)) {
            p.destroyForcibly();
            throw new IllegalStateException("spawn_session.ps1 超时（90 秒）");
        }
        if (!pidFile.exists()) {
            throw new IllegalStateException("spawn_session.ps1 未生成 session.pid，见 " + logDir + "\\spawn_ps1.log");
        }
        for (String line : new String(Files.readAllBytes(pidFile.toPath())).split("\\r?\\n")) {
            String[] kv = line.split("=");
            if (kv.length == 2) {
                if ("nmsd".equals(kv[0].trim())) spawnedNmsdPid = Integer.parseInt(kv[1].trim());
                if ("xtop".equals(kv[0].trim())) spawnedXtopPid = Integer.parseInt(kv[1].trim());
            }
        }
        log("会话已拉起: nmsd pid=" + spawnedNmsdPid + " xtop pid=" + spawnedXtopPid);
    }

    private static boolean processAlive(int pid) {
        if (pid <= 0) return false;
        return ProcessHandle.of(pid).map(p -> p.isAlive()).orElse(false);
    }

    private static void killPid(int pid) {
        if (pid <= 0) return;
        try {
            Process p = new ProcessBuilder("taskkill", "/PID", String.valueOf(pid), "/T", "/F").start();
            p.waitFor(10, TimeUnit.SECONDS);
        } catch (Throwable ignore) {}
    }

    private static void quit(Process p) {
        if (p == null) return;
        try { if (p.isAlive()) p.destroyForcibly(); } catch (Throwable ignore) {}
    }

    private static long pid(Process p) {
        try { return p.pid(); } catch (Throwable t) { return -1; }
    }

    // ================= 工具 =================

    private static String safeSessionInfo(Session s) {
        try {
            Model cur = s.GetCurrentModel();
            if (cur != null) return "当前模型=" + cur.GetFullName();
            return "无当前打开的模型(cwd=" + s.GetCurrentDirectory() + ")";
        } catch (Throwable t) {
            return "(会话信息读取: " + t.getClass().getSimpleName() + ")";
        }
    }

    static String root(Throwable t) {
        if (t == null) return "unknown";
        Throwable r = t; int d = 0;
        while (r.getCause() != null && d++ < 8) r = r.getCause();
        String m = r.getMessage();
        if (m != null && m.length() > 160) m = m.substring(0, 160);
        return r.getClass().getSimpleName() + (m == null ? "" : ": " + m);
    }

    static void log(String msg) {
        System.out.println("[CreoLink] " + msg);
        System.out.flush();
    }
}
