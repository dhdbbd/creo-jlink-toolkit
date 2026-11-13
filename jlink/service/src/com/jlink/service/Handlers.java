package com.jlink.service;

import com.sun.net.httpserver.*;
import java.io.*;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.*;

import com.ptc.pfc.pfcSession.*;
import com.ptc.pfc.pfcModel.*;
import com.ptc.pfc.pfcModelItem.*;
import com.ptc.pfc.pfcWindow.*;
import com.ptc.pfc.pfcFeature.*;
import com.ptc.pfc.pfcComponentFeat.*;
import com.ptc.pfc.pfcSolid.*;
import com.ptc.pfc.pfcPart.*;
import com.ptc.pfc.pfcExport.*;
import com.ptc.cipjava.*;

/**
 * Handlers —— 连接、会话、模型类端点实现。
 * 每个方法接收 HttpExchange，通过 CreoLink.withSession 在锁内访问 Creo。
 */
public class Handlers {

    interface SessionOp { Object run(Session session) throws Throwable; }

    /** 在 J-Link 锁内执行会话操作，统一异常处理与 JSON 输出 */
    @SuppressWarnings("unchecked")
    static void run(HttpExchange ex, SessionOp op) throws IOException {
        try {
            Object r = CreoLink.withSession(op::run);
            if (r instanceof Map && !((Map<?,?>) r).containsKey("ok")) {
                ((Map<String,Object>) r).put("ok", true);
            }
            sendJson(ex, 200, r);
        } catch (IllegalStateException e) {
            sendJson(ex, 409, MiniJson.obj("ok", false, "error", e.getMessage()));
        } catch (Throwable t) {
            sendJson(ex, 500, MiniJson.obj("ok", false, "error", rootMsg(t)));
        }
    }

    static void sendJson(HttpExchange ex, int code, Object payload) throws IOException {
        byte[] b = MiniJson.write(payload).getBytes(StandardCharsets.UTF_8);
        ex.getResponseHeaders().add("Content-Type", "application/json; charset=utf-8");
        ex.sendResponseHeaders(code, b.length);
        ex.getResponseBody().write(b);
        ex.close();
    }

    static Map<String,Object> readBody(HttpExchange ex) {
        try {
            byte[] raw = ex.getRequestBody().readAllBytes();
            if (raw.length == 0) return new LinkedHashMap<>();
            String s = new String(raw, StandardCharsets.UTF_8).trim();
            if (s.isEmpty()) return new LinkedHashMap<>();
            if (s.startsWith("{")) return MiniJson.parseObject(s);
            Map<String,Object> m = new LinkedHashMap<>();
            for (String kv : s.split("&")) {
                int i = kv.indexOf('=');
                if (i > 0) m.put(URLDecoder.decode(kv.substring(0, i), "UTF-8"),
                                 URLDecoder.decode(kv.substring(i + 1), "UTF-8"));
            }
            return m;
        } catch (Exception e) { return new LinkedHashMap<>(); }
    }

    static Map<String,String> query(HttpExchange ex) {
        Map<String,String> q = new LinkedHashMap<>();
        String s = ex.getRequestURI().getRawQuery();
        if (s == null) return q;
        try {
            for (String kv : s.split("&")) {
                int i = kv.indexOf('=');
                if (i > 0) q.put(URLDecoder.decode(kv.substring(0, i), "UTF-8"),
                                 URLDecoder.decode(kv.substring(i + 1), "UTF-8"));
                else q.put(URLDecoder.decode(kv, "UTF-8"), "");
            }
        } catch (Exception ignore) {}
        return q;
    }

    // ---- 基础转换 ----
    static String str(Object o) { return o == null ? null : String.valueOf(o); }
    static int toInt(Object o, int def) {
        if (o == null) return def;
        if (o instanceof Number) return ((Number) o).intValue();
        try { return Integer.parseInt(String.valueOf(o).trim()); } catch (Exception e) { return def; }
    }
    static double toDouble(Object o, double def) {
        if (o == null) return def;
        if (o instanceof Number) return ((Number) o).doubleValue();
        try { return Double.parseDouble(String.valueOf(o).trim()); } catch (Exception e) { return def; }
    }
    static String safe(String s) { return s; }
    static Object boolSafe(boolean b) { return Boolean.valueOf(b); }

    static String rootMsg(Throwable t) {
        Throwable r = t; int d = 0;
        while (r.getCause() != null && d++ < 8) r = r.getCause();
        return r.getClass().getSimpleName() + ": " + r.getMessage();
    }

    static String modelType(Model m) {
        try { ModelType t = m.GetType(); return modelTypeName(t); }
        catch (Throwable e) { return "unknown"; }
    }

    static String modelTypeName(ModelType t) {
        if (t == null) return "unknown";
        if (t == ModelType.MDL_PART)      return "part";
        if (t == ModelType.MDL_ASSEMBLY)  return "assembly";
        if (t == ModelType.MDL_DRAWING)   return "drawing";
        if (t == ModelType.MDL_MFG)       return "mfg";
        if (t == ModelType.MDL_LAYOUT)    return "layout";
        if (t == ModelType.MDL_DWG_FORMAT)return "format";
        if (t == ModelType.MDL_REPORT)    return "report";
        if (t == ModelType.MDL_MARKUP)    return "markup";
        if (t == ModelType.MDL_DIAGRAM)   return "diagram";
        if (t == ModelType.MDL_2D_SECTION)return "section";
        return "type" + t.getValue();
    }

    static String paramTypeName(ParamValueType t) {
        if (t == null) return "unknown";
        if (t == ParamValueType.PARAM_STRING)  return "string";
        if (t == ParamValueType.PARAM_INTEGER) return "integer";
        if (t == ParamValueType.PARAM_BOOLEAN) return "boolean";
        if (t == ParamValueType.PARAM_DOUBLE)  return "double";
        if (t == ParamValueType.PARAM_NOTE)    return "note";
        if (t == ParamValueType.PARAM_VOID)    return "void";
        return "type" + t.getValue();
    }

    static ModelType parseModelType(String s) {
        if (s == null) return ModelType.MDL_PART;
        switch (s.toLowerCase()) {
            case "part": case "prt": case "mdl_part":      return ModelType.MDL_PART;
            case "asm": case "assembly": case "mdl_assembly": return ModelType.MDL_ASSEMBLY;
            case "drw": case "drawing": case "mdl_drawing":  return ModelType.MDL_DRAWING;
            case "any": case "unspecified": case "":         return ModelType.MDL_UNSPECIFIED;
            default:                                         return ModelType.MDL_PART;
        }
    }

    /** 去掉扩展名 */
    static String baseName(String name) {
        if (name == null) return null;
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    // ============ 首页 ============
    static void index(HttpExchange ex) throws IOException {
        String html = "<!doctype html><meta charset='utf-8'><title>J-Link Creo Service</title>"
          + "<body style='font-family:Segoe UI,Arial;max-width:880px;margin:40px auto;color:#222;line-height:1.6'>"
          + "<h1 style='color:#b30000'>J-Link &rarr; Creo HTTP 服务</h1>"
          + "<p>运行中 | 连接状态: <b>" + (CreoLink.isConnected() ? "已连接(" + CreoLink.getMode() + ")" : "未连接") + "</b></p>"
          + "<h3>API 端点</h3><table border=1 cellspacing=0 cellpadding=6 style='border-collapse:collapse;font-size:14px'>"
          + row("GET","/health","健康检查")
          + row("POST","/connect","连接已运行的 Creo {retries:3}")
          + row("POST","/start","启动新的 Creo 进程")
          + row("POST","/disconnect","断开连接")
          + row("GET","/session","会话信息")
          + row("GET","/models","列出所有模型")
          + row("GET","/model/current","当前模型详情")
          + row("POST","/model/open","打开模型 {name,type,path}")
          + row("POST","/model/save","保存模型 {name?}")
          + row("POST","/model/erase","擦除模型 {name?,withDependencies?}")
          + row("POST","/model/display","显示模型 {name?}")
          + row("GET","/params","当前模型所有参数")
          + row("GET","/param?name=X","读参数值")
          + row("POST","/param","写参数 {name,value,type}")
          + row("POST","/dir","切换工作目录 {path}")
          + row("POST","/image","导出 JPEG {path,w,h,dpi}")
          + "</table><p style='color:#888;font-size:13px'>Creo: " + CreoLink.creoHome + " | nmsd端口: " + CreoLink.nmsdPort + "</p></body>";
        byte[] b = html.getBytes(StandardCharsets.UTF_8);
        ex.getResponseHeaders().add("Content-Type", "text/html; charset=utf-8");
        ex.sendResponseHeaders(200, b.length);
        ex.getResponseBody().write(b); ex.close();
    }
    static String row(String m,String p,String d){return "<tr><td>"+m+"</td><td><code>"+p+"</code></td><td>"+d+"</td></tr>";}

    // ============ 健康/连接 ============
    static void health(HttpExchange ex) throws IOException {
        sendJson(ex, 200, MiniJson.obj("ok", true, "service", "jlink-creo", "version", "2.0",
            "connected", CreoLink.isConnected(), "mode", CreoLink.getMode(),
            "ownsCreo", CreoLink.ownsCreo(),
            "creoHome", CreoLink.creoHome, "nmsdPort", CreoLink.nmsdPort,
            "time", new Date().toString()));
    }

    static void connect(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        int retries = toInt(body.get("retries"), 3);
        try {
            String r = CreoLink.connectRunning(retries);
            sendJson(ex, 200, MiniJson.obj("ok", true, "result", r, "connected", true, "mode", CreoLink.getMode()));
        } catch (Throwable t) {
            String msg = rootMsg(t);
            if (msg.contains("Ambiguous")) {
                sendJson(ex, 409, MiniJson.obj("ok", false, "error", msg,
                    "hint", "检测到多个 Creo 会话（可能包括你正打开着的 GUI Creo）。请只保留一个会话后重试，或先关闭 GUI Creo"));
            } else {
                sendJson(ex, 500, MiniJson.obj("ok", false, "error", msg,
                    "hint", "本机无运行中的 Creo 时请用 /start（服务会自动拉起后台无图形 Creo）"));
            }
        }
    }

    static void start(HttpExchange ex) throws IOException {
        try {
            String r = CreoLink.startNew();
            sendJson(ex, 200, MiniJson.obj("ok", true, "result", r, "connected", true, "mode", CreoLink.getMode()));
        } catch (Throwable t) {
            sendJson(ex, 500, MiniJson.obj("ok", false, "error", rootMsg(t)));
        }
    }

    static void disconnect(HttpExchange ex) throws IOException {
        sendJson(ex, 200, MiniJson.obj("ok", true, "result", CreoLink.disconnect(), "connected", false));
    }

    static void session(HttpExchange ex) throws IOException {
        run(ex, s -> {
            Map<String,Object> m = new LinkedHashMap<>();
            m.put("connected", true);
            m.put("mode", CreoLink.getMode());
            try { m.put("currentDirectory", s.GetCurrentDirectory()); } catch (Throwable t){ m.put("currentDirectory", null); }
            try { Model c = s.GetCurrentModel(); m.put("currentModel", c != null ? c.GetFullName() : null); }
            catch (Throwable t){ m.put("currentModel", null); }
            return m;
        });
    }

    // ============ 模型 ============
    static void models(HttpExchange ex) throws IOException {
        run(ex, s -> {
            List<Object> arr = new ArrayList<>();
            int n = 0;
            try {
                Models ms = s.ListModels();
                if (ms != null) {
                    n = ms.getarraysize();     // 空会话时可能 NPE（原生数组未分配）
                    for (int i = 0; i < n; i++) {
                        Model mo = ms.get(i);
                        arr.add(MiniJson.obj("name", safe(mo.GetInstanceName()), "fullName", safe(mo.GetFullName()),
                            "type", modelType(mo), "fileName", safe(mo.GetFileName()), "modified", boolSafe(mo.GetIsModified())));
                    }
                }
            } catch (Throwable t) {
                if (n == 0 && arr.isEmpty()) {
                    // 空会话：ListModels/getarraysize 抛 NPE 属正常
                    return MiniJson.obj("count", 0, "models", arr);
                }
                throw t;
            }
            return MiniJson.obj("count", n, "models", arr);
        });
    }

    static void currentModel(HttpExchange ex) throws IOException {
        run(ex, s -> {
            Model cur = s.GetCurrentModel();
            if (cur == null) return MiniJson.obj("current", null, "msg", "无当前模型");
            Map<String,Object> m = new LinkedHashMap<>();
            m.put("name", safe(cur.GetInstanceName()));
            m.put("fullName", safe(cur.GetFullName()));
            m.put("fileName", safe(cur.GetFileName()));
            m.put("type", modelType(cur));
            m.put("modified", boolSafe(cur.GetIsModified()));
            try { m.put("version", safe(cur.GetVersion())); } catch(Throwable ignore){}
            try { m.put("commonName", safe(cur.GetCommonName())); } catch(Throwable ignore){}
            return m;
        });
    }

    static void openModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String name = str(body.get("name"));
        if (name == null || name.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 name")); return; }
        String path = str(body.get("path"));
        ModelType type = parseModelType(str(body.get("type")));
        run(ex, s -> {
            // ModelDescriptor_Create(type, instanceName, genericName)  —— generic 传 null 避免原生层空串解析异常
            ModelDescriptor desc = pfcModel.ModelDescriptor_Create(type, baseName(name), null);
            if (path != null && !path.isEmpty()) desc.SetPath(path);
            Model m = s.RetrieveModel(desc);
            try { m.Display(); } catch (Throwable ignore) {}   // no_graphics 模式 Display 可能失败，忽略
            return MiniJson.obj("name", safe(m.GetInstanceName()), "fullName", safe(m.GetFullName()), "type", modelType(m));
        });
    }

    static void saveModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String name = str(body.get("name"));
        run(ex, s -> {
            Model m = targetModel(s, name);
            m.Save();
            return MiniJson.obj("saved", safe(m.GetFullName()));
        });
    }

    static void eraseModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String name = str(body.get("name"));
        boolean withDeps = Boolean.TRUE.equals(body.get("withDependencies"));
        run(ex, s -> {
            Model m = targetModel(s, name);
            String fn = safe(m.GetFullName());
            if (withDeps) m.EraseWithDependencies(); else m.Erase();
            return MiniJson.obj("erased", fn, "withDependencies", withDeps);
        });
    }

    static void displayModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String name = str(body.get("name"));
        String path = str(body.get("path"));
        ModelType type = parseModelType(str(body.get("type")));
        run(ex, s -> {
            Model m;
            if (name != null && !name.isEmpty()) {
                ModelDescriptor d = pfcModel.ModelDescriptor_Create(type, baseName(name), null);
                if (path != null && !path.isEmpty()) d.SetPath(path);
                m = s.RetrieveModel(d);
            } else m = s.GetCurrentModel();
            if (m == null) throw new IllegalStateException("没有可显示的模型");
            m.Display();
            return MiniJson.obj("displayed", safe(m.GetFullName()));
        });
    }

    static void dir(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String path = str(body.get("path"));
        if (path == null || path.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 path")); return; }
        run(ex, s -> { s.ChangeDirectory(path.replace('\\', '/')); return MiniJson.obj("currentDirectory", path.replace('\\', '/')); });
    }

    // ============ 参数读写 ============

    /** 把 ParamValue 转成可读的 Java 值 */
    static Object paramValueToObject(ParamValue v) throws jxthrowable {
        if (v == null) return null;
        ParamValueType t = v.Getdiscr();
        if (t == null) return null;
        if (t == ParamValueType.PARAM_STRING)  return v.GetStringValue();
        if (t == ParamValueType.PARAM_INTEGER) return Integer.valueOf(v.GetIntValue());
        if (t == ParamValueType.PARAM_DOUBLE)  return Double.valueOf(v.GetDoubleValue());
        if (t == ParamValueType.PARAM_BOOLEAN) return Boolean.valueOf(v.GetBoolValue());
        if (t == ParamValueType.PARAM_NOTE)    return Integer.valueOf(v.GetNoteId());
        return v.Getdiscr().toString();
    }

    /** 按类型把请求里的值构造成 ParamValue */
    static ParamValue objectToParamValue(Object raw, String type) throws jxthrowable {
        String ty = (type == null) ? "" : type.toLowerCase();
        if (ty.isEmpty()) {
            // 自动推断
            if (raw instanceof Boolean) ty = "bool";
            else if (raw instanceof Integer || raw instanceof Long) ty = "int";
            else if (raw instanceof Double || raw instanceof Float) ty = "double";
            else ty = "string";
        }
        switch (ty) {
            case "int": case "integer":
                return pfcModelItem.CreateIntParamValue(toInt(raw, 0));
            case "double": case "real": case "float":
                return pfcModelItem.CreateDoubleParamValue(toDouble(raw, 0.0));
            case "bool": case "boolean":
                boolean b = raw instanceof Boolean ? (Boolean) raw
                          : "true".equalsIgnoreCase(String.valueOf(raw)) || "1".equals(String.valueOf(raw));
                return pfcModelItem.CreateBoolParamValue(b);
            case "string": default:
                return pfcModelItem.CreateStringParamValue(String.valueOf(raw));
        }
    }

    /** 取目标模型：优先 model 参数（按名/扩展名推断类型检索），否则当前模型 */
    static Model targetModel(Session s, String name) throws jxthrowable {
        if (name == null || name.isEmpty()) {
            Model cur = s.GetCurrentModel();
            if (cur == null) throw new IllegalStateException(
                "无当前模型（no_graphics 模式无活动窗口）。请用 model=NAME 指定，或先 /model/open");
            return cur;
        }
        String base = baseName(name);
        String low = name.toLowerCase();
        ModelType[] tryTypes;
        if (low.endsWith(".prt"))      tryTypes = new ModelType[]{ModelType.MDL_PART};
        else if (low.endsWith(".asm")) tryTypes = new ModelType[]{ModelType.MDL_ASSEMBLY};
        else if (low.endsWith(".drw")) tryTypes = new ModelType[]{ModelType.MDL_DRAWING};
        else tryTypes = new ModelType[]{ModelType.MDL_PART, ModelType.MDL_ASSEMBLY, ModelType.MDL_DRAWING};
        Throwable last = null;
        for (ModelType t : tryTypes) {
            try {
                return s.RetrieveModel(pfcModel.ModelDescriptor_Create(t, base, null));
            } catch (Throwable e) { last = e; }
        }
        throw new IllegalStateException("找不到模型 " + name + ": " + CreoLink.root(last));
    }

    static void params(HttpExchange ex) throws IOException {
        Map<String,String> q = query(ex);
        String model = q.get("model");
        run(ex, s -> {
            Model m = targetModel(s, model);
            Parameters ps;
            int n = 0;
            List<Object> arr = new ArrayList<>();
            try {
                ps = m.ListParams();
                if (ps != null) n = ps.getarraysize();
            } catch (Throwable t) { ps = null; }
            for (int i = 0; i < n; i++) {
                Parameter p = ps.get(i);
                Map<String,Object> pm = new LinkedHashMap<>();
                try { pm.put("name", safe(p.GetName())); } catch (Throwable t) { continue; }
                try {
                    ParameterDriverType dt = p.GetDriverType();
                    pm.put("driver", dt == null ? null
                        : dt.getValue() == ParameterDriverType.PARAMDRIVER_RELATION.getValue() ? "relation"
                        : dt.getValue() == ParameterDriverType.PARAMDRIVER_FUNCTION.getValue() ? "function"
                        : "param");
                } catch (Throwable ignore) {}
                try {
                    if (p.GetRestriction() != null && p.GetRestriction().GetType() != null) {
                        Object rt = p.GetRestriction().GetType();
                        pm.put("restriction", String.valueOf(rt));
                    }
                } catch (Throwable ignore) {}
                try {
                    ParamValue v = p.GetValue();
                    pm.put("value", paramValueToObject(v));
                    pm.put("type", paramTypeName(v == null ? null : v.Getdiscr()));
                } catch (Throwable t) { pm.put("value", null); pm.put("type", "unreadable"); }
                arr.add(pm);
            }
            return MiniJson.obj("model", safe(m.GetInstanceName()), "count", n, "params", arr);
        });
    }

    static void param(HttpExchange ex) throws IOException {
        String method = ex.getRequestMethod();
        if ("GET".equalsIgnoreCase(method)) {
            Map<String,String> q = query(ex);
            String name = q.get("name");
            if (name == null || name.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 name")); return; }
            String model = q.get("model");
            run(ex, s -> {
                Model m = targetModel(s, model);
                Parameter p = m.GetParam(name);
                if (p == null) return MiniJson.obj("found", false, "name", name);
                ParamValue v = p.GetValue();
                return MiniJson.obj("found", true, "name", name,
                    "value", paramValueToObject(v),
                    "type", paramTypeName(v == null ? null : v.Getdiscr()));
            });
        } else if ("DELETE".equalsIgnoreCase(ex.getRequestMethod())) {
            Map<String,String> q = query(ex);
            String name = q.get("name");
            if (name == null || name.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 name")); return; }
            String model = q.get("model");
            run(ex, s -> {
                Model m = targetModel(s, model);
                Parameter p = m.GetParam(name);
                if (p == null) return MiniJson.obj("deleted", Boolean.FALSE, "name", name);
                p.Delete();
                return MiniJson.obj("deleted", Boolean.TRUE, "name", name, "model", safe(m.GetFullName()));
            });
        } else {
            // POST：创建或修改参数
            Map<String,Object> body = readBody(ex);
            String name = str(body.get("name"));
            if (name == null || name.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 name")); return; }
            String model = str(body.get("model"));
            Object value = body.get("value");
            String type = str(body.get("type"));
            run(ex, s -> {
                Model m = targetModel(s, model);
                ParamValue pv = objectToParamValue(value, type);
                Parameter existing = m.GetParam(name);
                if (existing != null) {
                    existing.SetValue(pv);
                    return MiniJson.obj("action", "updated", "name", name, "value", paramValueToObject(pv));
                } else {
                    Parameter created = m.CreateParam(name, pv);
                    return MiniJson.obj("action", "created", "name", name, "value", paramValueToObject(pv));
                }
            });
        }
    }

    // ============ 图像导出 ============
    static void image(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String path = str(body.get("path"));
        if (path == null || path.isEmpty()) { sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少 path")); return; }
        double w = toDouble(body.get("w"), 3.5);
        double h = toDouble(body.get("h"), 2.5);
        run(ex, s -> {
            JPEGImageExportInstructions instrs = pfcWindow.JPEGImageExportInstructions_Create(w, h);
            s.ExportCurrentRasterImage(path, instrs);
            java.io.File f = new java.io.File(path);
            return MiniJson.obj("exported", path, "exists", f.exists(), "bytes", f.length());
        });
    }

    /** POST /export  导出 STEP / IGES。body: {model?, type=step|iges, path?(基名，无扩展名), profile?(.dep_step 文件绝对路径)}
     *  profile 非空时先 Session.ExportProfileLoad(profile)（J-Link 侧的导出配置文件接口）；
     *  不传 profile 时沿用 flags 路径（quilts=off / blanked=off / solids=on）。 */
    static void exportModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String model = str(body.get("model"));
        String type = str(body.get("type"));
        String path = str(body.get("path"));
        String profile = str(body.get("profile"));
        run(ex, s -> {
            Model m = targetModel(s, model);
            String base;
            if (path != null && !path.isEmpty()) {
                base = path;
            } else {
                base = s.GetCurrentDirectory() + "\\" + baseName(m.GetFullName()) + "_export";
            }
            base = base.replace('\\', '/');
            String profileMsg = "(none)";
            if (profile != null && !profile.isEmpty()) {
                String pp = profile.replace('\\', '/');
                try {
                    s.ExportProfileLoad(pp);
                    profileMsg = "loaded:" + pp;
                } catch (Throwable t) {
                    throw new IOException("ExportProfileLoad 失败: " + rootMsg(t), t);
                }
            }
            GeometryFlags gf = pfcExport.GeometryFlags_Create();
            gf.SetAsQuilts(false);     // 取消面组
            gf.SetAsSolids(true);
            InclusionFlags ifl = pfcExport.InclusionFlags_Create();
            ifl.SetIncludeBlanked(false);  // 取消隐藏图元
            String out;
            if ("iges".equalsIgnoreCase(type)) {
                IGES3DNewExportInstructions ii = pfcExport.IGES3DNewExportInstructions_Create(
                    AssemblyConfiguration.EXPORT_ASM_SINGLE_FILE, gf);
                ii.SetIncludedEntities(ifl);
                m.Export(base, ii);
                out = base + ".igs";
            } else {
                STEP3DExportInstructions si = pfcExport.STEP3DExportInstructions_Create(
                    AssemblyConfiguration.EXPORT_ASM_SINGLE_FILE, gf);
                si.SetIncludedEntities(ifl);
                m.Export(base, si);
                // Creo 实际输出扩展名可能是 .step 或 .stp
                out = base + ".step";
                for (String e : new String[]{".step", ".stp", ".STEP"}) {
                    if (new java.io.File(base + e).exists()) { out = base + e; break; }
                }
            }
            java.io.File f = new java.io.File(out);
            return MiniJson.obj("exported", out, "exists", f.exists(), "bytes", f.length(),
                "profile", profileMsg,
                "options", "profile(如传)+noQuilts+noBlanked+solids");
        });
    }

    /** POST /model/rename  重命名模型。body: {model?, name=新名称(不带扩展名)} */
    static void renameModel(HttpExchange ex) throws IOException {
        Map<String,Object> body = readBody(ex);
        String model = str(body.get("model"));
        String newName = str(body.get("name"));
        if (newName == null || newName.trim().isEmpty()) {
            sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少新名称 name")); return;
        }
        String nn = baseName(newName);
        run(ex, s -> {
            Model m = targetModel(s, model);
            String old = safe(m.GetFullName());
            m.Rename(nn, Boolean.FALSE);
            m.Save();
            return MiniJson.obj("renamed", old, "to", safe(m.GetFullName()));
        });
    }

    /** GET /bom  ?model=NAME  装配结构（组件特征列表；非装配体返回 componentCount=0） */
    static void bom(HttpExchange ex) throws IOException {
        Map<String,String> q = query(ex);
        String model = q.get("model");
        run(ex, s -> {
            Model m = targetModel(s, model);
            Map<String,Object> r = MiniJson.obj("model", safe(m.GetFullName()),
                "type", modelType(m), "componentCount", 0, "components", new ArrayList<Object>());
            List<Object> comps = new ArrayList<>();
            try {
                if (modelType(m).equals("assembly") || m instanceof Solid) {
                    Solid sol = (Solid) m;
                    Features fs = sol.ListFeaturesByType(Boolean.FALSE, FeatureType.FEATTYPE_COMPONENT);
                    int n = (fs == null) ? 0 : fs.getarraysize();
                    for (int i = 0; i < n; i++) {
                        Feature f = fs.get(i);
                        Map<String,Object> c = new LinkedHashMap<>();
                        try { c.put("name", safe(f.GetName())); } catch (Throwable t) { c.put("name", null); }
                        try { c.put("id", f.GetId()); } catch (Throwable t) { c.put("id", null); }
                        try { c.put("number", f.GetNumber()); } catch (Throwable t) { c.put("number", null); }
                        try {
                            ModelItems subs = f.ListSubItems(null);
                            List<Object> subNames = new ArrayList<>();
                            if (subs != null) for (int j = 0; j < subs.getarraysize(); j++)
                                subNames.add(safe(subs.get(j).GetName()));
                            c.put("subItems", subNames);
                        } catch (Throwable ignore) {}
                        comps.add(c);
                    }
                    r.put("componentCount", n);
                }
            } catch (Throwable ignore) {}
            r.put("components", comps);
            return r;
        });
    }

    /** 递归遍历的元件总上限，防止极端装配把会话拖死 */
    static final int BOMTREE_MAX_ENTRIES = 20000;

    /** GET /bomtree ?model=NAME&maxDepth=30&commonName=1
     *  递归装配结构：把每个元件实例按"根/子装配/零件"路径平铺输出（只读，不改模型） */
    static void bomTree(HttpExchange ex) throws IOException {
        Map<String,String> q = query(ex);
        String model = q.get("model");
        int maxDepth = toInt(q.get("maxDepth"), 30);
        boolean wantCommon = truthy(q.get("commonName"));
        run(ex, s -> {
            Model m = targetModel(s, model);
            List<Object> out = new ArrayList<>();
            Map<String,Object> r = new LinkedHashMap<>();
            r.put("root", safe(m.GetFullName()));
            r.put("rootType", modelType(m));
            walkComponentTree(s, m, "", 0, maxDepth, new LinkedHashSet<String>(), out, wantCommon);
            r.put("entries", out);
            r.put("count", Integer.valueOf(out.size()));
            return r;
        });
    }

    /** 递归收集元件实例路径；stack 以模型名防自引用/环，条数上限 BOMTREE_MAX_ENTRIES */
    static void walkComponentTree(Session s, Model m, String parentPath, int depth, int maxDepth,
                                  Set<String> stack, List<Object> out, boolean wantCommon) throws jxthrowable {
        if (depth > maxDepth || out.size() >= BOMTREE_MAX_ENTRIES) return;
        Solid sol = (m instanceof Solid) ? (Solid) m : null;
        if (sol == null) return;
        String self = safe(m.GetFullName());
        String key = (self == null) ? "" : self.toLowerCase();
        if (!stack.add(key)) return;
        Features fs = null;
        try { fs = sol.ListFeaturesByType(Boolean.FALSE, FeatureType.FEATTYPE_COMPONENT); }
        catch (Throwable t) { stack.remove(key); return; }
        int n = (fs == null) ? 0 : fs.getarraysize();
        for (int i = 0; i < n && out.size() < BOMTREE_MAX_ENTRIES; i++) {
            Feature f = fs.get(i);
            Map<String,Object> e = new LinkedHashMap<>();
            e.put("depth", Integer.valueOf(depth + 1));
            e.put("parent", parentPath);
            try { e.put("featId", f.GetId()); } catch (Throwable t) {}
            String cname = null, inst = null, file = null, ctype = null;
            boolean bulk = false, isPart = false;
            ModelType mt = null;
            try {
                ComponentFeat cf = (ComponentFeat) f;
                ModelDescriptor md = cf.GetModelDescr();
                cname = md.GetGenericName();
                inst  = md.GetInstanceName();
                file  = md.GetFileName();
                mt    = md.GetType();
                ctype = modelTypeName(mt);
                bulk  = cf.GetIsBulkitem();
                isPart = (mt == ModelType.MDL_PART);
                // 本机实测 GetGenericName() 常为空，退回用文件名去扩展名（即 Creo 模型名）
                if (cname == null || cname.trim().isEmpty()) cname = baseName(file);
            } catch (Throwable t) {
                e.put("descrError", rootMsg(t));
                try { cname = f.GetName(); e.put("nameFrom", "feature"); } catch (Throwable t2) {}
            }
            String p = (cname == null || cname.isEmpty()) ? ("?id" + (i + 1)) : cname;
            String path = parentPath.isEmpty() ? p : parentPath + "/" + p;
            e.put("model", cname);
            e.put("instance", inst);
            e.put("fileName", file);
            e.put("type", ctype);
            e.put("bulk", Boolean.valueOf(bulk));
            e.put("path", path);
            if (wantCommon) {
                Model cm = null;
                try { if (mt != null && cname != null) cm = s.GetModel(cname, mt); } catch (Throwable t) {}
                if (cm != null) {
                    try {
                        String cn = cm.GetCommonName();
                        if (cn != null && !cn.isEmpty()) e.put("commonName", cn);
                    } catch (Throwable t) {}
                }
            }
            out.add(e);
            if (cname != null && !cname.isEmpty() && !isPart && mt != null) {
                Model child = null;
                try { child = s.GetModel(cname, mt); } catch (Throwable t) {}
                if (child != null) walkComponentTree(s, child, path, depth + 1, maxDepth, stack, out, wantCommon);
            }
        }
        stack.remove(key);
    }

    // ============ 材料 ============

    static boolean truthy(Object o) {
        if (o == null) return false;
        if (o instanceof Boolean) return ((Boolean) o).booleanValue();
        String s = String.valueOf(o).trim();
        return s.equalsIgnoreCase("true") || s.equals("1") || s.equalsIgnoreCase("yes");
    }

    /** 材料对象的常用属性（读不到的项跳过） */
    static Map<String,Object> materialProps(Material m) {
        Map<String,Object> r = new LinkedHashMap<>();
        if (m == null) return r;
        try { r.put("massDensity", Double.valueOf(m.GetMassDensity())); } catch (Throwable ignore) {}
        try { r.put("youngModulus", Double.valueOf(m.GetYoungModulus())); } catch (Throwable ignore) {}
        try { r.put("poissonRatio", Double.valueOf(m.GetPoissonRatio())); } catch (Throwable ignore) {}
        try { r.put("shearModulus", Double.valueOf(m.GetShearModulus())); } catch (Throwable ignore) {}
        try { r.put("thermExpCoef", Double.valueOf(m.GetThermExpCoef())); } catch (Throwable ignore) {}
        try { r.put("thermConductivity", Double.valueOf(m.GetThermConductivity())); } catch (Throwable ignore) {}
        try { r.put("specificHeat", Double.valueOf(m.GetSpecificHeat())); } catch (Throwable ignore) {}
        try { r.put("description", safe(m.GetDescription())); } catch (Throwable ignore) {}
        try { r.put("subType", safe(m.GetSubType())); } catch (Throwable ignore) {}
        try { r.put("structuralType", m.GetStructuralMaterialType().getCipTypeName()); } catch (Throwable ignore) {}
        return r;
    }

    static String currentMaterialName(Part p) {
        try { Material c = p.GetCurrentMaterial(); return c == null ? null : safe(c.GetName()); }
        catch (Throwable t) { return null; }
    }

    /** 模型内已存在的同名材料 */
    static Material findInModel(Part p, String name) {
        try {
            Materials ms = p.ListMaterials();
            if (ms == null) return null;
            for (int i = 0; i < ms.getarraysize(); i++) {
                Material m = ms.get(i);
                if (m != null && name.equalsIgnoreCase(safe(m.GetName()))) return m;
            }
        } catch (Throwable ignore) {}
        return null;
    }

    /** 把参考材料的定义复制到目标材料上（只搬运数值/字符串，不跨模型传对象） */
    static int copyMaterialDefinition(Material from, Material to) {
        if (from == null || to == null) return 0;
        int n = 0;
        try { to.SetStructuralMaterialType(from.GetStructuralMaterialType()); n++; } catch (Throwable ignore) {}
        try { to.SetThermalMaterialType(from.GetThermalMaterialType()); n++; } catch (Throwable ignore) {}
        try { to.SetMassDensity(from.GetMassDensity()); n++; } catch (Throwable ignore) {}
        try { to.SetYoungModulus(from.GetYoungModulus()); n++; } catch (Throwable ignore) {}
        try { to.SetPoissonRatio(from.GetPoissonRatio()); n++; } catch (Throwable ignore) {}
        try { to.SetShearModulus(from.GetShearModulus()); n++; } catch (Throwable ignore) {}
        try { to.SetThermExpCoef(from.GetThermExpCoef()); n++; } catch (Throwable ignore) {}
        try { to.SetThermExpRefTemp(from.GetThermExpRefTemp()); n++; } catch (Throwable ignore) {}
        try { to.SetThermConductivity(from.GetThermConductivity()); n++; } catch (Throwable ignore) {}
        try { to.SetSpecificHeat(from.GetSpecificHeat()); n++; } catch (Throwable ignore) {}
        try { to.SetEmissivity(from.GetEmissivity()); n++; } catch (Throwable ignore) {}
        try { to.SetStructDampCoef(from.GetStructDampCoef()); n++; } catch (Throwable ignore) {}
        try { to.SetHardness(from.GetHardness()); n++; } catch (Throwable ignore) {}
        try { to.SetInitBendYFactor(from.GetInitBendYFactor()); n++; } catch (Throwable ignore) {}
        try { to.SetDescription(from.GetDescription()); n++; } catch (Throwable ignore) {}
        try { to.SetSubType(from.GetSubType()); n++; } catch (Throwable ignore) {}
        try { to.SetMaterialModel(from.GetMaterialModel()); n++; } catch (Throwable ignore) {}
        try { to.SetCondition(from.GetCondition()); n++; } catch (Throwable ignore) {}
        try { to.SetHardnessType(from.GetHardnessType()); n++; } catch (Throwable ignore) {}
        return n;
    }

    /** 会话里找一个当前材料名 = name 的零件，用它作为该材料的定义来源；out[0] 回传模型名 */
    static Material findReferenceMaterial(Session s, String name, String[] out) {
        try {
            Models all = s.ListModels();
            int n = (all == null) ? 0 : all.getarraysize();
            for (int i = 0; i < n; i++) {
                try {
                    Model mm = all.get(i);
                    if (!(mm instanceof Part)) continue;
                    Material c = ((Part) mm).GetCurrentMaterial();
                    if (c != null && name.equalsIgnoreCase(safe(c.GetName()))) {
                        if (out != null) out[0] = safe(mm.GetFullName());
                        return c;
                    }
                } catch (Throwable ignore) {}
            }
        } catch (Throwable ignore) {}
        return null;
    }

    /** 逐项枚举材料属性表（MaterialPropertyType 全表，读不到的跳过）；键=枚举序号，避免重名 */
    static Map<String,Object> materialAllProps(Material m) {
        Map<String,Object> r = new LinkedHashMap<>();
        if (m == null) return r;
        for (int i = 0; i < MaterialPropertyType.__Last; i++) {
            MaterialPropertyType pt = MaterialPropertyType.FromInt(Integer.valueOf(i));
            if (pt == null) continue;
            try {
                MaterialProperty pv = m.GetPropertyValue(pt);
                if (pv == null) continue;
                Map<String,Object> one = new LinkedHashMap<>();
                one.put("idx", Integer.valueOf(i));
                try { one.put("value", Double.valueOf(pv.GetValue())); } catch (Throwable ignore) {}
                try { one.put("units", safe(pv.GetUnits())); } catch (Throwable ignore) {}
                r.put("prop" + i, one);
            } catch (Throwable ignore) {}
        }
        return r;
    }

    /** 通用属性表复制：整表逐项搬运（跨模型对象可能被拒，成功的计数） */
    static int copyMaterialGeneric(Material from, Material to) {
        if (from == null || to == null) return 0;
        int n = 0;
        for (int i = 0; i < MaterialPropertyType.__Last; i++) {
            MaterialPropertyType pt = MaterialPropertyType.FromInt(Integer.valueOf(i));
            if (pt == null) continue;
            try {
                MaterialProperty pv = from.GetPropertyValue(pt);
                if (pv == null) continue;
                to.SetPropertyValue(pt, pv);
                n++;
            } catch (Throwable ignore) {}
        }
        return n;
    }

    /** 材料自身的参数表（材料是 ParameterOwner：PTC_MASS_DENSITY、材料标准 等都挂在这张表上） */
    static Map<String,Object> materialParamsOf(Material m) {
        Map<String,Object> r = new LinkedHashMap<>();
        if (m == null) return r;
        try {
            Parameters ps = m.ListParams();
            int n = (ps == null) ? 0 : ps.getarraysize();
            for (int i = 0; i < n; i++) {
                try {
                    Parameter p = ps.get(i);
                    ParamValue v = p.GetValue();
                    r.put(safe(p.GetName()), paramValueToObject(v));
                } catch (Throwable ignore) {}
            }
        } catch (Throwable ignore) {}
        return r;
    }

    /** 把参考材料的参数表整份搬到目标材料上（缺的建、有的改） */
    static int copyMaterialParams(Material from, Material to) {
        if (from == null || to == null) return 0;
        int n = 0;
        try {
            Parameters ps = from.ListParams();
            int cnt = (ps == null) ? 0 : ps.getarraysize();
            for (int i = 0; i < cnt; i++) {
                try {
                    Parameter sp = ps.get(i);
                    String nm = sp.GetName();
                    ParamValue v = sp.GetValue();
                    if (nm == null || v == null) continue;
                    Parameter dp = to.GetParam(nm);
                    if (dp != null) { dp.SetValue(v); n++; }
                    else { to.CreateParam(nm, v); n++; }
                } catch (Throwable ignore) {}
            }
        } catch (Throwable ignore) {}
        return n;
    }

    static String paramText(Model m, String paramName) {
        try {
            Parameter p = m.GetParam(paramName);
            if (p == null) return null;
            ParamValue v = p.GetValue();
            return (v == null || v.Getdiscr() != ParamValueType.PARAM_STRING) ? null : v.GetStringValue();
        } catch (Throwable t) { return null; }
    }

    /** GET  /material ?model=NAME            当前材料 + 模型内材料清单 + 属性
     *  POST /material {model|models[], name=材料名, save?, ref=参考模型}
     *  取材料顺序：模型内已有 → 材料库 RetrieveMaterial → 新建并复制参考材料定义。
     *  参考材料：ref 指定的模型，否则自动在会话里找当前材料名相同的零件。 */
    static void material(HttpExchange ex) throws IOException {
        if ("GET".equalsIgnoreCase(ex.getRequestMethod())) {
            Map<String,String> q = query(ex);
            String model = q.get("model");
            String matName = q.get("name");
            boolean full = "1".equals(q.get("full")) || "true".equalsIgnoreCase(q.get("full"));
            run(ex, s -> {
                Model m = targetModel(s, model);
                if (!(m instanceof Part)) throw new IllegalStateException("不是零件模型: " + safe(m.GetFullName()));
                Part p = (Part) m;
                if (matName != null && !matName.isEmpty()) {
                    // 查看指定材料：先在模型内找，找不到再问材料库（只探测，不设为当前材料）
                    String where;
                    Material mm = findInModel(p, matName);
                    where = "in-model";
                    if (mm == null) {
                        try { mm = p.RetrieveMaterial(matName); where = "library"; }
                        catch (Throwable t) { where = null; }
                    }
                    Map<String,Object> r = MiniJson.obj("model", safe(p.GetFullName()), "material", matName,
                                                        "foundIn", where, "current", currentMaterialName(p));
                    if (mm != null) {
                        r.put("props", materialProps(mm));
                        r.put("materialParams", materialParamsOf(mm));
                        if (full) r.put("propsAll", materialAllProps(mm));
                    }
                    return r;
                }
                Map<String,Object> r = MiniJson.obj("model", safe(p.GetFullName()), "current", currentMaterialName(p));
                List<Object> names = new ArrayList<>();
                try {
                    Materials ms = p.ListMaterials();
                    if (ms != null) for (int i = 0; i < ms.getarraysize(); i++) names.add(safe(ms.get(i).GetName()));
                } catch (Throwable ignore) {}
                r.put("materials", names);
                Map<String,Object> props = new LinkedHashMap<>();
                try { props = materialProps(p.GetCurrentMaterial()); } catch (Throwable ignore) {}
                r.put("props", props);
                try { r.put("materialParams", materialParamsOf(p.GetCurrentMaterial())); } catch (Throwable ignore) {}
                // 实体密度 / 质量属性：确认材料密度真的作用到几何上（相当于看"主材料"是否生效）
                try {
                    Solid sol = (Solid) p;
                    Map<String,Object> bod = new LinkedHashMap<>();
                    try {
                        SolidBody b = sol.GetDefaultBody();
                        if (b != null) {
                            try { bod.put("defaultBody", safe(b.GetName())); } catch (Throwable ignore) {}
                            try { bod.put("bodyDensity", b.GetDensity()); } catch (Throwable ignore) {}
                            try { bod.put("sheetmetal", Boolean.valueOf(b.IsSheetmetal())); } catch (Throwable ignore) {}
                        }
                    } catch (Throwable t) { bod.put("bodyError", rootMsg(t)); }
                    try {
                        MassProperty mp = sol.GetMassProperty(null);
                        if (mp != null) {
                            try { bod.put("mass", Double.valueOf(mp.GetMass())); } catch (Throwable ignore) {}
                            try { bod.put("volume", Double.valueOf(mp.GetVolume())); } catch (Throwable ignore) {}
                            try { bod.put("densityUsed", Double.valueOf(mp.GetDensity())); } catch (Throwable ignore) {}
                        }
                    } catch (Throwable t) { bod.put("massError", rootMsg(t)); }
                    r.put("body", bod);
                } catch (Throwable ignore) {}
                if (full) {
                    Map<String,Object> all = new LinkedHashMap<>();
                    try { all = materialAllProps(p.GetCurrentMaterial()); } catch (Throwable ignore) {}
                    r.put("propsAll", all);
                }
                r.put("paramPTC_MATERIAL_NAME", paramText(p, "PTC_MATERIAL_NAME"));
                return r;
            });
            return;
        }

        Map<String,Object> body = readBody(ex);
        String raw = str(body.get("name"));
        if (raw == null || raw.trim().isEmpty()) {
            sendJson(ex, 400, MiniJson.obj("ok", false, "error", "缺少材料名 name")); return;
        }
        final String materialName = raw.trim();
        final boolean save = truthy(body.get("save"));
        final boolean forceCopy = truthy(body.get("copyProps"));
        final boolean preferLibrary = truthy(body.get("preferLibrary"));
        final String massDensityUnits = str(body.get("massDensityUnits"));
        final String setDensity = str(body.get("setDensity"));
        final String refModel = str(body.get("ref"));

        run(ex, s -> {
            List<String> targets = new ArrayList<>();
            Object arr = body.get("models");
            if (arr instanceof List) {
                for (Object o : (List<?>) arr) if (o != null) targets.add(String.valueOf(o).trim());
            } else if (arr instanceof String) {
                for (String tok : ((String) arr).split("[,;\\s]+")) if (!tok.trim().isEmpty()) targets.add(tok.trim());
            }
            if (targets.isEmpty()) {
                String one = str(body.get("model"));
                if (one != null && !one.isEmpty()) targets.add(one);
            }
            if (targets.isEmpty()) throw new IllegalStateException("缺少 model 或 models");

            // 参考材料（复制定义用）
            Material ref = null; String refFrom = null;
            if (refModel != null && !refModel.isEmpty()) {
                try {
                    Model rm = targetModel(s, refModel);
                    if (rm instanceof Part) { ref = ((Part) rm).GetCurrentMaterial(); refFrom = safe(rm.GetFullName()); }
                } catch (Throwable ignore) {}
            }
            if (ref == null) {
                if (refModel != null && !refModel.isEmpty())
                    throw new IllegalStateException("参考模型没有可用当前材料: " + refModel);
                String[] holder = new String[1];
                ref = findReferenceMaterial(s, materialName, holder);
                if (ref != null) refFrom = holder[0];
            }

            List<Object> results = new ArrayList<>();
            for (String tn : targets) {
                Map<String,Object> one = new LinkedHashMap<>();
                one.put("model", tn);
                try {
                    Model m = targetModel(s, tn);
                    if (!(m instanceof Part)) { one.put("ok", false); one.put("error", "不是零件模型"); results.add(one); continue; }
                    Part p = (Part) m;
                    one.put("before", currentMaterialName(p));

                    String source = null;
                    Material mat = findInModel(p, materialName);
                    if (mat != null) source = "in-model";
                    if (mat != null && preferLibrary) {
                        // 先把当前材料切到系统占位材料，再删掉模型内同名材料，好让 Creo 重新从材料库取
                        try {
                            Material sys = p.GetMaterial("PTC_SYSTEM_MTRL_PROPS");
                            if (sys != null && sys != mat) p.SetCurrentMaterial(sys);
                        } catch (Throwable ignore) {}
                        try { mat.Delete(); mat = null; source = null; }
                        catch (Throwable t) { one.put("deleteInModel", rootMsg(t)); }
                    }
                    if (mat == null) {
                        try { mat = p.RetrieveMaterial(materialName); source = "library"; }
                        catch (Throwable t) { mat = null; one.put("retrieveError", rootMsg(t)); }
                    }
                    if (mat == null) {
                        mat = p.CreateMaterial(materialName);
                        source = "created";
                    }
                    if (ref != null && (forceCopy || source.equals("created"))) {
                        int copied = copyMaterialDefinition(ref, mat);
                        int generic = copyMaterialGeneric(ref, mat);
                        int params = copyMaterialParams(ref, mat);
                        one.put("copiedProps", Integer.valueOf(copied));
                        one.put("copiedGeneric", Integer.valueOf(generic));
                        one.put("copiedParams", Integer.valueOf(params));
                        one.put("refFrom", refFrom);
                        if (forceCopy && source.equals("in-model")) source = "in-model+recopied";
                    }
                    p.SetCurrentMaterial(mat);

                    // 密度单位/数值显式设定：先改属性单位，再按该单位写入数值。
                    // （GetMassDensity/SetMassDensity 的单位语义不一致，不能用"读出再写回"来归一）
                    if (massDensityUnits != null && !massDensityUnits.isEmpty()) {
                        try {
                            mat.SetPropertyUnits(MaterialPropertyType.MTL_PROP_MASS_DENSITY, massDensityUnits, Boolean.TRUE);
                            one.put("densityUnitsSet", massDensityUnits);
                        } catch (Throwable t) { one.put("densityUnitsError", rootMsg(t)); }
                    }
                    if (setDensity != null && !setDensity.isEmpty()) {
                        try {
                            mat.SetMassDensity(Double.parseDouble(setDensity.trim()));
                            one.put("densitySet", setDensity);
                        } catch (Throwable t) { one.put("densityError", rootMsg(t)); }
                    }

                    one.put("source", source);
                    String after = currentMaterialName(p);
                    one.put("after", after);
                    one.put("paramPTC_MATERIAL_NAME", paramText(p, "PTC_MATERIAL_NAME"));
                    if (save) { p.Save(); one.put("saved", Boolean.TRUE); }
                    one.put("ok", materialName.equalsIgnoreCase(after));
                    results.add(one);
                } catch (Throwable t) {
                    one.put("ok", false);
                    one.put("error", rootMsg(t));
                    results.add(one);
                }
            }
            int okCount = 0;
            for (Object o : results) {
                Map<?,?> map = (Map<?,?>) o;
                if (Boolean.TRUE.equals(map.get("ok"))) okCount++;
            }
            return MiniJson.obj("material", materialName, "requested", Integer.valueOf(targets.size()),
                                "succeeded", Integer.valueOf(okCount), "saved", Boolean.valueOf(save), "results", results);
        });
    }
}
