/*
 * sync_export.c -- Creo 9 同步 Pro/TOOLKIT 导出 DLL（job 文件驱动，无头运行）
 *
 * �?Creo 启动时经 protk.dat 加载进进程，user_initialize 里读取工作目录下�?
 * export_job.txt，按行执行导出（支持加载 .dep_step 导出配置文件）�?
 *
 * export_job.txt 格式（UTF-8，空�?# 注释行忽略）�?
 *   DIR|<模型目录>                     可选，检索模型前切到此目�?
 *   EXPORT|<模型�?|<prt|asm>|<输出路径(不含扩展�?>|<配置文件路径，可�?
 *   EXPORT|...
 *
 * 运行结果�?sync_export_run.log（同时输出到 stderr，便于重定向抓取）�?
 * 结束行：SYNC_EXPORT_DONE ok=<n> fail=<m>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include <windows.h>

#include "ProToolkit.h"
#include "ProMdl.h"
#include "ProModelitem.h"
#include "ProUtil.h"
#include "ProSolid.h"
#include "ProAsmcomp.h"
#include "ProAsmcomppath.h"
#include "ProArray.h"
#include "ProFit.h"
#include "ProFeature.h"
#include "ProFeatType.h"
#include "ProIntf3Dexport.h"
#include "ProIntfimport.h"
#include "ProParameter.h"
#include "ProMaterial.h"
#include "ProMenu.h"
#include "ProView.h"
#include "ProPDF.h"
#include "ProWstring.h"
#include "ProDtlattach.h"
#include "ProDtlnote.h"
#include "ProDrawing.h"
#include "ProWindows.h"
#include "ProDrawingView.h"
#include "ProAnnotation.h"
#include "ProDtlsymdef.h"
#include "ProDtlsyminst.h"
#include "ProSelection.h"
#include "ProAxis.h"
#include "ProGeomitem.h"
#include "ProElement.h"
#include "ProElemId.h"
#include "ProDtmAxis.h"
#include "ProFeatType.h"
#include "ProDtlentity.h"
#include "ProSecdim.h"
#include "ProValue.h"
#include "ProItemerr.h"

#define JOB_FILE  "export_job.txt"
#define LOG_FILE  "sync_export_run.log"
#define MAXJOBS   32
static ProDimension g_last_nd;          /* 最近一次创建的图纸尺寸（拖动用�?*/
static int       g_last_nd_valid = 0;
static double    g_last_nd_value = -1.0;
static ProDimension g_r58_dimple_nd;   /* r58: dDIMPLE handle (struct, not pointer) */
static int       g_r58_dimple_valid = 0;
static ProDimension g_r60_thick_nd;   /* r60: total-thickness dim handle (format override 4 -> 4.0) */
static int       g_r60_thick_valid = 0;

/* 轮次54（用户要求）�?*自校一律用截图（grab_creo_window.ps1），默认不导任何 PDF**�?
   只有 DSH 明确�?交付"时（发命�?DELIVER）才允许导出�?*/
static int g_deliver_ok = 0;

/* 轮次55 前置声明：凸台间距尺寸（锚在十字中心线）*/
static void boss_cross_dims(ProMdl part, ProMdl drw, int fvi, int mode);


static FILE *g_log = NULL;

static void log_open(void)
{
    if (!g_log) g_log = fopen(LOG_FILE, "w");
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* 轮次54：PDF 导出门控日志（函数放�?logf_ 之后，避免前置声明问题） */
static void pdf_gate_log(const char *site)
{
    logf_("[pdfgate] %s: self-check PDF export DISABLED (r54: screenshots only; deliver=%d)", site, g_deliver_ok);
}

static const char *errname(ProError e)
{
    switch (e) {
    case PRO_TK_NO_ERROR:      return "NO_ERROR";
    case PRO_TK_GENERAL_ERROR: return "GENERAL_ERROR";
    case PRO_TK_BAD_INPUTS:    return "BAD_INPUTS";
    case PRO_TK_E_NOT_FOUND:   return "E_NOT_FOUND";
    case PRO_TK_NO_LICENSE:    return "NO_LICENSE";
    case PRO_TK_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
    case PRO_TK_INVALID_TYPE:  return "INVALID_TYPE";
    case PRO_TK_BAD_CONTEXT:   return "BAD_CONTEXT";
    default:                   return "OTHER";
    }
}

static int u8_to_wide(const char *s, wchar_t *dst, int cap)
{
    int n;
    /* skip UTF-8 BOM (PowerShell Set-Content -Encoding UTF8 writes one) */
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, dst, cap);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s, -1, dst, cap);
    return (n > 0) ? 1 : 0;
}

static void wide_to_u8(const wchar_t *s, char *dst, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, dst, cap, NULL, NULL);
    if (n <= 0 && cap > 0) dst[0] = 0;
}

/* �?UTF-8 路径打开文件：fopen �?ANSI 代码页，中文路径会打不开，故转宽字符�?_wfopen */
static FILE *wfopen_utf8(const char *u8, const wchar_t *mode)
{
    wchar_t w[PRO_PATH_SIZE * 2];
    if (!u8_to_wide(u8, w, (int)(sizeof(w) / sizeof(w[0])))) return NULL;
    return _wfopen(w, mode);
}

typedef struct {
    wchar_t model[PRO_NAME_SIZE + 8];
    wchar_t out[PRO_PATH_SIZE];
    wchar_t profile[PRO_PATH_SIZE];
    int     is_asm;
    int     has_profile;
} job_t;

/* 干涉检查作业：INTERF|<模型>|<prt|asm>|<setup>|<facets>|<quilts>|<fast>|<volume>
 *   setup: part | subasm | part_detailed | subasm_detailed
 *   facets/quilts/fast/volume: 0|1（fast=1 只给干涉对，不给体积；volume=1 时对每对单独算体积）*/
typedef struct {
    wchar_t model[PRO_NAME_SIZE + 8];
    int     is_asm;
    int     setup;
    int     facets;
    int     quilts;
    int     fast;
    int     volume;
} interf_t;

static job_t   g_jobs[MAXJOBS];
static int     g_njobs = 0;
static interf_t g_interfs[MAXJOBS];
static int     g_ninterfs = 0;
static wchar_t g_dir[PRO_PATH_SIZE];
static int     g_has_dir = 0;
static wchar_t g_verifies[MAXJOBS][PRO_PATH_SIZE];
static int     g_nverifies = 0;

/* 旧版 3D 导出作业（Creo 5 起已弃用�?ProIntf3DFileWrite，PiCreo 等�?DLL 走这条）�?
 * EXPORTV1|<模型>|<prt|asm>|<输出路径(不含扩展�?> */
static wchar_t g_v1_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_v1_out[MAXJOBS][PRO_PATH_SIZE];
static int     g_nv1 = 0;
static int     g_v1_asm[MAXJOBS];

/* 2D 导出作业（图�?-> DWG/DXF）：EXPORT2D|<图纸>|<输出文件(含扩展名)> */
static wchar_t g_2d_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_2d_out[MAXJOBS][PRO_PATH_SIZE];
static int     g_n2d = 0;

/* 打平装配作业�?
 *   FLATTEN|<输入 stp>|<输出无扩展名>|<profile �?->|<导入模型�?�?->
 * 导入 STEP 装配 -> 把下层装配体里的零件按总装坐标直接装到总装�?-> 删掉下层装配�?-> �?profile 导出 */
static wchar_t g_flat_in[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_flat_out[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_flat_prof[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_flat_name[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_flat_hasprof[MAXJOBS];
static int     g_nflat = 0;

#define MAXLEAF 8192
typedef struct { ProMdl mdl; ProMatrix mat; } flatleaf_t;
static flatleaf_t g_leafs[MAXLEAF];
static int        g_nleafs = 0;
static int        g_subids[MAXJOBS];
static int        g_nsubids = 0;

typedef struct { ProAssembly top; ProIdTable ids; int depth; } flatwalk_t;

static void log_file_head(const char *path);
static void log_export_head(const wchar_t *out_noext);

/* 统计作业（在 Creo 进程内读，绕过加密）：COUNT|<文件>|<子串> */
static wchar_t g_cnt_file[MAXJOBS][PRO_PATH_SIZE];
static char    g_cnt_pat[MAXJOBS][128];
static int     g_ncnt = 0;

/* 参数作业（进程内精确类型控制；异步侧只能�?改，删不了，改不了类型）�?
 *   PARAMDEL|<模型>|<prt|asm>|<参数�?
 *   PARAMSET|<模型>|<prt|asm>|<参数�?|<字符串�?
 * 用途：已有参数是数值型、需要改成字符型�?—�?�?PARAMDEL �?PARAMSET�?*/
static wchar_t g_pm_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_pm_name[MAXJOBS][PRO_NAME_SIZE];
static wchar_t g_pm_val[MAXJOBS][PRO_LINE_SIZE];
static char    g_pm_val8[MAXJOBS][PRO_LINE_SIZE * 2];
static int     g_pm_asm[MAXJOBS];
static int     g_pm_is_set[MAXJOBS];
static int     g_pm_del_first[MAXJOBS];
static int     g_npm = 0;

/* 参数表导出作业（导出“工�?参数”那张表 + 来源/指定/访问列，并在进程内读回打印）�?
 *   PARAMD|<模型>|<prt|asm>|<输出基名(不含扩展名，可空)> */
static wchar_t g_pd_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_pd_base[MAXJOBS][PRO_PATH_SIZE];
static int     g_pd_asm[MAXJOBS];
static int     g_npd = 0;

/* �?命令作业（触发已注册命令，如 PiCreo �?SD 系列）：
 *   MACRO|<宏文�?                     直接加载并执行一条命令宏（如 ~ Command `SDCurrentDrawing`;�?
 *   QUICKDRAW|<模型>|<prt|asm>         打开模型 -> 执行 PiCreo「快速出图�?SDCurrentDrawing) -> 等产�?PDF
 */
static wchar_t g_macro[MAXJOBS][1024];
static int     g_nmacro = 0;

static wchar_t g_qd_model[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_qd_asm[MAXJOBS];
static int     g_qd_wait[MAXJOBS];
static int     g_qd_defer[MAXJOBS];
static int     g_nqd = 0;

/* 延迟触发（等�?PiCreo 等模块加载完成后再执行命令宏）：
 * QUICKDRAW �?4 段带 d（如 QUICKDRAW|nb109_3|prt|300|d）时启用�?
 * �?DLL �?user_initialize 在启动早期执行，此时 PiCreo �?SD 命令尚未注册
 *（ProMacroExecute 返回 PRO_TK_NOT_VALID=-26），故用 SetTimer 推迟到主线程消息泵里执行�?*/
static ProMdl   g_def_mdl = NULL;
static wchar_t  g_def_model[PRO_NAME_SIZE + 8];
static ProMdl   g_loaded_drw = NULL;
/* ---- multi-drawing table (one Creo process, several drawing windows) ----
   DRWLOAD adds an entry, DRWSEL switches g_loaded_drw/g_def_model to it so every
   existing command keeps operating on "the active drawing". */
#define MAX_DRW 8
static struct {
    ProMdl   mdl;
    wchar_t  alias[64];
    wchar_t  base[64];
    wchar_t  path[PRO_PATH_SIZE];
} g_drw_tab[MAX_DRW];
static int g_drw_n = 0;
static int g_drw_active = -1;
static int      g_def_wait = 300;
static int      g_def_armed = 0;
static int      g_cmdonly = 0;   /* CMDONLY job: arm cmd channel without touching the drawing */
static UINT_PTR g_def_timer = 0;
static int      g_def_phase = 0;
static int      g_def_tries = 0;
static int      g_def_elapsed = 0;
static unsigned long long g_def_arm_ft = 0;   /* arm time (FILETIME) for output freshness check */

/* 目录内取“最新修改”的匹配文件并读出（进程内读，绕加密）：DUMPNEW|<目录>|<文件名前缀> */
static wchar_t g_dn_dir[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_dn_pfx[MAXJOBS][128];
static int     g_ndn = 0;

/* 在目录里按“文件头�?Process ID: <pid>”找文件并读出（定位本会�?trail）：
 *   DUMPPID|<目录>|<文件名前缀>|<pid> */
static wchar_t g_dp_dir[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_dp_pfx[MAXJOBS][128];
static int     g_dp_pid[MAXJOBS];
static int     g_ndp = 0;

/* 配置选项作业：GETOPT|<选项> ；SETOPT|<选项>|<�? */
typedef struct {
    wchar_t name[64];
    wchar_t val[512];
    int     is_set;
} optop_t;
static optop_t g_opts[MAXJOBS];
static int     g_nopts = 0;

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int read_job(void)
{
    FILE *f = fopen(JOB_FILE, "rb");
    long sz;
    char *buf, *p, *save, *line;

    if (!f) { logf_("[!] %s not found in cwd", JOB_FILE); return 0; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1048576) { fclose(f); logf_("[!] job file size bad: %ld", sz); return 0; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return 0; }
    buf[sz] = 0; fclose(f);

    p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    save = NULL;
    for (line = strtok_s(p, "\r\n", &save); line; line = strtok_s(NULL, "\r\n", &save)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        if (strncmp(l, "DIR|", 4) == 0) {
            if (u8_to_wide(l + 4, g_dir, PRO_PATH_SIZE)) g_has_dir = 1;
            logf_("[i] DIR = %s", l + 4);
        } else if (strncmp(l, "EXPORT|", 7) == 0) {
            char *rest = l + 7;
            char *fld[5];
            int nf = 0;
            job_t *j;
            fld[nf++] = rest;
            while (nf < 5) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 3) { logf_("[!] bad EXPORT line: %s", l); continue; }
            if (g_njobs >= MAXJOBS) { logf_("[!] too many jobs, ignored: %s", l); continue; }
            j = &g_jobs[g_njobs];
            memset(j, 0, sizeof(*j));
            u8_to_wide(fld[0], j->model, PRO_NAME_SIZE + 8);
            j->is_asm = (nf >= 2 && (fld[1][0] == 'a' || fld[1][0] == 'A')) ? 1 : 0;
            if (nf >= 3) u8_to_wide(fld[2], j->out, PRO_PATH_SIZE);
            if (nf >= 4 && fld[3][0]) {
                u8_to_wide(fld[3], j->profile, PRO_PATH_SIZE);
                j->has_profile = 1;
            }
            g_njobs++;
        } else if (strncmp(l, "INTERF|", 7) == 0) {
            char *rest = l + 7;
            char *fld[9];
            int nf = 0;
            interf_t *it;
            fld[nf++] = rest;
            while (nf < 8) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 2) { logf_("[!] bad INTERF line: %s", l); continue; }
            if (g_ninterfs >= MAXJOBS) { logf_("[!] too many INTERF jobs, ignored: %s", l); continue; }
            it = &g_interfs[g_ninterfs];
            memset(it, 0, sizeof(*it));
            u8_to_wide(fld[0], it->model, PRO_NAME_SIZE + 8);
            it->is_asm = (nf >= 2 && (fld[1][0] == 'a' || fld[1][0] == 'A')) ? 1 : 0;
            if (nf >= 3) {
                if      (strncmp(fld[2], "subasm", 6) == 0 && strncmp(fld[2], "subasm_detailed", 15) == 0) it->setup = 1;
                else if (strncmp(fld[2], "subasm_detailed", 15) == 0) it->setup = 3;
                else if (strncmp(fld[2], "part_detailed", 13) == 0)   it->setup = 2;
                else                                                  it->setup = 0;
            }
            if (nf >= 4) it->facets = (fld[3][0] == '1');
            if (nf >= 5) it->quilts = (fld[4][0] == '1');
            if (nf >= 6) it->fast   = (fld[5][0] == '1');
            if (nf >= 7) it->volume = (fld[6][0] == '1');
            g_ninterfs++;
            logf_("[i] INTERF = %s (setup=%d facets=%d quilts=%d fast=%d volume=%d)",
                  fld[0], it->setup, it->facets, it->quilts, it->fast, it->volume);
        } else if (strncmp(l, "VERIFY|", 7) == 0) {
            if (g_nverifies < MAXJOBS && u8_to_wide(l + 7, g_verifies[g_nverifies], PRO_PATH_SIZE)) {
                g_nverifies++;
                logf_("[i] VERIFY = %s", l + 7);
            }
        } else if (strncmp(l, "PARAMDEL|", 9) == 0 || strncmp(l, "PARAMSET|", 9) == 0) {
            int is_set = (strncmp(l, "PARAMSET|", 9) == 0);
            char *fld[5];
            int   nf = 0;
            fld[nf++] = l + 9;
            while (nf < 5) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 3) { logf_("[!] bad PARAM line: %s", l); continue; }
            if (g_npm >= MAXJOBS) { logf_("[!] too many PARAM jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_pm_model[g_npm], PRO_NAME_SIZE + 8);
            g_pm_asm[g_npm] = (fld[1][0] == 'a' || fld[1][0] == 'A') ? 1 : 0;
            u8_to_wide(trim(fld[2]), g_pm_name[g_npm], PRO_NAME_SIZE);
            g_pm_is_set[g_npm] = 0;
            g_pm_del_first[g_npm] = 0;
            if (is_set) {
                if (nf >= 4) {
                    u8_to_wide(trim(fld[3]), g_pm_val[g_npm], PRO_LINE_SIZE);
                    strncpy_s(g_pm_val8[g_npm], sizeof(g_pm_val8[0]), trim(fld[3]), _TRUNCATE);
                    g_pm_is_set[g_npm] = 1;
                }
                if (nf >= 5 && (fld[4][0] == '1' || fld[4][0] == 'y')) g_pm_del_first[g_npm] = 1;
            }
            logf_("[i] %s %s %s%s%s", is_set ? "PARAMSET" : "PARAMDEL", fld[0], fld[2],
                  g_pm_is_set[g_npm] ? " = " : "", g_pm_is_set[g_npm] ? g_pm_val8[g_npm] : "");
            g_npm++;
        } else if (strncmp(l, "COUNT|", 6) == 0) {
            char *rest = l + 6;
            char *q = strchr(rest, '|');
            if (!q) { logf_("[!] bad COUNT line: %s", l); continue; }
            if (g_ncnt >= MAXJOBS) { logf_("[!] too many COUNT jobs"); continue; }
            *q = 0;
            u8_to_wide(trim(rest), g_cnt_file[g_ncnt], PRO_PATH_SIZE);
            strncpy_s(g_cnt_pat[g_ncnt], sizeof(g_cnt_pat[0]), trim(q + 1), _TRUNCATE);
            logf_("[i] COUNT %s  pattern=%s", rest, g_cnt_pat[g_ncnt]);
            g_ncnt++;
        } else if (strncmp(l, "FLATTEN|", 8) == 0) {
            char *rest = l + 8;
            char *fld[5];
            int   nf = 0;
            fld[nf++] = rest;
            while (nf < 5) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 2) { logf_("[!] bad FLATTEN line: %s", l); continue; }
            if (g_nflat >= MAXJOBS) { logf_("[!] too many FLATTEN jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_flat_in[g_nflat], PRO_PATH_SIZE);
            u8_to_wide(trim(fld[1]), g_flat_out[g_nflat], PRO_PATH_SIZE);
            g_flat_hasprof[g_nflat] = 0;
            g_flat_name[g_nflat][0] = 0;
            if (nf >= 3 && fld[2][0] && fld[2][0] != '-') {
                u8_to_wide(trim(fld[2]), g_flat_prof[g_nflat], PRO_PATH_SIZE);
                g_flat_hasprof[g_nflat] = 1;
            }
            if (nf >= 4 && fld[3][0] && fld[3][0] != '-')
                u8_to_wide(trim(fld[3]), g_flat_name[g_nflat], PRO_NAME_SIZE + 8);
            logf_("[i] FLATTEN %s -> %s (profile=%s, name=%s)", fld[0], fld[1],
                  g_flat_hasprof[g_nflat] ? fld[2] : "-", g_flat_name[g_nflat][0] ? fld[3] : "-");
            g_nflat++;
        } else if (strncmp(l, "EXPORTV1|", 9) == 0) {
            char *rest = l + 9;
            char *f2 = strchr(rest, '|');
            char *f3 = f2 ? strchr(f2 + 1, '|') : NULL;
            if (!f2 || !f3) { logf_("[!] bad EXPORTV1 line: %s", l); continue; }
            if (g_nv1 >= MAXJOBS) { logf_("[!] too many EXPORTV1 jobs"); continue; }
            *f2 = 0; *f3 = 0;
            u8_to_wide(trim(rest), g_v1_model[g_nv1], PRO_NAME_SIZE + 8);
            g_v1_asm[g_nv1] = (f2[1] == 'a' || f2[1] == 'A') ? 1 : 0;
            u8_to_wide(trim(f3 + 1), g_v1_out[g_nv1], PRO_PATH_SIZE);
            logf_("[i] EXPORTV1 %s type=%s -> %s", rest, g_v1_asm[g_nv1] ? "asm" : "prt", f3 + 1);
            g_nv1++;
        } else if (strncmp(l, "EXPORT2D|", 9) == 0) {
            char *rest = l + 9;
            char *q = strchr(rest, '|');
            if (!q) { logf_("[!] bad EXPORT2D line: %s", l); continue; }
            if (g_n2d >= MAXJOBS) { logf_("[!] too many EXPORT2D jobs"); continue; }
            *q = 0;
            u8_to_wide(trim(rest), g_2d_model[g_n2d], PRO_NAME_SIZE + 8);
            u8_to_wide(trim(q + 1), g_2d_out[g_n2d], PRO_PATH_SIZE);
            logf_("[i] EXPORT2D %s -> %s", rest, q + 1);
            g_n2d++;
        } else if (strncmp(l, "PARAMD|", 7) == 0) {
            char *rest = l + 7;
            char *fld[4];
            int   nf = 0;
            fld[nf++] = rest;
            while (nf < 4) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 2) { logf_("[!] bad PARAMD line: %s", l); continue; }
            if (g_npd >= MAXJOBS) { logf_("[!] too many PARAMD jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_pd_model[g_npd], PRO_NAME_SIZE + 8);
            g_pd_asm[g_npd] = (fld[1][0] == 'a' || fld[1][0] == 'A') ? 1 : 0;
            g_pd_base[g_npd][0] = 0;
            if (nf >= 3 && fld[2][0]) u8_to_wide(trim(fld[2]), g_pd_base[g_npd], PRO_PATH_SIZE);
            logf_("[i] PARAMD %s type=%s", fld[0], g_pd_asm[g_npd] ? "asm" : "prt");
            g_npd++;
        } else if (strncmp(l, "GETOPT|", 7) == 0 || strncmp(l, "SETOPT|", 7) == 0) {
            int is_set = (l[0] == 'S');
            char *rest = l + 7;
            char *q = strchr(rest, '|');
            optop_t *o;
            if (g_nopts >= MAXJOBS) { logf_("[!] too many opt ops, ignored: %s", l); continue; }
            o = &g_opts[g_nopts++];
            memset(o, 0, sizeof(*o));
            if (is_set && q) {
                *q = 0;
                u8_to_wide(trim(rest), o->name, 64);
                u8_to_wide(trim(q + 1), o->val, 512);
                o->is_set = 1;
                logf_("[i] SETOPT %s = %s", rest, q + 1);
            } else {
                u8_to_wide(trim(rest), o->name, 64);
                logf_("[i] GETOPT %s", rest);
            }
        } else if (strncmp(l, "MACRO|", 6) == 0) {
            if (g_nmacro >= MAXJOBS) { logf_("[!] too many MACRO jobs"); continue; }
            u8_to_wide(trim(l + 6), g_macro[g_nmacro], 1024);
            logf_("[i] MACRO %s", l + 6);
            g_nmacro++;
        } else if (strcmp(l, "CMDONLY") == 0) {
            /* CMDONLY: arm ONLY the runtime command channel. Does not touch any
               drawing - this is the "open the drawing, then drive it" mode.
               QUICKDRAW would overwrite the sheet, so it cannot be used for
               incremental edits of an already-saved drawing. */
            g_cmdonly = 1;
            logf_("[i] CMDONLY: command channel only, drawing left untouched");
        } else if (strncmp(l, "QUICKDRAW|", 10) == 0) {
            char *rest = l + 10;
            char *q = strchr(rest, '|');
            char *q2;
            if (!q) { logf_("[!] bad QUICKDRAW line: %s", l); continue; }
            if (g_nqd >= MAXJOBS) { logf_("[!] too many QUICKDRAW jobs"); continue; }
            *q = 0;
            u8_to_wide(trim(rest), g_qd_model[g_nqd], PRO_NAME_SIZE + 8);
            g_qd_asm[g_nqd] = (q[1] == 'a' || q[1] == 'A') ? 1 : 0;
            g_qd_wait[g_nqd] = 300;
            g_qd_defer[g_nqd] = 0;
            q2 = strchr(q + 1, '|');
            if (q2) {
                char *q3;
                int v = atoi(q2 + 1);
                if (v > 0 && v <= 3600) g_qd_wait[g_nqd] = v;
                q3 = strchr(q2 + 1, '|');
                if (q3 && (q3[1] == 'd' || q3[1] == 'D')) g_qd_defer[g_nqd] = 1;
            }
            logf_("[i] QUICKDRAW %s (type=%s wait=%ds defer=%d)", rest, g_qd_asm[g_nqd] ? "asm" : "prt",
                  g_qd_wait[g_nqd], g_qd_defer[g_nqd]);
            g_nqd++;
        } else if (strncmp(l, "DUMPNEW|", 8) == 0) {
            char *rest = l + 8;
            char *q = strchr(rest, '|');
            if (!q) { logf_("[!] bad DUMPNEW line: %s", l); continue; }
            if (g_ndn >= MAXJOBS) { logf_("[!] too many DUMPNEW jobs"); continue; }
            *q = 0;
            u8_to_wide(trim(rest), g_dn_dir[g_ndn], PRO_PATH_SIZE);
            u8_to_wide(trim(q + 1), g_dn_pfx[g_ndn], 128);
            logf_("[i] DUMPNEW %s \\ %s*", rest, q + 1);
            g_ndn++;
        } else if (strncmp(l, "DUMPPID|", 8) == 0) {
            char *rest = l + 8;
            char *q1 = strchr(rest, '|');
            char *q2 = q1 ? strchr(q1 + 1, '|') : NULL;
            if (!q1 || !q2) { logf_("[!] bad DUMPPID line: %s", l); continue; }
            if (g_ndp >= MAXJOBS) { logf_("[!] too many DUMPPID jobs"); continue; }
            *q1 = 0; *q2 = 0;
            u8_to_wide(trim(rest), g_dp_dir[g_ndp], PRO_PATH_SIZE);
            u8_to_wide(trim(q1 + 1), g_dp_pfx[g_ndp], 128);
            g_dp_pid[g_ndp] = atoi(q2 + 1);
            logf_("[i] DUMPPID %s \\ %s* pid=%d", rest, q1 + 1, g_dp_pid[g_ndp]);
            g_ndp++;
        } else {
            logf_("[!] unknown line: %s", l);
        }
    }
    free(buf);
    return (g_njobs > 0 || g_nverifies > 0 || g_ninterfs > 0 || g_nopts > 0 ||
            g_n2d > 0 || g_nv1 > 0 || g_nflat > 0 || g_ncnt > 0 || g_npm > 0 || g_npd > 0 ||
            g_nmacro > 0 || g_nqd > 0 || g_ndn > 0 || g_ndp > 0 || g_cmdonly > 0);
}

static void run_opt_ops(void)
{
    int i;
    for (i = 0; i < g_nopts; i++) {
        optop_t *o = &g_opts[i];
        ProPath cur;
        char nu8[256], cu8[PRO_PATH_SIZE * 4];
        ProError e;
        memset(cur, 0, sizeof(cur));
        wide_to_u8(o->name, nu8, sizeof(nu8));
        e = ProConfigoptionGet(o->name, cur);
        wide_to_u8(cur, cu8, sizeof(cu8));
        logf_("[opt] %s = '%s'  (get -> %d %s)", nu8,
              e == PRO_TK_NO_ERROR ? cu8 : "", (int)e, errname(e));
        if (o->is_set) {
            char vu8[PRO_PATH_SIZE * 4];
            ProError es = ProConfigoptSet(o->name, o->val);
            wide_to_u8(o->val, vu8, sizeof(vu8));
            logf_("[opt] set %s = %s  (set -> %d %s)", nu8, vu8, (int)es, errname(es));
            memset(cur, 0, sizeof(cur));
            e = ProConfigoptionGet(o->name, cur);
            wide_to_u8(cur, cu8, sizeof(cu8));
            logf_("[opt] %s now = '%s'  (get -> %d %s)", nu8,
                  e == PRO_TK_NO_ERROR ? cu8 : "", (int)e, errname(e));
        }
    }
}

static int retrieve_model(const wchar_t *name, ProMdlfileType ftype, ProMdl *p_mdl)
{
    int attempt;
    for (attempt = 1; attempt <= 4; attempt++) {
        ProError e = ProMdlnameRetrieve(name, ftype, p_mdl);
        logf_("  ProMdlnameRetrieve attempt %d -> %d (%s)", attempt, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR && *p_mdl) return 1;
        Sleep(5000);
    }
    return 0;
}

static int retrieve_with_retry(job_t *j, ProMdl *p_mdl)
{
    return retrieve_model(j->model, j->is_asm ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, p_mdl);
}

/* ---------------- 打平装配（FLATTEN�?---------------- */

static ProError flat_filter(ProFeature *feat, ProAppData app)
{
    ProFeattype t = (ProFeattype)0;
    if (ProFeatureTypeGet(feat, &t) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    return (t == PRO_FEAT_COMPONENT) ? PRO_TK_NO_ERROR : PRO_TK_CONTINUE;
}

static ProError flat_visit(ProFeature *feat, ProError status, ProAppData app)
{
    flatwalk_t *w = (flatwalk_t *)app;
    ProMdl      comp_mdl = NULL;
    ProMdlType  mt = (ProMdlType)0;

    if (!w || g_nleafs >= MAXLEAF) return PRO_TK_NO_ERROR;
    if (w->depth >= PRO_MAX_ASSEM_LEVEL) return PRO_TK_NO_ERROR;
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &comp_mdl) != PRO_TK_NO_ERROR || !comp_mdl) return PRO_TK_NO_ERROR;
    if (ProMdlTypeGet(comp_mdl, &mt) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;

    w->ids[w->depth] = feat->id;

    if (mt == PRO_MDL_ASSEMBLY) {
        if (w->depth == 0 && g_nsubids < MAXJOBS) g_subids[g_nsubids++] = feat->id;
        if (w->depth + 1 < PRO_MAX_ASSEM_LEVEL) {
            w->depth++;
            ProSolidFeatVisit((ProSolid)comp_mdl, flat_visit, flat_filter, w);
            w->depth--;
        }
    } else if (w->depth > 0) {
        /* 下层装配体里的零件：记下它在总装坐标系里的位置，稍后直接装到总装�?*/
        ProAsmcomppath path;
        ProMatrix     mat;
        memset(&path, 0, sizeof(path));
        if (ProAsmcomppathInit((ProSolid)w->top, w->ids, w->depth + 1, &path) == PRO_TK_NO_ERROR &&
            ProAsmcomppathTrfGet(&path, PRO_B_TRUE, mat) == PRO_TK_NO_ERROR) {
            g_leafs[g_nleafs].mdl = comp_mdl;
            memcpy(g_leafs[g_nleafs].mat, mat, sizeof(ProMatrix));
            g_nleafs++;
        } else {
            logf_("  [!] could not get position for a leaf at depth %d (id=%d)", w->depth, feat->id);
        }
    }
    return PRO_TK_NO_ERROR;
}

static ProError log_comp_visit(ProFeature *feat, ProError status, ProAppData app)
{
    ProMdl  m = NULL;
    ProName nm;
    char    u8[160];

    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &m) == PRO_TK_NO_ERROR && m) {
        nm[0] = 0;
        ProMdlNameGet(m, nm);
        wide_to_u8(nm, u8, sizeof(u8));
        logf_("      id=%-6d %s", feat->id, u8);
    }
    return PRO_TK_NO_ERROR;
}

static void log_top_comps(ProAssembly asm, const char *tag)
{
    logf_("  -- top-level components (%s) --", tag);
    ProSolidFeatVisit((ProSolid)asm, log_comp_visit, flat_filter, NULL);
}

static int run_flatten_job(int k)
{
    ProMdl    imp = NULL;
    ProError  e;
    ProMdlType mt = (ProMdlType)0;
    int       i, ok = 0;
    char      in8[PRO_PATH_SIZE * 4], out8[PRO_PATH_SIZE * 4], nm8[256];

    g_nleafs = 0;
    g_nsubids = 0;

    wide_to_u8(g_flat_in[k], in8, sizeof(in8));
    wide_to_u8(g_flat_out[k], out8, sizeof(out8));
    logf_("---- FLATTEN: %s", in8);
    logf_("     out=%s  profile=%s", out8, g_flat_hasprof[k] ? "(given)" : "(default)");
    if (g_flat_name[k][0]) {
        wide_to_u8(g_flat_name[k], nm8, sizeof(nm8));
        logf_("     import model name=%s", nm8);
    }

    /* 1) 导入 STEP（profile=NULL 用默认；rep=MASTER；先�?Creo 自己判断模型类型�?/
    e = ProIntfimportModelWithOptionsMdlnameCreate(g_flat_in[k], NULL, PRO_INTF_IMPORT_STEP,
            PRO_MDL_UNUSED, PRO_IMPORTREP_MASTER, g_flat_name[k], NULL, NULL, &imp);
    logf_("  import STEP (auto type, master rep) -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR || !imp) {
        e = ProIntfimportModelWithOptionsMdlnameCreate(g_flat_in[k], NULL, PRO_INTF_IMPORT_STEP,
                PRO_MDL_ASSEMBLY, PRO_IMPORTREP_MASTER, g_flat_name[k], NULL, NULL, &imp);
        logf_("  import STEP (forced assembly) -> %d (%s)", (int)e, errname(e));
    }
    if (e != PRO_TK_NO_ERROR || !imp) {
        e = ProIntfimportModelWithOptionsMdlnameCreate(g_flat_in[k], NULL, PRO_INTF_IMPORT_STEP,
                PRO_MDL_PART, PRO_IMPORTREP_MASTER, g_flat_name[k], NULL, NULL, &imp);
        logf_("  import STEP (forced part) -> %d (%s)", (int)e, errname(e));
    }
    if (e != PRO_TK_NO_ERROR || !imp) { logf_("  FLATTEN FAILED (import)"); return 0; }

    /* 2) 打平 */
    ProMdlTypeGet(imp, &mt);
    if (mt == PRO_MDL_ASSEMBLY) {
        flatwalk_t w;
        memset(&w, 0, sizeof(w));
        w.top = (ProAssembly)imp;
        w.depth = 0;

        log_top_comps((ProAssembly)imp, "before");
        ProSolidFeatVisit((ProSolid)imp, flat_visit, flat_filter, &w);
        logf_("  collected: leaf parts=%d, top-level sub-assemblies=%d", g_nleafs, g_nsubids);

        for (i = 0; i < g_nleafs; i++) {
            ProAsmcomp nc;
            ProName    nm;
            char       u8[160];
            ProError   ea;
            memset(&nc, 0, sizeof(nc));
            nm[0] = 0;
            ProMdlNameGet(g_leafs[i].mdl, nm);
            wide_to_u8(nm, u8, sizeof(u8));
            ea = ProAsmcompAssemble((ProAssembly)imp, (ProSolid)g_leafs[i].mdl, g_leafs[i].mat, &nc);
            logf_("  assemble %d/%d  %s -> %d (%s)", i + 1, g_nleafs, u8, (int)ea, errname(ea));
        }

        if (g_nsubids > 0) {
            ProError ed = ProFeatureDelete((ProSolid)imp, g_subids, g_nsubids, NULL, 0);
            logf_("  delete %d top-level sub-assembly component(s) -> %d (%s)", g_nsubids, (int)ed, errname(ed));
            if (ed != PRO_TK_NO_ERROR) {
                int *arr = NULL;
                if (ProArrayAlloc(0, sizeof(int), 1, (ProArray *)&arr) == PRO_TK_NO_ERROR) {
                    for (i = 0; i < g_nsubids; i++)
                        ProArrayObjectAdd((ProArray *)&arr, PRO_VALUE_UNUSED, 1, &g_subids[i]);
                    ed = ProFeatureWithoptionsDelete((ProSolid)imp, arr, NULL, PRO_REGEN_NO_FLAGS);
                    logf_("  retry delete via ProFeatureWithoptionsDelete -> %d (%s)", (int)ed, errname(ed));
                    ProArrayFree((ProArray *)&arr);
                }
            }
        }

        {
            ProError er = ProSolidRegenerate((ProSolid)imp, PRO_REGEN_NO_FLAGS);
            logf_("  regenerate -> %d (%s)", (int)er, errname(er));
        }
        log_top_comps((ProAssembly)imp, "after");
    } else {
        logf_("  (imported model is not an assembly - nothing to flatten)");
    }

    /* 3) �?profile 导出 */
    if (g_flat_hasprof[k]) {
        ProError ep = ProIntfExportProfileLoad(g_flat_prof[k]);
        logf_("  ProIntfExportProfileLoad -> %d (%s)", (int)ep, errname(ep));
    }
    e = ProIntf3DFileWriteWithDefaultProfile((ProSolid)imp, PRO_INTF_EXPORT_STEP, g_flat_out[k]);
    logf_("  export STEP -> %d (%s)", (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) { ok = 1; log_export_head(g_flat_out[k]); }
    return ok;
}

/* �?改一个数值型参数（PRO_PARAM_DOUBLE）—�?&MASS[.4] 这类带格式的注释引用需要数值参�?*/
static int set_num_param(ProMdl mdl, const wchar_t *pname, double val, const char *tag)
{
    ProModelitem  owner;
    ProParameter  param;
    ProParamvalue pv;
    ProError      e;

    if (ProMdlToModelitem(mdl, &owner) != PRO_TK_NO_ERROR) {
        logf_("  %s: ProMdlToModelitem failed", tag);
        return 0;
    }
    memset(&pv, 0, sizeof(pv));
    pv.type = PRO_PARAM_DOUBLE;
    pv.value.d_val = val;
    e = ProParameterInit(&owner, (wchar_t *)pname, &param);
    if (e == PRO_TK_NO_ERROR) {
        ProParamvalue cur;
        ProUnititem   units;
        memset(&cur, 0, sizeof(cur)); memset(&units, 0, sizeof(units));
        if (ProParameterValueWithUnitsGet(&param, &cur, &units) == PRO_TK_NO_ERROR &&
            cur.type != PRO_PARAM_DOUBLE) {
            logf_("  %s: existing type %d -> delete", tag, (int)cur.type);
            ProParameterDelete(&param);
            e = PRO_TK_E_NOT_FOUND;
        } else {
            e = ProParameterValueWithUnitsSet(&param, &pv, NULL);
            logf_("  %s: valueSet(double) -> %d (%s)", tag, (int)e, errname(e));
            return (e == PRO_TK_NO_ERROR) ? 1 : 0;
        }
    }
    if (e == PRO_TK_E_NOT_FOUND) {
        e = ProParameterWithUnitsCreate(&owner, (wchar_t *)pname, &pv, NULL, &param);
        logf_("  %s: create(double) -> %d (%s)", tag, (int)e, errname(e));
        return (e == PRO_TK_NO_ERROR) ? 1 : 0;
    }
    logf_("  %s: init -> %d (%s)", tag, (int)e, errname(e));
    return 0;
}

/* ---------------- 参数作业（PARAMDEL / PARAMSET�?---------------- */

/* 删除模型上的某个参数（用于把数值型参数改成字符型：先删后建�?*/
static int del_param(ProMdl mdl, const wchar_t *pname, const char *tag)
{
    ProModelitem owner;
    ProParameter  param;
    ProError      e;

    if (ProMdlToModelitem(mdl, &owner) != PRO_TK_NO_ERROR) {
        logf_("  %s: ProMdlToModelitem failed", tag);
        return 0;
    }
    e = ProParameterInit(&owner, (wchar_t *)pname, &param);
    if (e != PRO_TK_NO_ERROR) {
        logf_("  %s: ProParameterInit(%ls) -> %d (%s)", tag, pname, (int)e, errname(e));
        return 0;
    }
    e = ProParameterDelete(&param);
    logf_("  %s: ProParameterDelete -> %d (%s)", tag, (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* �?改一个字符型参数；type 固定 string（ProParameterWithUnitsCreate + PRO_PARAM_STRING�?*/
static int set_string_param(ProMdl mdl, const wchar_t *pname, const char *val8, const char *tag)
{
    ProModelitem  owner;
    ProParameter  param;
    ProParamvalue pv;
    ProUnititem   units;
    ProError      e;

    if (ProMdlToModelitem(mdl, &owner) != PRO_TK_NO_ERROR) {
        logf_("  %s: ProMdlToModelitem failed", tag);
        return 0;
    }
    memset(&pv, 0, sizeof(pv));
    memset(&units, 0, sizeof(units));
    pv.type = PRO_PARAM_STRING;
    u8_to_wide(val8, pv.value.s_val, PRO_LINE_SIZE);

    e = ProParameterInit(&owner, (wchar_t *)pname, &param);
    if (e == PRO_TK_NO_ERROR) {
        ProParamvalue cur;
        memset(&cur, 0, sizeof(cur));
        if (ProParameterValueWithUnitsGet(&param, &cur, &units) == PRO_TK_NO_ERROR &&
            cur.type != PRO_PARAM_STRING) {
            logf_("  %s: existing param is type %d (not string) -> delete first", tag, (int)cur.type);
            ProParameterDelete(&param);
            e = PRO_TK_E_NOT_FOUND;
        } else {
            e = ProParameterValueWithUnitsSet(&param, &pv, NULL);
            logf_("  %s: ProParameterValueWithUnitsSet -> %d (%s)", tag, (int)e, errname(e));
            return (e == PRO_TK_NO_ERROR) ? 1 : 0;
        }
    }
    e = ProParameterWithUnitsCreate(&owner, (wchar_t *)pname, &pv, NULL, &param);
    logf_("  %s: ProParameterWithUnitsCreate(string) -> %d (%s)", tag, (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* 赋材料：按名从材料库（search_path 里的 .mtl）读入并设为当前材料�?
   材料一旦赋上，图框�?&MSTD（材料）�?&MASS[.4]（重量）才有真值来源�?*/
static int assign_material_name(ProMdl mdl, const wchar_t *matname, const char *tag)
{
    ProSolid    part = (ProSolid)mdl;
    ProMaterial mat;
    ProName     nm;
    ProError    e;

    wcsncpy_s(nm, PRO_NAME_SIZE, matname, _TRUNCATE);
    e = ProMaterialfileRead(part, nm);
    logf_("  %s: ProMaterialfileRead(%ls) -> %d (%s)", tag, nm, (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;
    e = ProMaterialCurrentGet(part, &mat);
    logf_("  %s: ProMaterialCurrentGet -> %d (%s)", tag, (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;
    e = ProMaterialCurrentSet(&mat);
    logf_("  %s: ProMaterialCurrentSet -> %d (%s)", tag, (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

static int run_param_job(int k)
{
    ProMdl mdl = NULL;
    char   m8[128], n8[PRO_LINE_SIZE * 2];
    int    ok = 0;

    wide_to_u8(g_pm_model[k], m8, sizeof(m8));
    wide_to_u8(g_pm_name[k], n8, sizeof(n8));
    logf_("---- PARAM %s : %s%s%s", m8, n8,
          g_pm_is_set[k] ? " = " : " (delete)", g_pm_is_set[k] ? g_pm_val8[k] : "");

    if (!retrieve_model(g_pm_model[k], g_pm_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  PARAM FAILED (retrieve)");
        return 0;
    }
    if (g_pm_del_first[k]) {
        del_param(mdl, g_pm_name[k], "pre-delete");
    }
    if (g_pm_is_set[k]) {
        ok = set_string_param(mdl, g_pm_name[k], g_pm_val8[k], "set");
    } else {
        ok = del_param(mdl, g_pm_name[k], "delete");
    }
    if (ok) {
        ProError es = ProMdlSave(mdl);
        logf_("  ProMdlSave -> %d (%s)", (int)es, errname(es));
        if (es != PRO_TK_NO_ERROR) ok = 0;
    }
    return ok;
}

/* ---------------- 参数表导出作业（PARAMD�?---------------- */

/* 把文件内容按行打到日志（在本进程内读；兼�?UTF-16LE / UTF-8 / GBK�?*/
static void dump_text_file(const char *path, const char *tag)
{
    FILE          *f = wfopen_utf8(path, L"rb");
    long           sz;
    size_t         got;
    unsigned char *raw;

    if (!f) { logf_("  [%s] cannot open %s", tag, path); return; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8L * 1024 * 1024) { logf_("  [%s] bad size %ld: %s", tag, sz, path); fclose(f); return; }
    raw = (unsigned char *)malloc((size_t)sz + 2);
    if (!raw) { fclose(f); return; }
    got = fread(raw, 1, (size_t)sz, f);
    raw[got] = 0; raw[got + 1] = 0;
    fclose(f);

    logf_("  ---- %s : %s (%ld bytes) ----", tag, path, sz);

    {
        wchar_t *wbuf = NULL;
        char    *u8buf = NULL;
        int      wn = 0;

        if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
            int n = (int)((got - 2) / 2), i;
            wbuf = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
            if (wbuf) {
                for (i = 0; i < n; i++) wbuf[i] = (wchar_t)(raw[2 + i * 2] | (raw[3 + i * 2] << 8));
                wbuf[n] = 0;
                wn = n;
            }
        } else {
            unsigned char *p = raw;
            size_t         len = got;
            if (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) { p = raw + 3; len = got - 3; }
            wbuf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
            if (wbuf) {
                wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)p, (int)len, wbuf, (int)len);
                if (wn <= 0)
                    wn = MultiByteToWideChar(CP_ACP, 0, (const char *)p, (int)len, wbuf, (int)len);
                if (wn < 0) wn = 0;
                wbuf[wn] = 0;
            }
        }

        if (wbuf && wn > 0) {
            u8buf = (char *)malloc((size_t)wn * 4 + 16);
            if (u8buf) {
                char *s, *line, *q;
                int   bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, wn, u8buf, (int)((size_t)wn * 4 + 8), NULL, NULL);
                if (bytes >= 0) u8buf[bytes] = 0;
                s = u8buf; line = s;
                for (q = s; ; q++) {
                    if (*q == '\n' || *q == 0) {
                        char save = *q;
                        *q = 0;
                        if (*line) logf_("    %s", line);
                        if (save == 0) break;
                        line = q + 1;
                    }
                }
            }
        } else {
            logf_("  [%s] (could not decode content)", tag);
        }
        if (wbuf) free(wbuf);
        if (u8buf) free(u8buf);
    }
    free(raw);
}

/* PARAMD：导出“工�?> 参数”那张表（CSV 指定�?+ TXT 会话列），并读回打印 */
static int run_paramd_job(int k)
{
    ProMdl         mdl = NULL;
    ProModelitem   owner;
    ProError       e;
    char           m8[128];
    wchar_t        base[PRO_PATH_SIZE];
    ProPath        csv_p, txt_p;
    char           csv8[PRO_PATH_SIZE * 4], txt8[PRO_PATH_SIZE * 4];
    ProParamColumn cols = (ProParamColumn)(
        PRO_PARAM_NAME_COLUMN | PRO_PARAM_TYPE_COLUMN | PRO_PARAM_VALUE_COLUMN |
        PRO_PARAM_DESIGNATE_COLUMN | PRO_PARAM_ACCESS_COLUMN | PRO_PARAM_SOURCE_COLUMN |
        PRO_PARAM_DESCRIPTION_COLUMN | PRO_PARAM_UNIT_COLUMN | PRO_PARAM_OWNER_COLUMN |
        PRO_PARAM_OWNER_TYPE_COLUMN | PRO_PARAM_REPORTINBOM_COLUMN);

    wide_to_u8(g_pd_model[k], m8, sizeof(m8));
    logf_("---- PARAMD %s (type=%s)", m8, g_pd_asm[k] ? "asm" : "prt");

    if (!retrieve_model(g_pd_model[k], g_pd_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  PARAMD FAILED (retrieve)");
        return 0;
    }
    if (ProMdlToModelitem(mdl, &owner) != PRO_TK_NO_ERROR) {
        logf_("  PARAMD FAILED (ProMdlToModelitem)");
        return 0;
    }

    if (g_pd_base[k][0]) swprintf_s(base, PRO_PATH_SIZE, L"%ls", g_pd_base[k]);
    else                 swprintf_s(base, PRO_PATH_SIZE, L"paramd_%ls", g_pd_model[k]);

    swprintf_s(csv_p, PRO_PATH_SIZE, L"%ls.csv", base);
    swprintf_s(txt_p, PRO_PATH_SIZE, L"%ls.txt", base);

    e = ProParameterTableExport(&owner, mdl, 0, PRO_PARAMTABLE_EXPORT_CSV, csv_p, cols);
    wide_to_u8(csv_p, csv8, sizeof(csv8));
    logf_("  export CSV -> %d (%s)  %s", (int)e, errname(e), csv8);
    if (e == PRO_TK_NO_ERROR) dump_text_file(csv8, "CSV");

    e = ProParameterTableExport(&owner, mdl, 0, PRO_PARAMTABLE_EXPORT_TXT, txt_p, cols);
    wide_to_u8(txt_p, txt8, sizeof(txt8));
    logf_("  export TXT -> %d (%s)  %s", (int)e, errname(e), txt8);
    if (e == PRO_TK_NO_ERROR) dump_text_file(txt8, "TXT");

    return 1;
}

/* ---------------- �?命令作业（MACRO / QUICKDRAW�?---------------- */

/* 加载并执行一条命令宏（与按下 mapkey 等价），例如 L"~ Command `SDCurrentDrawing`;" */
static int run_macro_text(const wchar_t *macro, const char *tag)
{
    ProError e;

    e = ProMacroLoad((wchar_t *)macro);
    logf_("  %s: ProMacroLoad -> %d (%s)", tag, (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;
    e = ProMacroExecute();
    logf_("  %s: ProMacroExecute -> %d (%s)", tag, (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* 轮询等待 <cwd>/<模型>.pdf 出现（PiExport 默认�?PDF），出现后打印明文大�?文件�?*/
static int wait_and_check_pdf(const wchar_t *model, int wait_sec)
{
    wchar_t path[PRO_PATH_SIZE];
    char    p8[PRO_PATH_SIZE * 4];
    int     i;

    swprintf_s(path, PRO_PATH_SIZE, L"%ls.pdf", model);
    wide_to_u8(path, p8, sizeof(p8));
    for (i = 0; i <= wait_sec; i += 5) {
        FILE *f = _wfopen(path, L"rb");
        if (f) {
            fclose(f);
            logf_("  [quickdraw] output found after ~%ds: %s", i, p8);
            log_file_head(p8);
            return 1;
        }
        /* 2026-11-03: PDF export is disabled by pdfgate (R0-4), so a PDF can never
           appear. Treat a written <model>.drw* as the real done marker. */
        {
            wchar_t pat[PRO_PATH_SIZE + 8];
            WIN32_FIND_DATAW fd;
            HANDLE h;
            swprintf_s(pat, PRO_PATH_SIZE + 8, L"%ls.drw*", model);
            h = FindFirstFileW(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                wide_to_u8(pat, p8, sizeof(p8));
                logf_("  [quickdraw] drawing file found after ~%ds: %s (%lu bytes)", i, p8, fd.nFileSizeLow);
                return 1;
            }
        }
        Sleep(5000);
    }
    logf_("  [quickdraw] output NOT found after %ds: %s", wait_sec, p8);
    return 0;
}

/* ---------------- 延迟触发定时器（主线程消息泵内回调） ---------------- */

static void def_write_marker(const char *text)
{
    FILE *mf = fopen("sync_export_done.txt", "w");
    logf_("[defer] done-marker: %s", text);
    if (mf) { fprintf(mf, "%s\n", text); fclose(mf); }
}

/* ---------------- front 定向视图 / PDF 导出 / 运行时命令通道 ---------------- */

/* 若模型没有名�?front 的定向视图，则按“最短轴=板法向”自动创建一个（仅内存，不保存模型）�?
   快速出图的主视图由 stk_drawing_config.ini �?STK_MAIN_VIEW=front|XY|Z 决定�?
   �?front 视图就用它，否则退�?XY|Z —�?这正是此前�?D 摆放不正”的原因�?*/
static void ensure_front_view(ProMdl mdl)
{
    ProLine *names = NULL, *alts = NULL;
    int cnt = 0, i, found = 0;
    ProError e;
    Pro3dPnt box[2];
    double sx, sy, sz;
    int n, a, b, flipped;
    ProMatrix m;
    ProView v = NULL;

    e = ProViewNamesGet(mdl, &names, &alts, &cnt);
    logf_("  [front] ProViewNamesGet -> %d (%s), count=%d", (int)e, errname(e), cnt);
    if (e == PRO_TK_NO_ERROR) {
        for (i = 0; i < cnt; i++) {
            char v8[200];
            wide_to_u8(names[i], v8, sizeof(v8));
            logf_("  [front]   view[%d] = '%s'", i, v8);
            if (!_wcsicmp(names[i], L"front")) found = 1;
        }
        /* 注意：此处不调用 ProWstringArrayFree —�?实测（强转类型桥接）会触�?
           Creo fatal error（Auto-traceback 弹窗、进程挂起）。泄漏几十字节可忽略�?*/
    }
    if (found) {
        /* Keep whatever 'front' the model carries. Verified 2026-10-29: for the
           nb109 family this stored 'front' is the one quick-draw wants - the
           three-view result (front / left / top) was correct and the top view
           showed all six bosses with their tangent edges. Forcing a
           "largest-face" matrix here instead collapsed quick-draw to a single
           main_view at 1:2, so do NOT re-derive. */
        logf_("  [front] model already has a 'front' view; keep it");
        return;
    }

    logf_("  [front] outline start");
    e = ProSolidOutlineGet((ProSolid)mdl, box);
    logf_("  [front] outline done -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) {
        logf_("  [front] ProSolidOutlineGet failed; cannot auto-create");
        return;
    }
    sx = box[1][0] - box[0][0];
    sy = box[1][1] - box[0][1];
    sz = box[1][2] - box[0][2];
    logf_("  [front] bbox = %.2f x %.2f x %.2f", sx, sy, sz);

    /* 最短尺寸轴 = 板件法向(朝观察�?；面内较长轴 = 屏幕水平方向 */
    if (sx <= sy && sx <= sz)      { n = 0; a = (sy >= sz) ? 1 : 2; }
    else if (sy <= sx && sy <= sz) { n = 1; a = (sx >= sz) ? 0 : 2; }
    else                           { n = 2; a = (sx >= sy) ? 0 : 1; }
    b = 3 - n - a;

    /* 右手系：(a,b,n) 为循环正序时不翻转，否则把上方向取反 */
    flipped = !((a == 0 && b == 1 && n == 2) ||
                (a == 1 && b == 2 && n == 0) ||
                (a == 2 && b == 0 && n == 1));

    memset(m, 0, sizeof(m));
    m[0][a] = 1.0;
    m[1][b] = flipped ? -1.0 : 1.0;
    m[2][n] = 1.0;
    m[3][3] = 1.0;

    {
        /* 优先拿到模型里已有的定向视图句柄作为设置目标（如 DEFAULT），
           拿不到则退回“当前视�?NULL)�?*/
        ProView v0 = NULL;
        int    got = 0;
        for (i = 0; i < cnt; i++) {
            if (!_wcsicmp(names[i], L"DEFAULT")) {
                if (ProViewNameToView(mdl, names[i], &v0) == PRO_TK_NO_ERROR && v0) got = 1;
                break;
            }
        }
        logf_("  [front] matrix target = %s", got ? "DEFAULT handle" : "current(NULL)");
        e = ProViewMatrixSet(mdl, got ? v0 : NULL, m);
        logf_("  [front] ProViewMatrixSet(n=%d a=%d b=%d flip=%d) -> %d (%s)",
              n, a, b, flipped, (int)e, errname(e));
    }
    if (e != PRO_TK_NO_ERROR) return;

    logf_("  [front] store start");
    e = ProViewStore(mdl, (wchar_t *)L"front", &v);
    logf_("  [front] ProViewStore('front') -> %d (%s)", (int)e, errname(e));
    /* 不调�?ProMdlSave：视图仅存内存，磁盘上的模型文件不变 */
}

/* 导出 PDF（模�?图纸或零件；输出到当前目�?<base>.pdf�?*/
static void export_pdf_model(ProMdl mdl, const wchar_t *base)
{
    ProPDFOptions opts = NULL;
    ProPath out;
    ProError e;

    swprintf_s(out, PRO_PATH_SIZE, L"%ls.pdf", base);
    e = ProPDFoptionsAlloc(&opts);
    logf_("  [pdf] ProPDFoptionsAlloc -> %d (%s)", (int)e, errname(e));
    e = ProPDFExport(mdl, out, opts);
    {
        char o8[PRO_PATH_SIZE * 2];
        wide_to_u8(out, o8, sizeof(o8));
        logf_("  [pdf] ProPDFExport -> %d (%s) -> %s", (int)e, errname(e), o8);
    }
    if (opts) ProPDFoptionsFree(opts);
}

/* 保存图纸前“让路”：把目标目录里已存在的 <model>.drw.* 移到 work\old\ 归档�?
   背景：磁盘上残留�?drw 版本文件（如异常退�?恢复会话留下的）会让
   ProMdlSave 在“同名已存在”场景下无限等待（实测挂起）。移开后即�?14:00 成功场景�?*/
static void clear_drw_targets(const wchar_t *dir, const wchar_t *model)
{
    wchar_t pat[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf_s(pat, PRO_PATH_SIZE, L"%ls\\%ls.drw.*", dir, model);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("[clear] no existing drw targets under %ls", dir);
        return;
    }
    do {
        wchar_t src[PRO_PATH_SIZE], dst[PRO_PATH_SIZE];
        SYSTEMTIME st;
        char    a8[PRO_PATH_SIZE * 2];
        GetLocalTime(&st);
        swprintf_s(src, PRO_PATH_SIZE, L"%ls\\%ls", dir, fd.cFileName);
        swprintf_s(dst, PRO_PATH_SIZE,
                   L"D:\\creo-agent\\sync-toolkit\\work\\old\\%ls.used-%04d%02d%02d-%02d%02d%02d",
                   fd.cFileName, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        wide_to_u8(fd.cFileName, a8, sizeof(a8));
        if (MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED)) {
            logf_("[clear] moved aside: %s", a8);
        } else {
            logf_("[clear] move FAILED (locked?): %s err=%lu", a8, GetLastError());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 激活图纸窗口：让交互式命令（SDAutoDimension 等）以图纸为当前窗口 */
static void activate_drawing_window(ProMdl drw)
{
    ProError e;
    int      wid = -1;

    e = ProMdlDisplay(drw);
    logf_("  [win] ProMdlDisplay(drw) -> %d (%s)", (int)e, errname(e));

    e = ProWindowCurrentGet(&wid);
    if (e == PRO_TK_NO_ERROR && wid > 0) {
        ProMdl wm = NULL;
        if (ProWindowMdlGet(wid, &wm) == PRO_TK_NO_ERROR) {
            logf_("  [win] current window %d holds %s", wid,
                  (wm == drw) ? "the DRAWING" : "another model");
            if (wm == drw) {
                e = ProWindowActivate(wid);
                logf_("  [win] ProWindowActivate -> %d (%s)", (int)e, errname(e));
                e = ProWindowCurrentSet(wid);
                logf_("  [win] ProWindowCurrentSet -> %d (%s)", (int)e, errname(e));
            }
        }
    } else {
        logf_("  [win] ProWindowCurrentGet -> %d (%s)", (int)e, errname(e));
    }
}

/* 技术要求注释：按样本风格用 ProDtlnote API 直接写入图纸（左下角）�?
   中文文本在中文语言的会话里渲染/导出正常（与 PDF 字体问题同源）�?
   轮次58：位置参数化（DSH：技术要求贴图框左下角、标题栏左侧空白区，不悬空）�?*/
static void add_technote_at(ProMdl drw, double px, double py)
{
    static const wchar_t *LINES[] = {
        L"技术要求",
        L"1. 表面需光洁、无划痕、油污、无明显变形，毛刺高度<0.05，冲裁方向与凸点方向相反",
        L"2. 材料硬度为 120-170HV",
        L"3. 所有增加自动点焊凸点处，除凸点外镜片应平整，不可起翘变形从而影响自动点焊效果",
        L"4. 能通过8小时5%浓度盐雾测试"
    };
    ProDtlnotedata data = NULL;
    ProDtlattach   attach = NULL;
    ProDtlnote     note;   /* struct type (ProModelitem family), not a pointer */
    ProVector      loc;
    ProError       e;
    int i;

    /* location is in SCREEN client-area pixels (origin lower-left), converted
       by Creo to sheet space with the live view scale.
       轮次58：默认落在图框左下角（标题栏左侧空白区）；可�?TECHNOTE|x|y 复调�?*/
    loc[0] = px;
    loc[1] = py;
    loc[2] = 0.0;

    e = ProDtlnotedataAlloc(drw, &data);
    logf_("  [tech] dataAlloc -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return;

    for (i = 0; i < 5; i++) {
        ProDtlnotetext text = NULL;
        ProDtlnoteline line = NULL;
        ProLine        s;

        wcsncpy_s(s, PRO_LINE_SIZE, LINES[i], _TRUNCATE);
        e = ProDtlnotetextAlloc(&text);
        if (e != PRO_TK_NO_ERROR) { logf_("  [tech] textAlloc[%d] -> %d", i, (int)e); break; }
        e = ProDtlnotetextStringSet(text, s);
        if (e != PRO_TK_NO_ERROR) { logf_("  [tech] stringSet[%d] -> %d", i, (int)e); break; }
        e = ProDtlnotelineAlloc(&line);
        if (e != PRO_TK_NO_ERROR) { logf_("  [tech] lineAlloc[%d] -> %d", i, (int)e); break; }
        e = ProDtlnotelineTextAdd(line, text);
        if (e != PRO_TK_NO_ERROR) { logf_("  [tech] textAdd[%d] -> %d", i, (int)e); break; }
        e = ProDtlnotedataLineAdd(data, line);
        if (e != PRO_TK_NO_ERROR) { logf_("  [tech] lineAdd[%d] -> %d", i, (int)e); break; }
    }
    logf_("  [tech] %d line(s) built", i);

    e = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &attach);
    logf_("  [tech] attachAlloc -> %d (%s)", (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) {
        e = ProDtlnotedataAttachmentSet(data, attach);
        logf_("  [tech] attachSet -> %d (%s)", (int)e, errname(e));
    }

    e = ProDtlnotedataDisplayedSet(data, PRO_B_TRUE);
    logf_("  [tech] DisplayedSet(TRUE) -> %d (%s)", (int)e, errname(e));

    e = ProDtlnoteCreate(drw, NULL, data, &note);
    logf_("  [tech] noteCreate -> %d (%s)", (int)e, errname(e));

    e = ProDwgSheetRegenerate((ProDrawing)drw, 1);
    logf_("  [tech] sheetRegenerate -> %d (%s)", (int)e, errname(e));

    if (attach) ProDtlattachFree(attach);
    ProDtlnotedataFree(data);
    logf_("  [tech] done");
}

/* 轮次58：TECHNOTE|x|y —�?�?技术要�?注释块移�?(x,y)（屏幕像素，origin 左下）�?
   做法：收集图纸注�?�?找到�?技术要�?行的 �?删旧 �?在同参数位重建；
   若一张也没有则直接新建。幂等：重复发同一位置不会产生第二块�?*/
/* r61: set the line-style "font" on every draft entity (the 12 cross centerlines),
   modify + read back from the DB; candidates probed from the command channel. */
static void sym_dump_vartexts(ProDtlsyminstdata data, const char *tag);   /* fwd (defined later) */
static ProView sym_front_view(ProMdl drw);                               /* fwd (defined later) */


/* probe: load (not execute) a Creo UI command macro to test whether the name exists */
static void cmdprobe_run(const wchar_t *name)
{
    wchar_t  buf[512];
    ProError e;

    swprintf_s(buf, 512, L"~ Command `%ls`;", name);
    e = ProMacroLoad(buf);
    logf_("[probe] load \x27%ls\x27 - (%s)", name, (int)e, errname(e));
}

/* r63: SNOTE|x|y|text - one-line note at screen px (screen coords, origin lower-left) */
static void snote_run(ProMdl drw, double x, double y, const wchar_t *txt, double height)
{
    ProDtlnotedata data = NULL;
    ProDtlattach   attach = NULL;
    ProDtlnote     note;
    ProVector      loc;
    ProError       e;

    if (ProDtlnotedataAlloc(drw, &data) != PRO_TK_NO_ERROR || !data) return;
    {
        ProDtlnotetext text = NULL;
        ProDtlnoteline line = NULL;
        ProLine        s;

        wcsncpy_s(s, PRO_LINE_SIZE, txt, _TRUNCATE);
        e = ProDtlnotetextAlloc(&text);
        if (e == PRO_TK_NO_ERROR) e = ProDtlnotetextStringSet(text, s);
        if (e == PRO_TK_NO_ERROR) e = ProDtlnotelineAlloc(&line);
        if (e == PRO_TK_NO_ERROR) e = ProDtlnotelineTextAdd(line, text);
        if (e == PRO_TK_NO_ERROR) e = ProDtlnotedataLineAdd(data, line);
        logf_("[snote] build line -> %d (%s)", (int)e, errname(e));
    }
    loc[0] = x; loc[1] = y; loc[2] = 0.0;
    if (ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &attach) == PRO_TK_NO_ERROR && attach)
        ProDtlnotedataAttachmentSet(data, attach);
    if (height > 0.5) {
        ProTextStyle st = NULL;

        if (ProTextStyleAlloc(&st) == PRO_TK_NO_ERROR && st) {
            logf_("[snote] style heightSet %.2f -> %d", height, (int)ProTextStyleHeightSet(st, height));
            logf_("[snote] dataStyleSet -> %d", (int)ProDtlnotedataTextStyleSet(data, st));
            {
                double hh = -1.0;

                if (ProTextStyleHeightGet(st, &hh) == PRO_TK_NO_ERROR)
                    logf_("[snote] style height readback = %.2f", hh);
            }
        }
    } else {
        logf_("[snote] height<=0: use drawing text_height default");
    }
    ProDtlnotedataDisplayedSet(data, PRO_B_TRUE);
    e = ProDtlnoteCreate(drw, NULL, data, &note);
    logf_("[snote] create @(%.1f,%.1f) -> %d (%s)", x, y, (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) {
        ProTextStyle gs = NULL;

        if (ProDtlnotedataTextStyleGet(data, &gs) == PRO_TK_NO_ERROR && gs) {
            double hh = -1.0;

            if (ProTextStyleHeightGet(gs, &hh) == PRO_TK_NO_ERROR)
                logf_("[snote] effective style height = %.2f", hh);
            ProTextStyleFree(&gs);
        }
    }
    logf_("[snote] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
    if (attach) ProDtlattachFree(attach);
    ProDtlnotedataFree(data);
}

/* r63: dump the symbol-definition group tree (ids + names) */
static void grp_dump_rec(ProDtlsymdef *def, int parent_id, int level)
{
    ProDtlsymgroup  parent;
    ProDtlsymgroup *subs = NULL;
    int n = 0, i;

    if (level > 3) return;
    memset(&parent, 0, sizeof(parent));
    parent.symbol_def = *def;
    parent.sym_group_id = parent_id;
    if (ProDtlsymgroupSubgroupsCollect(&parent, &subs) != PRO_TK_NO_ERROR || !subs) return;
    ProArraySizeGet((ProArray)subs, &n);
    for (i = 0; i < n && i < 48; i++) {
        ProDtlsymgroupdata gd = NULL;
        ProName          nm;
        char             b8[PRO_NAME_SIZE * 4];

        nm[0] = 0;
        if (ProDtlsymgroupDataGet(&subs[i], &gd) == PRO_TK_NO_ERROR && gd)
            ProDtlsymgroupdataNameGet(gd, nm);
        b8[0] = 0;
        wide_to_u8(nm, b8, sizeof(b8));
        logf_("[grp] L%d parent=%d id=%d name='%s'", level, parent_id, subs[i].sym_group_id, b8);
        grp_dump_rec(def, subs[i].sym_group_id, level + 1);
    }
}

/* r63: retrieve WITSURFTEXT; dump groups; dump vars after GROUP_ALL */
static void roughgrp_run(ProMdl drw, const wchar_t *cat, const wchar_t *name)
{
    ProDtlsymdef def;
    ProError     er = PRO_TK_E_NOT_FOUND;
    int          ci, nc = 0;
    struct { const wchar_t *grp; const wchar_t *nm; int ver; } cand[7];

    if (name && name[0]) {
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = 1;
        nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = 0;
        nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = (int)PRO_VALUE_UNUSED;
        nc++;
    }
    cand[nc].grp = L""; cand[nc].nm = L"WITSURFTEXT"; cand[nc].ver = 1; nc++;
    cand[nc].grp = L""; cand[nc].nm = L"witsurftext"; cand[nc].ver = 1; nc++;
    cand[nc].grp = L""; cand[nc].nm = L"witsurftext"; cand[nc].ver = 0; nc++;

    memset(&def, 0, sizeof(def));
    {
        for (ci = 0; ci < nc; ci++) {
            memset(&def, 0, sizeof(def));
            er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)cand[ci].grp, (wchar_t *)cand[ci].nm, cand[ci].ver, PRO_B_TRUE, &def);
            logf_("[grp] retrieve '%ls'/'%ls' ver=%d -> %d (%s)", cand[ci].grp, cand[ci].nm, cand[ci].ver, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR) break;
        }
    }
    if (er != PRO_TK_NO_ERROR) return;
    logf_("[grp] ==== group tree ====");
    grp_dump_rec(&def, -1, 0);
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) == PRO_TK_NO_ERROR && id) {
            ProDtlsyminstdataDefSet(id, &def);
            logf_("[grp] groupoption ALL -> %d", (int)ProDtlsyminstdataGroupoptionsSet(id, PRO_DTLSYMINST_GROUP_ALL, NULL));
            {
                ProDtlvartext *vts = NULL;
                ProError       ev = ProDtlsyminstdataVartextsCollect(id, &vts);

                logf_("[grp] vartextsCollect(all) -> %d (%s)", (int)ev, errname(ev));
                if (ev == PRO_TK_NO_ERROR && vts) {
                    int n = 0, i;

                    ProArraySizeGet((ProArray)vts, &n);
                    logf_("[grp] var count=%d", n);
                    for (i = 0; i < n; i++) {
                        ProLine pr, va;
                        char    a8[512], b8[512];

                        pr[0] = 0; va[0] = 0;
                        if (ProDtlvartextDataGet(vts[i], pr, va) == PRO_TK_NO_ERROR) {
                            a8[0] = 0; b8[0] = 0;
                            wide_to_u8(pr, a8, sizeof(a8));
                            wide_to_u8(va, b8, sizeof(b8));
                            logf_("[grp] vt[%d] prompt='%s' value='%s'", i, a8, b8);
                        }
                    }
                }
            }
        }
    }
}

/* r63: create WITSURFTEXT with group selection: groups = "ALL" or "CUSTOM:id1,id2,..." */
static int g_rough_set_mode = 0;

static void roughsym_run(ProMdl drw, double x, double y, const wchar_t *value, const wchar_t *method, const wchar_t *groups, double height, const wchar_t *cat, const wchar_t *name)
{
    ProDtlsymdef def;
    ProError     er = PRO_TK_E_NOT_FOUND;
    int          ci, nc = 0;
    struct { const wchar_t *grp; const wchar_t *nm; int ver; } cand[7];

    memset(&def, 0, sizeof(def));
    if (name && name[0]) {
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = 1;
        nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = 0;
        nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L"";
        cand[nc].nm  = name;
        cand[nc].ver = (int)PRO_VALUE_UNUSED;
        nc++;
    }
    cand[nc].grp = L""; cand[nc].nm = L"WITSURFTEXT"; cand[nc].ver = 1; nc++;
    cand[nc].grp = L""; cand[nc].nm = L"witsurftext"; cand[nc].ver = 1; nc++;
    cand[nc].grp = L""; cand[nc].nm = L"witsurftext"; cand[nc].ver = 0; nc++;
    {
        for (ci = 0; ci < nc; ci++) {
            memset(&def, 0, sizeof(def));
            er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)cand[ci].grp, (wchar_t *)cand[ci].nm, cand[ci].ver, PRO_B_TRUE, &def);
            logf_("[rough] retrieve '%ls'/'%ls' ver=%d -> %d (%s)", cand[ci].grp, cand[ci].nm, cand[ci].ver, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR) break;
        }
    }
    if (er != PRO_TK_NO_ERROR) return;
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) != PRO_TK_NO_ERROR || !id) { logf_("[rough] data alloc failed"); return; }
        ProDtlsyminstdataDefSet(id, &def);
        if (groups && !wcsncmp(groups, L"CUSTOM:", 7)) {
            int ids[16], nid = 0;
            const wchar_t *q = groups + 7;

            while (*q && nid < 16) {
                ids[nid++] = (int)wcstol(q, (wchar_t **)&q, 10);
                if (*q == L',') q++;
            }
            {
                ProDtlsymgroup *arr = NULL;
                int i;

                ProArrayAlloc(nid ? nid : 1, sizeof(ProDtlsymgroup), 1, (ProArray *)&arr);
                for (i = 0; i < nid; i++) {
                    memset(&arr[i], 0, sizeof(ProDtlsymgroup));
                    arr[i].symbol_def = def;
                    arr[i].sym_group_id = ids[i];
                }
                logf_("[rough] groupoption CUSTOM n=%d ids=%ls -> %d", nid, groups + 7,
                      (int)ProDtlsyminstdataGroupoptionsSet(id, PRO_DTLSYMINST_GROUP_CUSTOM, arr));
            }
        } else {
            logf_("[rough] groupoption ALL -> %d", (int)ProDtlsyminstdataGroupoptionsSet(id, PRO_DTLSYMINST_GROUP_ALL, NULL));
        }
        {
            ProDtlvartext *vts = NULL;
            ProError       ev = ProDtlsyminstdataVartextsCollect(id, &vts);

            logf_("[rough] vartextsCollect(group) -> %d (%s)", (int)ev, errname(ev));
            if (ev == PRO_TK_NO_ERROR && vts) {
                int n = 0, i;

                ProArraySizeGet((ProArray)vts, &n);
                logf_("[rough] var count=%d", n);
                for (i = 0; i < n; i++) {
                    ProLine pr, va;
                    char    a8[512], b8[512];

                    pr[0] = 0; va[0] = 0;
                    if (ProDtlvartextDataGet(vts[i], pr, va) == PRO_TK_NO_ERROR) {
                        ProDtlvartext nv = NULL;
                        wchar_t      lo32[128];
                        int          k, isVal = 0, isMeth = 0;

                        a8[0] = 0; b8[0] = 0;
                        wide_to_u8(pr, a8, sizeof(a8));
                        wide_to_u8(va, b8, sizeof(b8));
                        logf_("[rough] vt[%d] prompt='%s' value='%s'", i, a8, b8);
                        for (k = 0; k < 127 && pr[k]; k++) lo32[k] = (wchar_t)towlower(pr[k]);
                        lo32[k] = 0;
                        if (wcsstr(lo32, L"value") || wcsstr(lo32, L"d_val") || wcsstr(lo32, L"roughness")) isVal = 1;
                        if (wcsstr(lo32, L"method") || wcsstr(lo32, L"prod")) isMeth = 1;
                        {
                            const wchar_t *setv = NULL;

                            if (isVal && value && value[0]) setv = value;
                            if (isMeth && method && method[0]) setv = method;
                            if (setv && ProDtlvartextAlloc((wchar_t *)pr, (wchar_t *)setv, &nv) == PRO_TK_NO_ERROR && nv) {
                                ProError ea = ProDtlsyminstdataVartextAdd(id, nv);

                                logf_("[rough] set prompt[%d] -> add %d (%s)", i, (int)ea, errname(ea));
                            }
                        }
                    }
                }
            }
        }
        {
        /* r63: the group-scoped collect is empty before create; set the REAL prompts directly */
        if (value && value[0]) {
            static const wchar_t *gp[6] = { L"aver_roughness", L"max_roughness", L"min_roughness", L"other_roughness",
                                            L"rough_ip_1", L"rough_ip_2" };
            int gi;

            for (gi = 0; gi < 6; gi++) {
                ProDtlvartext vt = NULL;
                wchar_t      sv[32];

                wcsncpy_s(sv, 32, value, _TRUNCATE);
                if (ProDtlvartextAlloc((wchar_t *)gp[gi], sv, &vt) == PRO_TK_NO_ERROR && vt) {
                    ProError ea = ProDtlsyminstdataVartextAdd(id, vt);
                    logf_("[rough] guess-set prompt%d -> %d (%s)", gi, (int)ea, errname(ea));
                }
            }
        }
        if (g_rough_set_mode) {
            ProDtlvartext *arr = NULL;
            int n = 0;
            ProArrayAlloc(4, sizeof(ProDtlvartext), 1, (ProArray *)&arr);
            {
                ProDtlvartext a = NULL, b = NULL, c = NULL, d = NULL;
                if (ProDtlvartextAlloc((wchar_t *)L"rough_ip_1", (wchar_t *)L"3.2", &a) == PRO_TK_NO_ERROR && a) arr[n++] = a;
                if (ProDtlvartextAlloc((wchar_t *)L"ROUGH_IP_1", (wchar_t *)L"3.2", &b) == PRO_TK_NO_ERROR && b) arr[n++] = b;
                if (ProDtlvartextAlloc((wchar_t *)L"_INFO_", (wchar_t *)L"\u51B2\u88C1\u9762", &c) == PRO_TK_NO_ERROR && c) arr[n++] = c;
                if (ProDtlvartextAlloc((wchar_t *)L"INFO", (wchar_t *)L"\u51B2\u88C1\u9762", &d) == PRO_TK_NO_ERROR && d) arr[n++] = d;
            }
            logf_("[rough] SETMODE VartextsSet n=%d -> %d", n, (int)ProDtlsyminstdataVartextsSet(id, arr));
        }
            ProView     fv = sym_front_view(drw);
            ProVector   loc;
            ProDtlattach att = NULL;

            loc[0] = x; loc[1] = y; loc[2] = 0.0;
            if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                ProDtlsyminstdataAttachmentSet(id, att);
                ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
            }
            logf_("[rough] heightSet %.2f -> %d", height, (int)ProDtlsyminstdataScaledheightSet(id, height));
            {
                double hh = -1.0;

                if (ProDtlsyminstdataScaledheightGet(id, &hh) == PRO_TK_NO_ERROR)
                    logf_("[rough] height readback = %.3f (requested %.2f)", hh, height);
            }
            {
                ProDtlsyminst inst;
                ProError      ec;

                memset(&inst, 0, sizeof(inst));
                ec = ProDtlsyminstCreate(drw, id, &inst);
                logf_("[rough] create @(%.1f,%.1f) groups=%ls -> %d (%s)", x, y, groups ? groups : L"-", (int)ec, errname(ec));
                if (ec == PRO_TK_NO_ERROR) {
                    ProError e2 = ProDtlsyminstShow(&inst);

                    logf_("[rough] show(syminst) -> %d (%s)", (int)e2, errname(e2));
                    {
                        ProDtlsyminstdata cd = NULL;

                        if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd)
                            sym_dump_vartexts(cd, "rough");
                    }
                }
            }
        }
    }
    {
        ProError er2 = ProDwgSheetRegenerate((ProDrawing)drw, 1);

        logf_("[rough] regen -> %d (%s)", (int)er2, errname(er2));
    }
}


static void rsym_unescape(const wchar_t *in, wchar_t *out, int cap)
{
    int oi = 0;

    while (*in && oi < cap - 1) {
        if (in[0] == L'\\' && (in[1] == L'u' || in[1] == L'U') &&
            iswxdigit(in[2]) && iswxdigit(in[3]) && iswxdigit(in[4]) && iswxdigit(in[5])) {
            wchar_t hex[5] = { in[2], in[3], in[4], in[5], 0 };
            out[oi++] = (wchar_t)wcstol(hex, NULL, 16);
            in += 6;
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = 0;
}

/* RSYM2|x|y|v1|v2|height|cat|name �?copy PiCreo/user roughness params:
   groups CUSTOM:30,31, two vartexts rough_ip_1/rough_ip_2, instance height, FREE attach. */
static void rsym2_run(ProMdl drw, double x, double y, const wchar_t *v1, const wchar_t *v2,
                      double height, const wchar_t *groups, const wchar_t *cat, const wchar_t *name)
{
    ProDtlsymdef def;
    ProError     er = PRO_TK_E_NOT_FOUND;
    int          ci, nc = 0;
    struct { const wchar_t *grp; const wchar_t *nm; int ver; } cand[3];
    wchar_t      s1[256], s2[256];

    rsym_unescape(v1, s1, 256);
    rsym_unescape(v2, s2, 256);
    memset(&def, 0, sizeof(def));
    if (name && name[0]) {
        cand[nc].grp = (cat && cat[0]) ? cat : L""; cand[nc].nm = name; cand[nc].ver = 0; nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L""; cand[nc].nm = name; cand[nc].ver = 1; nc++;
        cand[nc].grp = (cat && cat[0]) ? cat : L""; cand[nc].nm = name; cand[nc].ver = (int)PRO_VALUE_UNUSED; nc++;
    }
    for (ci = 0; ci < nc; ci++) {
        memset(&def, 0, sizeof(def));
        er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)cand[ci].grp, (wchar_t *)cand[ci].nm, cand[ci].ver, PRO_B_TRUE, &def);
        logf_("[rsym2] retrieve '%ls'/'%ls' ver=%d -> %d (%s)", cand[ci].grp, cand[ci].nm, cand[ci].ver, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR) break;
    }
    if (er != PRO_TK_NO_ERROR) return;
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) != PRO_TK_NO_ERROR || !id) { logf_("[rsym2] data alloc failed"); return; }
        ProDtlsyminstdataDefSet(id, &def);
        {
            int ids[16], nid = 0;
            ProDtlsymgroup *arr = NULL;
            int i;
            const wchar_t *q = (groups && groups[0]) ? groups : L"30,31";
            const wchar_t *q0 = q;

            while (*q && nid < 16) {
                ids[nid++] = (int)wcstol(q, (wchar_t **)&q, 10);
                if (*q == L',') q++;
            }
            ProArrayAlloc(nid ? nid : 1, sizeof(ProDtlsymgroup), 1, (ProArray *)&arr);
            for (i = 0; i < nid; i++) {
                memset(&arr[i], 0, sizeof(ProDtlsymgroup));
                arr[i].symbol_def = def;
                arr[i].sym_group_id = ids[i];
            }
            logf_("[rsym2] groupoption CUSTOM n=%d ids=%ls -> %d", nid, q0,
                  (int)ProDtlsyminstdataGroupoptionsSet(id, PRO_DTLSYMINST_GROUP_CUSTOM, arr));
        }
        {
            ProDtlvartext  t1 = NULL, t2 = NULL;
            ProDtlvartext *arr = NULL;
            ProError       e;

            if (ProDtlvartextAlloc((wchar_t *)L"rough_ip_1", (wchar_t *)s1, &t1) != PRO_TK_NO_ERROR ||
                ProDtlvartextAlloc((wchar_t *)L"rough_ip_2", (wchar_t *)s2, &t2) != PRO_TK_NO_ERROR) {
                logf_("[rsym2] vartext alloc failed");
            } else {
                ProArrayAlloc(2, sizeof(ProDtlvartext), 1, (ProArray *)&arr);
                arr[0] = t1; arr[1] = t2;
                e = ProDtlsyminstdataVartextsSet(id, arr);
                logf_("[rsym2] vartextsSet(rough_ip_1='%ls', rough_ip_2='%ls') -> %d (%s)", s1, s2, (int)e, errname(e));
                if (e != PRO_TK_NO_ERROR) {
                    logf_("[rsym2] fallback vartextAdd -> %d / %d",
                          (int)ProDtlsyminstdataVartextAdd(id, t1), (int)ProDtlsyminstdataVartextAdd(id, t2));
                }
            }
        }
        {
            ProView     fv = sym_front_view(drw);
            ProVector   loc;
            ProDtlattach att = NULL;

            loc[0] = x; loc[1] = y; loc[2] = 0.0;
            if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                ProDtlsyminstdataAttachmentSet(id, att);
                ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
            }
            logf_("[rsym2] heightSet %.3f -> %d", height, (int)ProDtlsyminstdataScaledheightSet(id, height));
            {
                double hh = -1.0;
                if (ProDtlsyminstdataScaledheightGet(id, &hh) == PRO_TK_NO_ERROR)
                    logf_("[rsym2] height readback = %.3f", hh);
            }
            {
                ProDtlsyminst inst;
                ProError      ec;

                memset(&inst, 0, sizeof(inst));
                ec = ProDtlsyminstCreate(drw, id, &inst);
                logf_("[rsym2] create @(%.1f,%.1f) -> %d (%s)", x, y, (int)ec, errname(ec));
                if (ec == PRO_TK_NO_ERROR) {
                    logf_("[rsym2] show -> %d", (int)ProDtlsyminstShow(&inst));
                    {
                        ProLineEnvelope env;

                        env[0][0] = env[0][1] = -9999; env[3][0] = env[3][1] = -9999;
                        ProDtlsyminstEnvelopeGet(&inst, env);
                        logf_("[rsym2] bbox=(%.2f,%.2f)-(%.2f,%.2f) size=%.2fx%.2f", env[0][0], env[0][1], env[3][0], env[3][1],
                              fabs(env[3][0] - env[0][0]), fabs(env[0][1] - env[3][1]));
                    }
                    {
                        ProDtlsyminstdata cd = NULL;
                        if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd)
                            sym_dump_vartexts(cd, "rsym2");
                    }
                }
            }
        }
        logf_("[rsym2] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
    }
}

static void crossstyle_run(ProMdl drw, const wchar_t *font)
{
    ProDtlentity *ents = NULL;
    int cnt = 0, i, ok = 0, fail = 0;

    if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents) {
        logf_("[xstyle] collect failed"); return;
    }
    ProArraySizeGet((ProArray)ents, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlentitydata d = NULL;
        ProName f0, f1;
        ProError e1, e2;
        char o8[120], n8[120];

        if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
        f0[0] = 0;
        ProDtlentitydataFontGet(d, f0);
        wcsncpy_s(f1, PRO_NAME_SIZE, font, _TRUNCATE);
        e1 = ProDtlentitydataFontSet(d, f1);
        e2 = (e1 == PRO_TK_NO_ERROR) ? ProDtlentityModify(&ents[i], NULL, d) : (ProError)(-999);
        ProDtlentitydataFree(d);
        f1[0] = 0;
        {
            ProDtlentitydata d2 = NULL;

            if (ProDtlentityDataGet(&ents[i], NULL, &d2) == PRO_TK_NO_ERROR && d2) {
                ProDtlentitydataFontGet(d2, f1);
                ProDtlentitydataFree(d2);
            }
        }
        o8[0] = 0; n8[0] = 0;
        wide_to_u8(f0, o8, sizeof(o8));
        wide_to_u8(f1, n8, sizeof(n8));
        logf_("[xstyle] ent[%2d] was '%s' -> set %d (%s) modify %d ; readback '%s'", i, o8,
              (int)e1, errname(e1), (int)e2, n8);
        if (e1 == PRO_TK_NO_ERROR && e2 == PRO_TK_NO_ERROR) ok++; else fail++;
    }
    logf_("[xstyle] font set ok=%d fail=%d", ok, fail);
    logf_("[xstyle] regen -> %d ; repaint -> %d",
          (int)ProDwgSheetRegenerate((ProDrawing)drw, 1), (int)ProWindowRepaint(-1));
}

static void technote_run(ProMdl drw, double x, double y)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i, deleted = 0;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) == PRO_TK_NO_ERROR && notes) {
        ProArraySizeGet((ProArray)notes, &cnt);
        for (i = 0; i < cnt; i++) {
            ProDtlnotedata nd = NULL;
            int is_tech = 0;

            if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
            {
                ProDtlnoteline *lines = NULL;

                if (ProDtlnotedataLinesCollect(nd, &lines) == PRO_TK_NO_ERROR && lines) {
                    int lc = 0, l;

                    ProArraySizeGet((ProArray)lines, &lc);
                    for (l = 0; l < lc && !is_tech; l++) {
                        ProDtlnotetext *txts = NULL;

                        if (ProDtlnotelineTextsCollect(lines[l], &txts) == PRO_TK_NO_ERROR && txts) {
                            int tc = 0, t;

                            ProArraySizeGet((ProArray)txts, &tc);
                            for (t = 0; t < tc; t++) {
                                ProLine s;

                                s[0] = 0;
                                if (ProDtlnotetextStringGet(txts[t], s) == PRO_TK_NO_ERROR &&
                                    wcsstr(s, L"\u6280\u672F\u8981\u6C42")) { is_tech = 1; break; }
                            }
                        }
                    }
                }
            }
            if (is_tech) {
                ProError ed = ProDtlnoteDelete(&notes[i], NULL);

                logf_("[tech] delete note[%d] -> %d (%s)", i, (int)ed, errname(ed));
                if (ed == PRO_TK_NO_ERROR) deleted++;
            }
        }
    }
    logf_("[tech] TECHNOTE: moved to (%.1f,%.1f), old notes deleted=%d", x, y, deleted);
    add_technote_at(drw, x, y);
}

/* 显示模型注释：把 3D 里的尺寸/公差等“捞”到图纸（教程第 3 章的“主力做法”） */
static void show_model_annotations(ProMdl drw)
{
    /* PRO_AXIS 已移除：模型轴在图纸上是"全长"线，污染图面�? 个凸点中心线
       改由 boss_centerlines() �?ProDtlentity 手画短十字（已验证）�?*/
    static const ProType TYPES[] = { PRO_DIMENSION, PRO_REF_DIMENSION, PRO_GTOL, PRO_SURF_FIN };
    ProView *views = NULL;
    int vc = 0, i, t;
    ProError e;

    e = ProDrawingViewsCollect((ProDrawing)drw, &views);
    logf_("  [show] viewsCollect -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR || !views) return;
    ProArraySizeGet((ProArray)views, &vc);
    logf_("  [show] %d view(s) to process", vc);
    for (i = 0; i < vc; i++) {
        for (t = 0; t < 4; t++) {
            ProError es = ProAnnotationByViewShow((ProDrawing)drw, views[i], TYPES[t]);
            logf_("  [show] view[%d] type=%d -> %d (%s)", i, (int)TYPES[t], (int)es, errname(es));
        }
    }
    ProArrayFree((ProArray *)&views);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
    logf_("  [show] done (sheet regenerated)");
}

/* 关键尺寸白名单（对照连接片样本重新学习后的结论，mm）——只标“人该标的”：
   41.30=总宽�?3.00=端子盘横向中心距�?3.50=端子盘纵向中心距�?
   3.30=端子孔径�?.00=凸点高度�?.25=料厚�?.80=台阶�?.65=深度�?
   盘径 Ø20 �?R1/R0.5 不单标（圆角并入技术要求“未注圆�?R0.5”）�?
   其余草图构造值（163/22.35/24.7/35.55...）与中间值（5/10/7/3...）全部不标�?*/
static int value_in_keep_list(double v)
{
    /* 6×Ø3.3 已按用户裁定删除（五张人样一�?n×Ø 都没�?�?�?非功能圆�?不标�?*/
    static const double KEEP[] = { 41.30, 23.00, 23.50, 2.00, 0.25, 4.80, 9.65, 63.60 };
    int i;
    for (i = 0; i < (int)(sizeof(KEEP) / sizeof(KEEP[0])); i++) {
        double d = v - KEEP[i];
        if (d < 0) d = -d;
        if (d < 0.02) return 1;
    }
    return 0;
}

/* 遍历回调：非关键尺寸从图纸上拭除（保留工程关键尺寸） */
static ProError dim_filter_cb(ProDimension *dim, ProError status, ProAppData app)
{
    ProDrawing drw = (ProDrawing)app;
    double     v = 0.0;
    ProError   e;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (value_in_keep_list(v)) {
        logf_("[filter] KEEP   %.4f", v);
        return PRO_TK_NO_ERROR;
    }
    e = ProAnnotationUndisplay((ProAnnotation *)dim, NULL, drw);
    logf_("[filter] drop   %.4f -> %d (%s)", v, (int)e, errname(e));
    return PRO_TK_NO_ERROR;
}

static void filter_key_dims(ProMdl part, ProMdl drw)
{
    ProError e = ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_filter_cb, NULL, (ProAppData)drw);
    logf_("[filter] solidDimensionVisit -> %d (%s)", (int)e, errname(e));
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 正攻法：不“全显后拭除”，而是只把关键尺寸逐个“显示”到合适的视图�?
   对不匹配的视图，ProAnnotationShow 会返�?PRO_TK_INVALID_MATRIX（方向冲突）�?
   相当于让 Creo 自己判断每个尺寸应该出现在哪个视图�?*/
typedef struct {
    ProDrawing drw;
    ProView    view;
    double     seenv[128];  /* 已显示过的尺寸“值”：同值多实例只标一次（避免双尺寸） */
    int        nseen;
} showk_ctx_t;

static ProError dim_show_cb(ProDimension *dim, ProError status, ProAppData app)
{
    showk_ctx_t *c = (showk_ctx_t *)app;
    double       v = 0.0;
    ProError     e;
    int          k;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (!value_in_keep_list(v)) return PRO_TK_NO_ERROR;
    /* 轮次26：已改由图纸侧创建的尺寸不再从模型显示（避免新旧重复）�?
       Undisplay �?Show 过的注释无效、且�?SDK �?ProAnnotationErase —�?
       最干净的做法是让它们从一开始就【不显示】�?*/
    /* 轮次32（用户裁�?R1：所有尺寸必须由工具包创建、禁止用显示）�?
       至此跳过清单覆盖全部"改为创建"的值：2 / 23 / 23.5 / 9.65 / 41.3 / 0.25 / 4.8
       —�?41.3（不可检�?冗余）�?.8（几何不可见）就此彻底不出现在图上；
       0.25 �?CONVDIM 创建。清单漏�?= 尺寸悄悄走显示混进交付件（轮26-31 的教训）�?*/
    if ((v > 1.99 && v < 2.01) || (v > 22.99 && v < 23.01) ||
        (v > 23.49 && v < 23.51) || (v > 9.64 && v < 9.66) ||
        (v > 41.29 && v < 41.31) || (v > 0.24 && v < 0.26) ||
        (v > 4.79 && v < 4.81)) {
        logf_("[showk] %.4f skipped (round32: created by API, display forbidden)", v);
        return PRO_TK_NO_ERROR;
    }
    for (k = 0; k < c->nseen; k++) {
        double d = c->seenv[k] - v;
        if (d < 0) d = -d;
        if (d < 0.02) return PRO_TK_NO_ERROR;   /* 同值已标过 */
    }
    /* 先把公差/前后缀设好，再显示：显示瞬间的注释文本会带上当前属�?*/
    {
        /* 关键尺寸加公差：装配定位 ±0.1、功能尺�?±0.05（国标规则：配合/定位必标公差�?*/
        double tol = -1.0;
        if ((v > 22.99 && v < 23.01) || (v > 23.49 && v < 23.51) || (v > 19.99 && v < 20.01)) tol = 0.10;
        else if ((v > 1.99 && v < 2.01) || (v > 0.24 && v < 0.26) || (v > 4.79 && v < 4.81)) tol = 0.05;
        if (tol > 0) {
            ProDimToleranceType pt0 = (ProDimToleranceType)0;
            double pu0 = -999, pl0 = -999;
            ProName tab0;
            /* 写之前先读模型自带公差：区分"显示的是模型自带还是设置副作�?（P0c�?*/
            ProDimensionToltypeGet(dim, &pt0);
            ProDimensionToleranceGet(dim, &pu0, &pl0);
            logf_("[showk]   pre-tol %.4f: type=%d up=%.4f lo=%.4f", v, (int)pt0, pu0, pl0);
            /* P0c 实测：尺寸默认挂 ISO 通用公差表（ttab=1），regen 会用表值覆盖显式值；
               先摘除表标签，显式公差才能被图面采用 */
            tab0[0] = 0;
            {
                ProError elb = ProDimensionTollabelSet(dim, PROTOLTABLE_NONE, tab0, 0);
                logf_("[showk]   tollabel clear -> %d (%s)", (int)elb, errname(elb));
            }
            {
                ProError e1 = ProDimensionToltypeSet(dim, PRO_TOL_PLUS_MINUS_SYM);
                ProError e2 = ProDimensionToleranceSet(dim, tol, -tol);   /* �?+tol / �?-tol */
                double up = -999, lo = -999;
                ProError e3 = ProDimensionToleranceGet(dim, &up, &lo);
                logf_("[showk]   tol sym %.2f type->%d set->%d ; readback up=%.3f lo=%.3f (%d)",
                      tol, (int)e1, (int)e2, up, lo, (int)e3);
            }
        }
        /* 端子孔数量前缀�?×Ø3.3（�?= U+00D8，Creo 的直径符号） */
        if (v > 3.29 && v < 3.31) {
            ProError e3 = ProDimensionPrefixSet(dim, (wchar_t *)L"6\u00D7\u00D8");   /* 6×Ø */
            logf_("[showk]   prefix 6x-dia -> %d (%s)", (int)e3, errname(e3));
        }
        /* 功能类尺寸加 �?标记（五张人样硬规则�?±0.05�?/ 0.25±0.05�?/ 4.8±0.05△）�?
           0.25 同时以此替代模型自带的英文后缀 "THICK" */
        /* 轮次46：△ 字符后缀取消（改用符号库 jizhun 的符号实例，�?SYMARKS�?*/
    }
    e = ProAnnotationShow((ProAnnotation *)dim, NULL, c->view);
    logf_("[showk] %.4f -> view %d : %d (%s)", v, (int)c->view, (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) {
        /* 轮次32 自检：交付前日志里不允许出现任何 SHOWN —�?出现即说明有尺寸走了"显示" */
        logf_("[showk] %.4f -> view %d : SHOWN  <-- 不该出现", v, (int)c->view);
        if (c->nseen < 128) c->seenv[c->nseen++] = v;
    }
    return PRO_TK_NO_ERROR;
}

static void show_key_dims(ProMdl part, ProMdl drw)
{
    ProView *views = NULL;
    int      vc = 0, i, pass;
    ProError e;

    e = ProDrawingViewsCollect((ProDrawing)drw, &views);
    if (e != PRO_TK_NO_ERROR || !views) { logf_("[showk] viewsCollect -> %d", (int)e); return; }
    ProArraySizeGet((ProArray)views, &vc);
    {
        showk_ctx_t ctx;
        ctx.drw = (ProDrawing)drw;
        ctx.nseen = 0;
        /* 两遍：先主视图（名含 front），再其余视图——让板面尺寸优先落在主视图上 */
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < vc; i++) {
                ProName nm;
                int     is_front = 0;

                nm[0] = 0;
                if (ProDrawingViewNameGet((ProDrawing)drw, views[i], nm) == PRO_TK_NO_ERROR) {
                    if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT") || wcsstr(nm, L"Front")) is_front = 1;
                }
                if ((pass == 0) != is_front) continue;
                ctx.view = views[i];
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_show_cb, NULL, &ctx);
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
    logf_("[showk] done");
}

/* 整理尺寸（教�?clm）：对全图自动分层排布，options 走当�?detail options */
static void cleanup_dims(ProMdl drw)
{
    ProError e0, e;

    /* 调大整理间距：更接近样本的规整分层（偏移/增量�?A3 图幅试值） */
    e0 = ProMdlDetailOptionSet(drw, (wchar_t *)L"default_clean_dim_offset", (wchar_t *)L"0.5");
    logf_("[clm] set offset -> %d (%s)", (int)e0, errname(e0));
    e0 = ProMdlDetailOptionSet(drw, (wchar_t *)L"default_clean_dim_increment", (wchar_t *)L"0.6");
    logf_("[clm] set increment -> %d (%s)", (int)e0, errname(e0));

    e = ProDrawingDimensionsCleanup((ProDrawing)drw, NULL);
    logf_("[clm] dimensionsCleanup -> %d (%s)", (int)e, errname(e));
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 保存前清除模型“待重算”状态：改过尺寸属性（公差/前缀）后直接 ProMdlSave 会死等（实测�?*/
static void prep_save_regen(void)
{
    ProMdl part = NULL;
    if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
        ProError erg = ProSolidRegenerate((ProSolid)part, 0);
        logf_("[save-fix] ProSolidRegenerate -> %d (%s)", (int)erg, errname(erg));
    } else {
        logf_("[save-fix] part retrieve failed");
    }
}

/* 2026-11-13: VIEWADD/其它按名取模型的命令要同时支持零件与装配 */
static ProError retrieve_model_any(const wchar_t *name, ProMdl *p_mdl)
{
    ProError e;
    if (!name || !name[0] || !p_mdl) return PRO_TK_BAD_INPUTS;
    *p_mdl = NULL;
    e = ProMdlnameRetrieve((wchar_t *)name, PRO_MDLFILE_PART, p_mdl);
    if (e == PRO_TK_NO_ERROR && *p_mdl) return e;
    *p_mdl = NULL;
    return ProMdlnameRetrieve((wchar_t *)name, PRO_MDLFILE_ASSEMBLY, p_mdl);
}

static HANDLE g_save_evt = NULL;
static ProPath g_save_dir;
static double g_xdim_cx = 300.0;
static double g_xdim_cy = 520.0;
static double g_cross_dx = 28.0;   /* half the measured boss-grid span, X */
static double g_cross_dy = 28.0;   /* half the measured boss-grid span, Y */

static DWORD WINAPI save_thread_proc(LPVOID p)
{
    ProMdl drw = (ProMdl)p;
    ProError es = ProMdlSave(drw);
    logf_("[savethread] ProMdlSave -> %d (%s)", (int)es, errname(es));
    if (g_save_evt) SetEvent(g_save_evt);
    return 0;
}

static DWORD WINAPI saveback_thread_proc(LPVOID p)
{
    ProMdl drw = (ProMdl)p;
    ProError es = ProMdlnameBackup(drw, g_save_dir);
    logf_("[savethread] ProMdlnameBackup('%ls') -> %d (%s)", g_save_dir, (int)es, errname(es));
    if (g_save_evt) SetEvent(g_save_evt);
    return 0;
}

/* MOVE 实验：把指定数值的尺寸移到图纸坐标点（精摆布局用） */
typedef struct {
    double     target;
    Pro3dPnt   pt;
    ProDrawing drw;
} move_ctx_t;

static ProError dim_move_cb(ProDimension *dim, ProError status, ProAppData app)
{
    move_ctx_t *mc = (move_ctx_t *)app;
    double      v = 0.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < mc->target - 0.02 || v > mc->target + 0.02) return PRO_TK_NO_ERROR;
    {
        /* 移动前文本位置（图纸坐标读回�?*/
        ProDimlocation loc0 = NULL;
        ProBoolean he0 = PRO_B_FALSE;
        ProPoint3d  p0;
        double      el0 = 0.0;
        p0[0] = p0[1] = p0[2] = -9999.0;
        if (ProDimensionLocationGet(dim, NULL, mc->drw, &loc0) == PRO_TK_NO_ERROR && loc0)
            ProDimlocationTextGet(loc0, &he0, p0, &el0);
        logf_("[move] id=%d v=%.4f BEFORE text=(%.2f,%.2f,%.2f) elbow=%d",
              (int)dim->id, v, p0[0], p0[1], p0[2], (int)he0);
    }
    {
        ProError e = ProDrawingDimensionMove(mc->drw, dim, mc->pt);
        logf_("[move] id=%d value=%.4f -> (%.2f,%.2f,%.2f) : %d (%s)",
              (int)dim->id, v, mc->pt[0], mc->pt[1], mc->pt[2], (int)e, errname(e));
    }
    {
        /* 移动后文本位�?*/
        ProDimlocation loc1 = NULL;
        ProBoolean he1 = PRO_B_FALSE;
        ProPoint3d  p1;
        double      el1 = 0.0;
        p1[0] = p1[1] = p1[2] = -9999.0;
        if (ProDimensionLocationGet(dim, NULL, mc->drw, &loc1) == PRO_TK_NO_ERROR && loc1)
            ProDimlocationTextGet(loc1, &he1, p1, &el1);
        logf_("[move] id=%d AFTER  text=(%.2f,%.2f,%.2f) elbow=%d",
              (int)dim->id, p1[0], p1[1], p1[2], (int)he1);
    }
    return PRO_TK_NO_ERROR;
}

/* LEADER（轮�?9，含撤销支持）：`LEADER|<�?` = 只读（打�?config 实测值）�?
   `LEADER|<�?|<mode>` = 设置 config�?=LINEAR, 2=CENTER_LEADER；轮�?9 撤销时用 1）�?
   只改显示配置，不动测量附件�?*/
typedef struct {
    double     target;
    ProDrawing drw;
    int        mode;      /* -1 = 只读 */
} leader_ctx_t;

static ProError dim_leader_cb(ProDimension *dim, ProError status, ProAppData app)
{
    leader_ctx_t *lc = (leader_ctx_t *)app;
    double        v = 0.0;
    (void)status;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < lc->target - 0.02 || v > lc->target + 0.02) return PRO_TK_NO_ERROR;
    {
        ProDimensionconfig cfg0 = PRO_DIMCONFIG_LINEAR;
        ProError eg0 = ProDimensionConfigGet(dim, lc->drw, &cfg0);
        logf_("[leader] id=%d v=%.4f config now=%d (get->%d %s) mode=%d",
              (int)dim->id, v, (int)cfg0, (int)eg0, errname(eg0), lc->mode);
    }
    if (lc->mode >= 0 && lc->mode <= 3) {
        ProDimensionconfig cfg1 = PRO_DIMCONFIG_LINEAR;
        ProError e = ProDimensionConfigSet(dim, lc->drw, (ProDimensionconfig)lc->mode);
        logf_("[leader]   set config=%d : %d (%s)", lc->mode, (int)e, errname(e));
        if (ProDimensionConfigGet(dim, lc->drw, &cfg1) == PRO_TK_NO_ERROR)
            logf_("[leader]   after config=%d", (int)cfg1);
    }
    {
        ProDimlocation loc1 = NULL;
        ProBoolean he1 = PRO_B_FALSE;
        ProPoint3d  p1;
        double      el1 = 0.0;
        p1[0] = p1[1] = p1[2] = -9999.0;
        if (ProDimensionLocationGet(dim, NULL, lc->drw, &loc1) == PRO_TK_NO_ERROR && loc1)
            ProDimlocationTextGet(loc1, &he1, p1, &el1);
        logf_("[leader] id=%d text=(%.2f,%.2f) elbow=%d", (int)dim->id, p1[0], p1[1], (int)he1);
    }
    return PRO_TK_NO_ERROR;
}

/* ===== 轮次22 实验：把 ProSelection 注入运行中的 hd 命令（ProSelect 机制�?====
   SELINJ|<mode>：先扫出零件两条端部竖边，构造带主视图上下文�?ProSelection�?
   然后�?mode 尝试注入�?
     mode 1 = ProSelectExternalselectionRecord（文档："Places external selection into ProSelect() buffer"�?
     mode 2 = ProSelbufferSelectionAdd（UI 选择缓冲区）
     mode 4 = mode 1 但去掉视图上下文
   运行前应先用 MACRO|~ Command `SDHorizonDimension` ; 启动 hd�?*/
static void selinj(ProMdl part, ProMdl drw, int mode)
{
    Pro3dPnt box[2];
    double   bx0, bx1;
    int      best_l = -1, best_r = -1, hid;
    double   blx = 1e30, brx = -1e30;

    if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) {
        logf_("[selinj] bbox failed");
        return;
    }
    bx0 = box[0][0];
    bx1 = box[1][0];
    logf_("[selinj] mode=%d bbox x[%.2f..%.2f]", mode, bx0, bx1);

    for (hid = 1; hid <= 3000; hid++) {
        ProEdge   e = NULL;
        Pro3dPnt *pts = NULL;
        int       cnt = 0, t;
        double    exmin = 1e30, exmax = -1e30, eymin = 1e30, eymax = -1e30;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeTessellationGet(e, &pts, NULL, NULL, NULL, &cnt) != PRO_TK_NO_ERROR || !pts || cnt < 2) continue;
        for (t = 0; t < cnt; t++) {
            if (pts[t][0] < exmin) exmin = pts[t][0];
            if (pts[t][0] > exmax) exmax = pts[t][0];
            if (pts[t][1] < eymin) eymin = pts[t][1];
            if (pts[t][1] > eymax) eymax = pts[t][1];
        }
        ProArrayFree((ProArray *)&pts);
        if ((eymax - eymin) <= 30.0 || (exmax - exmin) > 2.0) continue;
        {
            ProGeomitem      gi0;
            ProGeomitemdata *gd0 = NULL;
            int              ltype = -1;

            if (ProEdgeToGeomitem((ProSolid)part, e, &gi0) == PRO_TK_NO_ERROR &&
                ProGeomitemdataGet(&gi0, &gd0) == PRO_TK_NO_ERROR && gd0 && gd0->data.p_curve_data)
                ltype = gd0->data.p_curve_data->line.type;
            if (gd0) ProGeomitemdataFree(&gd0);
            if (ltype != PRO_ENT_LINE) continue;
        }
        {
            double cx = (exmin + exmax) / 2.0;
            if (cx < bx0 - 0.5 || cx > bx1 + 0.5) continue;
            if (cx < blx) { blx = cx; best_l = hid; }
            if (cx > brx) { brx = cx; best_r = hid; }
        }
    }
    logf_("[selinj] picked L x=%.2f e=%d ; R x=%.2f e=%d", blx, best_l, brx, best_r);
    if (best_l < 0 || best_r < 0) {
        logf_("[selinj] no in-bbox vertical edge pair");
        return;
    }
    {
        ProEdge      le = NULL, re = NULL;
        ProGeomitem  gi1, gi2;
        ProSelection s1 = NULL, s2 = NULL;
        ProView      front = NULL;
        ProView     *views = NULL;

        if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
            int vc = 0, vi;
            ProArraySizeGet((ProArray)views, &vc);
            for (vi = 0; vi < vc; vi++) {
                ProName nm;
                nm[0] = 0;
                ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[vi]; break; }
            }
            ProArrayFree((ProArray *)&views);
        }
        ProEdgeInit((ProSolid)part, best_l, &le);
        ProEdgeInit((ProSolid)part, best_r, &re);
        if (!le || !re) { logf_("[selinj] edgeInit failed"); return; }
        if (ProEdgeToGeomitem((ProSolid)part, le, &gi1) != PRO_TK_NO_ERROR) { logf_("[selinj] gi L failed"); return; }
        if (ProEdgeToGeomitem((ProSolid)part, re, &gi2) != PRO_TK_NO_ERROR) { logf_("[selinj] gi R failed"); return; }

        if (mode == 4) {
            /* 无视图上下文版本 */
            ProSelection sL = NULL, sR = NULL;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &sL) != PRO_TK_NO_ERROR) { logf_("[selinj] alloc L failed"); return; }
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &sR) != PRO_TK_NO_ERROR) { ProSelectionFree(&sL); logf_("[selinj] alloc R failed"); return; }
            {
                ProError e1 = ProSelectExternalselectionRecord(sL, PRO_SELECT_OVERRIDE);
                logf_("[selinj] ExtRecord no-view #1 -> %d (%s)", (int)e1, errname(e1));
            }
            {
                ProError e2 = ProSelectExternalselectionRecord(sR, PRO_SELECT_OVERRIDE);
                logf_("[selinj] ExtRecord no-view #2 -> %d (%s)", (int)e2, errname(e2));
            }
            ProSelectionFree(&sL);
            ProSelectionFree(&sR);
            return;
        }

        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &s1) != PRO_TK_NO_ERROR) { logf_("[selinj] alloc L failed"); return; }
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); logf_("[selinj] alloc R failed"); return; }
        if (front) { ProSelectionViewSet(front, s1); ProSelectionViewSet(front, s2); }
        logf_("[selinj] selections built (front ctx=%s)", front ? "yes" : "no");

        if (mode == 1) {
            {
                ProError e1 = ProSelectExternalselectionRecord(s1, PRO_SELECT_OVERRIDE);
                logf_("[selinj] ExtRecord #1 -> %d (%s)", (int)e1, errname(e1));
            }
            {
                ProError e2 = ProSelectExternalselectionRecord(s2, PRO_SELECT_OVERRIDE);
                logf_("[selinj] ExtRecord #2 -> %d (%s)", (int)e2, errname(e2));
            }
        } else if (mode == 2) {
            {
                ProError ec = ProSelbufferClear();
                logf_("[selinj] SelbufferClear -> %d (%s)", (int)ec, errname(ec));
            }
            {
                ProError e1 = ProSelbufferSelectionAdd(s1);
                logf_("[selinj] SelbufferAdd #1 -> %d (%s)", (int)e1, errname(e1));
            }
            {
                ProError e2 = ProSelbufferSelectionAdd(s2);
                logf_("[selinj] SelbufferAdd #2 -> %d (%s)", (int)e2, errname(e2));
            }
        } else {
            logf_("[selinj] unknown mode %d", mode);
        }
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
    }
}

/* ===== 轮次23：给基础草绘加总长尺寸（ProSecdimCreate —�?唯一未试过的路线�?====
   遍历 PRO_FEAT_PROTRUSION �?SectionCopy �?枚举 2D 线实�?�?
   找一�?平行且间距最接近 63.6"的端线（同时把实测距离打进日志）�?
   ProSecdimCreate(PRO_TK_DIM_LINE_LINE) �?回读�?�?dims 遍历验证 �?再生 + bbox 对比�?
   SECDIM|<mode>：mode 0（默认）= 仅创建；mode 1 = 创建�?Strengthen（强尺寸）�?
   注意：不 Save（本环境保存会死等）；改的是会话内模型�?*/
typedef struct {
    int        n;
    int        fid[8];
    int        nsec[8];
    ProFeature feat[8];
} secfinder_t;

static void dims_report(void);   /* 前向声明（定义在后面�?*/

static ProError secfind_filter(ProFeature *feat, ProAppData app)
{
    ProFeattype t = (ProFeattype)0;
    (void)app;
    if (ProFeatureTypeGet(feat, &t) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    return (t == PRO_FEAT_PROTRUSION) ? PRO_TK_NO_ERROR : PRO_TK_CONTINUE;
}

static ProError secfind_visit(ProFeature *feat, ProError status, ProAppData app)
{
    secfinder_t *f = (secfinder_t *)app;
    (void)status;
    if (f->n >= 8) return PRO_TK_NO_ERROR;
    f->feat[f->n] = *feat;
    f->fid[f->n] = feat->id;
    f->nsec[f->n] = 0;
    {
        int ns = 0;
        if (ProFeatureNumSectionsGet(feat, &ns) == PRO_TK_NO_ERROR) f->nsec[f->n] = ns;
    }
    logf_("[secdim] protrusion feature[%d]: id=%d sections=%d", f->n, f->fid[f->n], f->nsec[f->n]);
    f->n++;
    return PRO_TK_NO_ERROR;
}

static void secdim_try(ProMdl part, int strengthen)
{
    secfinder_t f;
    int fi;
    int found = 0;

    memset(&f, 0, sizeof(f));
    ProSolidFeatVisit((ProSolid)part, secfind_visit, secfind_filter, &f);
    logf_("[secdim] protrusion features found: %d", f.n);
    if (f.n <= 0) { logf_("[secdim] no protrusion feature"); return; }

    for (fi = 0; fi < f.n; fi++) {
        ProSection sec = NULL;
        ProError   es;

        if (f.nsec[fi] <= 0) continue;
        es = ProFeatureSectionCopy(&f.feat[fi], 0, &sec);
        logf_("[secdim] feat id=%d SectionCopy(0) -> %d (%s)", f.fid[fi], (int)es, errname(es));
        if (es != PRO_TK_NO_ERROR || !sec) continue;

        {
            ProIntlist idl = NULL;
            int n = 0, i;

            if (ProSectionEntityIdsGet(sec, &idl, &n) != PRO_TK_NO_ERROR || !idl || n <= 0) {
                logf_("[secdim]   entityIds failed (n=%d)", n);
                if (idl) ProArrayFree((ProArray *)&idl);
                ProSectionFree(&sec);
                continue;
            }
            logf_("[secdim]   entities n=%d", n);
            {
                struct { int id; double x1, y1, x2, y2, len; } L[256];
                int    lc = 0, a, b;
                double best_d = -1e30;
                int    best_a = -1, best_b = -1;
                double best_cx = 0, best_cy = 0, best_nx = 0, best_ny = 0;

                for (i = 0; i < n && lc < 256; i++) {
                    Pro2dEntdef *ent = NULL;
                    if (ProSectionEntityGet(sec, idl[i], &ent) != PRO_TK_NO_ERROR || !ent) continue;
                    if (ent->type == PRO_2D_LINE) {
                        Pro2dLinedef *ld = (Pro2dLinedef *)ent;
                        double dx = ld->end2[0] - ld->end1[0];
                        double dy = ld->end2[1] - ld->end1[1];
                        L[lc].id = idl[i];
                        L[lc].x1 = ld->end1[0]; L[lc].y1 = ld->end1[1];
                        L[lc].x2 = ld->end2[0]; L[lc].y2 = ld->end2[1];
                        L[lc].len = sqrt(dx * dx + dy * dy);
                        logf_("[secdim]   line id=%d len=%.3f (%.2f,%.2f)-(%.2f,%.2f)",
                              L[lc].id, L[lc].len, L[lc].x1, L[lc].y1, L[lc].x2, L[lc].y2);
                        lc++;
                    }
                }
                for (a = 0; a < lc; a++) {
                    for (b = a + 1; b < lc; b++) {
                        double ax = L[a].x2 - L[a].x1, ay = L[a].y2 - L[a].y1;
                        double bx = L[b].x2 - L[b].x1, by = L[b].y2 - L[b].y1;
                        double na = sqrt(ax * ax + ay * ay), nb = sqrt(bx * bx + by * by);
                        double cross, dist;
                        if (na < 1e-6 || nb < 1e-6) continue;
                        cross = (ax * by - ay * bx) / (na * nb);
                        if (cross > 0.02 || cross < -0.02) continue;   /* 平行判定 */
                        {
                            double mx = (L[b].x1 + L[b].x2) / 2.0 - (L[a].x1 + L[a].x2) / 2.0;
                            double my = (L[b].y1 + L[b].y2) / 2.0 - (L[a].y1 + L[a].y2) / 2.0;
                            dist = (mx * (-ay) + my * ax) / na;
                            if (dist < 0) dist = -dist;
                        }
                        if (fabs(dist - 63.60) < 0.5 && dist > best_d) {
                            best_d = dist;
                            best_a = L[a].id; best_b = L[b].id;
                            best_cx = ((L[a].x1 + L[a].x2) + (L[b].x1 + L[b].x2)) / 4.0;
                            best_cy = ((L[a].y1 + L[a].y2) + (L[b].y1 + L[b].y2)) / 4.0;
                            best_nx = -ay / na; best_ny = ax / na;
                        }
                    }
                }
                logf_("[secdim]   best parallel pair: dist=%.4f ids=%d,%d", best_d, best_a, best_b);
                if (best_a >= 0 && best_b >= 0) {
                    int                 ids[2];
                    ProSectionPointType pt[2];
                    Pro2dPnt            place;
                    int                 dimid = -1;
                    ProError            ec;

                    ids[0] = best_a; ids[1] = best_b;
                    pt[0] = PRO_ENT_WHOLE; pt[1] = PRO_ENT_WHOLE;
                    place[0] = best_cx + best_nx * 12.0;
                    place[1] = best_cy + best_ny * 12.0;
                    ec = ProSecdimCreate(sec, ids, pt, 2, PRO_TK_DIM_LINE_LINE, place, &dimid);
                    logf_("[secdim]   ProSecdimCreate -> %d (%s) dim_id=%d", (int)ec, errname(ec), dimid);
                    if (ec == PRO_TK_NO_ERROR && dimid >= 0) {
                        double v = -1.0;
                        ProSecdimType dt = (ProSecdimType)0;
                        found = 1;
                        if (ProSecdimValueGet(sec, dimid, &v) == PRO_TK_NO_ERROR)
                            logf_("[secdim]   secdim value = %.4f", v);
                        if (ProSecdimTypeGet(sec, dimid, &dt) == PRO_TK_NO_ERROR)
                            logf_("[secdim]   secdim type = %d", (int)dt);
                        if (strengthen) {
                            ProError es2 = ProSecdimStrengthen(sec, dimid);
                            logf_("[secdim]   Strengthen -> %d (%s)", (int)es2, errname(es2));
                        }
                    }
                } else {
                    logf_("[secdim]   no pair near 63.6 in this feature");
                }
            }
            if (idl) ProArrayFree((ProArray *)&idl);
        }
        ProSectionFree(&sec);
        if (found) {
            logf_("[secdim] created in feature id=%d; stop", f.fid[fi]);
            break;
        }
    }
    /* —�?验证链：打印全部模型尺寸（看 63.60 是否出现）→ 再生 �?bbox 对比 —�?*/
    dims_report();
    {
        ProError er = ProSolidRegenerate((ProSolid)part, 0);
        logf_("[secdim] regenerate -> %d (%s)", (int)er, errname(er));
        {
            Pro3dPnt box2[2];
            if (ProSolidOutlineGet((ProSolid)part, box2) == PRO_TK_NO_ERROR)
                logf_("[secdim] bbox after = %.3f x %.3f x %.3f",
                      box2[1][0] - box2[0][0], box2[1][1] - box2[0][1], box2[1][2] - box2[0][2]);
        }
    }
}

/* ===== 轮次24：总长路线A 阶段1 —�?元素树重定义提交（ProFeatureWithoptionsRedefine�?====
   REDEF：备份记录（bbox + 特征树快照）�?ElemtreeExtract(基础拉伸) �?定位 PRO_E_SKETCHER
   �?ValueGet/DataGet �?section 指针 �?重扫端线�?�?3.6) �?ProSecdimCreate �?
   ProFeatureWithoptionsRedefine 提交 �?再生 �?bbox 守门�?3.580×41.100×4.000）→ dims 遍历验证�?
   任何一步失败即停并完整记录�?*/
typedef struct {
    ProElement sketcher;
} redef_ctx_t;

static ProError redef_find_action(ProElement elem_tree, ProElement elem, ProElempath elem_path, ProAppData appdata)
{
    redef_ctx_t *c = (redef_ctx_t *)appdata;
    ProElemId    id = (ProElemId)0;
    (void)elem_tree;
    (void)elem_path;
    if (c->sketcher) return PRO_TK_NO_ERROR;
    if (ProElementIdGet(elem, &id) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if ((int)id == PRO_E_SKETCHER) {
        c->sketcher = elem;
        logf_("[redef]   found PRO_E_SKETCHER element");
    }
    return PRO_TK_NO_ERROR;
}

/* 扫描 section 的线实体，找平行且间距最接近 63.6 的一对；返回 1=找到 */
static int find_pair_636(ProSection sec, int *pa, int *pb, double *pdist, Pro2dPnt place_out)
{
    ProIntlist idl = NULL;
    int        n = 0, i, lc = 0, a, b;
    struct { int id; double x1, y1, x2, y2; } L[256];
    double     best_d = -1e30, bcx = 0, bcy = 0, bnx = 0, bny = 0;
    int        ba = -1, bb = -1;

    if (ProSectionEntityIdsGet(sec, &idl, &n) != PRO_TK_NO_ERROR || !idl) return 0;
    for (i = 0; i < n && lc < 256; i++) {
        Pro2dEntdef *ent = NULL;
        if (ProSectionEntityGet(sec, idl[i], &ent) != PRO_TK_NO_ERROR || !ent) continue;
        if (ent->type == PRO_2D_LINE) {
            Pro2dLinedef *ld = (Pro2dLinedef *)ent;
            L[lc].id = idl[i];
            L[lc].x1 = ld->end1[0]; L[lc].y1 = ld->end1[1];
            L[lc].x2 = ld->end2[0]; L[lc].y2 = ld->end2[1];
            lc++;
        }
    }
    for (a = 0; a < lc; a++) {
        for (b = a + 1; b < lc; b++) {
            double ax = L[a].x2 - L[a].x1, ay = L[a].y2 - L[a].y1;
            double bx = L[b].x2 - L[b].x1, by = L[b].y2 - L[b].y1;
            double na = sqrt(ax * ax + ay * ay), nb = sqrt(bx * bx + by * by);
            double cross, dist;
            if (na < 1e-6 || nb < 1e-6) continue;
            cross = (ax * by - ay * bx) / (na * nb);
            if (cross > 0.02 || cross < -0.02) continue;
            {
                double mx = (L[b].x1 + L[b].x2) / 2.0 - (L[a].x1 + L[a].x2) / 2.0;
                double my = (L[b].y1 + L[b].y2) / 2.0 - (L[a].y1 + L[a].y2) / 2.0;
                dist = (mx * (-ay) + my * ax) / na;
                if (dist < 0) dist = -dist;
            }
            if (fabs(dist - 63.60) < 0.5 && dist > best_d) {
                best_d = dist;
                ba = L[a].id; bb = L[b].id;
                bcx = ((L[a].x1 + L[a].x2) + (L[b].x1 + L[b].x2)) / 4.0;
                bcy = ((L[a].y1 + L[a].y2) + (L[b].y1 + L[b].y2)) / 4.0;
                bnx = -ay / na; bny = ax / na;
            }
        }
    }
    ProArrayFree((ProArray *)&idl);
    logf_("[redef]   pair scan: lines=%d best dist=%.4f ids=%d,%d", lc, best_d, ba, bb);
    if (ba < 0) return 0;
    *pa = ba; *pb = bb; *pdist = best_d;
    place_out[0] = bcx + bnx * 12.0;
    place_out[1] = bcy + bny * 12.0;
    return 1;
}

static void redef_try(ProMdl part)
{
    secfinder_t f;
    int         fi, k = -1;
    ProElement  tree = NULL;

    {
        Pro3dPnt b[2];
        if (ProSolidOutlineGet((ProSolid)part, b) == PRO_TK_NO_ERROR)
            logf_("[redef] bbox BEFORE = %.3f x %.3f x %.3f",
                  b[1][0] - b[0][0], b[1][1] - b[0][1], b[1][2] - b[0][2]);
    }
    memset(&f, 0, sizeof(f));
    ProSolidFeatVisit((ProSolid)part, secfind_visit, secfind_filter, &f);
    for (fi = 0; fi < f.n; fi++)
        logf_("[redef] featsnap: protrusion[%d] id=%d nsec=%d", fi, f.fid[fi], f.nsec[fi]);
    for (fi = 0; fi < f.n; fi++) if (f.nsec[fi] > 0) { k = fi; break; }
    logf_("[redef] base protrusion index=%d (id=%d)", k, k >= 0 ? f.fid[k] : -1);
    if (k < 0) return;

    {
        ProError ee = ProFeatureElemtreeExtract(&f.feat[k], NULL, PRO_FEAT_EXTRACT_NO_OPTS, &tree);
        logf_("[redef] ElemtreeExtract -> %d (%s) tree=%p", (int)ee, errname(ee), (void *)tree);
        if (ee != PRO_TK_NO_ERROR || !tree) return;
    }
    {
        redef_ctx_t c;
        c.sketcher = NULL;
        {
            ProError ev = ProElemtreeElementVisit(tree, NULL, NULL, redef_find_action, &c);
            logf_("[redef] ElementVisit -> %d (%s)", (int)ev, errname(ev));
        }
        if (!c.sketcher) {
            logf_("[redef] no PRO_E_SKETCHER element");
            ProFeatureElemtreeFree(&f.feat[k], &tree);
            return;
        }
        {
            ProAppData sp = NULL;
            ProError   espv = ProElementSpecialvalueGet(c.sketcher, NULL, &sp);

            logf_("[redef] SpecialvalueGet(sketcher) -> %d (%s) ptr=%p", (int)espv, errname(espv), sp);
            if (espv == PRO_TK_NO_ERROR && sp) {
                ProSection sec = (ProSection)sp;
                int        ia = -1, ib = -1;
                double     d63 = 0;
                Pro2dPnt   place;

                place[0] = place[1] = 0;
                if (find_pair_636(sec, &ia, &ib, &d63, place)) {
                    int                 ids[2];
                    ProSectionPointType pt[2];
                    int                 dimid = -1;
                    ProError            ec;

                    ids[0] = ia; ids[1] = ib;
                    pt[0] = PRO_ENT_WHOLE; pt[1] = PRO_ENT_WHOLE;
                    ec = ProSecdimCreate(sec, ids, pt, 2, PRO_TK_DIM_LINE_LINE, place, &dimid);
                    logf_("[redef]   ProSecdimCreate(tree section) -> %d (%s) dim_id=%d",
                          (int)ec, errname(ec), dimid);
                    if (ec == PRO_TK_NO_ERROR && dimid >= 0) {
                        double v = -1.0;
                        if (ProSecdimValueGet(sec, dimid, &v) == PRO_TK_NO_ERROR)
                            logf_("[redef]   value=%.4f", v);
                    } else {
                        logf_("[redef] create failed; stop before redefine");
                        ProFeatureElemtreeFree(&f.feat[k], &tree);
                        return;
                    }
                } else {
                    logf_("[redef] no 63.6 pair in tree section; stop");
                    ProFeatureElemtreeFree(&f.feat[k], &tree);
                    return;
                }
                /* —�?提交重定�?—�?*/
                {
                    ProErrorlist errs;
                    ProError     er;

                    memset(&errs, 0, sizeof(errs));
                    er = ProFeatureWithoptionsRedefine(NULL, &f.feat[k], tree, NULL,
                                                       PRO_REGEN_NO_FLAGS, &errs);
                    logf_("[redef] WithoptionsRedefine -> %d (%s) errors=%d",
                          (int)er, errname(er), errs.error_number);
                    if (errs.error_number > 0 && errs.error_list)
                        logf_("[redef]   first error: item=%d type=%d code=%d",
                              errs.error_list[0].err_item_id,
                              (int)errs.error_list[0].err_item_type,
                              (int)errs.error_list[0].error);
                }
                /* —�?再生 + bbox 守门 —�?*/
                {
                    ProError eg = ProSolidRegenerate((ProSolid)part, PRO_REGEN_NO_FLAGS);
                    Pro3dPnt b2[2];

                    logf_("[redef] regenerate -> %d (%s)", (int)eg, errname(eg));
                    if (ProSolidOutlineGet((ProSolid)part, b2) == PRO_TK_NO_ERROR) {
                        double dx = b2[1][0] - b2[0][0];
                        double dy = b2[1][1] - b2[0][1];
                        double dz = b2[1][2] - b2[0][2];
                        logf_("[redef] bbox AFTER = %.3f x %.3f x %.3f", dx, dy, dz);
                        if (fabs(dx - 63.580) < 0.005 && fabs(dy - 41.100) < 0.005 && fabs(dz - 4.000) < 0.005)
                            logf_("[redef] bbox gate PASS");
                        else
                            logf_("[redef] *** bbox gate FAIL - geometry changed ***");
                    }
                }
                /* —�?决定性验证：能否遍历�?63.6000 —�?*/
                dims_report();
            } else {
                logf_("[redef] sketcher special value unavailable");
            }
        }
        ProFeatureElemtreeFree(&f.feat[k], &tree);
        logf_("[redef] done");
    }
}

/* ===== 轮次25：图纸侧标总长 63.6（照抄官�?UgDrawingDimensions.c 的结构）=====
   关键三点（此前全错）�?
   �?ProDimAttachment 数组 2 个条目，每项 [0]=选择副本、[1]=NULL�?
   �?ProSelectionViewSet(view, &sel) 传【地址】；
   �?ProDrawingDimensionCreate �?orient 参数�?PRO_DIM_ORNT_NONE（朝向放 sense）�?
   顺序：找主视�?�?扫两条端部竖�?�?选择+视图上下�?�?附件/朝向数组 �?
   创建 �?ProDimensionValueGet 回读 �?ProAnnotationShow 显示�?*/
static void tot_v2(ProMdl drw, ProMdl part, double lx, double ly)
{
    ProView   front = NULL;
    ProView  *views = NULL;
    ProEdge   le = NULL, re = NULL;
    ProGeomitem gi1, gi2;
    int       best_l = -1, best_r = -1, hid;
    double    blx = 1e30, brx = -1e30;

    if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
        int vc = 0, vi;
        ProArraySizeGet((ProArray)views, &vc);
        for (vi = 0; vi < vc; vi++) {
            ProName nm;
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[vi]; break; }
        }
        ProArrayFree((ProArray *)&views);
    }
    if (!front) { logf_("[tot2] no front view"); return; }

    {
        Pro3dPnt box[2];
        int major = 2;          /* 0=X 1=Y 2=Z : the longest bbox axis = length dir */
        if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) { logf_("[tot2] bbox failed"); return; }
        {
            double dx = box[1][0] - box[0][0], dy = box[1][1] - box[0][1], dz = box[1][2] - box[0][2];
            major = (dx >= dy && dx >= dz) ? 0 : ((dy >= dx && dy >= dz) ? 1 : 2);
            logf_("[tot2] bbox %.2f x %.2f x %.2f -> major axis=%s",
                  dx, dy, dz, major == 0 ? "X" : (major == 1 ? "Y" : "Z"));
        }
        /* The length is measured across the two end faces. In the front view the
           length runs horizontally, so the two edges to attach are the ones
           perpendicular to the major axis; take the two with the extreme
           coordinate along the major axis. Works for any plate orientation. */
        for (hid = 1; hid <= 6000; hid++) {
            ProEdge   e = NULL;
            Pro3dPnt *pts = NULL;
            int       cnt = 0, t;
            double    emin[3] = {1e30, 1e30, 1e30}, emax[3] = {-1e30, -1e30, -1e30};
            double    span_major, span_minor;

            if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
            if (ProEdgeTessellationGet(e, &pts, NULL, NULL, NULL, &cnt) != PRO_TK_NO_ERROR || !pts || cnt < 2) continue;
            for (t = 0; t < cnt; t++) {
                int k;
                for (k = 0; k < 3; k++) {
                    if (pts[t][k] < emin[k]) emin[k] = pts[t][k];
                    if (pts[t][k] > emax[k]) emax[k] = pts[t][k];
                }
            }
            ProArrayFree((ProArray *)&pts);
            span_major = emax[major] - emin[major];
            span_minor = 0.0;
            {
                int k;
                for (k = 0; k < 3; k++) if (k != major && (emax[k] - emin[k]) > span_minor) span_minor = emax[k] - emin[k];
            }
            /* end edge: thin along the length axis, substantial across it */
            if (span_major > 1.0) continue;
            if (span_minor < 5.0) continue;
            {
                ProGeomitem      gi0;
                ProGeomitemdata *gd0 = NULL;
                int              ltype = -1;

                if (ProEdgeToGeomitem((ProSolid)part, e, &gi0) == PRO_TK_NO_ERROR &&
                    ProGeomitemdataGet(&gi0, &gd0) == PRO_TK_NO_ERROR && gd0 && gd0->data.p_curve_data)
                    ltype = gd0->data.p_curve_data->line.type;
                if (gd0) ProGeomitemdataFree(&gd0);
                if (ltype != PRO_ENT_LINE) continue;
            }
            {
                double cx = emin[major];
                if (span_major <= 1.0 && span_minor >= 5.0)
                    logf_("[tot2] cand e=%d majorspan=%.4f minorspan=%.3f coord[%d]=%.3f (bbox %.1f..%.1f)",
                          hid, span_major, span_minor, major, cx, box[0][major], box[1][major]);
                /* Hard window: the edge must lie INSIDE the solid bbox. Stray
                   curves outside it (e.g. copied reference geometry at
                   z=-314.2 for nb109_5) must never win, or the "total length"
                   measures from a curve that is not on the part at all. */
                if (cx < box[0][major] - 0.05 || cx > box[1][major] + 0.05) continue;
                /* And it must sit against one of the two bbox limits. */
                if (cx > box[0][major] + 0.6 && cx < box[1][major] - 0.6) continue;
                if (cx < blx) { blx = cx; best_l = hid; }
                if (cx > brx) { brx = cx; best_r = hid; }
            }
        }
        logf_("[tot2] bbox major range [%.3f .. %.3f]", box[0][major], box[1][major]);
    }
    logf_("[tot2] picked L x=%.2f e=%d ; R x=%.2f e=%d", blx, best_l, brx, best_r);
    if (best_l < 0 || best_r < 0) return;
    ProEdgeInit((ProSolid)part, best_l, &le);
    ProEdgeInit((ProSolid)part, best_r, &re);
    if (!le || !re) { logf_("[tot2] edgeInit failed"); return; }
    if (ProEdgeToGeomitem((ProSolid)part, le, &gi1) != PRO_TK_NO_ERROR) { logf_("[tot2] gi L failed"); return; }
    if (ProEdgeToGeomitem((ProSolid)part, re, &gi2) != PRO_TK_NO_ERROR) { logf_("[tot2] gi R failed"); return; }
    {
        ProSelection     *att = NULL;
        ProDimAttachment *arr = NULL;
        ProDimSense      *sen = NULL;
        ProDimension      dim;
        ProVector         dim_pos;
        ProError          e;

        ProArrayAlloc(2, sizeof(ProSelection), 1, (ProArray *)&att);
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &att[0]) != PRO_TK_NO_ERROR) { logf_("[tot2] sel L failed"); return; }
        ProSelectionViewSet(front, &att[0]);          /* �?官方：传地址 */
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &att[1]) != PRO_TK_NO_ERROR) { logf_("[tot2] sel R failed"); return; }
        ProSelectionViewSet(front, &att[1]);
        logf_("[tot2] selections built (view ctx by-pointer)");

        ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);   /* �?2 条目 */
        ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
        ProSelectionCopy(att[0], &arr[0][0]);   /* �?[0]=选择副本 */
        arr[0][1] = NULL;                       /* �?[1]=NULL */
        ProSelectionCopy(att[1], &arr[1][0]);
        arr[1][1] = NULL;
        memset(&sen[0], 0, sizeof(ProDimSense));
        memset(&sen[1], 0, sizeof(ProDimSense));
        sen[0].type = PRO_DIM_SNS_TYP_NONE;
        sen[0].orient_hint = PRO_DIM_ORNT_HORIZ;
        sen[1].type = PRO_DIM_SNS_TYP_NONE;
        sen[1].orient_hint = PRO_DIM_ORNT_HORIZ;
        logf_("[tot2] att arr: 2 entries, [0][1]=NULL [1][1]=NULL; senses=NONE/HORIZ x2");

        dim_pos[0] = lx;
        dim_pos[1] = ly;
        dim_pos[2] = 0.0;
        memset(&dim, 0, sizeof(dim));
        e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, dim_pos,
                                      PRO_B_FALSE, &dim);
        logf_("[tot2] ProDrawingDimensionCreate -> %d (%s)", (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            double v = -1.0;
            if (ProDimensionValueGet(&dim, &v) == PRO_TK_NO_ERROR)
                logf_("[tot2] dim value = %.4f", v);
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&dim, NULL, front);
                logf_("[tot2] ProAnnotationShow -> %d (%s)", (int)es, errname(es));
            }
        }
        ProArrayFree((ProArray *)&arr);
        ProArrayFree((ProArray *)&sen);
        ProSelectionFree(&att[0]);
        ProSelectionFree(&att[1]);
        ProArrayFree((ProArray *)&att);
    }
}

/* ===== 轮次24 尝试3：section 副本"写入是否提交"的计数定�?=====
   copyA 取尺寸数 �?�?copyA 上创�?63.6 �?copyA 再数 �?全新 copyB 数：
   �?copyB 数量 == copyA(创建�? �?提交成功（弱尺寸，模型遍历不可见）；
   �?copyB == 原始�?�?副本写入不提交�?*/
static void secdim_count_test(ProMdl part)
{
    secfinder_t f;
    int        fi, k = -1;
    int        nA = -1, nA2 = -1, nB = -1;

    memset(&f, 0, sizeof(f));
    ProSolidFeatVisit((ProSolid)part, secfind_visit, secfind_filter, &f);
    for (fi = 0; fi < f.n; fi++) if (f.nsec[fi] > 0) { k = fi; break; }
    logf_("[seccnt] base protrusion index=%d (id=%d)", k, k >= 0 ? f.fid[k] : -1);
    if (k < 0) return;

    {
        ProSection secA = NULL;
        ProIntlist idsA = NULL;

        if (ProFeatureSectionCopy(&f.feat[k], 0, &secA) != PRO_TK_NO_ERROR || !secA) {
            logf_("[seccnt] copyA failed");
            return;
        }
        if (ProSecdimIdsGet(secA, &idsA, &nA) != PRO_TK_NO_ERROR) nA = -1;
        logf_("[seccnt] copyA dim count = %d", nA);
        if (idsA) ProArrayFree((ProArray *)&idsA);
        {
            int       ia = -1, ib = -1;
            double    d63 = 0;
            Pro2dPnt  place;

            place[0] = place[1] = 0;
            if (find_pair_636(secA, &ia, &ib, &d63, place)) {
                int                 ids[2];
                ProSectionPointType pt[2];
                int                 dimid = -1;
                ProError            ec;

                ids[0] = ia; ids[1] = ib;
                pt[0] = PRO_ENT_WHOLE; pt[1] = PRO_ENT_WHOLE;
                ec = ProSecdimCreate(secA, ids, pt, 2, PRO_TK_DIM_LINE_LINE, place, &dimid);
                logf_("[seccnt] create on copyA -> %d (%s) dim=%d", (int)ec, errname(ec), dimid);
            }
        }
        {
            ProIntlist ids2 = NULL;

            if (ProSecdimIdsGet(secA, &ids2, &nA2) != PRO_TK_NO_ERROR) nA2 = -1;
            logf_("[seccnt] copyA dim count after create = %d", nA2);
            if (ids2) ProArrayFree((ProArray *)&ids2);
        }
        ProSectionFree(&secA);
    }
    {
        ProSection secB = NULL;
        ProIntlist idsB = NULL;

        if (ProFeatureSectionCopy(&f.feat[k], 0, &secB) == PRO_TK_NO_ERROR && secB) {
            if (ProSecdimIdsGet(secB, &idsB, &nB) != PRO_TK_NO_ERROR) nB = -1;
            logf_("[seccnt] FRESH copyB dim count = %d", nB);
            if (idsB) ProArrayFree((ProArray *)&idsB);
            ProSectionFree(&secB);
        } else {
            logf_("[seccnt] copyB failed");
        }
    }
    logf_("[seccnt] VERDICT: original=%d, after-create=%d, fresh-copy=%d -> %s",
          nA, nA2, nB,
          (nB == nA2 && nA2 > nA) ? "COMMITTED (weak dim; not in model visit)" :
          (nB == nA ? "NOT COMMITTED (copy write-back does not persist)" : "UNEXPECTED"));
}

/* ===== 轮次26（任�?）：把模型自带（显示出来）的尺寸转成图纸侧创�?=====
   CONVDIM|<�?：对每个"已显示且值匹�?的模型尺寸：
   �?ProDimensionAttachmentsGet 读原附件/朝向（同一测量对象！）�?
   �?选择加视图上下文后原样重�?�?ProDrawingDimensionCreate（验证过的结构）�?
   �?复刻公差/△（�?showk 同规则）�?ProAnnotationShow 新尺寸；
   �?ProAnnotationUndisplay 旧尺寸（同值只转一根；其余同值旧实例一并隐藏）�?
   �?ProDrawingDimensionMove 到旧文本位置（排版不变）�?
   —�?�?regen（Undisplay �?直到下次再生"约束正好满足）�?*/
typedef struct {
    double     target;
    ProDrawing drw;
    ProView    view;
    ProMdl     part;
    int        converted;
    int        hidden;
    int        has_pos;      /* 1 = 使用显式目标位置（tx,ty�?*/
    double     tx, ty;
    int        view_kind;    /* 0=front（主视图�? 1=左侧（侧视图�?.8 用） */
    int        route;        /* 0=边对（默认）, 1=面附件（41.3/4.8�? 2=边对+同平面过滤（0.25�? 3=视图平面共面实验 */
    int        only_shown;   /* 1 = 只处理图纸上真正显示的实例（同值多实例防拿错几何） */
    int        attempted;    /* 轮次32：该值已尝试过一轮——同值多实例不再重复试建（预�?2�?*/
    int        no_move;      /* 轮次35 实验B：创建即最终位置，跳过 ProDrawingDimensionMove */
    ProVector  vp_n;         /* route 3：从视图推导的注释平面法向（模型空间，已归一�?*/
    ProPoint3d vp_o;         /* route 3：该平面原点 */
    int        vp_ok;        /* 1 = 视图平面可用 */
} conv_ctx_t;

static void conv_apply_tol(ProDimension *dim, double v)
{
    double tol = -1.0;
    if ((v > 22.99 && v < 23.01) || (v > 23.49 && v < 23.51) || (v > 19.99 && v < 20.01)) tol = 0.10;
    else if ((v > 1.99 && v < 2.01) || (v > 0.24 && v < 0.26) || (v > 4.79 && v < 4.81)) tol = 0.05;
    if (tol > 0) {
        ProName tab;
        tab[0] = 0;
        {
            ProError elb = ProDimensionTollabelSet(dim, PROTOLTABLE_NONE, tab, 0);
            ProError e1 = ProDimensionToltypeSet(dim, PRO_TOL_PLUS_MINUS_SYM);
            ProError e2 = ProDimensionToleranceSet(dim, tol, -tol);
            logf_("[conv]   tol %.2f: tollabel->%d type->%d set->%d", tol, (int)elb, (int)e1, (int)e2);
        }
    }
    /* 轮次46：△ 字符后缀取消——重要控制项改用符号�?yat/jizhun 的符号实例（SYMARKS 放编号） */
}

/* ===== 轮次27：面附件路线�?1.3 / 4.8�?====
   41.3/4.8 是厚�?深度类尺寸，附件是【面】而不是边对；r26 的边对扫描对它们无候选�?
   面来源：ProSurfaceInit + ProSurfaceDataGet（旧 faceold 版验证过枚举可行），
   选择�?ProSurfaceToGeomitem + ProSelectionAlloc，附件结构仍用已验证�?
   2 条目、每�?[1]=NULL（与 tot_v2 相同）�?*/
/* ===== 轮次45 改造A/B（公共工具）：包围盒 / 边合法�?/ 中心合法�?/ 矩形与线段几�?=====
   合法性两道检查（简报要求）：① 两端点全�?ProSolidOutlineGet 包围盒内（X/Y/Z 三轴都查）；
   �?长度 �?该边主轴方向�?bbox 跨度（如 283.6 > 41.1 的辅助构造线 �?直接排除）�?*/
typedef struct { double x0, y0, z0, x1, y1, z1; } ab_bbox_t;

static void ab_bbox_get(ProMdl part, ab_bbox_t *b)
{
    Pro3dPnt box[2];

    box[0][0] = box[0][1] = box[0][2] = -9999.0;
    box[1][0] = box[1][1] = box[1][2] = -9999.0;
    ProSolidOutlineGet((ProSolid)part, box);
    b->x0 = box[0][0]; b->y0 = box[0][1]; b->z0 = box[0][2];
    b->x1 = box[1][0]; b->y1 = box[1][1]; b->z1 = box[1][2];
}

static int ab_legal_edge(const ab_bbox_t *b, const double p1[3], const double p2[3],
                         double len, char *why, size_t whycap)
{
    double adx = fabs(p2[0] - p1[0]), ady = fabs(p2[1] - p1[1]), adz = fabs(p2[2] - p1[2]);
    double span, tol = 0.6;
    const char *ax;
    int i;

    for (i = 0; i < 2; i++) {
        const double *p = i ? p2 : p1;

        if (p[0] < b->x0 - tol || p[0] > b->x1 + tol ||
            p[1] < b->y0 - tol || p[1] > b->y1 + tol ||
            p[2] < b->z0 - tol || p[2] > b->z1 + tol) {
            sprintf_s(why, whycap, "endpoint%d (%.1f,%.1f,%.1f) outside bbox (any axis)", i + 1, p[0], p[1], p[2]);
            return 0;
        }
    }
    if (adx >= ady && adx >= adz)      { span = b->x1 - b->x0; ax = "X"; }
    else if (ady >= adx && ady >= adz) { span = b->y1 - b->y0; ax = "Y"; }
    else                               { span = b->z1 - b->z0; ax = "Z"; }
    if (len > span + 1.0) {
        sprintf_s(why, whycap, "len %.1f > bbox %s span %.1f (construction/panel-like)", len, ax, span);
        return 0;
    }
    sprintf_s(why, whycap, "OK len %.1f <= %s span %.1f", len, ax, span);
    return 1;
}

static int ab_legal_center(const ab_bbox_t *b, double cx, double cy, double cz, char *why, size_t whycap)
{
    if (cx < b->x0 - 0.6 || cx > b->x1 + 0.6 ||
        cy < b->y0 - 0.6 || cy > b->y1 + 0.6 ||
        cz < b->z0 - 0.6 || cz > b->z1 + 0.6) {
        sprintf_s(why, whycap, "center (%.1f,%.1f,%.1f) outside bbox", cx, cy, cz);
        return 0;
    }
    sprintf_s(why, whycap, "OK center in bbox");
    return 1;
}

typedef struct { double x0, y0, x1, y1; const char *tag; } ab_rect_t;

static double ab_pt_rect_dist(double x, double y, const ab_rect_t *r)
{
    double dx = 0.0, dy = 0.0;

    if (x < r->x0) dx = r->x0 - x; else if (x > r->x1) dx = x - r->x1;
    if (y < r->y0) dy = r->y0 - y; else if (y > r->y1) dy = y - r->y1;
    return sqrt(dx * dx + dy * dy);
}

static int ab_seg_hits_rect(double ax, double ay, double bx, double by, const ab_rect_t *r)
{
    int i;

    for (i = 0; i <= 8; i++) {
        double t = (double)i / 8.0;
        double x = ax + (bx - ax) * t;
        double y = ay + (by - ay) * t;

        if (x >= r->x0 - 0.5 && x <= r->x1 + 0.5 && y >= r->y0 - 0.5 && y <= r->y1 + 0.5) return 1;
    }
    return 0;
}

static double ab_rect_gap(const ab_rect_t *a, const ab_rect_t *b)
{
    double dx = 0.0, dy = 0.0;

    if (a->x1 < b->x0) dx = b->x0 - a->x1; else if (b->x1 < a->x0) dx = a->x0 - b->x1;
    if (a->y1 < b->y0) dy = b->y0 - a->y1; else if (b->y1 < a->y0) dy = a->y0 - b->y1;
    if (dx > 0.0 || dy > 0.0) return sqrt(dx * dx + dy * dy);
    return -1.0;   /* 相交 */
}

typedef struct { int id; double ox, oy, oz, nx, ny, nz; } conv_fplane_t;
typedef struct { int ia, ib; double dist, err, prox; } conv_fpair_t;   /* 匿名结构体不能跨声明点赋值（C2440�?*/
typedef struct { int ia, ib; double dist, err, mz; } conv_chkpair_t;

static int conv_faces_collect(ProMdl part, conv_fplane_t *FP, int cap)
{
    int id, n = 0;

    for (id = 100; id <= 4000 && n < cap; id++) {   /* id>=100 跳过基准面（�?faceold�?*/
        ProSurface      s = NULL;
        ProGeomitemdata *gd = NULL;

        if (ProSurfaceInit((ProMdl)part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        if (gd->data.p_surface_data->type == PRO_SRF_PLANE) {
            ProSurfacedata *sd = gd->data.p_surface_data;

            FP[n].id = id;
            FP[n].ox = sd->srf_shape.plane.origin[0];
            FP[n].oy = sd->srf_shape.plane.origin[1];
            FP[n].oz = sd->srf_shape.plane.origin[2];
            FP[n].nx = sd->srf_shape.plane.e3[0];
            FP[n].ny = sd->srf_shape.plane.e3[1];
            FP[n].nz = sd->srf_shape.plane.e3[2];
            n++;
        }
        ProGeomitemdataFree(&gd);
    }
    return n;
}

/* 用两个已带视图上下文的选择 + 已验证的 2 条目附件结构建尺寸；
   成功则复刻公�?△、显示新的、隐藏旧的、移�?p0。返�?1=成功�?*/
static int conv_create_with_sels(conv_ctx_t *c, ProDimension *olddim, double v,
                                 ProSelection selA, ProSelection selB,
                                 ProDimOrient orienth, int sensekind, ProPoint3d p0)
{
    ProDimAttachment *arr2 = NULL;
    ProDimSense      *sen2 = NULL;
    ProDimension      nd;
    ProVector         loc;
    ProError          ec;
    int               ok = 0;

    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr2);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen2);
    ProSelectionCopy(selA, &arr2[0][0]); arr2[0][1] = NULL;
    ProSelectionCopy(selB, &arr2[1][0]); arr2[1][1] = NULL;
    memset(&sen2[0], 0, sizeof(ProDimSense));
    memset(&sen2[1], 0, sizeof(ProDimSense));
    if (sensekind == 1) {          /* �?圆：两点圆心 */
        sen2[0].type = PRO_DIM_SNS_TYP_PNT; sen2[0].sense = PRO_POINT_TYP_CENTER; sen2[0].orient_hint = orienth;
        sen2[1].type = PRO_DIM_SNS_TYP_PNT; sen2[1].sense = PRO_POINT_TYP_CENTER; sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
    } else if (sensekind == 2) {   /* �?圆：�?+ 圆心 */
        sen2[0].type = PRO_DIM_SNS_TYP_NONE; sen2[0].orient_hint = orienth;
        sen2[1].type = PRO_DIM_SNS_TYP_PNT; sen2[1].sense = PRO_POINT_TYP_CENTER; sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
    } else {                       /* �?�?/ �?�?*/
        sen2[0].type = PRO_DIM_SNS_TYP_NONE; sen2[0].orient_hint = orienth;
        sen2[1].type = PRO_DIM_SNS_TYP_NONE; sen2[1].orient_hint = orienth;
    }
    loc[0] = p0[0];
    loc[1] = p0[1];
    loc[2] = 0.0;
    memset(&nd, 0, sizeof(nd));
    ec = ProDrawingDimensionCreate((ProDrawing)c->drw, arr2, sen2, PRO_DIM_ORNT_NONE,
                                   loc, PRO_B_FALSE, &nd);
    logf_("[conv]   create -> %d (%s)", (int)ec, errname(ec));
    if (ec == PRO_TK_NO_ERROR) {
        double nv = -1.0;

        if (ProDimensionValueGet(&nd, &nv) == PRO_TK_NO_ERROR)
            logf_("[conv]   new value = %.4f (old %.4f) %s", nv, v,
                  (nv > v - 0.02 && nv < v + 0.02) ? "MATCH" : "*** MISMATCH ***");
        conv_apply_tol(&nd, v);
        {
            ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, c->view);
            logf_("[conv]   show new -> %d (%s)", (int)es, errname(es));
        }
        {
            ProError eh = ProAnnotationUndisplay((ProAnnotation *)olddim, NULL, (ProDrawing)c->drw);
            logf_("[conv]   undisplay old -> %d (%s)", (int)eh, errname(eh));
            if (eh == PRO_TK_NO_ERROR) c->hidden++;
        }
        if (p0[0] > -9998.0 && !c->no_move) {
            ProError em = ProDrawingDimensionMove((ProDrawing)c->drw, &nd, p0);
            logf_("[conv]   move new -> old pos : %d (%s)", (int)em, errname(em));
        } else if (c->no_move) {
            logf_("[conv]   no_move mode (round35-B): Move skipped; create-loc = final pos");
        }
        c->converted++;
        ok = 1;
        /* 轮次49：登记最后创建的尺寸句柄（供 ab_dim_fixup 拖动/复检�?*/
        g_last_nd = nd;
        g_last_nd_valid = 1;
        g_last_nd_value = v;
    }
    ProArrayFree((ProArray *)&arr2);
    ProArrayFree((ProArray *)&sen2);
    return ok;
}

/* 面路线本体：平行平面对（间距≈目标）�?�?top-2 试建（上�?2，遵照本轮预算）�?*/
static void conv_face_route(conv_ctx_t *c, ProDimension *olddim, double v,
                            ProPoint3d p0, ProPoint3d dl)
{
    static conv_fplane_t FP[400];
    conv_fpair_t CD2[300];
    int nF, nD = 0, a, b, k;

    nF = conv_faces_collect(c->part, FP, 400);
    logf_("[conv]   face scan: faces=%d target=%.4f", nF, c->target);
    for (k = 0; k < nF && k < 400; k++) {
        if (k < 10)
            logf_("[conv]   f%d id=%d o=(%.1f,%.1f,%.1f) n=(%.3f,%.3f,%.3f)",
                  k, FP[k].id, FP[k].ox, FP[k].oy, FP[k].oz, FP[k].nx, FP[k].ny, FP[k].nz);
    }
    for (a = 0; a < nF; a++) {
        for (b = a + 1; b < nF; b++) {
            double dot = FP[a].nx * FP[b].nx + FP[a].ny * FP[b].ny + FP[a].nz * FP[b].nz;
            double dx, dy, dz2, dist, err, prox;

            if (fabs(dot) < 0.999) continue;
            /* 轮次32：排�?法向平行于视�?的面对（在该视图里投影退化，不可能在此视图标注）�?
               前视图视�?z（掉 |nz|>0.9）；侧视图视�?x（掉 |nx|>0.9）�?*/
            if (c->view_kind == 0) { if (fabs(FP[a].nz) > 0.9) continue; }
            else                   { if (fabs(FP[a].nx) > 0.9) continue; }
            dx  = FP[b].ox - FP[a].ox;
            dy  = FP[b].oy - FP[a].oy;
            dz2 = FP[b].oz - FP[a].oz;
            dist = fabs(dx * FP[a].nx + dy * FP[a].ny + dz2 * FP[a].nz);
            err = fabs(dist - c->target);
            if (err > 0.06 || nD >= 300) continue;
            prox = 0;
            if (dl[0] > -9998.0) {
                double mx = (FP[a].ox + FP[b].ox) / 2 - dl[0];
                double my = (FP[a].oy + FP[b].oy) / 2 - dl[1];
                prox = sqrt(mx * mx + my * my);
            }
            CD2[nD].ia = a; CD2[nD].ib = b;
            CD2[nD].dist = dist; CD2[nD].err = err; CD2[nD].prox = prox;
            nD++;
        }
    }
    /* �?err*1000+prox 升序 */
    for (a = 1; a < nD; a++) {
        conv_fpair_t tmp = CD2[a];
        b = a - 1;
        while (b >= 0 && (CD2[b].err * 1000.0 + CD2[b].prox) > (tmp.err * 1000.0 + tmp.prox)) { CD2[b + 1] = CD2[b]; b--; }
        CD2[b + 1] = tmp;
    }
    for (k = 0; k < nD && k < 6; k++)
        logf_("[conv]   fpair[%d] ids=%d,%d dist=%.4f err=%.4f n=(%.2f,%.2f,%.2f) prox=%.1f",
              k, FP[CD2[k].ia].id, FP[CD2[k].ib].id, CD2[k].dist, CD2[k].err,
              FP[CD2[k].ia].nx, FP[CD2[k].ia].ny, FP[CD2[k].ia].nz, CD2[k].prox);
    if (nD <= 0) {
        logf_("[conv]   NO face pair near %.4f; old display kept", c->target);
        return;
    }
    {
        int di, done = 0;

        for (di = 0; di < nD && di < 2 && !done; di++) {
            ProSurface    sa = NULL, sb = NULL;
            ProGeomitem   g1, g2;
            ProSelection  sel0 = NULL, sel1 = NULL;
            ProDimOrient  oh;
            double        ax, ay, az;

            logf_("[conv]   ftry[%d]: ids=%d,%d dist=%.4f n=(%.2f,%.2f,%.2f)",
                  di, FP[CD2[di].ia].id, FP[CD2[di].ib].id, CD2[di].dist,
                  FP[CD2[di].ia].nx, FP[CD2[di].ia].ny, FP[CD2[di].ia].nz);
            if (ProSurfaceInit((ProMdl)c->part, FP[CD2[di].ia].id, &sa) != PRO_TK_NO_ERROR || !sa) continue;
            if (ProSurfaceInit((ProMdl)c->part, FP[CD2[di].ib].id, &sb) != PRO_TK_NO_ERROR || !sb) continue;
            if (ProSurfaceToGeomitem((ProSolid)c->part, sa, &g1) != PRO_TK_NO_ERROR) continue;
            if (ProSurfaceToGeomitem((ProSolid)c->part, sb, &g2) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &sel0) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionViewSet(c->view, &sel0) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); continue; }
            if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); continue; }
            if (ProSelectionViewSet(c->view, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); ProSelectionFree(&sel1); continue; }
            /* orient_hint 由法向主分量定：X 向→水平（主视图�? Y 向→垂直 / Z 向→水平（侧视图�?*/
            ax = fabs(FP[CD2[di].ia].nx); ay = fabs(FP[CD2[di].ia].ny); az = fabs(FP[CD2[di].ia].nz);
            if (ax >= ay && ax >= az)      oh = PRO_DIM_ORNT_HORIZ;
            else if (ay >= az)             oh = PRO_DIM_ORNT_VERT;
            else                           oh = PRO_DIM_ORNT_HORIZ;
            done = conv_create_with_sels(c, olddim, v, sel0, sel1, oh, 0, p0);
            ProSelectionFree(&sel0);
            ProSelectionFree(&sel1);
        }
        if (!done) {
            /* 轮次32 R1：面路线失败不再回显旧尺寸（禁止"显示"出图——缺比显好） */
            logf_("[conv] *** face route: all tries failed; old dim NOT re-shown (R1: display forbidden) ***");
        }
    }
}

/* ===== 轮次33：Z 向间距路线（route Z�?====
   用户口径�?0.25�?.25 厚法兰台阶的"顶面/底面"两条长边——间距在 **Z** 方向�?
   在俯视图里渲染为"竖直"尺寸（上下箭头相对）�?D(x,y) 距离扫描对这种纯 Z 间距
   投影�?dist=0，永远进不了候选（20+ 次试建全盲的根因之一）�?
   本路线：只收"方向⊥视向、同深度、x/y 投影重合�?D 垂距≈目�?的平行线对�?*/
typedef struct { int id; double ax, ay, az, bx, by, bz, mx, my, mz; } zgap_line_t;
typedef struct { int ia, ib; double dist3, err, prox, score; } zgap_pair_t;

static void conv_zgap_route(conv_ctx_t *c, ProDimension *olddim, double v, ProPoint3d p0, ProPoint3d dl)
{
    static zgap_line_t LL[500];
    static zgap_pair_t CD[500];
    int nL = 0, nD = 0, hid, a, b;

    for (hid = 1; hid <= 3000 && nL < 500; hid++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;

        if (ProEdgeInit((ProSolid)c->part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)c->part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        if (gd->data.p_curve_data->line.type == PRO_ENT_LINE) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double ddx = ld->end2[0] - ld->end1[0];
            double ddy = ld->end2[1] - ld->end1[1];
            double ddz = ld->end2[2] - ld->end1[2];
            double len = sqrt(ddx * ddx + ddy * ddy + ddz * ddz);

            if (len > 1.0) {
                int dup = 0, k;

                for (k = 0; k < nL; k++) {   /* 去重：同一条几何线可能有重复边（dxy 过滤的前提） */
                    if (fabs(LL[k].ax - ld->end1[0]) < 1e-3 && fabs(LL[k].ay - ld->end1[1]) < 1e-3 &&
                        fabs(LL[k].az - ld->end1[2]) < 1e-3 && fabs(LL[k].bx - ld->end2[0]) < 1e-3 &&
                        fabs(LL[k].by - ld->end2[1]) < 1e-3 && fabs(LL[k].bz - ld->end2[2]) < 1e-3) { dup = 1; break; }
                    if (fabs(LL[k].ax - ld->end2[0]) < 1e-3 && fabs(LL[k].ay - ld->end2[1]) < 1e-3 &&
                        fabs(LL[k].az - ld->end2[2]) < 1e-3 && fabs(LL[k].bx - ld->end1[0]) < 1e-3 &&
                        fabs(LL[k].by - ld->end1[1]) < 1e-3 && fabs(LL[k].bz - ld->end1[2]) < 1e-3) { dup = 1; break; }
                }
                if (!dup) {
                    LL[nL].id = hid;
                    LL[nL].ax = ld->end1[0]; LL[nL].ay = ld->end1[1]; LL[nL].az = ld->end1[2];
                    LL[nL].bx = ld->end2[0]; LL[nL].by = ld->end2[1]; LL[nL].bz = ld->end2[2];
                    LL[nL].mx = (LL[nL].ax + LL[nL].bx) / 2;
                    LL[nL].my = (LL[nL].ay + LL[nL].by) / 2;
                    LL[nL].mz = (LL[nL].az + LL[nL].bz) / 2;
                    nL++;
                }
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[conv]   zgap scan: uniq lines=%d target=%.4f", nL, c->target);
    for (a = 0; a < nL; a++) {
        for (b = a + 1; b < nL; b++) {
            double ua[3], ub[3], la, lb, cross, px, py, pz, t, dist3, dxy, dz2, err, prox;

            ua[0] = LL[a].bx - LL[a].ax; ua[1] = LL[a].by - LL[a].ay; ua[2] = LL[a].bz - LL[a].az;
            ub[0] = LL[b].bx - LL[b].ax; ub[1] = LL[b].by - LL[b].ay; ub[2] = LL[b].bz - LL[b].az;
            la = sqrt(ua[0] * ua[0] + ua[1] * ua[1] + ua[2] * ua[2]);
            lb = sqrt(ub[0] * ub[0] + ub[1] * ub[1] + ub[2] * ub[2]);
            if (la < 1e-9 || lb < 1e-9) continue;
            ua[0] /= la; ua[1] /= la; ua[2] /= la;
            ub[0] /= lb; ub[1] /= lb; ub[2] /= lb;
            cross = fabs(ua[0] * ub[1] - ua[1] * ub[0]) + fabs(ua[1] * ub[2] - ua[2] * ub[1]) + fabs(ua[2] * ub[0] - ua[0] * ub[2]);
            if (cross > 0.03) continue;   /* 平行 */
            /* 方向必须⊥视向（否则在该视图投影退化，无法标注�?*/
            if (c->view_kind == 2)      { if (fabs(ua[1]) > 0.3) continue; }   /* 俯视图：视向=y */
            else if (c->view_kind == 1) { if (fabs(ua[0]) > 0.3) continue; }   /* 侧视图：视向=x */
            else                        { if (fabs(ua[2]) > 0.3) continue; }   /* 主视图：视向=z */
            /* 同深度：y（俯视图）基本相�?*/
            if (fabs(LL[a].my - LL[b].my) > 0.3) continue;
            /* x/y 投影必须几乎重合（纯 Z 向间距） */
            dxy = sqrt((LL[a].mx - LL[b].mx) * (LL[a].mx - LL[b].mx) + (LL[a].my - LL[b].my) * (LL[a].my - LL[b].my));
            if (dxy > 0.05) continue;
            /* 3D 垂距 */
            px = LL[b].mx - LL[a].mx; py = LL[b].my - LL[a].my; pz = LL[b].mz - LL[a].mz;
            t = px * ua[0] + py * ua[1] + pz * ua[2];
            px -= t * ua[0]; py -= t * ua[1]; pz -= t * ua[2];
            dist3 = sqrt(px * px + py * py + pz * pz);
            err = fabs(dist3 - c->target);
            if (err > 0.06 || nD >= 500) continue;
            dz2 = fabs(LL[a].mz - LL[b].mz);
            if (dz2 < 0.1) continue;      /* 只收�?z 间距的对 */
            prox = 0;
            if (dl[0] > -9998.0) {
                double mmx = (LL[a].mx + LL[b].mx) / 2 - dl[0];
                double mmy = (LL[a].my + LL[b].my) / 2 - dl[1];
                prox = sqrt(mmx * mmx + mmy * mmy);
            }
            CD[nD].ia = a; CD[nD].ib = b;
            CD[nD].dist3 = dist3; CD[nD].err = err; CD[nD].prox = prox;
            CD[nD].score = err * 1000.0 + prox;
            nD++;
        }
    }
    logf_("[conv]   zgap candidates nD=%d", nD);
    for (a = 1; a < nD; a++) {   /* �?score 升序 */
        zgap_pair_t tmp = CD[a];
        b = a - 1;
        while (b >= 0 && CD[b].score > tmp.score) { CD[b + 1] = CD[b]; b--; }
        CD[b + 1] = tmp;
    }
    for (a = 0; a < nD && a < 6; a++)
        logf_("[conv]   ztop[%d] ids=%d,%d dist3=%.4f err=%.4f dz=%.1f prox=%.1f",
              a, LL[CD[a].ia].id, LL[CD[a].ib].id, CD[a].dist3, CD[a].err,
              fabs(LL[CD[a].ia].mz - LL[CD[a].ib].mz), CD[a].prox);
    {
        /* 轮次45 改造A：z 路线候选合法性判定（同上两道检查） */
        ab_bbox_t abb;

        ab_bbox_get(c->part, &abb);
        for (a = 0; a < nD && a < 4; a++) {
            int ias[2], k;

            ias[0] = CD[a].ia; ias[1] = CD[a].ib;
            for (k = 0; k < 2; k++) {
                int    ia2 = ias[k];
                double p1[3], p2[3], dx, dy, dz, len;
                char   why[96];
                int    lg;

                p1[0] = LL[ia2].ax; p1[1] = LL[ia2].ay; p1[2] = LL[ia2].az;
                p2[0] = LL[ia2].bx; p2[1] = LL[ia2].by; p2[2] = LL[ia2].bz;
                dx = p2[0] - p1[0]; dy = p2[1] - p1[1]; dz = p2[2] - p1[2];
                len = sqrt(dx * dx + dy * dy + dz * dz);
                lg = ab_legal_edge(&abb, p1, p2, len, why, sizeof(why));
                logf_("[legal] ztop[%d] edge %d -> %s (%s)", a, LL[ia2].id, lg ? "OK" : "REJECT", why);
            }
        }
    }
    if (nD <= 0) {
        logf_("[conv]   NO z-gap pair near %.4f; nothing created", c->target);
        return;
    }
    {
        int di, done = 0;

        for (di = 0; di < nD && di < 2 && !done; di++) {
            int           ia2 = CD[di].ia, ib2 = CD[di].ib;
            ProEdge       e1 = NULL, e2 = NULL;
            ProGeomitem   g1, g2;
            ProSelection  sel0 = NULL, sel1 = NULL;

            logf_("[conv]   ztry[%d]: ids=%d,%d dist3=%.4f", di, LL[ia2].id, LL[ib2].id, CD[di].dist3);
            ProEdgeInit((ProSolid)c->part, LL[ia2].id, &e1);
            ProEdgeInit((ProSolid)c->part, LL[ib2].id, &e2);
            if (!e1 || !e2) { logf_("[conv]   edgeInit failed"); continue; }
            if (ProEdgeToGeomitem((ProSolid)c->part, e1, &g1) != PRO_TK_NO_ERROR) continue;
            if (ProEdgeToGeomitem((ProSolid)c->part, e2, &g2) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &sel0) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionViewSet(c->view, &sel0) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); continue; }
            if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); continue; }
            if (ProSelectionViewSet(c->view, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); ProSelectionFree(&sel1); continue; }
            /* 轮次33�?.25 = 竖直测量（俯视图�?z 间距呈竖直；用户�? 上下箭头相对）→ 显式 VERT */
            done = conv_create_with_sels(c, olddim, v, sel0, sel1, PRO_DIM_ORNT_VERT, 0, p0);
            ProSelectionFree(&sel0);
            ProSelectionFree(&sel1);
        }
        if (!done)
            logf_("[conv] *** zgap route: all tries failed; old dim NOT re-shown (R1) ***");
    }
}

static ProError convdim_cb(ProDimension *dim, ProError status, ProAppData app)
{
    conv_ctx_t *c = (conv_ctx_t *)app;
    ProMdl      part = c->part;
    double      v = 0.0;
    ProBoolean  shown = PRO_B_FALSE;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    /* 轮次26：不再要�?已显�?——showk 对已转出的尺寸已不显示，
       本命令负责一次性创建（同值多实例时只建一根）�?*/
    if (ProAnnotationIsShown((ProAnnotation *)dim, (ProDrawing)c->drw, &shown) != PRO_TK_NO_ERROR)
        shown = PRO_B_FALSE;
    logf_("[conv] %.4f id=%d shown=%d", v, (int)dim->id, (int)shown);
    /* 轮次27：同值多实例时（0.25 有三胞胎），只处理图纸上真正显示的那一根—�?
       否则会拿隐藏实例�?3D 位置去反查几何，锚错特征（r26 �?8 连败嫌疑点） */
    if (c->only_shown && !shown) {
        logf_("[conv]   not the displayed instance; skip (shown-preference)");
        return PRO_TK_NO_ERROR;
    }

    if (c->converted > 0) {
        /* 同值的其它已显示实例（罕见重复）：只隐藏，不再�?*/
        ProError eh = ProAnnotationUndisplay((ProAnnotation *)dim, NULL, (ProDrawing)c->drw);
        logf_("[conv]   extra instance hidden -> %d (%s)", (int)eh, errname(eh));
        if (eh == PRO_TK_NO_ERROR) c->hidden++;
        return PRO_TK_NO_ERROR;
    }
    /* 轮次32：同值多实例�?.25 三胞胎）——一轮尝试后其余实例直接跳过�?
       防止"每个实例各试 2 �?把预算翻倍（r32 实测踩过�? 实例 = 6 次试建） */
    if (c->attempted) {
        logf_("[conv]   same value already attempted; skip (budget cap 2)");
        return PRO_TK_NO_ERROR;
    }
    c->attempted = 1;
    {
        /* 旧文本位置（图纸坐标；有显式目标则优先用�? 旧尺�?3D 位置（就近判据） */
        ProPoint3d p0 = { -9999.0, -9999.0, -9999.0 };
        ProPoint3d dl = { -9999.0, -9999.0, -9999.0 };
        Pro3dPnt   box[2];

        box[0][0] = box[0][1] = box[0][2] = -9999.0;
        box[1][0] = box[1][1] = box[1][2] = -9999.0;
        ProSolidOutlineGet((ProSolid)part, box);
        if (c->has_pos) {
            p0[0] = c->tx;
            p0[1] = c->ty;
            logf_("[conv]   target pos (explicit) = (%.1f,%.1f)", p0[0], p0[1]);
        } else {
            ProDimlocation loc0 = NULL;
            ProBoolean     he0 = PRO_B_FALSE;
            double         el0 = 0.0;
            if (ProDimensionLocationGet(dim, NULL, (ProDrawing)c->drw, &loc0) == PRO_TK_NO_ERROR && loc0)
                ProDimlocationTextGet(loc0, &he0, p0, &el0);
            logf_("[conv]   old text pos = (%.1f,%.1f)", p0[0], p0[1]);
        }
        {
            ProDimlocation ml = NULL;
            ProBoolean     he = PRO_B_FALSE;
            double         el = 0.0;
            if (ProDimensionLocationGet(dim, NULL, NULL, &ml) == PRO_TK_NO_ERROR && ml)
                ProDimlocationTextGet(ml, &he, dl, &el);
            logf_("[conv]   dim 3D loc = (%.1f,%.1f,%.1f)", dl[0], dl[1], dl[2]);
        }
        if (c->route == 1) {           /* 面附件路线（41.3 / 4.8�?*/
            conv_face_route(c, dim, v, p0, dl);
            return PRO_TK_NO_ERROR;
        }
        if (c->route == 5) {           /* 轮次33：Z 向间距路线（0.25 法兰台阶�?*/
            conv_zgap_route(c, dim, v, p0, dl);
            return PRO_TK_NO_ERROR;
        }
        {
            /* —�?按值反查几何：�?LINE �?CIRCLE/ARC 边（不依�?AttachmentsGet——实�?-27）—�?*/
            struct { int id; double mx, my, mz, ux, uy; double ax, ay, az, bx, by, bz, len; } LL[400];
            struct { int id; double cx, cy, cz; } CC[400];
            struct cand_t { int kind; int ia; int ib; double dist, err, dz, prox, score; };
            struct cand_t CD[600];
            int nL = 0, nC = 0, nD = 0, hid, a, b;
            int nDropC = 0, nDropLog = 0;   /* route C�?.25）：同平面过滤的丢弃计数 */

            for (hid = 1; hid <= 3000; hid++) {
                ProEdge          e = NULL;
                ProGeomitem      gi;
                ProGeomitemdata *gd = NULL;
                int              t = -1;

                if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
                if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
                if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
                    if (gd) ProGeomitemdataFree(&gd);
                    continue;
                }
                t = gd->data.p_curve_data->line.type;   /* 所有联合体成员首字段都�?type */
                if (t == PRO_ENT_LINE && nL < 400) {
                    ProLinedata *ld = &gd->data.p_curve_data->line;
                    double       dx = ld->end2[0] - ld->end1[0];
                    double       dy = ld->end2[1] - ld->end1[1];
                    double       len = sqrt(dx * dx + dy * dy);

                    if (len > 0.5 &&
                        ld->end1[0] > box[0][0] - 0.5 && ld->end1[0] < box[1][0] + 0.5 &&
                        ld->end2[0] > box[0][0] - 0.5 && ld->end2[0] < box[1][0] + 0.5 &&
                        ld->end1[1] > box[0][1] - 0.5 && ld->end1[1] < box[1][1] + 0.5 &&
                        ld->end2[1] > box[0][1] - 0.5 && ld->end2[1] < box[1][1] + 0.5) {
                        LL[nL].id = hid;
                        LL[nL].mx = (ld->end1[0] + ld->end2[0]) / 2;
                        LL[nL].my = (ld->end1[1] + ld->end2[1]) / 2;
                        LL[nL].mz = (ld->end1[2] + ld->end2[2]) / 2;
                        LL[nL].ux = dx / len;
                        LL[nL].uy = dy / len;
                        LL[nL].ax = ld->end1[0]; LL[nL].ay = ld->end1[1]; LL[nL].az = ld->end1[2];
                        LL[nL].bx = ld->end2[0]; LL[nL].by = ld->end2[1]; LL[nL].bz = ld->end2[2];
                        LL[nL].len = len;
                        nL++;
                    }
                } else if ((t == PRO_ENT_CIRCLE || t == PRO_ENT_ARC) && nC < 400) {
                    double cx, cy, cz, r;

                    if (t == PRO_ENT_CIRCLE) {
                        cx = gd->data.p_curve_data->circle.center[0];
                        cy = gd->data.p_curve_data->circle.center[1];
                        cz = gd->data.p_curve_data->circle.center[2];
                        r  = gd->data.p_curve_data->circle.radius;
                    } else {
                        cx = gd->data.p_curve_data->arc.origin[0];
                        cy = gd->data.p_curve_data->arc.origin[1];
                        cz = gd->data.p_curve_data->arc.origin[2];
                        r  = gd->data.p_curve_data->arc.radius;
                    }
                    if (r > 0.3 &&
                        cx > box[0][0] - 0.5 && cx < box[1][0] + 0.5 &&
                        cy > box[0][1] - 0.5 && cy < box[1][1] + 0.5) {
                        CC[nC].id = hid;
                        CC[nC].cx = cx; CC[nC].cy = cy; CC[nC].cz = cz;
                        nC++;
                    }
                }
                ProGeomitemdataFree(&gd);
            }
            logf_("[conv]   edge scan: lines=%d circles=%d", nL, nC);

            /* 候选一：线-线（平行、垂距≈目标�?*/
            for (a = 0; a < nL; a++) {
                for (b = a + 1; b < nL; b++) {
                    double cross = fabs(LL[a].ux * LL[b].uy - LL[a].uy * LL[b].ux);
                    double dx, dy, dist, err, dz, prox;

                    if (cross > 0.02) continue;
                    dx = LL[b].mx - LL[a].mx;
                    dy = LL[b].my - LL[a].my;
                    dist = fabs(dx * (-LL[a].uy) + dy * LL[a].ux);
                    err = fabs(dist - c->target);
                    if (err > 0.06 || nD >= 600) continue;
                    dz = fabs(LL[a].mz - LL[b].mz);
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤：两条线必须同平面且在标注平面�?*/
                        int dropf = 0;
                        if (dz > 0.3) dropf = 1;
                        else if (fabs(LL[a].mz - dl[2]) > 2.0 || fabs(LL[b].mz - dl[2]) > 2.0) dropf = 1;
                        if (dropf) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   drop kind=0 ids=%d,%d dist=%.4f mz=(%.1f,%.1f) dlz=%.1f",
                                      LL[a].id, LL[b].id, dist, LL[a].mz, LL[b].mz, dl[2]);
                            }
                            continue;
                        }
                    }
                    else if (c->route == 3 && c->vp_ok) {   /* route O：视图平面共面判�?*/
                        double da = c->vp_n[0] * LL[a].mx + c->vp_n[1] * LL[a].my + c->vp_n[2] * LL[a].mz;
                        double db = c->vp_n[0] * LL[b].mx + c->vp_n[1] * LL[b].my + c->vp_n[2] * LL[b].mz;
                        if (fabs(da - db) > 0.3) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   vdrop kind=0 ids=%d,%d dist=%.4f dep=(%.2f,%.2f)", LL[a].id, LL[b].id, dist, da, db);
                            }
                            continue;
                        }
                    }
                    prox = 0;
                    if (dl[0] > -9998.0) {
                        double mx = (LL[a].mx + LL[b].mx) / 2 - dl[0];
                        double my = (LL[a].my + LL[b].my) / 2 - dl[1];
                        prox = sqrt(mx * mx + my * my);
                    }
                    CD[nD].kind = 0; CD[nD].ia = a; CD[nD].ib = b;
                    CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = prox;
                    CD[nD].score = err * 1000.0 + dz * 50.0 + prox;
                    nD++;
                }
            }
            /* 候选二：圆-圆（圆心距≈目标�?*/
            for (a = 0; a < nC; a++) {
                for (b = a + 1; b < nC; b++) {
                    double dx = CC[b].cx - CC[a].cx;
                    double dy = CC[b].cy - CC[a].cy;
                    double dist = sqrt(dx * dx + dy * dy);
                    double err = fabs(dist - c->target);
                    double dz, prox;

                    if (err > 0.06 || nD >= 600) continue;
                    dz = fabs(CC[a].cz - CC[b].cz);
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤（�?圆） */
                        int dropf = 0;
                        if (dz > 0.3) dropf = 1;
                        else if (fabs(CC[a].cz - dl[2]) > 2.0 || fabs(CC[b].cz - dl[2]) > 2.0) dropf = 1;
                        if (dropf) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   drop kind=1 ids=%d,%d dist=%.4f cz=(%.1f,%.1f) dlz=%.1f",
                                      CC[a].id, CC[b].id, dist, CC[a].cz, CC[b].cz, dl[2]);
                            }
                            continue;
                        }
                    }
                    else if (c->route == 3 && c->vp_ok) {
                        double da = c->vp_n[0] * CC[a].cx + c->vp_n[1] * CC[a].cy + c->vp_n[2] * CC[a].cz;
                        double db = c->vp_n[0] * CC[b].cx + c->vp_n[1] * CC[b].cy + c->vp_n[2] * CC[b].cz;
                        if (fabs(da - db) > 0.3) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   vdrop kind=1 ids=%d,%d dist=%.4f dep=(%.2f,%.2f)", CC[a].id, CC[b].id, dist, da, db);
                            }
                            continue;
                        }
                    }
                    prox = 0;
                    if (dl[0] > -9998.0) {
                        double mx = (CC[a].cx + CC[b].cx) / 2 - dl[0];
                        double my = (CC[a].cy + CC[b].cy) / 2 - dl[1];
                        prox = sqrt(mx * mx + my * my);
                    }
                    CD[nD].kind = 1; CD[nD].ia = a; CD[nD].ib = b;
                    CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = prox;
                    CD[nD].score = err * 1000.0 + dz * 50.0 + prox;
                    nD++;
                }
            }
            /* 候选三：线-圆（圆心到直线垂距≈目标�?.8 这类高度尺寸�?*/
            for (a = 0; a < nL; a++) {
                for (b = 0; b < nC; b++) {
                    double dxc = CC[b].cx - LL[a].mx;
                    double dyc = CC[b].cy - LL[a].my;
                    double dist = fabs(dxc * (-LL[a].uy) + dyc * LL[a].ux);
                    double err = fabs(dist - c->target);
                    double dz, prox;

                    if (err > 0.06 || nD >= 600) continue;
                    dz = fabs(CC[b].cz - LL[a].mz);
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤（�?圆） */
                        int dropf = 0;
                        if (dz > 0.3) dropf = 1;
                        else if (fabs(LL[a].mz - dl[2]) > 2.0 || fabs(CC[b].cz - dl[2]) > 2.0) dropf = 1;
                        if (dropf) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   drop kind=2 ids=%d,%d dist=%.4f mz=%.1f cz=%.1f dlz=%.1f",
                                      LL[a].id, CC[b].id, dist, LL[a].mz, CC[b].cz, dl[2]);
                            }
                            continue;
                        }
                    }
                    else if (c->route == 3 && c->vp_ok) {
                        double da = c->vp_n[0] * LL[a].mx + c->vp_n[1] * LL[a].my + c->vp_n[2] * LL[a].mz;
                        double db = c->vp_n[0] * CC[b].cx + c->vp_n[1] * CC[b].cy + c->vp_n[2] * CC[b].cz;
                        if (fabs(da - db) > 0.3) {
                            nDropC++;
                            if (nDropLog < 10) {
                                nDropLog++;
                                logf_("[conv]   vdrop kind=2 ids=%d,%d dist=%.4f dep=(%.2f,%.2f)", LL[a].id, CC[b].id, dist, da, db);
                            }
                            continue;
                        }
                    }
                    prox = 0;
                    if (dl[0] > -9998.0) {
                        double mx = CC[b].cx - dl[0];
                        double my = CC[b].cy - dl[1];
                        prox = sqrt(mx * mx + my * my);
                    }
                    CD[nD].kind = 2; CD[nD].ia = a; CD[nD].ib = b;
                    CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = prox;
                    CD[nD].score = err * 1000.0 + dz * 50.0 + prox;
                    nD++;
                }
            }
            if (c->route == 2)
                logf_("[conv]   coplanar gate: dlz=%.1f dropped=%d", dl[2], nDropC);
            else if (c->route == 3)
                logf_("[conv]   view-plane gate: n=(%.3f,%.3f,%.3f) o=(%.2f,%.2f,%.2f) vp_ok=%d dropped=%d",
                      c->vp_n[0], c->vp_n[1], c->vp_n[2], c->vp_o[0], c->vp_o[1], c->vp_o[2], c->vp_ok, nDropC);
            logf_("[conv]   candidates nD=%d", nD);
            if (nD <= 0) {
                logf_("[conv]   no geometric pair matches %.4f; skip", c->target);
                return PRO_TK_NO_ERROR;
            }
            /* �?score 升序（插入排序） */
            for (a = 1; a < nD; a++) {
                struct cand_t tmp = CD[a];
                b = a - 1;
                while (b >= 0 && CD[b].score > tmp.score) { CD[b + 1] = CD[b]; b--; }
                CD[b + 1] = tmp;
            }
            for (a = 0; a < nD && a < 6; a++)
                logf_("[conv]   top[%d] kind=%d ids=%d,%d dist=%.4f err=%.4f dz=%.1f prox=%.1f sc=%.1f",
                      a, CD[a].kind,
                      CD[a].kind == 1 ? CC[CD[a].ia].id : LL[CD[a].ia].id,
                      CD[a].kind == 0 ? LL[CD[a].ib].id : CC[CD[a].ib].id,
                      CD[a].dist, CD[a].err, CD[a].dz, CD[a].prox, CD[a].score);
            {
                /* 轮次45 改造A：候选合法性判定（两道检查）——辅助构造线在此被明确排除并留证 */
                ab_bbox_t abb;

                ab_bbox_get(c->part, &abb);
                for (a = 0; a < nD && a < 6; a++) {
                    char why[96];

                    if (CD[a].kind == 0 || CD[a].kind == 2) {
                        double p1[3], p2[3];
                        int    ia2 = CD[a].ia, ib2 = CD[a].ib;

                        p1[0] = LL[ia2].ax; p1[1] = LL[ia2].ay; p1[2] = LL[ia2].az;
                        p2[0] = LL[ia2].bx; p2[1] = LL[ia2].by; p2[2] = LL[ia2].bz;
                        {
                            int lg = ab_legal_edge(&abb, p1, p2, LL[ia2].len, why, sizeof(why));

                            logf_("[legal] top[%d] edge %d -> %s (%s)", a, LL[ia2].id, lg ? "OK" : "REJECT", why);
                        }
                        if (CD[a].kind == 0) {
                            p1[0] = LL[ib2].ax; p1[1] = LL[ib2].ay; p1[2] = LL[ib2].az;
                            p2[0] = LL[ib2].bx; p2[1] = LL[ib2].by; p2[2] = LL[ib2].bz;
                            {
                                int lg = ab_legal_edge(&abb, p1, p2, LL[ib2].len, why, sizeof(why));

                                logf_("[legal] top[%d] edge %d -> %s (%s)", a, LL[ib2].id, lg ? "OK" : "REJECT", why);
                            }
                        }
                    }
                    if (CD[a].kind == 1 || CD[a].kind == 2) {
                        int ib2 = CD[a].ib;

                        {
                            int lg = ab_legal_center(&abb, CC[ib2].cx, CC[ib2].cy, CC[ib2].cz, why, sizeof(why));

                            logf_("[legal] top[%d] circle %d -> %s (%s)", a, CC[ib2].id, lg ? "OK" : "REJECT", why);
                        }
                        if (CD[a].kind == 1) {
                            int ia2 = CD[a].ia;

                            {
                                int lg = ab_legal_center(&abb, CC[ia2].cx, CC[ia2].cy, CC[ia2].cz, why, sizeof(why));

                                logf_("[legal] top[%d] circle %d -> %s (%s)", a, CC[ia2].id, lg ? "OK" : "REJECT", why);
                            }
                        }
                    }
                }
            }
            if (c->route == 3) {   /* 轮次28：候选明细（端点/长度/该对所在平�?法向+偏移），供共面判定留�?*/
                int k2;

                for (k2 = 0; k2 < nD && k2 < 24; k2++) {
                    if (CD[k2].kind == 0) {
                        int ia2 = CD[k2].ia, ib2 = CD[k2].ib;
                        double ux3 = LL[ia2].bx - LL[ia2].ax, uy3 = LL[ia2].by - LL[ia2].ay, uz3 = LL[ia2].bz - LL[ia2].az;
                        double l3 = sqrt(ux3 * ux3 + uy3 * uy3 + uz3 * uz3);
                        double ddx = LL[ib2].mx - LL[ia2].mx, ddy = LL[ib2].my - LL[ia2].my, ddz = LL[ib2].mz - LL[ia2].mz;
                        double pnx = 0, pny = 0, pnz = 0, pnn = 0, poff = 0;

                        if (l3 > 1e-9) {
                            ux3 /= l3; uy3 /= l3; uz3 /= l3;
                            pnx = uy3 * ddz - uz3 * ddy;
                            pny = uz3 * ddx - ux3 * ddz;
                            pnz = ux3 * ddy - uy3 * ddx;
                            pnn = sqrt(pnx * pnx + pny * pny + pnz * pnz);
                            if (pnn > 1e-9) {
                                pnx /= pnn; pny /= pnn; pnz /= pnn;
                                poff = pnx * LL[ia2].mx + pny * LL[ia2].my + pnz * LL[ia2].mz;
                            }
                        }
                        logf_("[conv]   det[%d] k0 ids=%d,%d L=(%.2f,%.2f)", k2, LL[ia2].id, LL[ib2].id, LL[ia2].len, LL[ib2].len);
                        logf_("[conv]     e1=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)", LL[ia2].ax, LL[ia2].ay, LL[ia2].az, LL[ia2].bx, LL[ia2].by, LL[ia2].bz);
                        logf_("[conv]     e2=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)", LL[ib2].ax, LL[ib2].ay, LL[ib2].az, LL[ib2].bx, LL[ib2].by, LL[ib2].bz);
                        logf_("[conv]     pair-plane n=(%.3f,%.3f,%.3f) off=%.2f", pnx, pny, pnz, poff);
                    } else if (CD[k2].kind == 2) {
                        int ia2 = CD[k2].ia, ib2 = CD[k2].ib;

                        logf_("[conv]   det[%d] k2 ids=%d,%d L=%.2f C=(%.2f,%.2f,%.2f)",
                              k2, LL[ia2].id, CC[ib2].id, LL[ia2].len, CC[ib2].cx, CC[ib2].cy, CC[ib2].cz);
                        logf_("[conv]     e1=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)", LL[ia2].ax, LL[ia2].ay, LL[ia2].az, LL[ia2].bx, LL[ia2].by, LL[ia2].bz);
                    } else {
                        logf_("[conv]   det[%d] k1 ids=%d,%d C1=(%.2f,%.2f,%.2f) C2=(%.2f,%.2f,%.2f)",
                              k2, CC[CD[k2].ia].id, CC[CD[k2].ib].id,
                              CC[CD[k2].ia].cx, CC[CD[k2].ia].cy, CC[CD[k2].ia].cz,
                              CC[CD[k2].ib].cx, CC[CD[k2].ib].cy, CC[CD[k2].ib].cz);
                    }
                    if (c->vp_ok) {
                        double de_a, de_b;

                        if (CD[k2].kind == 1) {
                            de_a = c->vp_n[0] * CC[CD[k2].ia].cx + c->vp_n[1] * CC[CD[k2].ia].cy + c->vp_n[2] * CC[CD[k2].ia].cz;
                            de_b = c->vp_n[0] * CC[CD[k2].ib].cx + c->vp_n[1] * CC[CD[k2].ib].cy + c->vp_n[2] * CC[CD[k2].ib].cz;
                        } else {
                            de_a = c->vp_n[0] * LL[CD[k2].ia].mx + c->vp_n[1] * LL[CD[k2].ia].my + c->vp_n[2] * LL[CD[k2].ia].mz;
                            de_b = c->vp_n[0] * CC[CD[k2].ib].cx + c->vp_n[1] * CC[CD[k2].ib].cy + c->vp_n[2] * CC[CD[k2].ib].cz;
                            if (CD[k2].kind == 0)
                                de_b = c->vp_n[0] * LL[CD[k2].ib].mx + c->vp_n[1] * LL[CD[k2].ib].my + c->vp_n[2] * LL[CD[k2].ib].mz;
                        }
                        logf_("[conv]     depth e1=%.2f e2=%.2f (view plane)", de_a, de_b);
                    }
                }
            }
            {
                int di, done = 0;
                int cap = (c->route == 0) ? 8 : 2;   /* 轮次27：面/过滤路线试建上限 2（预算纪律） */

                for (di = 0; di < nD && di < cap && !done; di++) {
                    int              kind = CD[di].kind;
                    int              idA = (kind == 1) ? CC[CD[di].ia].id : LL[CD[di].ia].id;
                    int              idB = (kind == 0) ? LL[CD[di].ib].id : CC[CD[di].ib].id;
                    ProEdge          le2 = NULL, re2 = NULL;
                    ProGeomitem      g1, g2;
                    ProSelection    *att2 = NULL;
                    ProDimAttachment *arr2 = NULL;
                    ProDimSense      *sen2 = NULL;
                    ProDimension      nd;
                    ProVector         loc;
                    ProDimOrient      orienth;
                    ProError          ec;
                    double            sx, sy;

                    logf_("[conv]   try[%d]: kind=%d ids=%d,%d dist=%.4f dz=%.1f", di, kind, idA, idB, CD[di].dist, CD[di].dz);
                    if (c->route == 4 && kind != 0) {
                        logf_("[conv]   route V: skip non-line candidate");
                        continue;
                    }
                    if (kind == 0)      { sx = LL[CD[di].ib].mx - LL[CD[di].ia].mx; sy = LL[CD[di].ib].my - LL[CD[di].ia].my; }
                    else if (kind == 1) { sx = CC[CD[di].ib].cx - CC[CD[di].ia].cx; sy = CC[CD[di].ib].cy - CC[CD[di].ia].cy; }
                    else                { sx = CC[CD[di].ib].cx - LL[CD[di].ia].mx; sy = CC[CD[di].ib].cy - LL[CD[di].ia].my; }
                    orienth = (fabs(sx) >= fabs(sy)) ? PRO_DIM_ORNT_HORIZ : PRO_DIM_ORNT_VERT;
                    if (c->route == 3) orienth = PRO_DIM_ORNT_HORIZ;   /* 轮次28：显式给值（水平尺寸），不做自动判定 */
                    if (c->route == 7) orienth = PRO_DIM_ORNT_VERT;    /* 轮次38 实验：强�?VERT */
                    logf_("[conv]   orient_hint explicit = %d (route %d)", (int)orienth, c->route);
                    ProEdgeInit((ProSolid)part, idA, &le2);
                    ProEdgeInit((ProSolid)part, idB, &re2);
                    if (!le2 || !re2) continue;
                    if (ProEdgeToGeomitem((ProSolid)part, le2, &g1) != PRO_TK_NO_ERROR) continue;
                    if (ProEdgeToGeomitem((ProSolid)part, re2, &g2) != PRO_TK_NO_ERROR) continue;
                    ProArrayAlloc(2, sizeof(ProSelection), 1, (ProArray *)&att2);
                    ProSelectionAlloc(NULL, (ProModelitem *)&g1, &att2[0]);
                    ProSelectionViewSet(c->view, &att2[0]);
                    ProSelectionAlloc(NULL, (ProModelitem *)&g2, &att2[1]);
                    ProSelectionViewSet(c->view, &att2[1]);
                    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr2);
                    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen2);
                    ProSelectionCopy(att2[0], &arr2[0][0]); arr2[0][1] = NULL;
                    ProSelectionCopy(att2[1], &arr2[1][0]); arr2[1][1] = NULL;
                    memset(&sen2[0], 0, sizeof(ProDimSense));
                    memset(&sen2[1], 0, sizeof(ProDimSense));
                    if (kind == 0) {
                        sen2[0].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[1].orient_hint = orienth;
                    } else if (kind == 1) {
                        sen2[0].type = PRO_DIM_SNS_TYP_PNT;
                        sen2[0].sense = PRO_POINT_TYP_CENTER;
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_PNT;
                        sen2[1].sense = PRO_POINT_TYP_CENTER;
                        sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
                    } else {
                        sen2[0].type = PRO_DIM_SNS_TYP_NONE;         /* �?*/
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_PNT;          /* �?�?圆心 */
                        sen2[1].sense = PRO_POINT_TYP_CENTER;
                        sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
                    }
                    if (c->route == 8) {
                        /* 轮次39 实验 N：不使用 PNT/CENTER——把两个�?当边"（NONE sense），
                           等价于人�?UI �?Ctrl 选两条可见的圆边去标 */
                        sen2[0].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[1].orient_hint = orienth;
                        logf_("[conv]   route N senses: NONE/NONE (circles as edges)");
                    }
                    if (c->route == 4 && kind == 0) {
                        /* 轮次32 route V：附件改�?�?+ 端点"（PNT sense）——量最近的角点�?
                           �?.25 缺口的角点到角点；与 NONE/面两类附件都不同的一种附件类�?*/
                        int    ia2 = CD[di].ia, ib2 = CD[di].ib;
                        double d00, d01, d10, d11, bestd;
                        int    aepick, bepick;

                        d00 = (LL[ia2].ax - LL[ib2].ax) * (LL[ia2].ax - LL[ib2].ax) + (LL[ia2].ay - LL[ib2].ay) * (LL[ia2].ay - LL[ib2].ay);
                        d01 = (LL[ia2].ax - LL[ib2].bx) * (LL[ia2].ax - LL[ib2].bx) + (LL[ia2].ay - LL[ib2].by) * (LL[ia2].ay - LL[ib2].by);
                        d10 = (LL[ia2].bx - LL[ib2].ax) * (LL[ia2].bx - LL[ib2].ax) + (LL[ia2].by - LL[ib2].ay) * (LL[ia2].by - LL[ib2].ay);
                        d11 = (LL[ia2].bx - LL[ib2].bx) * (LL[ia2].bx - LL[ib2].bx) + (LL[ia2].by - LL[ib2].by) * (LL[ia2].by - LL[ib2].by);
                        bestd = d00; aepick = 0; bepick = 0;
                        if (d01 < bestd) { bestd = d01; aepick = 0; bepick = 1; }
                        if (d10 < bestd) { bestd = d10; aepick = 1; bepick = 0; }
                        if (d11 < bestd) { bestd = d11; aepick = 1; bepick = 1; }
                        sen2[0].type  = PRO_DIM_SNS_TYP_PNT;
                        sen2[0].sense = aepick ? PRO_POINT_TYP_END2 : PRO_POINT_TYP_END1;
                        sen2[0].orient_hint = orienth;
                        sen2[1].type  = PRO_DIM_SNS_TYP_PNT;
                        sen2[1].sense = bepick ? PRO_POINT_TYP_END2 : PRO_POINT_TYP_END1;
                        sen2[1].orient_hint = orienth;
                        logf_("[conv]   route V senses: e%d end%d / e%d end%d (minpt dist=%.3f)",
                              idA, aepick + 1, idB, bepick + 1, sqrt(bestd));
                    }
                    loc[0] = p0[0];
                    loc[1] = p0[1];
                    loc[2] = 0.0;
                    memset(&nd, 0, sizeof(nd));
                    {
                        /* 轮次28：route 3 显式�?HORIZ（对照实验）；轮�?8：route 7 显式�?VERT；其余路线保�?NONE */
                        ProDimOrient op = (c->route == 3) ? PRO_DIM_ORNT_HORIZ :
                                          ((c->route == 7) ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_NONE);

                        logf_("[conv]   create orient param = %d", (int)op);
                        ec = ProDrawingDimensionCreate((ProDrawing)c->drw, arr2, sen2, op,
                                                       loc, PRO_B_FALSE, &nd);
                    }
                    logf_("[conv]   create -> %d (%s)", (int)ec, errname(ec));
                    if (ec == PRO_TK_NO_ERROR) {
                        double nv = -1.0;

                        if (ProDimensionValueGet(&nd, &nv) == PRO_TK_NO_ERROR)
                            logf_("[conv]   new value = %.4f (old %.4f) %s", nv, v,
                                  (nv > v - 0.02 && nv < v + 0.02) ? "MATCH" : "*** MISMATCH ***");
                        conv_apply_tol(&nd, v);
                        {
                            ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, c->view);
                            logf_("[conv]   show new -> %d (%s)", (int)es, errname(es));
                        }
                        {
                            ProError eh = ProAnnotationUndisplay((ProAnnotation *)dim, NULL, (ProDrawing)c->drw);
                            logf_("[conv]   undisplay old -> %d (%s)", (int)eh, errname(eh));
                            if (eh == PRO_TK_NO_ERROR) c->hidden++;
                        }
                        if (p0[0] > -9998.0 && !c->no_move) {
                            ProError em = ProDrawingDimensionMove((ProDrawing)c->drw, &nd, p0);
                            logf_("[conv]   move new -> old pos : %d (%s)", (int)em, errname(em));
                        } else if (c->no_move) {
                            logf_("[conv]   no_move mode (round35-B): Move skipped; create-loc = final pos");
                        }
                        c->converted++;
                        done = 1;
                        /* 轮次49：登记最后创建的尺寸句柄（供 ab_dim_fixup 拖动/复检�?*/
                        g_last_nd = nd;
                        g_last_nd_valid = 1;
                        g_last_nd_value = v;
                    }
                    ProArrayFree((ProArray *)&arr2);
                    ProArrayFree((ProArray *)&sen2);
                    ProSelectionFree(&att2[0]);
                    ProSelectionFree(&att2[1]);
                    ProArrayFree((ProArray *)&att2);
                }
                if (!done) {
                    /* 轮次32 R1：全部候选失败不再回显旧尺寸（禁�?显示"出图——缺比显好） */
                    logf_("[conv] *** all %d attempts failed; old dim NOT re-shown (R1: display forbidden) ***",
                          nD < cap ? nD : cap);
                }
            }
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 轮次27：预扫描——该值有多少"已显�?的实例（决定是否启用 shown 优先�?*/
typedef struct { double target; ProDrawing drw; int n_shown; } convscan_t;

static ProError conv_scan_cb(ProDimension *dim, ProError status, ProAppData app)
{
    convscan_t *c = (convscan_t *)app;
    double      v = 0.0;
    ProBoolean  sh = PRO_B_FALSE;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    if (ProAnnotationIsShown((ProAnnotation *)dim, (ProDrawing)c->drw, &sh) == PRO_TK_NO_ERROR && sh)
        c->n_shown++;
    return PRO_TK_NO_ERROR;
}

/* 遍历模型尺寸：打�?id/类型/值（用于挑选“像人一样”的关键尺寸白名单） */
static void convdim_run(ProMdl part, ProMdl drw, double value, int has_pos, double tx, double ty,
                        int view_kind, int route)
{
    conv_ctx_t c;
    ProView   *views = NULL;

    memset(&c, 0, sizeof(c));
    c.target = value;
    c.drw = (ProDrawing)drw;
    c.part = part;
    c.has_pos = has_pos;
    c.tx = tx;
    c.ty = ty;
    c.view_kind = view_kind;
    c.route = route;
    c.no_move = (route == 6 || route == 7 || route == 8) ? 1 : 0;   /* 轮次35/38/39 实验：创建即终位、不�?Move */
    if (route == 3) {
        /* 轮次28：视图注释平面（从模�?FRONT 视图推导）—�?.25 �?dl 读不出（-9999），改用它做共面判定 */
        ProView mv = NULL;

        c.vp_ok = 0;
        if (ProViewNameToView(part, (wchar_t *)L"front", &mv) != PRO_TK_NO_ERROR || !mv)
            ProViewNameToView(part, (wchar_t *)L"FRONT", &mv);
        logf_("[conv] vplane: model view -> %s", mv ? "found" : "MISSING");
        if (mv) {
            ProAnnotationPlane apl;

            memset(&apl, 0, sizeof(apl));
            {
                ProError ep = ProAnnotationplaneFromViewCreate((ProSolid)part, mv, &apl);

                logf_("[conv] vplane FromViewCreate -> %d (%s)", (int)ep, errname(ep));
                if (ep == PRO_TK_NO_ERROR) {
                    ProVector    nv;
                    ProPlanedata pd;
                    ProError     ev, eg;

                    memset(&pd, 0, sizeof(pd));
                    ev = ProAnnotationplaneVectorGet(&apl, nv);
                    eg = ProAnnotationplanePlaneGet(&apl, &pd);
                    logf_("[conv] vplane vecGet -> %d (%s) planeGet -> %d (%s)",
                          (int)ev, errname(ev), (int)eg, errname(eg));
                    {
                        double nx = 0, ny = 0, nz = 0;

                        if (ev == PRO_TK_NO_ERROR) { nx = nv[0]; ny = nv[1]; nz = nv[2]; }
                        else if (eg == PRO_TK_NO_ERROR) { nx = pd.e3[0]; ny = pd.e3[1]; nz = pd.e3[2]; }
                        if (nx * nx + ny * ny + nz * nz > 1e-12) {
                            double nn = sqrt(nx * nx + ny * ny + nz * nz);

                            c.vp_n[0] = nx / nn; c.vp_n[1] = ny / nn; c.vp_n[2] = nz / nn;
                            c.vp_o[0] = pd.origin[0]; c.vp_o[1] = pd.origin[1]; c.vp_o[2] = pd.origin[2];
                            c.vp_ok = 1;
                        }
                    }
                }
            }
        }
        logf_("[conv] vplane: vp_ok=%d n=(%.3f,%.3f,%.3f) o=(%.2f,%.2f,%.2f)",
              c.vp_ok, c.vp_n[0], c.vp_n[1], c.vp_n[2], c.vp_o[0], c.vp_o[1], c.vp_o[2]);
    }
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
        int vc = 0, vi;
        ProArraySizeGet((ProArray)views, &vc);
        for (vi = 0; vi < vc; vi++) {
            ProName nm;
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
            if (view_kind == 1) {
                /* 左侧视图（名�?左侧�?.8 量的是零件厚度方向，标在侧视图上�?*/
                if (wcsstr(nm, L"\u5DE6\u4FA7")) { c.view = views[vi]; break; }
            } else if (view_kind == 2) {
                /* 俯视图（名含 顶部�?.25 �?z 间距在这里呈竖直�?*/
                if (wcsstr(nm, L"\u9876\u90E8")) { c.view = views[vi]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { c.view = views[vi]; break; }
            }
        }
        ProArrayFree((ProArray *)&views);
    }
    if (!c.view) { logf_("[conv] no target view (kind=%d)", view_kind); return; }
    logf_("[conv] >> value=%.4f view=%d route=%d", value, (int)c.view, route);
    {
        convscan_t sc;

        sc.target = value;
        sc.drw = (ProDrawing)drw;
        sc.n_shown = 0;
        ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, conv_scan_cb, NULL, &sc);
        c.only_shown = (sc.n_shown > 0) ? 1 : 0;
        logf_("[conv] shown-instance scan: n_shown=%d only_shown=%d", sc.n_shown, c.only_shown);
    }
    ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, convdim_cb, NULL, &c);
    logf_("[conv] done: converted=%d hidden=%d", c.converted, c.hidden);
}

/* ===== 轮次27：FACHK|<�? —�?只读面诊�?=====
   枚举全部平面面，列出平行面片对里间距≈目标（±1.0，比试建窗口宽）的组合，
   用于开建之前确�?41.3/4.8 到底对应哪两个面"，避免盲试�?*/
static void facechk_run(ProMdl part, double target)
{
    static conv_fplane_t FP[400];
    conv_chkpair_t PR[300];
    int nF, nP = 0, a, b, k;
    ProPoint3d box[2];

    memset(box, 0, sizeof(box));
    ProSolidOutlineGet((ProSolid)part, box);
    logf_("[fachk] bbox (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
          box[0][0], box[0][1], box[0][2], box[1][0], box[1][1], box[1][2]);
    nF = conv_faces_collect(part, FP, 400);
    logf_("[fachk] planar faces=%d target=%.4f", nF, target);
    for (k = 0; k < nF && k < 10; k++)
        logf_("[fachk] f%d id=%d o=(%.1f,%.1f,%.1f) n=(%.3f,%.3f,%.3f)",
              k, FP[k].id, FP[k].ox, FP[k].oy, FP[k].oz, FP[k].nx, FP[k].ny, FP[k].nz);
    for (a = 0; a < nF; a++) {
        for (b = a + 1; b < nF; b++) {
            double dot = FP[a].nx * FP[b].nx + FP[a].ny * FP[b].ny + FP[a].nz * FP[b].nz;
            double dx, dy, dz2, dist, err;

            if (fabs(dot) < 0.999) continue;
            dx  = FP[b].ox - FP[a].ox;
            dy  = FP[b].oy - FP[a].oy;
            dz2 = FP[b].oz - FP[a].oz;
            dist = fabs(dx * FP[a].nx + dy * FP[a].ny + dz2 * FP[a].nz);
            err = fabs(dist - target);
            if (err > 1.0 || nP >= 300) continue;
            PR[nP].ia = a; PR[nP].ib = b;
            PR[nP].dist = dist; PR[nP].err = err;
            PR[nP].mz = (FP[a].oz + FP[b].oz) / 2;
            nP++;
        }
    }
    /* �?err 升序 */
    for (a = 1; a < nP; a++) {
        conv_chkpair_t tmp = PR[a];
        b = a - 1;
        while (b >= 0 && PR[b].err > tmp.err) { PR[b + 1] = PR[b]; b--; }
        PR[b + 1] = tmp;
    }
    logf_("[fachk] pairs within 1.0: %d", nP);
    for (k = 0; k < nP && k < 12; k++)
        logf_("[fachk] pair[%d] err=%.3f dist=%.3f ids=%d,%d n=(%.2f,%.2f,%.2f) midz=%.1f",
              k, PR[k].err, PR[k].dist, FP[PR[k].ia].id, FP[PR[k].ib].id,
              FP[PR[k].ia].nx, FP[PR[k].ia].ny, FP[PR[k].ia].nz, PR[k].mz);
}

/* ===== 轮次30：GEOQ 只读几何转储（调查专用；不创�?不修改任何东西） =====
   GEOQ|P|<�?        按值反查几何（只读版）：线-�?�?�?�?�?候选对，�?.06，日志前 10
   GEOQ|L|<�?|<�?   中点坐标 �?值的直线边清单（0=x,1=y,2=z,容差 0.8�?
   GEOQ|F|<�?|<�?   轴向平面面清单（|n_轴|>0.99 且平面常�?�?值） */
typedef struct { int kind, ia, ib; double dist, err, dz, prox, score; } geoq_cand_t;

static void geoq_lines_axis(ProMdl part, int axis, double val)
{
    int hid, n = 0;

    for (hid = 1; hid <= 3000; hid++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        if (gd->data.p_curve_data->line.type == PRO_ENT_LINE) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double mx = (ld->end1[0] + ld->end2[0]) / 2;
            double my = (ld->end1[1] + ld->end2[1]) / 2;
            double mz = (ld->end1[2] + ld->end2[2]) / 2;
            double cc = (axis == 0) ? mx : ((axis == 1) ? my : mz);

            if (fabs(cc - val) <= 0.8) {
                double dx = ld->end2[0] - ld->end1[0];
                double dy = ld->end2[1] - ld->end1[1];
                double dz = ld->end2[2] - ld->end1[2];
                double L = sqrt(dx * dx + dy * dy + dz * dz);

                logf_("[geoq] L id=%d e1=(%.2f,%.2f,%.2f) e2=(%.2f,%.2f,%.2f) len=%.3f",
                      hid, ld->end1[0], ld->end1[1], ld->end1[2],
                      ld->end2[0], ld->end2[1], ld->end2[2], L);
                n++;
                if (n >= 80) { logf_("[geoq] L ...cap 80"); break; }
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[geoq] lines near axis%d=%.3f: %d", axis, val, n);
}

static void geoq_faces_axis(ProMdl part, int axis, double val)
{
    static conv_fplane_t FP[400];
    int nF = conv_faces_collect(part, FP, 400), k, n = 0;

    for (k = 0; k < nF; k++) {
        double na  = (axis == 0) ? FP[k].nx : ((axis == 1) ? FP[k].ny : FP[k].nz);
        double off = FP[k].ox * FP[k].nx + FP[k].oy * FP[k].ny + FP[k].oz * FP[k].nz;

        if (fabs(na) > 0.99 && fabs(off - val) <= 0.8) {
            logf_("[geoq] F id=%d n=(%.2f,%.2f,%.2f) off=%.3f", FP[k].id, FP[k].nx, FP[k].ny, FP[k].nz, off);
            n++;
            if (n >= 80) { logf_("[geoq] F ...cap 80"); break; }
        }
    }
    logf_("[geoq] faces |n_axis%d|>0.99 near off=%.3f: %d", axis, val, n);
}

static void geoq_pairs(ProMdl part, double target)
{
    /* 只读�?按值反�?（不创建任何东西）：�?CONVDIM 同款扫描，只打日�?*/
    struct { int id; double mx, my, mz, ux, uy; double ax, ay, az, bx, by, bz, len; } LL[400];
    struct { int id; double cx, cy, cz; } CC[400];
    geoq_cand_t CD[600];
    int nL = 0, nC = 0, nD = 0, hid, a, b;
    Pro3dPnt box[2];

    box[0][0] = box[0][1] = box[0][2] = -9999.0;
    box[1][0] = box[1][1] = box[1][2] = -9999.0;
    ProSolidOutlineGet((ProSolid)part, box);
    for (hid = 1; hid <= 3000; hid++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;
        int              t;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        t = gd->data.p_curve_data->line.type;
        if (t == PRO_ENT_LINE && nL < 400) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double dx = ld->end2[0] - ld->end1[0];
            double dy = ld->end2[1] - ld->end1[1];
            double len = sqrt(dx * dx + dy * dy);

            if (len > 0.5) {
                LL[nL].id = hid;
                LL[nL].mx = (ld->end1[0] + ld->end2[0]) / 2;
                LL[nL].my = (ld->end1[1] + ld->end2[1]) / 2;
                LL[nL].mz = (ld->end1[2] + ld->end2[2]) / 2;
                LL[nL].ux = dx / len;
                LL[nL].uy = dy / len;
                LL[nL].ax = ld->end1[0]; LL[nL].ay = ld->end1[1]; LL[nL].az = ld->end1[2];
                LL[nL].bx = ld->end2[0]; LL[nL].by = ld->end2[1]; LL[nL].bz = ld->end2[2];
                LL[nL].len = len;
                nL++;
            }
        } else if ((t == PRO_ENT_CIRCLE || t == PRO_ENT_ARC) && nC < 400) {
            double cx, cy, cz, r;

            if (t == PRO_ENT_CIRCLE) {
                cx = gd->data.p_curve_data->circle.center[0];
                cy = gd->data.p_curve_data->circle.center[1];
                cz = gd->data.p_curve_data->circle.center[2];
                r  = gd->data.p_curve_data->circle.radius;
            } else {
                cx = gd->data.p_curve_data->arc.origin[0];
                cy = gd->data.p_curve_data->arc.origin[1];
                cz = gd->data.p_curve_data->arc.origin[2];
                r  = gd->data.p_curve_data->arc.radius;
            }
            if (r > 0.05) {
                CC[nC].id = hid;
                CC[nC].cx = cx; CC[nC].cy = cy; CC[nC].cz = cz;
                nC++;
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[geoq] P scan lines=%d circles=%d target=%.4f", nL, nC, target);
    for (a = 0; a < nL; a++) for (b = a + 1; b < nL; b++) {
        double cross = fabs(LL[a].ux * LL[b].uy - LL[a].uy * LL[b].ux);
        double dx, dy, dist, err, dz, score;

        if (cross > 0.02) continue;
        dx = LL[b].mx - LL[a].mx;
        dy = LL[b].my - LL[a].my;
        dist = fabs(dx * (-LL[a].uy) + dy * LL[a].ux);
        err = fabs(dist - target);
        if (err > 0.06 || nD >= 600) continue;
        dz = fabs(LL[a].mz - LL[b].mz);
        score = err * 1000.0 + dz * 50.0;
        CD[nD].kind = 0; CD[nD].ia = a; CD[nD].ib = b; CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = 0; CD[nD].score = score;
        nD++;
    }
    for (a = 0; a < nC; a++) for (b = a + 1; b < nC; b++) {
        double dx = CC[b].cx - CC[a].cx;
        double dy = CC[b].cy - CC[a].cy;
        double dist = sqrt(dx * dx + dy * dy);
        double err = fabs(dist - target);
        double dz, score;

        if (err > 0.06 || nD >= 600) continue;
        dz = fabs(CC[a].cz - CC[b].cz);
        score = err * 1000.0 + dz * 50.0;
        CD[nD].kind = 1; CD[nD].ia = a; CD[nD].ib = b; CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = 0; CD[nD].score = score;
        nD++;
    }
    for (a = 0; a < nL; a++) for (b = 0; b < nC; b++) {
        double dxc = CC[b].cx - LL[a].mx;
        double dyc = CC[b].cy - LL[a].my;
        double dist = fabs(dxc * (-LL[a].uy) + dyc * LL[a].ux);
        double err = fabs(dist - target);
        double dz, score;

        if (err > 0.06 || nD >= 600) continue;
        dz = fabs(CC[b].cz - LL[a].mz);
        score = err * 1000.0 + dz * 50.0;
        CD[nD].kind = 2; CD[nD].ia = a; CD[nD].ib = b; CD[nD].dist = dist; CD[nD].err = err; CD[nD].dz = dz; CD[nD].prox = 0; CD[nD].score = score;
        nD++;
    }
    logf_("[geoq] P candidates nD=%d", nD);
    for (a = 1; a < nD; a++) {   /* �?score 升序 */
        geoq_cand_t tmp = CD[a];
        b = a - 1;
        while (b >= 0 && CD[b].score > tmp.score) { CD[b + 1] = CD[b]; b--; }
        CD[b + 1] = tmp;
    }
    for (a = 0; a < nD && a < 10; a++) {
        if (CD[a].kind == 0) {
            int ia2 = CD[a].ia, ib2 = CD[a].ib;

            logf_("[geoq]   top[%d] k0 ids=%d,%d dist=%.4f err=%.4f dz=%.1f", a, LL[ia2].id, LL[ib2].id, CD[a].dist, CD[a].err, CD[a].dz);
            logf_("[geoq]     e1=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) L=%.2f", LL[ia2].ax, LL[ia2].ay, LL[ia2].az, LL[ia2].bx, LL[ia2].by, LL[ia2].bz, LL[ia2].len);
            logf_("[geoq]     e2=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) L=%.2f", LL[ib2].ax, LL[ib2].ay, LL[ib2].az, LL[ib2].bx, LL[ib2].by, LL[ib2].bz, LL[ib2].len);
        } else if (CD[a].kind == 1) {
            logf_("[geoq]   top[%d] k1 ids=%d,%d dist=%.4f err=%.4f dz=%.1f C1=(%.2f,%.2f,%.2f) C2=(%.2f,%.2f,%.2f)",
                  a, CC[CD[a].ia].id, CC[CD[a].ib].id, CD[a].dist, CD[a].err, CD[a].dz,
                  CC[CD[a].ia].cx, CC[CD[a].ia].cy, CC[CD[a].ia].cz,
                  CC[CD[a].ib].cx, CC[CD[a].ib].cy, CC[CD[a].ib].cz);
        } else {
            logf_("[geoq]   top[%d] k2 ids=%d,%d dist=%.4f err=%.4f dz=%.1f line=(%.2f,%.2f,%.2f) C=(%.2f,%.2f,%.2f)",
                  a, LL[CD[a].ia].id, CC[CD[a].ib].id, CD[a].dist, CD[a].err, CD[a].dz,
                  LL[CD[a].ia].mx, LL[CD[a].ia].my, LL[CD[a].ia].mz,
                  CC[CD[a].ib].cx, CC[CD[a].ib].cy, CC[CD[a].ib].cz);
        }
    }
    logf_("[geoq] P done (read-only; nothing created)");
}

/* 轮次43：GEOQ|Z|<vk>|<val> —�?只读 z 向间距扫描（route Z 的只读版，绝不创建）�?
   规则�?route Z 一致：平行 + 方向⊥视�?vk) + 同深�?y) + x/y 投影重合 + 3D 垂距≈目�?+ �?z 差�?*/
static void geoq_zpairs(ProMdl part, int vk, double target)
{
    static zgap_line_t LL[500];
    static zgap_pair_t CD[500];
    int nL = 0, nD = 0, hid, a, b, k;

    for (hid = 1; hid <= 3000 && nL < 500; hid++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        if (gd->data.p_curve_data->line.type == PRO_ENT_LINE) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double ddx = ld->end2[0] - ld->end1[0];
            double ddy = ld->end2[1] - ld->end1[1];
            double ddz = ld->end2[2] - ld->end1[2];
            double len = sqrt(ddx * ddx + ddy * ddy + ddz * ddz);

            if (len > 1.0) {
                int dup = 0, kk;

                for (kk = 0; kk < nL; kk++) {
                    if (fabs(LL[kk].ax - ld->end1[0]) < 1e-3 && fabs(LL[kk].ay - ld->end1[1]) < 1e-3 &&
                        fabs(LL[kk].az - ld->end1[2]) < 1e-3 && fabs(LL[kk].bx - ld->end2[0]) < 1e-3 &&
                        fabs(LL[kk].by - ld->end2[1]) < 1e-3 && fabs(LL[kk].bz - ld->end2[2]) < 1e-3) { dup = 1; break; }
                    if (fabs(LL[kk].ax - ld->end2[0]) < 1e-3 && fabs(LL[kk].ay - ld->end2[1]) < 1e-3 &&
                        fabs(LL[kk].az - ld->end2[2]) < 1e-3 && fabs(LL[kk].bx - ld->end1[0]) < 1e-3 &&
                        fabs(LL[kk].by - ld->end1[1]) < 1e-3 && fabs(LL[kk].bz - ld->end1[2]) < 1e-3) { dup = 1; break; }
                }
                if (!dup) {
                    LL[nL].id = hid;
                    LL[nL].ax = ld->end1[0]; LL[nL].ay = ld->end1[1]; LL[nL].az = ld->end1[2];
                    LL[nL].bx = ld->end2[0]; LL[nL].by = ld->end2[1]; LL[nL].bz = ld->end2[2];
                    LL[nL].mx = (LL[nL].ax + LL[nL].bx) / 2;
                    LL[nL].my = (LL[nL].ay + LL[nL].by) / 2;
                    LL[nL].mz = (LL[nL].az + LL[nL].bz) / 2;
                    nL++;
                }
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[geoqz] scan uniq lines=%d target=%.4f vk=%d", nL, target, vk);
    for (a = 0; a < nL; a++) {
        for (b = a + 1; b < nL; b++) {
            double ua[3], ub[3], la, lb, cross, px, py, pz, t, dist3, dxy, dz2, err;

            ua[0] = LL[a].bx - LL[a].ax; ua[1] = LL[a].by - LL[a].ay; ua[2] = LL[a].bz - LL[a].az;
            ub[0] = LL[b].bx - LL[b].ax; ub[1] = LL[b].by - LL[b].ay; ub[2] = LL[b].bz - LL[b].az;
            la = sqrt(ua[0] * ua[0] + ua[1] * ua[1] + ua[2] * ua[2]);
            lb = sqrt(ub[0] * ub[0] + ub[1] * ub[1] + ub[2] * ub[2]);
            if (la < 1e-9 || lb < 1e-9) continue;
            ua[0] /= la; ua[1] /= la; ua[2] /= la;
            ub[0] /= lb; ub[1] /= lb; ub[2] /= lb;
            cross = fabs(ua[0] * ub[1] - ua[1] * ub[0]) + fabs(ua[1] * ub[2] - ua[2] * ub[1]) + fabs(ua[2] * ub[0] - ua[0] * ub[2]);
            if (cross > 0.03) continue;
            if (vk == 2)      { if (fabs(ua[1]) > 0.3) continue; }
            else if (vk == 1) { if (fabs(ua[0]) > 0.3) continue; }
            else              { if (fabs(ua[2]) > 0.3) continue; }
            if (fabs(LL[a].my - LL[b].my) > 0.3) continue;
            dxy = sqrt((LL[a].mx - LL[b].mx) * (LL[a].mx - LL[b].mx) + (LL[a].my - LL[b].my) * (LL[a].my - LL[b].my));
            if (dxy > 0.05) continue;
            px = LL[b].mx - LL[a].mx; py = LL[b].my - LL[a].my; pz = LL[b].mz - LL[a].mz;
            t = px * ua[0] + py * ua[1] + pz * ua[2];
            px -= t * ua[0]; py -= t * ua[1]; pz -= t * ua[2];
            dist3 = sqrt(px * px + py * py + pz * pz);
            err = fabs(dist3 - target);
            if (err > 0.06 || nD >= 500) continue;
            dz2 = fabs(LL[a].mz - LL[b].mz);
            if (dz2 < 0.1) continue;
            CD[nD].ia = a; CD[nD].ib = b;
            CD[nD].dist3 = dist3; CD[nD].err = err; CD[nD].prox = 0;
            CD[nD].score = err * 1000.0;
            nD++;
        }
    }
    logf_("[geoqz] candidates nD=%d", nD);
    for (a = 1; a < nD; a++) {
        zgap_pair_t tmp = CD[a];
        b = a - 1;
        while (b >= 0 && CD[b].score > tmp.score) { CD[b + 1] = CD[b]; b--; }
        CD[b + 1] = tmp;
    }
    for (k = 0; k < nD && k < 8; k++)
        logf_("[geoqz] top[%d] ids=%d,%d dist3=%.4f dz=%.1f e1=(%.2f,%.2f,%.2f) e2=(%.2f,%.2f,%.2f)",
              k, LL[CD[k].ia].id, LL[CD[k].ib].id, CD[k].dist3,
              fabs(LL[CD[k].ia].mz - LL[CD[k].ib].mz),
              LL[CD[k].ia].ax, LL[CD[k].ia].ay, LL[CD[k].ia].az,
              LL[CD[k].ib].ax, LL[CD[k].ib].ay, LL[CD[k].ib].az);
    logf_("[geoqz] done (read-only; nothing created)");
}


static ProError dim_visit_cb(ProDimension *dim, ProError status, ProAppData app)
{
    double  v = -999999.0;
    ProError e = ProDimensionValueGet(dim, &v);
    logf_("[dims] id=%d type=%d value=%.4f (get->%d %s)",
          (int)dim->id, (int)dim->type, v, (int)e, errname(e));
    return PRO_TK_NO_ERROR;
}

static void dims_report(void)
{
    ProMdl part = NULL;
    ProError er = retrieve_model_any(g_def_model, &part);
    logf_("[dims] retrieve -> %d (%s)", (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR || !part) return;
    {
        ProError ev = ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_visit_cb, NULL, NULL);
        logf_("[dims] solidDimensionVisit -> %d (%s)", (int)ev, errname(ev));
    }
}

/* ===== 轮次31：DDIMS 只读转储"存在图纸里的尺寸"（dd/rdd/add �?—�?图纸侧创�?用户手标�?
   不含"模型自带并显示的"。用于查用户新标的尺�?+ 我们转换出来的尺寸。绝不改动任何东西�?===== */
static int ab_dim_display_len(ProDimension *nd, char *buf, size_t cap);

static ProError ddims_cb(ProDimension *dim, ProError status, ProAppData app)
{
    ProDrawing drw = (ProDrawing)app;
    double     v = 0.0;
    ProBoolean sh = PRO_B_FALSE;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    ProAnnotationIsShown((ProAnnotation *)dim, drw, &sh);
    {
        ProDimlocation loc = NULL;
        ProBoolean     he = PRO_B_FALSE;
        ProPoint3d     tp = { -9999.0, -9999.0, -9999.0 };
        double         el = 0.0;
        int            rt = -1;
        double         ru = 0.0, rl = 0.0;
        char           db[64] = "-";

        if (ProDimensionLocationGet(dim, NULL, drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, tp, &el);
        ProDimensionToltypeGet(dim, (ProDimToleranceType *)&rt);
        ProDimensionToleranceGet(dim, &ru, &rl);
        ab_dim_display_len(dim, db, sizeof(db));
        logf_("[ddims] id=%d type=%d value=%.4f shown=%d text=(%.1f,%.1f) tol=%d up=%.4f lo=%.4f disp='%s'",
              (int)dim->id, (int)dim->type, v, (int)sh, tp[0], tp[1], rt, ru, rl, db);
    }
    return PRO_TK_NO_ERROR;
}

static void ddims_run(ProMdl drw)
{
    {
        ProError e1 = ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, ddims_cb, NULL, (ProAppData)drw);
        logf_("[ddims] visit PRO_DIMENSION -> %d (%s)", (int)e1, errname(e1));
    }
    {
        ProError e2 = ProDrawingDimensionVisit((ProDrawing)drw, PRO_REF_DIMENSION, ddims_cb, NULL, (ProAppData)drw);
        logf_("[ddims] visit PRO_REF_DIMENSION -> %d (%s)", (int)e2, errname(e2));
    }
}

/* ===== 轮次69：增量编辑命令（并入�?DLL，彻底禁用辅助应用） =====
   仅通过 cmd_now.txt 调用；不注册任何第二�?DLL、不弹任何对话框�?*/

typedef struct {
    ProDrawing drw;
    ProView    front;
    int        fixed;
} dimfix_ctx_t;

static ProError dimfix_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dimfix_ctx_t *c = (dimfix_ctx_t *)app;
    double v = 0.0;
    ProBoolean sh = PRO_B_FALSE;
    int id;
    (void)status;
    if (!c || !dim) return PRO_TK_NO_ERROR;
    id = (int)dim->id;
    if (id != 9 && id != 10) return PRO_TK_NO_ERROR;
    ProDimensionValueGet(dim, &v);
    ProAnnotationIsShown((ProAnnotation *)dim, c->drw, &sh);
    {
        ProPoint3d tp = { -9999.0, -9999.0, -9999.0 };
        ProDimlocation loc = NULL;
        ProBoolean he = PRO_B_FALSE;
        double el = 0.0;
        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc) {
            ProDimlocationTextGet(loc, &he, tp, &el);
            ProDimlocationFree(loc);
        }
        logf_("[dimfix] id=%d value=%.4f shown=%d text=(%.1f,%.1f) view=%p",
              id, v, (int)sh, tp[0], tp[1], (void *)c->front);
    }
    {
        ProPoint3d pos;
        ProError em, es;
        pos[0] = (id == 9) ? 412.8 : 365.0;
        pos[1] = (id == 9) ? 545.0 : 497.0;
        pos[2] = 0.0;
        em = ProDimensionMove(dim, pos);
        es = ProAnnotationShow((ProAnnotation *)dim, NULL, c->front);
        logf_("[dimfix] id=%d move(%.1f,%.1f) -> %d (%s); show -> %d (%s)",
              id, pos[0], pos[1], (int)em, errname(em), (int)es, errname(es));
        if (es == PRO_TK_NO_ERROR) c->fixed++;
    }
    return PRO_TK_NO_ERROR;
}

static void dimfix_run(ProMdl drw)
{
    dimfix_ctx_t c;
    ProView *views = NULL;
    ProPlotPaperSize ptype;
    double sw = 0.0, sh = 0.0;
    c.drw = (ProDrawing)drw;
    c.front = NULL;
    c.fixed = 0;
    ProDrawingSheetSizeGet((ProDrawing)drw, 1, &ptype, &sw, &sh);
    logf_("[dimfix] sheet size type=%d width=%.1f height=%.1f", (int)ptype, sw, sh);
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
        int vc = 0, i;
        ProArraySizeGet((ProArray)views, &vc);
        for (i = 0; i < vc; i++) {
            ProName nm;
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { c.front = views[i]; break; }
        }
        ProArrayFree((ProArray *)&views);
    }
    logf_("[dimfix] front view=%p", (void *)c.front);
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dimfix_cb, NULL, (ProAppData)&c);
    logf_("[dimfix] fixed=%d", c.fixed);
    logf_("[dimfix] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
}

static ProMdl inc_drw(void)
{
    ProMdl drw = NULL;
    /* r133: LOADFILE 精确加载的图优先，避�?search_path 里同名旧图被误编�?*/
    if (g_loaded_drw) return g_loaded_drw;
    if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) != PRO_TK_NO_ERROR) return NULL;
    return drw;
}

static void inc_note_text(ProDtlnote *note, wchar_t *out, int cap)
{
    ProDtlnotedata nd = NULL;
    out[0] = 0;
    if (ProDtlnoteDataGet(note, NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) return;
    {
        ProDtlnoteline *lines = NULL;
        if (ProDtlnotedataLinesCollect(nd, &lines) == PRO_TK_NO_ERROR && lines) {
            int lc = 0, l;
            ProArraySizeGet((ProArray)lines, &lc);
            for (l = 0; l < lc; l++) {
                ProDtlnotetext *txts = NULL;
                if (ProDtlnotelineTextsCollect(lines[l], &txts) == PRO_TK_NO_ERROR && txts) {
                    int tc = 0, t;
                    ProArraySizeGet((ProArray)txts, &tc);
                    for (t = 0; t < tc; t++) {
                        ProLine s; s[0] = 0;
                        if (ProDtlnotetextStringGet(txts[t], s) == PRO_TK_NO_ERROR) {
                            if (out[0] && wcslen(out) + 1 < (size_t)cap) wcscat_s(out, cap, L"|");
                            wcsncat_s(out, cap, s, _TRUNCATE);
                        }
                    }
                }
            }
        }
    }
}

static void inc_state_views(ProDrawing drw)
{
    ProView *views = NULL;
    int vc = 0, i;
    if (ProDrawingViewsCollect(drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[state] views collect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    logf_("[state] views = %d", vc);
    for (i = 0; i < vc; i++) {
        ProName nm; ProPoint3d o[2]; int vid = -1; char n8[128];
        nm[0] = 0;
        ProDrawingViewNameGet(drw, views[i], nm);
        wide_to_u8(nm, n8, sizeof(n8));
        ProDrawingViewIdGet(drw, views[i], &vid);
        o[0][0] = o[0][1] = o[0][2] = -9999.0;
        o[1][0] = o[1][1] = o[1][2] = -9999.0;
        ProDrawingViewOutlineGet(drw, views[i], o);
        logf_("[state] view[%d] id=%d name='%s' outline x[%.1f..%.1f] y[%.1f..%.1f]",
              i, vid, n8, o[0][0], o[1][0], o[0][1], o[1][1]);
    }
    ProArrayFree((ProArray *)&views);
}

static void inc_state_notes(ProDrawing drw)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i;
    if (ProDrawingDtlnotesCollect(drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[state] notes collect failed");
        return;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    logf_("[state] notes = %d", cnt);
    for (i = 0; i < cnt; i++) {
        wchar_t txt[512]; char t8[2048]; ProLineEnvelope env;
        inc_note_text(&notes[i], txt, 512);
        wide_to_u8(txt, t8, sizeof(t8));
        env[0][0] = env[0][1] = -9999.0;
        env[3][0] = env[3][1] = -9999.0;
        ProDtlnoteLineEnvelopeGet(&notes[i], 0, env);
        logf_("[state] note[%d] id=%d env=(%.1f,%.1f)-(%.1f,%.1f) text='%s'",
              i, (int)notes[i].id, env[0][0], env[0][1], env[3][0], env[3][1], t8);
    }
    ProArrayFree((ProArray *)&notes);
}

static void inc_state_syms(ProDrawing drw)
{
    ProDtlsyminst *insts = NULL;
    int cnt = 0, i;
    if (ProDrawingDtlsyminstsCollect(drw, PRO_VALUE_UNUSED, &insts) != PRO_TK_NO_ERROR || !insts) {
        logf_("[state] syms collect failed");
        return;
    }
    ProArraySizeGet((ProArray)insts, &cnt);
    logf_("[state] syms = %d", cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlsyminstdata d = NULL; ProDtlsymdef def; ProDtlsymdefdata dd = NULL;
        ProName nm; char n8[128]; int id = -1; ProLineEnvelope env;
        nm[0] = 0;
        if (ProDtlsyminstDataGet(&insts[i], PRODISPMODE_NUMERIC, &d) == PRO_TK_NO_ERROR && d) {
            ProDtlsyminstdataIdGet(d, &id);
            if (ProDtlsyminstdataDefGet(d, &def) == PRO_TK_NO_ERROR &&
                ProDtlsymdefDataGet(&def, &dd) == PRO_TK_NO_ERROR && dd)
                ProDtlsymdefdataNameGet(dd, nm);
        }
        wide_to_u8(nm, n8, sizeof(n8));
        env[0][0] = env[0][1] = -9999.0;
        env[3][0] = env[3][1] = -9999.0;
        ProDtlsyminstEnvelopeGet(&insts[i], env);
        logf_("[state] sym[%d] id=%d name='%s' env=(%.1f,%.1f)-(%.1f,%.1f)",
              i, id, n8, env[0][0], env[0][1], env[3][0], env[3][1]);
    }
    ProArrayFree((ProArray *)&insts);
}

static void inc_state_run(void)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    if (!drw) { logf_("[state] drawing not found"); return; }
    logf_("[state] ===== BEGIN =====");
    inc_state_views(drw);
    ddims_run(m);
    inc_state_notes(drw);
    inc_state_syms(drw);
    logf_("[state] ===== END =====");
}

static int inc_delsym_run(const wchar_t *name)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    ProDtlsyminst *insts = NULL;
    int cnt = 0, i, deleted = 0;
    if (!drw) { logf_("[delsym] drawing not found"); return 0; }
    if (ProDrawingDtlsyminstsCollect(drw, PRO_VALUE_UNUSED, &insts) != PRO_TK_NO_ERROR || !insts) {
        logf_("[delsym] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)insts, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlsyminstdata d = NULL; ProDtlsymdef def; ProDtlsymdefdata dd = NULL;
        ProName nm; char n8[128]; int id = -1;
        nm[0] = 0;
        if (ProDtlsyminstDataGet(&insts[i], PRODISPMODE_NUMERIC, &d) != PRO_TK_NO_ERROR || !d) continue;
        ProDtlsyminstdataIdGet(d, &id);
        if (ProDtlsyminstdataDefGet(d, &def) == PRO_TK_NO_ERROR &&
            ProDtlsymdefDataGet(&def, &dd) == PRO_TK_NO_ERROR && dd)
            ProDtlsymdefdataNameGet(dd, nm);
        wide_to_u8(nm, n8, sizeof(n8));
        if (wcscmp(nm, name) != 0) continue;
        {
            ProError e = ProDtlsyminstDelete(&insts[i]);
            logf_("[delsym] id=%d name='%s' -> %d (%s)", id, n8, (int)e, errname(e));
            if (e == PRO_TK_NO_ERROR) deleted++;
        }
    }
    ProArrayFree((ProArray *)&insts);
    logf_("[delsym] name='%ls' deleted=%d/%d", name, deleted, cnt);
    if (deleted) ProDwgSheetRegenerate(drw, 1);
    return deleted;
}

static int inc_delnote_run(const wchar_t *sub)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    ProDtlnote *notes = NULL;
    int cnt = 0, i, deleted = 0;
    if (!drw) { logf_("[delnote] drawing not found"); return 0; }
    if (ProDrawingDtlnotesCollect(drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[delnote] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        wchar_t txt[512]; char t8[2048];
        inc_note_text(&notes[i], txt, 512);
        if (!wcsstr(txt, sub)) continue;
        wide_to_u8(txt, t8, sizeof(t8));
        {
            ProError e = ProDtlnoteDelete(&notes[i], NULL);
            logf_("[delnote] id=%d text='%s' -> %d (%s)", (int)notes[i].id, t8, (int)e, errname(e));
            if (e == PRO_TK_NO_ERROR) deleted++;
        }
    }
    ProArrayFree((ProArray *)&notes);
    logf_("[delnote] substr='%ls' deleted=%d/%d", sub, deleted, cnt);
    if (deleted) ProDwgSheetRegenerate(drw, 1);
    return deleted;
}

/* 2026-11-05: DELNOTEID|<id>[,<id>...] - delete draft notes by id.
   DELNOTE matches text only, and the leftover A3 format shares every label
   (cup-lu/shen-he/xxx/1:1/...) with the A4 one, so text matching cannot remove
   just the stray block. */
static int inc_delnoteid_run(const wchar_t *list)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    ProDtlnote *notes = NULL;
    int cnt = 0, i, deleted = 0, nwant = 0;
    int want[64];
    const wchar_t *q = list;

    if (!drw) { logf_("[delnoteid] drawing not found"); return 0; }
    while (q && *q && nwant < 64) {
        wchar_t *end = NULL;
        long v = wcstol(q, &end, 10);
        if (end == q) break;
        want[nwant++] = (int)v;
        q = end;
        while (*q == L',' || *q == L' ') q++;
    }
    if (!nwant) { logf_("[delnoteid] no ids"); return 0; }
    if (ProDrawingDtlnotesCollect(drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[delnoteid] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    logf_("[delnoteid] notes=%d want=%d", cnt, nwant);
    for (i = 0; i < cnt; i++) {
        int k, hit = 0;
        wchar_t txt[512];
        char t8[2048];
        ProError e;
        for (k = 0; k < nwant; k++) if (want[k] == (int)notes[i].id) hit = 1;
        if (!hit) continue;
        inc_note_text(&notes[i], txt, 512);
        wide_to_u8(txt, t8, sizeof(t8));
        e = ProDtlnoteDelete(&notes[i], NULL);
        logf_("[delnoteid] id=%d text='%s' -> %d (%s)", (int)notes[i].id, t8, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) deleted++;
    }
    ProArrayFree((ProArray *)&notes);
    logf_("[delnoteid] deleted=%d/%d", deleted, nwant);
    if (deleted) ProDwgSheetRegenerate(drw, 1);
    return deleted;
}

/* 2026-10-27: HIDENOTE|<子串> —�?只把匹配注记�?显示"标志关掉，不删除注记�?
   用于隐藏 Creo 自动�?独立比例视图"加的那行「比�?1:1」标签：
   这类注记由系统持有，ProDtlnoteDelete 返回 -1 删不掉，但显示标志可以关�?*/
static int inc_hidenote_run(const wchar_t *sub)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    ProDtlnote *notes = NULL;
    int cnt = 0, i, hidden = 0;
    if (!drw) { logf_("[hidenote] drawing not found"); return 0; }
    if (ProDrawingDtlnotesCollect(drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[hidenote] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        wchar_t txt[512];
        char t8[2048];
        inc_note_text(&notes[i], txt, 512);
        if (!wcsstr(txt, sub)) continue;
        wide_to_u8(txt, t8, sizeof(t8));
        {
            ProDtlnotedata data = NULL;
            ProError eg = ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &data);
            if (eg == PRO_TK_NO_ERROR && data) {
                ProError es = ProDtlnotedataDisplayedSet(data, PRO_B_FALSE);
                logf_("[hidenote] id=%d text='%s' set->%d (%s)", (int)notes[i].id, t8, (int)es, errname(es));
                if (es == PRO_TK_NO_ERROR) hidden++;
            } else {
                logf_("[hidenote] id=%d dataGet->%d (%s) text='%s'", (int)notes[i].id, (int)eg, errname(eg), t8);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
    logf_("[hidenote] substr='%ls' hidden=%d/%d", sub, hidden, cnt);
    if (hidden) ProDwgSheetRegenerate(drw, 1);
    return hidden;
}

static void inc_delview_run(const wchar_t *name)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    ProView *views = NULL;
    int vc = 0, i, deleted = 0;
    if (!drw) { logf_("[delview] drawing not found"); return; }
    if (ProDrawingViewsCollect(drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[delview] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    /* 2026-10-27: 支持 DELVIEW|#<n> 按索引删。中文视图名经命令通道编码不一致，
       wcscmp 必然匹配不上；索引稳定可靠�?*/
    if (name[0] == L'#') {
        int want = (int)wcstol(name + 1, NULL, 10);
        if (want >= 0 && want < vc) {
            ProName nm2; char n8b[128]; int vidb = -1;
            nm2[0] = 0;
            ProDrawingViewNameGet(drw, views[want], nm2);
            wide_to_u8(nm2, n8b, sizeof(n8b));
            ProDrawingViewIdGet(drw, views[want], &vidb);
            {
                ProError e2 = ProDrawingViewDelete(drw, views[want], PRO_B_TRUE);
                logf_("[delview] #%d id=%d name='%s' -> %d (%s)", want, vidb, n8b, (int)e2, errname(e2));
                if (e2 == PRO_TK_NO_ERROR) deleted++;
            }
        } else {
            logf_("[delview] #%d out of range (vc=%d)", want, vc);
        }
        ProArrayFree((ProArray *)&views);
        logf_("[delview] index delete done=%d/%d", deleted, vc);
        if (deleted) ProDwgSheetRegenerate(drw, 1);
        return;
    }
    for (i = 0; i < vc; i++) {
        ProName nm; char n8[128]; int vid = -1;
        nm[0] = 0;
        ProDrawingViewNameGet(drw, views[i], nm);
        wide_to_u8(nm, n8, sizeof(n8));
        ProDrawingViewIdGet(drw, views[i], &vid);
        if (wcscmp(nm, name) != 0) continue;
        {
            ProError e = ProDrawingViewDelete(drw, views[i], PRO_B_TRUE);
            logf_("[delview] index=%d id=%d name='%s' -> %d (%s)", i, vid, n8, (int)e, errname(e));
            if (e == PRO_TK_NO_ERROR) deleted++;
        }
    }
    ProArrayFree((ProArray *)&views);
    logf_("[delview] name='%ls' deleted=%d/%d", name, deleted, vc);
    if (deleted) ProDwgSheetRegenerate(drw, 1);
}

typedef struct { int want; int n; ProDimension hits[32]; } inc_deldim_ctx_t;

static ProError inc_deldim_cb(ProDimension *dim, ProError status, ProAppData app)
{
    inc_deldim_ctx_t *c = (inc_deldim_ctx_t *)app;
    (void)status;
    if ((int)dim->id == c->want && c->n < 32) { c->hits[c->n++] = *dim; }
    return PRO_TK_NO_ERROR;
}

typedef struct { ProDimension dims[512]; int n; } inc_deldim_all_ctx_t;

static ProError inc_deldim_all_cb(ProDimension *dim, ProError status, ProAppData app)
{
    inc_deldim_all_ctx_t *c = (inc_deldim_all_ctx_t *)app;
    (void)status;
    if (c->n < 512) c->dims[c->n++] = *dim;
    return PRO_TK_NO_ERROR;
}

static void inc_deldim_all_run(void)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    inc_deldim_all_ctx_t c;
    if (!drw) { logf_("[deldim] drawing not found"); return; }
    c.n = 0;
    ProDrawingDimensionVisit(drw, PRO_DIMENSION, inc_deldim_all_cb, NULL, (ProAppData)&c);
    ProDrawingDimensionVisit(drw, PRO_REF_DIMENSION, inc_deldim_all_cb, NULL, (ProAppData)&c);
    {
        int i, deleted = 0;
        for (i = 0; i < c.n; i++) {
            double v = -1.0;
            int did = (int)c.dims[i].id;
            ProDimensionValueGet(&c.dims[i], &v);
            {
                ProError e = ProDimensionDelete(&c.dims[i]);
                logf_("[deldim] ALL id=%d value=%.4f -> %d (%s)", did, v, (int)e, errname(e));
                if (e == PRO_TK_NO_ERROR) deleted++;
            }
        }
        logf_("[deldim] ALL collected=%d deleted=%d", c.n, deleted);
        if (deleted) ProDwgSheetRegenerate(drw, 1);
    }
}
static void inc_deldim_run(int id)
{
    ProMdl m = inc_drw();
    ProDrawing drw = (ProDrawing)m;
    inc_deldim_ctx_t c;
    if (!drw) { logf_("[deldim] drawing not found"); return; }
    c.want = id; c.n = 0;
    ProDrawingDimensionVisit(drw, PRO_DIMENSION, inc_deldim_cb, NULL, (ProAppData)&c);
    ProDrawingDimensionVisit(drw, PRO_REF_DIMENSION, inc_deldim_cb, NULL, (ProAppData)&c);
    {
        int i, deleted = 0;
        for (i = 0; i < c.n; i++) {
            double v = -1.0;
            ProDimensionValueGet(&c.hits[i], &v);
            {
                ProError e = ProDimensionDelete(&c.hits[i]);
                logf_("[deldim] id=%d value=%.4f -> %d (%s)", id, v, (int)e, errname(e));
                if (e == PRO_TK_NO_ERROR) deleted++;
            }
        }
        logf_("[deldim] id=%d deleted=%d", id, deleted);
        if (deleted) ProDwgSheetRegenerate(drw, 1);
    }
}

/* ===== 轮次31：AFIND 只读转储"模型注解元素"�?D 注释——用户可能在零件里标的尺寸）�?===== */
static ProError afind_cb(ProAnnotationElem *elem, ProError status, ProAppData app)
{
    ProAnnotation ann;

    (void)status;
    (void)app;
    memset(&ann, 0, sizeof(ann));
    if (ProAnnotationelemAnnotationGet(elem, &ann) != PRO_TK_NO_ERROR) {
        logf_("[afind] elem: no annotation");
        return PRO_TK_NO_ERROR;
    }
    {
        int t  = (int)((ProModelitem *)&ann)->type;
        int id = (int)((ProModelitem *)&ann)->id;

        if (t == PRO_DIMENSION || t == PRO_REF_DIMENSION) {
            double v = -1.0;

            if (ProDimensionValueGet((ProDimension *)&ann, &v) == PRO_TK_NO_ERROR)
                logf_("[afind] DIM annotation id=%d type=%d value=%.4f", id, t, v);
            else
                logf_("[afind] DIM annotation id=%d type=%d (value read failed)", id, t);
        } else {
            logf_("[afind] annotation id=%d type=%d (non-dim)", id, t);
        }
    }
    return PRO_TK_NO_ERROR;
}

static void afind_run(ProMdl part)
{
    ProError e = ProSolidAnnotationelemsVisit((ProSolid)part, afind_cb, NULL, NULL);

    logf_("[afind] solidAnnotationelemsVisit -> %d (%s)", (int)e, errname(e));
}

/* ===== 轮次37：DARROW|<�?|<类型> —�?对图纸上该值的尺寸逐箭头设 ProLeaderType
   （实验：10=PROLEADERTYPE_DOUBLEARROW, 1=ARROWHEAD；只读之外唯一改动=箭头类型�?*/
typedef struct { double target; int ty; ProDrawing drw; int n; } darrow_ctx_t;

static ProError darrow_cb(ProDimension *dim, ProError status, ProAppData app)
{
    darrow_ctx_t *c = (darrow_ctx_t *)app;
    double        v = 0.0;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    {
        ProError e1 = ProDimensionArrowtypeSet(dim, c->drw, 1, (ProLeaderType)c->ty);
        ProError e2 = ProDimensionArrowtypeSet(dim, c->drw, 2, (ProLeaderType)c->ty);

        logf_("[darrow] dim id=%d v=%.4f -> arrow1=%d (%s) ; arrow2=%d (%s)",
              (int)dim->id, v, (int)e1, errname(e1), (int)e2, errname(e2));
        c->n++;
    }
    return PRO_TK_NO_ERROR;
}

/* ===== 轮次43：FDIM|idA|idB|x|y|O|V —�?直选两个面建尺寸（R3�?看着那两个面去�?）�?
   O: H/V（orient_hint）；V: F=主视�?/ S=侧视图。创建即终位（不�?Move）、无兜底、如实打�?Creo 算值�?*/
static void fdim_run(ProMdl part, ProMdl drw, int fida, int fidb, double fx, double fy, wchar_t oc, wchar_t vc)
{
    ProView   *views = NULL;
    ProView    view = NULL;
    ProSurface sa = NULL, sb = NULL;
    ProGeomitem g1, g2;
    ProSelection s1 = NULL, s2 = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProDimOrient oh;
    ProVector loc;

    oh = (oc == L'V' || oc == L'v') ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[fdim] viewsCollect failed");
        return;
    }
    {
        int vc = 0, i;

        ProArraySizeGet((ProArray)views, &vc);
        for (i = 0; i < vc; i++) {
            ProName nm;

            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (vc == L'S' || vc == L's') {
                if (wcsstr(nm, L"\u5DE6\u4FA7") || wcsstr(nm, L"\u5DE6\u89C6") ||
                    wcsstr(nm, L"left") || wcsstr(nm, L"LEFT") || wcsstr(nm, L"Left")) { view = views[i]; break; }
            } else if (vc == L'T' || vc == L't') {
                if (wcsstr(nm, L"\u9876\u90E8") || wcsstr(nm, L"\u4FEF\u89C6") ||
                    wcsstr(nm, L"top") || wcsstr(nm, L"TOP") || wcsstr(nm, L"Top")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT") || wcsstr(nm, L"\u4E3B\u89C6")) { view = views[i]; break; }
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    if (!view) { logf_("[fdim] target view not found"); return; }
    if (ProSurfaceInit(part, fida, &sa) != PRO_TK_NO_ERROR || !sa) { logf_("[fdim] surface %d init failed", fida); return; }
    if (ProSurfaceInit(part, fidb, &sb) != PRO_TK_NO_ERROR || !sb) { logf_("[fdim] surface %d init failed", fidb); return; }
    if (ProSurfaceToGeomitem((ProSolid)part, sa, &g1) != PRO_TK_NO_ERROR) { logf_("[fdim] surfaceToGi %d failed", fida); return; }
    if (ProSurfaceToGeomitem((ProSolid)part, sb, &g2) != PRO_TK_NO_ERROR) { logf_("[fdim] surfaceToGi %d failed", fidb); return; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1) != PRO_TK_NO_ERROR) { logf_("[fdim] selA failed"); return; }
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); logf_("[fdim] selB failed"); return; }
    ProSelectionViewSet(view, &s2);
    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = oh;
    sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = oh;
    loc[0] = fx; loc[1] = fy; loc[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &dim);

        logf_("[fdim] faces %d/%d create -> %d (%s)", fida, fidb, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;

            if (ProDimensionValueGet(&dim, &nv) == PRO_TK_NO_ERROR)
                logf_("[fdim] *** Creo computed value = %.4f *** (faces %d/%d, view=%s, orient=%s)",
                      nv, fida, fidb, (vc == L'S' || vc == L's') ? "SIDE" : "FRONT",
                      (oh == PRO_DIM_ORNT_VERT) ? "VERT" : "HORIZ");
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&dim, NULL, view);
                logf_("[fdim] show -> %d (%s)", (int)es, errname(es));
            }
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
}

/* ===== 轮次44：EDIM|idA|idB|x|y|O|V —…直选两条边建尺寸（63.6 界线锚点调查后的专用工具）…
   O: H/V orient；V: F=主视图 / S=侧视图（参数样式…FDIM 相同）…*/
static void cdimx_run(ProMdl part, ProMdl drw, int idA, int idB, wchar_t oc, wchar_t vc)
{
    ProView   *views = NULL;
    ProView    view = NULL;
    ProEdge    ea = NULL, eb = NULL;
    ProGeomitem g1, g2;
    ProSelection s1 = NULL, s2 = NULL;
    ProDimOrient oh;
    int         combo;

    oh = (oc == L'V' || oc == L'v') ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) return;
    {
        int vc2 = 0, i;
        ProArraySizeGet((ProArray)views, &vc2);
        for (i = 0; i < vc2; i++) {
            ProName nm; nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { view = views[i]; break; }
        }
    }
    ProArrayFree((ProArray *)&views);
    if (!view) { logf_("[cdimx] no front view"); return; }
    if (ProEdgeInit((ProSolid)part, idA, &ea) != PRO_TK_NO_ERROR || !ea) { logf_("[cdimx] edge %d bad", idA); return; }
    if (ProEdgeInit((ProSolid)part, idB, &eb) != PRO_TK_NO_ERROR || !eb) { logf_("[cdimx] edge %d bad", idB); return; }
    if (ProEdgeToGeomitem((ProSolid)part, ea, &g1) != PRO_TK_NO_ERROR) return;
    if (ProEdgeToGeomitem((ProSolid)part, eb, &g2) != PRO_TK_NO_ERROR) return;
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1) != PRO_TK_NO_ERROR) return;
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2) != PRO_TK_NO_ERROR) return;
    ProSelectionViewSet(view, &s2);

    for (combo = 0; combo < 6; combo++) {
        ProDimAttachment *arr = NULL;
        ProDimSense *sen = NULL;
        ProDimension dim;
        ProVector loc;
        ProError e;
        int ptype = 0, ornt_in_call = 0;

        switch (combo) {
            case 0: ptype = PRO_POINT_TYP_CENTER; ornt_in_call = 0; break;
            case 1: ptype = PRO_POINT_TYP_CENTER; ornt_in_call = 1; break;
            case 2: ptype = PRO_POINT_TYP_END1;   ornt_in_call = 1; break;
            case 3: ptype = PRO_POINT_TYP_END2;   ornt_in_call = 1; break;
            case 4: ptype = PRO_POINT_TYP_MIDPT;  ornt_in_call = 1; break;
            default: ptype = PRO_POINT_TYP_NONE;  ornt_in_call = 1; break;
        }
        ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
        ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
        ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
        ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
        memset(&sen[0], 0, sizeof(ProDimSense));
        memset(&sen[1], 0, sizeof(ProDimSense));
        sen[0].type = PRO_DIM_SNS_TYP_PNT; sen[0].sense = ptype; sen[0].orient_hint = oh;
        sen[1].type = PRO_DIM_SNS_TYP_PNT; sen[1].sense = ptype; sen[1].orient_hint = oh;
        loc[0] = 205.0; loc[1] = 520.0; loc[2] = 0.0;
        memset(&dim, 0, sizeof(dim));
        e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen,
                                      ornt_in_call ? oh : PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &dim);
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;
            ProDimensionValueGet(&dim, &nv);
            logf_("[cdimx] combo=%d ptype=%d orntcall=%d -> value=%.4f", combo, ptype, ornt_in_call, nv);
            {
                ProError ed = ProAnnotationErase((ProAnnotation *)&dim, view);
                logf_("[cdimx] erase -> %d", (int)ed);
            }
        } else {
            logf_("[cdimx] combo=%d ptype=%d create -> %d (%s)", combo, ptype, (int)e, errname(e));
        }
        ProArrayFree((ProArray *)&arr);
        ProArrayFree((ProArray *)&sen);
    }
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
    logf_("[cdimx] done");
}

static void cdim_run(ProMdl part, ProMdl drw, int idA, int idB, double fx, double fy, wchar_t oc, wchar_t vc, int pt)
{
    ProView   *views = NULL;
    ProView    view = NULL;
    ProEdge    ea = NULL, eb = NULL;
    ProGeomitem g1, g2;
    ProSelection s1 = NULL, s2 = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProDimOrient oh;
    ProVector loc;

    oh = (oc == L'V' || oc == L'v') ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    {   /* dimension creation needs the drawing displayed */
        int wid = -1;
        ProError ed = ProMdlDisplay(drw);
        ProMdlWindowGet(drw, &wid);
        if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
        logf_("[cdim] display=%d win=%d", (int)ed, wid);
    }
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[cdim] viewsCollect failed");
        return;
    }
    {
        int vc2 = 0, i;

        ProArraySizeGet((ProArray)views, &vc2);
        for (i = 0; i < vc2; i++) {
            ProName nm;

            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (vc == L'S' || vc == L's') {
                if (wcsstr(nm, L"left") || wcsstr(nm, L"LEFT") || wcsstr(nm, L"Left")) { view = views[i]; break; }
            } else if (vc == L'T' || vc == L't') {
                if (wcsstr(nm, L"top") || wcsstr(nm, L"TOP") || wcsstr(nm, L"Top")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { view = views[i]; break; }
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    if (!view) { logf_("[cdim] target view not found"); return; }
    if (ProEdgeInit((ProSolid)part, idA, &ea) != PRO_TK_NO_ERROR || !ea) { logf_("[cdim] edge %d init failed", idA); return; }
    if (ProEdgeInit((ProSolid)part, idB, &eb) != PRO_TK_NO_ERROR || !eb) { logf_("[cdim] edge %d init failed", idB); return; }
    if (ProEdgeToGeomitem((ProSolid)part, ea, &g1) != PRO_TK_NO_ERROR) { logf_("[cdim] edgeToGi %d failed", idA); return; }
    if (ProEdgeToGeomitem((ProSolid)part, eb, &g2) != PRO_TK_NO_ERROR) { logf_("[cdim] edgeToGi %d failed", idB); return; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1) != PRO_TK_NO_ERROR) { logf_("[cdim] selA failed"); return; }
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); logf_("[cdim] selB failed"); return; }
    ProSelectionViewSet(view, &s2);
    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_PNT; sen[0].sense = (pt >= 1 && pt <= 4) ? pt : PRO_POINT_TYP_CENTER;
    sen[1].type = PRO_DIM_SNS_TYP_PNT; sen[1].sense = (pt >= 1 && pt <= 4) ? pt : PRO_POINT_TYP_CENTER;
    sen[0].orient_hint = oh;
    sen[1].orient_hint = oh;
    loc[0] = fx; loc[1] = fy; loc[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, oh, loc, PRO_B_FALSE, &dim);
        logf_("[cdim] edges %d/%d pt=%d create -> %d (%s)", idA, idB, pt, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;

            if (ProDimensionValueGet(&dim, &nv) == PRO_TK_NO_ERROR)
                logf_("[cdim] *** Creo computed value = %.4f *** (edges %d/%d, view=%s, orient=%s)",
                      nv, idA, idB, (vc == L'S' || vc == L's') ? "SIDE" : "FRONT",
                      (oh == PRO_DIM_ORNT_VERT) ? "VERT" : "HORIZ");
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&dim, NULL, view);
                logf_("[cdim] show -> %d (%s)", (int)es, errname(es));
            }
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
}

static void edim_run(ProMdl part, ProMdl drw, int idA, int idB, double fx, double fy, wchar_t oc, wchar_t vc)
{
    ProView   *views = NULL;
    ProView    view = NULL;
    ProEdge    ea = NULL, eb = NULL;
    ProGeomitem g1, g2;
    ProSelection s1 = NULL, s2 = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProDimOrient oh;
    ProVector loc;

    oh = (oc == L'V' || oc == L'v') ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    {   /* dimension creation needs the drawing displayed */
        int wid = -1;
        ProError ed = ProMdlDisplay(drw);
        ProMdlWindowGet(drw, &wid);
        if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
        logf_("[edim] display=%d win=%d", (int)ed, wid);
    }
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[edim] viewsCollect failed");
        return;
    }
    {
        int vc2 = 0, i;

        ProArraySizeGet((ProArray)views, &vc2);
        for (i = 0; i < vc2; i++) {
            ProName nm;

            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (vc == L'S' || vc == L's') {
                if (wcsstr(nm, L"\u5DE6\u4FA7") || wcsstr(nm, L"\u5DE6\u89C6") ||
                    wcsstr(nm, L"left") || wcsstr(nm, L"LEFT") || wcsstr(nm, L"Left")) { view = views[i]; break; }
            } else if (vc == L'T' || vc == L't') {
                if (wcsstr(nm, L"\u9876\u90E8") || wcsstr(nm, L"\u4FEF\u89C6") ||
                    wcsstr(nm, L"top") || wcsstr(nm, L"TOP") || wcsstr(nm, L"Top")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT") || wcsstr(nm, L"\u4E3B\u89C6")) { view = views[i]; break; }
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    if (!view) { logf_("[edim] target view not found"); return; }
    if (ProEdgeInit((ProSolid)part, idA, &ea) != PRO_TK_NO_ERROR || !ea) { logf_("[edim] edge %d init failed", idA); return; }
    if (ProEdgeInit((ProSolid)part, idB, &eb) != PRO_TK_NO_ERROR || !eb) { logf_("[edim] edge %d init failed", idB); return; }
    if (ProEdgeToGeomitem((ProSolid)part, ea, &g1) != PRO_TK_NO_ERROR) { logf_("[edim] edgeToGi %d failed", idA); return; }
    if (ProEdgeToGeomitem((ProSolid)part, eb, &g2) != PRO_TK_NO_ERROR) { logf_("[edim] edgeToGi %d failed", idB); return; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1) != PRO_TK_NO_ERROR) { logf_("[edim] selA failed"); return; }
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); logf_("[edim] selB failed"); return; }
    ProSelectionViewSet(view, &s2);
    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = oh;
    sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = oh;
    loc[0] = fx; loc[1] = fy; loc[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &dim);

        logf_("[edim] edges %d/%d create -> %d (%s)", idA, idB, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;

            if (ProDimensionValueGet(&dim, &nv) == PRO_TK_NO_ERROR)
                logf_("[edim] *** Creo computed value = %.4f *** (edges %d/%d, view=%s, orient=%s)",
                      nv, idA, idB, (vc == L'S' || vc == L's') ? "SIDE" : "FRONT",
                      (oh == PRO_DIM_ORNT_VERT) ? "VERT" : "HORIZ");
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&dim, NULL, view);
                logf_("[edim] show -> %d (%s)", (int)es, errname(es));
            }
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
}

/* XDIM|idA|idB|x|y|O|V ：以两条 draft 实体（十字中心线）为附件建尺寸�?
   �?EDIM 的区别：附件是图纸实体，视图上下文取真实 front 视图，loc 一次给对；
   建完立刻 ProAnnotationShow(view) + regen，避�?shown=0�?*/
static void xdim_run(ProMdl drw, int idA, int idB, double fx, double fy, wchar_t oc, wchar_t vc)
{
    ProView   *views = NULL;
    ProView    view = NULL;
    ProModelitem mA, mB;
    ProSelection s1 = NULL, s2 = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProDimOrient oh;
    ProVector loc;

    oh = (oc == L'V' || oc == L'v') ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[xdim2] viewsCollect failed");
        return;
    }
    {
        int vc2 = 0, i;

        ProArraySizeGet((ProArray)views, &vc2);
        for (i = 0; i < vc2; i++) {
            ProName nm;

            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (vc == L'S' || vc == L's') {
                if (wcsstr(nm, L"\u5DE6\u4FA7") || wcsstr(nm, L"\u5DE6\u89C6") ||
                    wcsstr(nm, L"left") || wcsstr(nm, L"LEFT") || wcsstr(nm, L"Left")) { view = views[i]; break; }
            } else if (vc == L'T' || vc == L't') {
                if (wcsstr(nm, L"\u9876\u90E8") || wcsstr(nm, L"\u4FEF\u89C6") ||
                    wcsstr(nm, L"top") || wcsstr(nm, L"TOP") || wcsstr(nm, L"Top")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT") || wcsstr(nm, L"\u4E3B\u89C6")) { view = views[i]; break; }
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    if (!view) { logf_("[xdim2] target view not found"); return; }
    memset(&mA, 0, sizeof(mA)); memset(&mB, 0, sizeof(mB));
    if (ProModelitemInit(drw, idA, PRO_DRAFT_ENTITY, &mA) != PRO_TK_NO_ERROR) { logf_("[xdim2] item %d failed", idA); return; }
    if (ProModelitemInit(drw, idB, PRO_DRAFT_ENTITY, &mB) != PRO_TK_NO_ERROR) { logf_("[xdim2] item %d failed", idB); return; }
    if (ProSelectionAlloc(NULL, &mA, &s1) != PRO_TK_NO_ERROR) { logf_("[xdim2] selA failed"); return; }
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, &mB, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); logf_("[xdim2] selB failed"); return; }
    ProSelectionViewSet(view, &s2);
    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = oh;
    sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = oh;
    loc[0] = fx; loc[1] = fy; loc[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError e = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &dim);

        logf_("[xdim2] draft %d/%d create -> %d (%s) loc=(%.1f,%.1f)", idA, idB, (int)e, errname(e), fx, fy);
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;
            ProError es;

            if (ProDimensionValueGet(&dim, &nv) == PRO_TK_NO_ERROR)
                logf_("[xdim2] *** computed = %.4f *** (%d/%d, %s, %s)", nv, idA, idB,
                      (vc == L'S' || vc == L's') ? "SIDE" : "FRONT", (oh == PRO_DIM_ORNT_VERT) ? "VERT" : "HORIZ");
            es = ProAnnotationShow((ProAnnotation *)&dim, NULL, view);
            logf_("[xdim2] show -> %d (%s)", (int)es, errname(es));
            ProDwgSheetRegenerate((ProDrawing)drw, 1);
            {
                ProBoolean sh = PRO_B_FALSE;
                ProDimlocation dl = NULL;
                ProPoint3d tp = { -9999.0, -9999.0, -9999.0 };
                ProBoolean he = PRO_B_FALSE;
                double el = 0.0;

                ProAnnotationIsShown((ProAnnotation *)&dim, (ProDrawing)drw, &sh);
                if (ProDimensionLocationGet(&dim, NULL, (ProDrawing)drw, &dl) == PRO_TK_NO_ERROR && dl)
                    ProDimlocationTextGet(dl, &he, tp, &el);
                logf_("[xdim2] id=%d shown=%d text=(%.1f,%.1f)", (int)dim.id, (int)sh, tp[0], tp[1]);
                {
                    ProDimAttachment *aa = NULL;
                    ProDimSense *ss = NULL;
                    ProError eg = ProDrawingDimAttachpointsGet((ProDrawing)drw, &dim, &aa, &ss);

                    if (eg == PRO_TK_NO_ERROR && aa) {
                        int n = 0;
                        ProArraySizeGet((ProArray)aa, &n);
                        logf_("[xdim2] attachments n=%d", n);
                        ProDimattachmentarrayFree(aa);
                    } else logf_("[xdim2] attachments get -> %d (%s)", (int)eg, errname(eg));
                }
            }
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
}
/* MOVED|<id>|<x>|<y>：按图纸尺寸 id 精确移动（ProDrawingDimensionMove，绝对位置）�?
   �?MOVE|value 的区别：MOVE 只访问模型尺寸；本命令访问图纸尺寸，id 不歧义�?*/
typedef struct { int id; ProDrawing drw; double x, y; int found; } moved_ctx_t;

static ProError moved_cb(ProDimension *dim, ProError status, ProAppData app)
{
    moved_ctx_t *c = (moved_ctx_t *)app;
    ProPoint3d   pos;
    ProError     em;
    double       v = 0.0;

    (void)status;
    if ((int)dim->id != c->id) return PRO_TK_NO_ERROR;
    c->found = 1;
    pos[0] = c->x; pos[1] = c->y; pos[2] = 0.0;
    {
        ProDimlocation loc = NULL;
        ProBoolean he = PRO_B_FALSE;
        ProPoint3d p = { -9999.0, -9999.0, -9999.0 };
        double el = 0.0;
        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, p, &el);
        ProDimensionValueGet(dim, &v);
        logf_("[moved] id=%d v=%.4f BEFORE (%.2f,%.2f)", c->id, v, p[0], p[1]);
    }
    em = ProDrawingDimensionMove(c->drw, dim, pos);
    logf_("[moved] id=%d move(%.2f,%.2f) -> %d (%s)", c->id, c->x, c->y, (int)em, errname(em));
    {
        ProDimlocation loc = NULL;
        ProBoolean he = PRO_B_FALSE;
        ProPoint3d p = { -9999.0, -9999.0, -9999.0 };
        double el = 0.0;
        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, p, &el);
        logf_("[moved] id=%d AFTER  (%.2f,%.2f)", c->id, p[0], p[1]);
    }
    return PRO_TK_NO_ERROR;
}

/* LEADINFO|<id>: print text point, arrow tips, witness ends and their distances (read-only) */
typedef struct { int id; ProDrawing drw; int found; } leadinfo_ctx_t;

static double li_dist(const ProPoint3d a, const ProPoint3d b)
{
    double dx = a[0] - b[0], dy = a[1] - b[1];
    return sqrt(dx * dx + dy * dy);
}

static ProError leadinfo_cb(ProDimension *dim, ProError status, ProAppData app)
{
    leadinfo_ctx_t *c = (leadinfo_ctx_t *)app;
    ProDimlocation   loc = NULL;

    (void)status;
    if ((int)dim->id != c->id) return PRO_TK_NO_ERROR;
    c->found = 1;
    if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) != PRO_TK_NO_ERROR || !loc) {
        logf_("[leadinfo] id=%d no location", c->id);
        return PRO_TK_NO_ERROR;
    }
    {
        ProBoolean he = PRO_B_FALSE;
        ProPoint3d tp = { 0,0,0 }, a1 = { 0,0,0 }, a2 = { 0,0,0 }, w1 = { 0,0,0 }, w2 = { 0,0,0 };
        double el = 0.0;

        ProDimlocationTextGet(loc, &he, tp, &el);
        ProDimlocationArrowsGet(loc, a1, a2);
        ProDimlocationWitnesslinesGet(loc, w1, w2);
        logf_("[leadinfo] id=%d text=(%.2f,%.2f) a1=(%.2f,%.2f) a2=(%.2f,%.2f) w1=(%.2f,%.2f) w2=(%.2f,%.2f)",
             c->id, tp[0], tp[1], a1[0], a1[1], a2[0], a2[1], w1[0], w1[1], w2[0], w2[1]);
        logf_("[leadinfo] id=%d d(text,a1)=%.2f d(text,a2)=%.2f d(text,w1)=%.2f d(text,w2)=%.2f d(w1,a1)=%.2f d(w2,a2)=%.2f",
             c->id, li_dist(tp, a1), li_dist(tp, a2), li_dist(tp, w1), li_dist(tp, w2),
             li_dist(w1, a1), li_dist(w2, a2));
    }
    return PRO_TK_NO_ERROR;
}

static void leadinfo_run(ProMdl drw, int id)
{
    leadinfo_ctx_t c;

    memset(&c, 0, sizeof(c));
    c.id = id; c.drw = (ProDrawing)drw;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, leadinfo_cb, NULL, (ProAppData)&c);
    if (!c.found) logf_("[leadinfo] id=%d not found", id);
}

static void moved_run(ProMdl drw, int id, double x, double y)
{
    moved_ctx_t c;

    c.id = id; c.drw = (ProDrawing)drw; c.x = x; c.y = y; c.found = 0;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, moved_cb, NULL, (ProAppData)&c);
    if (!c.found) logf_("[moved] id=%d not found", id);
}

/* DTOL|<id>|<ty>|<up>|<lo>：给图纸尺寸设公差（ProDimensionToltypeSet/ToleranceSet）�?
   TOLSET 只访问模型尺寸，图纸尺寸�?3/23.5 等）必须走这里�?*/
typedef struct { int id, ty, dec; double up, lo; ProDrawing drw; int found; } dtol_ctx_t;

static void dtol_log_text(ProDimension *dim, const char *tag)
{
    ProLine *pt = NULL;

    if (ProDimensionTextGet(dim, &pt) == PRO_TK_NO_ERROR && pt) {
        int n = 0, i;

        ProArraySizeGet((ProArray)pt, &n);
        for (i = 0; i < n; i++)
            logf_("[dtol] %s id=%d text[%d]='%ls'", tag, (int)dim->id, i, pt[i]);
        ProArrayFree((ProArray *)&pt);
    } else logf_("[dtol] %s id=%d textGet failed", tag, (int)dim->id);
}

static ProError dtol_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dtol_ctx_t *c = (dtol_ctx_t *)app;

    (void)status;
    if ((int)dim->id != c->id) return PRO_TK_NO_ERROR;
    c->found = 1;
    if (c->ty < 0) { dtol_log_text(dim, "AFTER"); return PRO_TK_NO_ERROR; }
    dtol_log_text(dim, "BEFORE");
    {
        ProError e0 = ProDimensionTollabelSet(dim, PROTOLTABLE_NONE, L"", 0);
        ProError e1 = ProDimensionToltypeSet(dim, (ProDimToleranceType)c->ty);
        ProError e2 = ProDimensionToleranceSet(dim, c->up, c->lo);
        ProError e3 = ProDimensionTolerancedecimalsSet(dim, (c->dec >= 0 && c->dec <= 6) ? c->dec : 1);

        logf_("[dtol] id=%d ty=%d up=%.3f lo=%.3f -> label=%d (%s) type=%d (%s) tol=%d (%s) dec=%d (%s)",
             c->id, c->ty, c->up, c->lo, (int)e0, errname(e0), (int)e1, errname(e1), (int)e2, errname(e2), (int)e3, errname(e3));
    }
    return PRO_TK_NO_ERROR;
}

static void dtol_run(ProMdl drw, int id, int ty, double up, double lo, int dec)
{
    dtol_ctx_t c;

    memset(&c, 0, sizeof(c));
    c.id = id; c.ty = ty; c.up = up; c.lo = lo; c.drw = (ProDrawing)drw; c.dec = dec;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dtol_cb, NULL, (ProAppData)&c);
    if (!c.found) { logf_("[dtol] id=%d not found", id); return; }
    logf_("[dtol] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
    c.found = 0; c.ty = -1;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dtol_cb, NULL, (ProAppData)&c);
}

/* ===== 轮次45 改造B/C 运行时：视图适配 / 占用�?/ 位置可用空间搜索 / 界线验证 / AUTOBUILD =====
   位置决定全部来自"可用空间搜索"（无命令传入坐标、无视图中心+固定值、无硬编码兜底）�?
   占用�?= 各视图框 + 图幅页边 + 标题�?技术要求（模板固定件）+ 已放尺寸（线�?界线带）�?
   候�?= 方向 × 层距，评�?= min(间隙V,间隙F)×0.2 �?层序号；界线预检（不得穿其它视图）不通过即弃�?
   放完读回界线（ProDimlocationWitnesslinesGet）：端点须在自身视图�?120px 内、界线不穿其它视图——日志留证�?*/

typedef struct { ProView v; char tag[16]; ab_rect_t rect; double kx, ky, ocx, ocy; int isfront, isside, istop; } ab_view_t;
typedef struct { int id; double e1[3], e2[3], len, mx, my, mz, topy, boty; } ab_edge_t;
typedef struct { double target; ProDrawing drw; int ownvi; int found; } ab_vctx_t;

#define AB_VMAX   8
#define AB_DMAX   32

static ab_view_t g_abv[AB_VMAX];
static int       g_abnv = 0;
static ab_rect_t g_abfurn[4];
static int       g_abnfurn = 0;
static ab_rect_t g_abdr[AB_DMAX];
static char      g_abdrtag[AB_DMAX][24];
static int       g_abndr = 0;
static ab_bbox_t g_abbb;

/* ===== 轮次50：B13/B14 判据改写（旧版作废）=====
   旧版（轮�?9�?界线不得跨越零件轮廓 / 界线不得与其它尺寸相�?【已作废】——理由：
   �?制图常态：锚点在零件内部（凸台中心/孔心）时界线必须穿出轮廓；人样即如此（样�?c021006-011
      �?25±0.1 界线同样穿出上边缘）。把"跨越"当违�?�?所有内部要素尺寸无解�?
   �?界线相交在制图里是常态（人样上也有），真正的红线�?并行贴近（视觉并成一条粗线）"�?穿过文字"�?
   新版�?
   B13 = 界线在零件内部的长度 �?锚点到最近轮廓边距离 × 1.5 + 10mm（就近穿�?OK，长途穿�?✗）�?
   B14 = 允许相交；禁�?0 < 平行间距 < 2mm(4.76px) 且投影重叠（与其它尺寸的尺寸�?界线）；
   B15 = 界线/尺寸线不得穿过任何尺寸的文字框�?*/

/* 界线线段在盒内的长度（px�?*/
static double ab_seg_internal_len(double ax, double ay, double bx, double by, const ab_rect_t *b)
{
    double dx = bx - ax, dy = by - ay;
    double t0 = 0.0, t1 = 1.0, t, tmp;
    int    i;

    if (b->x1 - b->x0 < 1.0 || b->y1 - b->y0 < 1.0) return 0.0;
    for (i = 0; i < 4; i++) {
        double p, q;

        switch (i) {
        case 0: p = -dx; q = ax - b->x0; break;
        case 1: p =  dx; q = b->x1 - ax; break;
        case 2: p = -dy; q = ay - b->y0; break;
        default: p = dy; q = b->y1 - ay; break;
        }
        if (fabs(p) < 1e-9) { if (q < 0.0) return 0.0; }
        else {
            t = q / p;
            if (p < 0.0) { if (t > t1) return 0.0; if (t > t0) t0 = t; }
            else         { if (t < t0) return 0.0; if (t < t1) t1 = t; }
        }
    }
    tmp = t1 - t0;
    if (tmp <= 0.0) return 0.0;
    return tmp * sqrt(dx * dx + dy * dy);
}

/* 点到矩形最近边界的距离（在矩形内为正距离；在外�?0�?*/
static double ab_pt_rect_out_dist(double x, double y, const ab_rect_t *b)
{
    double d;

    if (x <= b->x0 || x >= b->x1 || y <= b->y0 || y >= b->y1) return 0.0;
    d = x - b->x0;
    if (b->x1 - x < d) d = b->x1 - x;
    if (y - b->y0 < d) d = y - b->y0;
    if (b->y1 - y < d) d = b->y1 - y;
    return d;
}

/* 轮次51 裁定1：锚点到【该界线实际穿出的那条轮廓边】的距离�?
   穿出边由界线方向判定（|dx|>=|dy| �?�?右，否则 �?下），距离按该边所在的轮廓矩形轴计算�?*/
static double ab_exit_edge_dist(double ax, double ay, double bx, double by, const ab_rect_t *outline, const char **edge)
{
    double dx = bx - ax, dy = by - ay;

    if (ax <= outline->x0 || ax >= outline->x1 || ay <= outline->y0 || ay >= outline->y1) {
        *edge = "OUTSIDE";
        return 0.0;
    }
    if (fabs(dx) >= fabs(dy)) {
        if (dx > 0.0) { *edge = "RIGHT"; return outline->x1 - ax; }
        *edge = "LEFT";  return ax - outline->x0;
    }
    if (dy > 0.0) { *edge = "UP";   return outline->y1 - ay; }
    *edge = "DOWN"; return ay - outline->y0;
}

/* 尺寸文字框（B15 用）：每根尺寸一个（含尺寸句�?方向/文字点，�?文字滑动"用） */
#define AB_TBMAX 24
typedef struct {
    double       x0, y0, x1, y1;
    const char  *tag;
    ProDimension nd;
    int          orient;      /* 0=竖尺寸线（文字旋转） 1=横尺寸线（文字横排） */
    int          ownvi;
    int          valid;
    int          exc;         /* 轮次54：受裁定接受�?B15 例外（保文字居中，不滑） */
    double       tx, ty;      /* 文字点（ProDimlocationTextGet�?*/
} ab_tbox_t;
static ab_tbox_t g_abtb[AB_TBMAX];
static int       g_abntb = 0;

/* 轮次54：尺寸矩形间距检查（可跳过符号自己标注的那根尺寸的矩形——DSH 裁定：靠近自身尺寸本来就该允许） */
static double ab_gap_dims_ex(const ab_rect_t *r, const char *skip_tag, const char **who)
{
    double best = 1e9;
    int    i;

    *who = "-";
    for (i = 0; i < g_abndr; i++) {
        double g;

        if (skip_tag && g_abdr[i].tag && strcmp(g_abdr[i].tag, skip_tag) == 0) continue;
        g = ab_rect_gap(r, &g_abdr[i]);
        if (g < best) { best = g; *who = g_abdr[i].tag; }
    }
    return best;
}

static void ab_tbox_add(double x0, double y0, double x1, double y1, const char *tag)
{
    if (g_abntb < AB_TBMAX) {
        memset(&g_abtb[g_abntb], 0, sizeof(g_abtb[g_abntb]));
        g_abtb[g_abntb].x0 = x0; g_abtb[g_abntb].y0 = y0;
        g_abtb[g_abntb].x1 = x1; g_abtb[g_abntb].y1 = y1;
        g_abtb[g_abntb].tag = tag;
        g_abntb++;
    }
}

/* 轮次58：文字沿尺寸线滑动后重登记——同名文本框原地更新（旧框残留会造成�?B15�?*/
static void ab_tbox_replace_for_tag(const char *tag, double x0, double y0, double x1, double y1)
{
    int i;

    for (i = 0; i < g_abntb; i++) {
        if (g_abtb[i].tag && tag && !strcmp(g_abtb[i].tag, tag)) {
            g_abtb[i].x0 = x0; g_abtb[i].y0 = y0;
            g_abtb[i].x1 = x1; g_abtb[i].y1 = y1;
            return;
        }
    }
    ab_tbox_add(x0, y0, x1, y1, tag);
}

static int ab_tbox_hit(double ax, double ay, double bx, double by, const ab_tbox_t *t)
{
    ab_rect_t r;

    r.x0 = t->x0; r.y0 = t->y0; r.x1 = t->x1; r.y1 = t->y1; r.tag = t->tag;
    /* 轮次51：精确求交（采样法对"长线 vs 小文字框"会漏判——实测漏判过一�?B15�?*/
    return ab_seg_internal_len(ax, ay, bx, by, &r) > 0.01;
}

/* 轮次52/59：尺寸显示文本（�?公差）长度——API 只给格式�?{0:@D}，必须自己算�?
   轮次59：改为按 Creo 的实际小数位设置（ProDimensionDecimalsGet / TolerancedecimalsGet�?
   格式化——整数补 .0�? �?"4.0"）；公差按公差小数位。返回字符数，buf 里是文本�?*/
static void ab_trim_zeros(char *s)
{
    char *dot = strchr(s, '.');
    char *e;

    if (!dot) return;
    e = s + strlen(s) - 1;
    while (e > dot && *e == '0') { *e = 0; e--; }
    if (e == dot) *e = 0;
}

static int ab_dim_display_len(ProDimension *nd, char *buf, size_t cap)
{
    double v = 0.0, up = 0.0, lo = 0.0;
    int    dec = 2, tdec = 2;
    int    trim = 0;

    buf[0] = 0;
    if (g_loaded_drw) {
        ProLine z;
        z[0] = 0;
        if (ProDrawingSetupOptionGet((ProDrawing)g_loaded_drw, (wchar_t *)L"lead_trail_zeros", z) == PRO_TK_NO_ERROR &&
            z[0] && wcscmp(z, L"YES") != 0)
            trim = 1;
    }
    if (ProDimensionDecimalsGet(nd, &dec) != PRO_TK_NO_ERROR || dec < 0 || dec > 6) dec = 2;
    if (ProDimensionValueGet(nd, &v) == PRO_TK_NO_ERROR) {
        char t[64];

        snprintf(t, sizeof(t), "%.*f", dec, v);
        if (trim) ab_trim_zeros(t);
        strcpy_s(buf, cap, t);
    }
    /* 轮次53：公差后缀只在"该尺寸确实显示公�?时计入（ToltypeGet 判定�?*/
    {
        ProDimToleranceType tt = PRO_TOL_DEFAULT;

        if (ProDimensionToltypeGet(nd, &tt) == PRO_TK_NO_ERROR &&
            (tt == PRO_TOL_PLUS_MINUS || tt == PRO_TOL_PLUS_MINUS_SYM || tt == PRO_TOL_LIMITS)) {
            if (ProDimensionToleranceGet(nd, &up, &lo) == PRO_TK_NO_ERROR &&
                (fabs(up) > 1e-9 || fabs(lo) > 1e-9)) {
                char t[64];

                if (ProDimensionTolerancedecimalsGet(nd, &tdec) != PRO_TK_NO_ERROR || tdec < 0 || tdec > 6)
                    tdec = 2;
                snprintf(t, sizeof(t), "\xC2\xB1%.*f", tdec, fabs(up));
                if (trim) ab_trim_zeros(t + 2);  /* keep the UTF-8 ± two bytes */
                strncat_s(buf, cap, t, _TRUNCATE);
            }
        }
    }
    return (int)strlen(buf);
}

/* ===== 轮次49（已被轮�?0改写）：线段登记 ===== */
#define AB_SEGMAX 48
typedef struct { double x0, y0, x1, y1; const char *tag; } ab_seg_t;

static ab_seg_t g_abseg[AB_SEGMAX];
static int      g_abnseg = 0;
static int      g_ab_tbox_replace = 0;   /* r58: 1=re-register replaces the same-tag textbox (after a text slide) */
static ab_rect_t g_abblock[8];       /* 拖动失败�?换位置重�?的封锁区 */
static char      g_abblocktag[8][24];
static int       g_abnblock = 0;

/* 轮次49：搜索统计与"最不坏"兜底候�?*/
static int  g_l49_b14 = 0;              /* �?B14 预检否决的候选数 */
static int  g_l49_b15 = 0;              /* �?B15 预检否决的候选数 */
static int  g_l49_bestharm = 99, g_l49_bestL = 0, g_l49_bad_ok = 0, g_l49_bad_dir = 0;
static double g_l49_bad_lx = 0.0, g_l49_bad_ly = 0.0;

static void ab_seg_add(double x0, double y0, double x1, double y1, const char *tag)
{
    if (g_abnseg < AB_SEGMAX) {
        g_abseg[g_abnseg].x0 = x0; g_abseg[g_abnseg].y0 = y0;
        g_abseg[g_abnseg].x1 = x1; g_abseg[g_abnseg].y1 = y1;
        g_abseg[g_abnseg].tag = tag;
        g_abnseg++;
    }
}

static void ab_block_add(double x0, double y0, double x1, double y1, const char *tag)
{
    if (g_abnblock < 8) {
        g_abblock[g_abnblock].x0 = x0; g_abblock[g_abnblock].y0 = y0;
        g_abblock[g_abnblock].x1 = x1; g_abblock[g_abnblock].y1 = y1;
        strcpy_s(g_abblocktag[g_abnblock], 24, tag);
        g_abblock[g_abnblock].tag = g_abblocktag[g_abnblock];
        g_abnblock++;
    }
}

/* 零件内容包围盒（内缩 2mm=4.76px；再�?ab 视图 rect 自带 4px 垫） */
static void ab_content_box(int vi, ab_rect_t *b)
{
    const ab_view_t *v = &g_abv[vi];

    b->x0 = v->rect.x0 + 4.0 + 4.76; b->x1 = v->rect.x1 - 4.0 - 4.76;
    b->y0 = v->rect.y0 + 4.0 + 4.76; b->y1 = v->rect.y1 - 4.0 - 4.76;
    b->tag = v->tag;
}

/* 旧版 ab_seg_breach_box()（跨越轮廓判据）�?ab_seg_cross()（相交判据）已于轮次50删除�?
   前者把所有内部要素尺寸判违规（作废），后者把制图常态的"界线相交"判违规（作废）�?*/

/* 两条近平行线段的间距（投影重叠时）；不平�?不重叠返�?-1 */
static double ab_seg_par_gap(double ax, double ay, double bx, double by,
                             double cx, double cy, double dx, double dy)
{
    double vx = bx - ax, vy = by - ay, wx = dx - cx, wy = dy - cy;
    double la = sqrt(vx * vx + vy * vy), lw = sqrt(wx * wx + wy * wy);
    double cr, dl, u1, u2, c1, c2, lo, hi, ov;

    if (la < 1e-6 || lw < 1e-6) return -1.0;
    cr = (vx * wy - vy * wx) / (la * lw);
    if (fabs(cr) > 0.05) return -1.0;                      /* 不平行（>�?°�?*/
    dl = fabs(vx * (cy - ay) - vy * (cx - ax)) / la;       /* 平行线间�?*/
    u1 = ((cx - ax) * vx + (cy - ay) * vy) / la;
    u2 = ((dx - ax) * vx + (dy - ay) * vy) / la;
    c1 = u1; c2 = u2;
    if (c1 > c2) { double t = c1; c1 = c2; c2 = t; }
    lo = (c1 > 0.0) ? c1 : 0.0;
    hi = (c2 < la) ? c2 : la;
    ov = hi - lo;
    if (ov <= 3.0) return -1.0;                            /* 投影重叠 <3px 视为不相�?*/
    return dl;
}


static void ab_occ_reset(void) { g_abnv = 0; g_abnfurn = 0; g_abndr = 0; g_abnseg = 0; g_abnblock = 0; g_last_nd_valid = 0; }

static void ab_furn_add(double x0, double y0, double x1, double y1, const char *tag)
{
    if (g_abnfurn < 4) {
        g_abfurn[g_abnfurn].x0 = x0; g_abfurn[g_abnfurn].y0 = y0;
        g_abfurn[g_abnfurn].x1 = x1; g_abfurn[g_abnfurn].y1 = y1;
        g_abfurn[g_abnfurn].tag = tag;
        g_abnfurn++;
    }
}

static void ab_dim_add(double x0, double y0, double x1, double y1, const char *tag)
{
    if (g_abndr < AB_DMAX) {
        g_abdr[g_abndr].x0 = x0; g_abdr[g_abndr].y0 = y0;
        g_abdr[g_abndr].x1 = x1; g_abdr[g_abndr].y1 = y1;
        strcpy_s(g_abdrtag[g_abndr], 24, tag);
        g_abdr[g_abndr].tag = g_abdrtag[g_abndr];
        g_abndr++;
    }
}

static double ab_gap_views(const ab_rect_t *r, int ownvi, const char **who)
{
    double best = 1e9;
    int i;

    *who = "-";
    for (i = 0; i < g_abnv; i++) {
        double g;
        if (i == ownvi) continue;
        g = ab_rect_gap(r, &g_abv[i].rect);
        if (g < best) { best = g; *who = g_abv[i].tag; }
    }
    return best;
}

static double ab_gap_furn(const ab_rect_t *r, const char **who)
{
    double best = 1e9;
    int i;

    *who = "-";
    for (i = 0; i < g_abnfurn; i++) {
        double g = ab_rect_gap(r, &g_abfurn[i]);
        if (g < best) { best = g; *who = g_abfurn[i].tag; }
    }
    return best;
}

static double ab_gap_dims(const ab_rect_t *r, const char **who)
{
    double best = 1e9;
    int i;

    *who = "-";
    for (i = 0; i < g_abndr; i++) {
        double g = ab_rect_gap(r, &g_abdr[i]);
        if (g < best) { best = g; *who = g_abdr[i].tag; }
    }
    return best;
}

static int ab_collect_views(ProMdl drw)
{
    ProView *views = NULL;
    int      vc = 0, i;

    g_abnv = 0;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) return 0;
    ProArraySizeGet((ProArray)views, &vc);
    for (i = 0; i < vc && g_abnv < AB_VMAX; i++) {
        ProName    nm;
        ProPoint3d out[2];
        double     sc = 1.0;

        nm[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
        out[0][0] = out[0][1] = out[0][2] = -9999.0;
        out[1][0] = out[1][1] = out[1][2] = -9999.0;
        ProDrawingViewOutlineGet((ProDrawing)drw, views[i], out);
        if (ProDrawingViewScaleGet((ProDrawing)drw, views[i], &sc) != PRO_TK_NO_ERROR || sc <= 0.0) sc = 1.0;
        {
            ab_view_t *v = &g_abv[g_abnv];
            char       n8[64];
            double     sx = (g_abbb.x1 - g_abbb.x0) > 0.01 ? (g_abbb.x1 - g_abbb.x0) : 1.0;
            double     sy = (g_abbb.y1 - g_abbb.y0) > 0.01 ? (g_abbb.y1 - g_abbb.y0) : 1.0;
            double     sz = (g_abbb.z1 - g_abbb.z0) > 0.01 ? (g_abbb.z1 - g_abbb.z0) : 1.0;

            memset(v, 0, sizeof(*v));
            v->v = views[i];
            wide_to_u8(nm, n8, sizeof(n8));
            sprintf_s(v->tag, sizeof(v->tag), "%s", n8);
            v->rect.x0 = out[0][0] - 4.0; v->rect.y0 = out[0][1] - 4.0;
            v->rect.x1 = out[1][0] + 4.0; v->rect.y1 = out[1][1] + 4.0;
            v->rect.tag = v->tag;
            v->ocx = (out[0][0] + out[1][0]) / 2.0;
            v->ocy = (out[0][1] + out[1][1]) / 2.0;
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) {
                v->isfront = 1;
                v->kx = (out[1][0] - out[0][0]) / sx;
                v->ky = (out[1][1] - out[0][1]) / sy;
            } else if (wcsstr(nm, L"\u5DE6\u4FA7")) {
                v->isside = 1;
                v->kx = (out[1][0] - out[0][0]) / sz;
                v->ky = (out[1][1] - out[0][1]) / sy;
            } else if (wcsstr(nm, L"\u9876\u90E8") || wcsstr(nm, L"top") || wcsstr(nm, L"TOP")) {
                v->istop = 1;
                v->kx = (out[1][0] - out[0][0]) / sx;
                v->ky = (out[1][1] - out[0][1]) / sz;
            }
            logf_("[auto] view '%s' rect x[%.1f..%.1f] y[%.1f..%.1f] center(%.1f,%.1f) kx=%.3f ky=%.3f",
                  n8, v->rect.x0, v->rect.x1, v->rect.y0, v->rect.y1, v->ocx, v->ocy, v->kx, v->ky);
            g_abnv++;
        }
    }
    ProArrayFree((ProArray *)&views);
    return g_abnv;
}

/* 位置搜索：orient 0=竖排(方向=左右) 1=横排(方向=上下)；gx/gy=几何投影范围（px�?*/
typedef struct { int orient, dir1, dir2, ownvi; double gx0, gy0, gx1, gy1; } ab_req_t;

static void ab_search(const ab_req_t *q, double *olx, double *oly, int *ok)
{
    static const double LAY[11] = { 12.0, 18.0, 26.0, 36.0, 48.0, 62.0, 80.0, 96.0, 110.0, 130.0, 155.0 };
    const ab_view_t  *own = &g_abv[q->ownvi];
    double best = -1e9;
    int    bd = 0, bl = 0, d, L;

    *ok = 0; *olx = 0; *oly = 0;
    g_l49_bad_ok = 0; g_l49_bestharm = 99; g_l49_b14 = 0;
    for (d = 0; d < 2; d++) {
        int dir = (d == 0) ? q->dir1 : q->dir2;

        if (dir == 0) continue;
        for (L = 0; L < 11; L++) {
            double lx, ly, gv = -1.0, gf = -1.0, gd = -1.0, score;
            const char *wv = "-", *wf = "-", *wd = "-";
            ab_rect_t  lr;
            int        valid = 1;
            char       why[80];

            why[0] = 0;
            if (q->orient == 0) {
                lx = (dir > 0) ? own->rect.x1 + LAY[L] : own->rect.x0 - LAY[L];
                ly = (q->gy0 + q->gy1) / 2.0;
                lr.x0 = lx - 7.0; lr.x1 = lx + 7.0;
                lr.y0 = q->gy0 - 8.0; lr.y1 = q->gy1 + 8.0;
            } else {
                ly = (dir > 0) ? own->rect.y1 + LAY[L] : own->rect.y0 - LAY[L];
                lx = (q->gx0 + q->gx1) / 2.0;
                lr.x0 = q->gx0 - 8.0; lr.x1 = q->gx1 + 8.0;
                lr.y0 = ly - 7.0; lr.y1 = ly + 7.0;
            }
            lr.tag = "cand";
            /* 轮次48：内容须在内框内�?10mm 的区域内（内框实�?x[59.5..987.8] y[78.7..762]�?0mm=23.8px�?*/
            if (lr.x0 < 83.3 || lr.x1 > 964.0 || lr.y0 < 102.5 || lr.y1 > 738.2) {
                valid = 0; sprintf_s(why, sizeof(why), "off-sheet(margin10mm)");
            }
            if (valid) {
                gv = ab_gap_views(&lr, q->ownvi, &wv);
                if (gv < 6.0) { valid = 0; sprintf_s(why, sizeof(why), "close to view '%s' gap=%.1f<6", wv, gv); }
            }
            if (valid) {
                gf = ab_gap_furn(&lr, &wf);
                if (gf < 6.0) { valid = 0; sprintf_s(why, sizeof(why), "close to '%s' gap=%.1f<6", wf, gf); }
            }
            if (valid) {
                double need = (q->orient == 1) ? 12.0 : 24.0;   /* 轮次47：竖排矩形gap24 �?线间距≥~52px�?.65/23.5 必须明显分开�?*/

                gd = ab_gap_dims(&lr, &wd);
                if (gd < need) { valid = 0; sprintf_s(why, sizeof(why), "close to dim '%s' gap=%.1f<%.0f", wd, gd, need); }
            }
            if (valid) {   /* 界线预检：从近边沿轴两端到尺寸线，不得穿其它视图 */
                int i;

                for (i = 0; i < g_abnv && valid; i++) {
                    if (i == q->ownvi) continue;
                    if (q->orient == 0) {
                        double sx = (dir > 0) ? own->rect.x1 : own->rect.x0;

                        if (ab_seg_hits_rect(sx, q->gy0, lx, q->gy0, &g_abv[i].rect) ||
                            ab_seg_hits_rect(sx, q->gy1, lx, q->gy1, &g_abv[i].rect)) {
                            valid = 0; sprintf_s(why, sizeof(why), "witness crosses view '%s'", g_abv[i].tag);
                        }
                    } else {
                        double sy = (dir > 0) ? own->rect.y1 : own->rect.y0;

                        if (ab_seg_hits_rect(q->gx0, sy, q->gx0, ly, &g_abv[i].rect) ||
                            ab_seg_hits_rect(q->gx1, sy, q->gx1, ly, &g_abv[i].rect)) {
                            valid = 0; sprintf_s(why, sizeof(why), "witness crosses view '%s'", g_abv[i].tag);
                        }
                    }
                }
            }
            if (valid) {   /* 轮次50 B14 预检：预测界线与已登记尺寸线/界线"并行且间�?2mm"(4.76px) �?否决（相交允许） */
                double wx0s, wy0s, wx1s, wy1s, wx0e, wy0e, wx1e, wy1e;
                int    i;

                if (q->orient == 0) {
                    double sx = (dir > 0) ? own->rect.x1 : own->rect.x0;
                    wx0s = sx;   wy0s = q->gy0; wx1s = lx;   wy1s = q->gy0;
                    wx0e = sx;   wy0e = q->gy1; wx1e = lx;   wy1e = q->gy1;
                } else {
                    double sy = (dir > 0) ? own->rect.y1 : own->rect.y0;
                    wx0s = q->gx0; wy0s = sy;   wx1s = q->gx0; wy1s = ly;
                    wx0e = q->gx1; wy0e = sy;   wx1e = q->gx1; wy1e = ly;
                }
                for (i = 0; i < g_abnseg; i++) {
                    double g1 = ab_seg_par_gap(wx0s, wy0s, wx1s, wy1s, g_abseg[i].x0, g_abseg[i].y0, g_abseg[i].x1, g_abseg[i].y1);
                    double g2 = ab_seg_par_gap(wx0e, wy0e, wx1e, wy1e, g_abseg[i].x0, g_abseg[i].y0, g_abseg[i].x1, g_abseg[i].y1);

                    if ((g1 > 0.05 && g1 < 4.76) || (g2 > 0.05 && g2 < 4.76)) {
                        double gg = (g1 > 0.05 && g1 < 4.76) ? g1 : g2;

                        valid = 0; g_l49_b14++;
                        sprintf_s(why, sizeof(why), "B14: parallel %.2fmm(<2mm) with '%s'", gg / 2.3805, g_abseg[i].tag);
                        break;
                    }
                }
            }
            if (valid) {   /* 轮次50 B15 预检：预测界�?尺寸线不得穿过任何尺寸文字框 */
                double wx0s, wy0s, wx1s, wy1s, wx0e, wy0e, wx1e, wy1e, sp0x, sp0y, sp1x, sp1y;
                int    i;

                if (q->orient == 0) {
                    double sx = (dir > 0) ? own->rect.x1 : own->rect.x0;
                    wx0s = sx;   wy0s = q->gy0; wx1s = lx;   wy1s = q->gy0;
                    wx0e = sx;   wy0e = q->gy1; wx1e = lx;   wy1e = q->gy1;
                    sp0x = lx;   sp0y = q->gy0; sp1x = lx;   sp1y = q->gy1;
                } else {
                    double sy = (dir > 0) ? own->rect.y1 : own->rect.y0;
                    wx0s = q->gx0; wy0s = sy;   wx1s = q->gx0; wy1s = ly;
                    wx0e = q->gx1; wy0e = sy;   wx1e = q->gx1; wy1e = ly;
                    sp0x = q->gx0; sp0y = ly;   sp1x = q->gx1; sp1y = ly;
                }
                for (i = 0; i < g_abntb; i++) {
                    if (ab_tbox_hit(wx0s, wy0s, wx1s, wy1s, &g_abtb[i]) ||
                        ab_tbox_hit(wx0e, wy0e, wx1e, wy1e, &g_abtb[i]) ||
                        ab_tbox_hit(sp0x, sp0y, sp1x, sp1y, &g_abtb[i])) {
                        valid = 0; g_l49_b15++;
                        sprintf_s(why, sizeof(why), "B15: crosses text box of '%s'", g_abtb[i].tag);
                        break;
                    }
                }
            }
            if (valid) {   /* 轮次49：不得落�?换位置重�?的封锁区（拖动无效时的兜底） */
                int i;

                for (i = 0; i < g_abnblock; i++) {
                    if (ab_rect_gap(&lr, &g_abblock[i]) < 6.0) {
                        valid = 0; sprintf_s(why, sizeof(why), "blocked by '%s'", g_abblock[i].tag);
                        break;
                    }
                }
            }
            score = valid ? ((gv < gf ? gv : gf) * 0.2 - LAY[L]) : -1e9;   /* 罚项=实际层距像素（防远推�?*/
            logf_("[pos] cand dir=%s layer=%.0f loc=(%.1f,%.1f) gapV=%.1f gapF=%.1f gapD=%.1f valid=%d %s score=%.2f",
                  (q->orient == 0) ? (dir > 0 ? "RIGHT" : "LEFT") : (dir > 0 ? "UP" : "DOWN"),
                  LAY[L], lx, ly, gv, gf, gd, valid, why[0] ? why : "-", score);
            if (valid && score > best) { best = score; bd = dir; bl = L; *olx = lx; *oly = ly; *ok = 1; }
            if (!valid && why[0] && !strstr(why, "off-sheet") && !strstr(why, "blocked")) {
                /* 轮次49：收�?最不坏"兜底候选（无合法候选时用，等级：非B14 < B14 < 视野/家具/间距�?*/
                int harm = 2;

                if (strstr(why, "B14")) harm = 1;
                else if (strstr(why, "B15")) harm = 2;
                else if (strstr(why, "crosses view")) harm = 5;
                else if (strstr(why, "close to")) harm = 3;
                if (harm < g_l49_bestharm || (harm == g_l49_bestharm && LAY[L] < g_l49_bestL)) {
                    g_l49_bestharm = harm; g_l49_bestL = L;
                    g_l49_bad_dir = dir; g_l49_bad_lx = lx; g_l49_bad_ly = ly;
                    g_l49_bad_ok = 1;
                }
            }
        }
    }
    if (*ok)
        logf_("[pos] CHOSEN dir=%s layer=%.0f loc=(%.1f,%.1f) score=%.2f (B14-vetoed cands=%d)",
              (q->orient == 0) ? (bd > 0 ? "RIGHT" : "LEFT") : (bd > 0 ? "UP" : "DOWN"), LAY[bl], *olx, *oly, best,
              g_l49_b14);
    else if (g_l49_bad_ok) {
        /* 无合法候�?�?兜底：取最不坏候选并打待裁定标记（如实报告，不弄丢尺寸） */
        *olx = g_l49_bad_lx; *oly = g_l49_bad_ly; *ok = 2;
        logf_("[pos] *** NO valid candidate *** -> FALLBACK (least-bad) dir=%s layer=%.0f loc=(%.1f,%.1f) harm=%d [先放最不坏候选，B15 �?b15pass]",
              (q->orient == 0) ? (g_l49_bad_dir > 0 ? "RIGHT" : "LEFT") : (g_l49_bad_dir > 0 ? "UP" : "DOWN"),
              LAY[g_l49_bestL], *olx, *oly, g_l49_bestharm);
    } else
        logf_("[pos] *** NO valid candidate ***");
}

/* 改造C：界线读回验�?*/
static ProError ab_verify_cb(ProDimension *dim, ProError status, ProAppData app)
{
    ab_vctx_t *c = (ab_vctx_t *)app;
    double     v = 0.0;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    {
        ProDimlocation loc = NULL;

        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc) {
            ProPoint3d a1 = { -9999.0, -9999.0, -9999.0 }, a2 = { -9999.0, -9999.0, -9999.0 };
            ProPoint3d w1 = { -9999.0, -9999.0, -9999.0 }, w2 = { -9999.0, -9999.0, -9999.0 };
            ProError   ea = ProDimlocationArrowsGet(loc, a1, a2);
            ProError   ew = ProDimlocationWitnesslinesGet(loc, w1, w2);
            const ab_rect_t *own = &g_abv[c->ownvi].rect;
            double     d1 = ab_pt_rect_dist(w1[0], w1[1], own);
            double     d2 = ab_pt_rect_dist(w2[0], w2[1], own);
            int        nearok = (d1 <= 120.0 && d2 <= 120.0);
            int        cross = 0, i;
            const char *crosswho = "none";

            for (i = 0; i < g_abnv; i++) {
                if (i == c->ownvi) continue;
                if (ab_seg_hits_rect(w1[0], w1[1], a1[0], a1[1], &g_abv[i].rect) ||
                    ab_seg_hits_rect(w2[0], w2[1], a2[0], a2[1], &g_abv[i].rect)) {
                    cross = 1; crosswho = g_abv[i].tag; break;
                }
            }
            logf_("[verify] v=%.4f id=%d arrows(%d) witns(%d) w1=(%.1f,%.1f) w2=(%.1f,%.1f) nearOwnView=%.1f/%.1f px [%s]; witnessCrossOther=%d(%s)",
                  v, (int)dim->id, (int)ea, (int)ew, w1[0], w1[1], w2[0], w2[1], d1, d2,
                  nearok ? "PASS" : "*** FAIL ***", cross, crosswho);
            c->found++;
        }
    }
    return PRO_TK_NO_ERROR;
}

static void ab_verify_dim(ProMdl drw, double target, int ownvi)
{
    ab_vctx_t c;

    memset(&c, 0, sizeof(c));
    c.target = target; c.drw = (ProDrawing)drw; c.ownvi = ownvi;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, ab_verify_cb, NULL, (ProAppData)&c);
    if (!c.found) logf_("[verify] *** value %.4f not found (nothing created?) ***", target);
}

/* 直建（与 convdim 同款已验证结构；�?tol/olddim�?*/
static void ab_auto_create(ProDrawing drw, ProSelection sA, ProSelection sB, int sensekind,
                           ProDimOrient orienth, double lx, double ly, ProView view, const char *tag)
{
    ProDimAttachment *arr = NULL;
    ProDimSense      *sen = NULL;
    ProDimension      nd;
    ProVector         loc;

    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(sA, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(sB, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    if (sensekind == 1) {
        sen[0].type = PRO_DIM_SNS_TYP_PNT; sen[0].sense = PRO_POINT_TYP_CENTER; sen[0].orient_hint = orienth;
        sen[1].type = PRO_DIM_SNS_TYP_PNT; sen[1].sense = PRO_POINT_TYP_CENTER; sen[1].orient_hint = PRO_DIM_ORNT_NONE;
    } else {
        sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = orienth;
        sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = orienth;
    }
    loc[0] = lx; loc[1] = ly; loc[2] = 0.0;
    memset(&nd, 0, sizeof(nd));
    {
        ProError e = ProDrawingDimensionCreate(drw, arr, sen, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &nd);

        logf_("[auto] %s create -> %d (%s)", tag, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            double nv = -1.0;

            if (ProDimensionValueGet(&nd, &nv) == PRO_TK_NO_ERROR)
                logf_("[auto] %s *** Creo computed value = %.4f ***", tag, nv);
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, view);

                logf_("[auto] %s show -> %d (%s)", tag, (int)es, errname(es));
            }
            /* 轮次49：登记最后创建的尺寸句柄（供 ab_dim_fixup 拖动/复检�?*/
            g_last_nd = nd;
            g_last_nd_valid = 1;
            g_last_nd_value = (nv > -0.5) ? nv : -1.0;
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
}

/* —�?63.6：意�?= 零件左右�?最长的合法竖边"（每候选打印合法性判定，含辅助构造线的排除） —�?*/
static void ab_dim_settle(ProMdl drw, double value, const char *name, ab_req_t *q, double *lx, double *ly);  /* r49 forward decl */
static void ab_dim_636(ProMdl part, ProMdl drw, int ownvi)
{
    ab_edge_t le[128], re[128];
    int       nl = 0, nr = 0, hid, selL = -1, selR = -1, i;

    logf_("[auto] ==== dim 63.6 (direct; intent = leftmost/rightmost tallest LEGAL vertical edge) ====");
    for (hid = 1; hid <= 3000; hid++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        if (gd->data.p_curve_data->line.type == PRO_ENT_LINE) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double       p1[3], p2[3], dx, dy, dz, len, mx;

            p1[0] = ld->end1[0]; p1[1] = ld->end1[1]; p1[2] = ld->end1[2];
            p2[0] = ld->end2[0]; p2[1] = ld->end2[1]; p2[2] = ld->end2[2];
            dx = p2[0] - p1[0]; dy = p2[1] - p1[1]; dz = p2[2] - p1[2];
            len = sqrt(dx * dx + dy * dy + dz * dz);
            mx = (p1[0] + p2[0]) / 2.0;
            if (fabs(mx - g_abbb.x0) < 0.7 || fabs(mx - g_abbb.x1) < 0.7) {
                char why[128];
                int  lg = ab_legal_edge(&g_abbb, p1, p2, len, why, sizeof(why));
                int  vert = (fabs(dx) < 0.3 && fabs(dy) > 0.5);

                logf_("[legal] 636-cand edge %d cx=%.2f len=%.1f vert=%d -> %s (%s)",
                      hid, mx, len, vert, lg ? "OK" : "REJECT", why);
                if (lg && vert) {
                    ab_edge_t *t;
                    int       *nt;

                    if (fabs(mx - g_abbb.x0) < 0.7) { t = &le[nl]; nt = &nl; }
                    else                              { t = &re[nr]; nt = &nr; }
                    if (*nt < 128) {
                        memset(t, 0, sizeof(*t));
                        t->id = hid;
                        memcpy(t->e1, p1, sizeof(p1)); memcpy(t->e2, p2, sizeof(p2));
                        t->len = len; t->mx = mx;
                        t->my = (p1[1] + p2[1]) / 2.0; t->mz = (p1[2] + p2[2]) / 2.0;
                        t->topy = (p1[1] > p2[1]) ? p1[1] : p2[1];
                        t->boty = (p1[1] > p2[1]) ? p2[1] : p1[1];
                        (*nt)++;
                    }
                }
            }
        }
        if (gd) ProGeomitemdataFree(&gd);
    }
    logf_("[sel636] legal verticals in end clusters: left=%d right=%d", nl, nr);
    if (!nl || !nr) { logf_("[auto] 636: legal edges missing; SKIP"); return; }
    {
        int b = 0;

        for (i = 1; i < nl; i++) if (le[i].len > le[b].len + 1e-9) b = i;
        selL = b;
        b = 0;
        {
            double bdz = 1e9;

            for (i = 0; i < nr; i++) {
                double dz = fabs(re[i].mz - le[selL].mz);

                if (re[i].len > re[b].len + 1e-9 ||
                    (fabs(re[i].len - re[b].len) <= 1e-9 && dz < bdz)) { b = i; bdz = dz; }
            }
        }
        selR = b;
    }
    logf_("[sel636] picked L=edge %d (x=%.2f len=%.1f z=%.2f) R=edge %d (x=%.2f len=%.1f z=%.2f)",
          le[selL].id, le[selL].mx, le[selL].len, le[selL].mz,
          re[selR].id, re[selR].mx, re[selR].len, re[selR].mz);
    {
        const ab_view_t *fv = &g_abv[ownvi];
        double  bcx = (g_abbb.x0 + g_abbb.x1) / 2.0, bcy = (g_abbb.y0 + g_abbb.y1) / 2.0;
        double  pxL = fv->ocx + (le[selL].mx - bcx) * fv->kx;
        double  pxR = fv->ocx + (re[selR].mx - bcx) * fv->kx;
        double  pyT = fv->ocy + (le[selL].topy - bcy) * fv->ky;
        ab_req_t q;
        double  lx, ly;
        int     ok;
        ProSelection s1 = NULL, s2 = NULL;

        memset(&q, 0, sizeof(q));
        q.orient = 1; q.dir1 = +1; q.dir2 = 0; q.ownvi = ownvi;   /* 界线锚点在零件顶缘，只允许上�?*/
        q.gx0 = pxL; q.gx1 = pxR; q.gy0 = pyT; q.gy1 = pyT;
        logf_("[sel636] projected px L=%.1f R=%.1f topY=%.1f", pxL, pxR, pyT);
        ab_search(&q, &lx, &ly, &ok);
        if (!ok) { logf_("[auto] 636: no position; SKIP"); return; }
        {
            ProEdge e1 = NULL, e2 = NULL;
            ProGeomitem g1, g2;

            ProEdgeInit((ProSolid)part, le[selL].id, &e1);
            ProEdgeInit((ProSolid)part, re[selR].id, &e2);
            if (!e1 || !e2 ||
                ProEdgeToGeomitem((ProSolid)part, e1, &g1) != PRO_TK_NO_ERROR ||
                ProEdgeToGeomitem((ProSolid)part, e2, &g2) != PRO_TK_NO_ERROR) {
                logf_("[auto] 636: geomitem failed; SKIP");
                return;
            }
            ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1);
            ProSelectionViewSet(fv->v, &s1);
            ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2);
            ProSelectionViewSet(fv->v, &s2);
        }
        g_last_nd_valid = 0;
        ab_auto_create((ProDrawing)drw, s1, s2, 0, PRO_DIM_ORNT_HORIZ, lx, ly, fv->v, "636");
        ab_verify_dim(drw, 63.6, ownvi);
        ab_dim_settle(drw, 63.6, "dim636", &q, &lx, &ly);   /* 轮次49：B13/B14 复检+拖动+换位 */
        ab_dim_add(q.gx0 - 8.0, ly - 7.0, q.gx1 + 8.0, ly + 7.0, "dim636");
        ab_dim_add(pxL - 3.0, pyT - 6.0, pxR + 3.0, ly + 8.0, "dim636W");
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
    }
}

/* —�?41.1：意�?= bbox 上下极值的合法 ±y 平面（面附着点实测约�?56.85 列，预检按保守取右缘�?—�?*/
static void ab_dim_411(ProMdl part, ProMdl drw, int ownvi)
{
    static conv_fplane_t FP[400];
    int    nf, i, selA = -1, selB = -1;
    double offA = 0.0, offB = 0.0;

    logf_("[auto] ==== dim 41.1 (direct; intent = legal +/-y planes at bbox Y extremes) ====");
    nf = conv_faces_collect(part, FP, 400);
    for (i = 0; i < nf; i++) {
        double off, ay;
        char   why[128];
        int    lg;

        if (fabs(FP[i].ny) < 0.99) continue;
        off = FP[i].ox * FP[i].nx + FP[i].oy * FP[i].ny + FP[i].oz * FP[i].nz;
        ay = (FP[i].ny > 0.0) ? off : -off;   /* 该面所�?y �?*/
        lg = ab_legal_center(&g_abbb, FP[i].ox, FP[i].oy, FP[i].oz, why, sizeof(why));
        logf_("[legal] 411-cand face %d y=%.2f n=(%.2f,%.2f,%.2f) -> %s (%s)",
              FP[i].id, ay, FP[i].nx, FP[i].ny, FP[i].nz, lg ? "OK" : "REJECT", why);
        if (!lg) continue;
        if (fabs(ay - g_abbb.y0) < 0.8 && (selA < 0 || fabs(ay - g_abbb.y0) < fabs(offA - g_abbb.y0))) { selA = i; offA = ay; }
        if (fabs(ay - g_abbb.y1) < 0.8 && (selB < 0 || fabs(ay - g_abbb.y1) < fabs(offB - g_abbb.y1))) { selB = i; offB = ay; }
    }
    if (selA < 0 || selB < 0) { logf_("[auto] 411: extreme faces missing (bottom=%d top=%d); SKIP", selA, selB); return; }
    logf_("[sel411] picked bottom face %d (y=%.2f) top face %d (y=%.2f)", FP[selA].id, offA, FP[selB].id, offB);
    {
        const ab_view_t *fv = &g_abv[ownvi];
        ab_req_t q;
        double   lx, ly;
        int      ok;
        ProSelection s1 = NULL, s2 = NULL;

        memset(&q, 0, sizeof(q));
        q.orient = 0; q.dir1 = +1; q.dir2 = 0; q.ownvi = ownvi;   /* 面附着点在视图右侧内（440 列），只允许右侧 */
        q.gy0 = fv->rect.y0 + 4.0; q.gy1 = fv->rect.y1 - 4.0;
        q.gx0 = fv->rect.x0; q.gx1 = fv->rect.x1;
        ab_search(&q, &lx, &ly, &ok);
        if (!ok) { logf_("[auto] 411: no position; SKIP"); return; }
        {
            ProSurface sa = NULL, sb = NULL;
            ProGeomitem g1, g2;

            if (ProSurfaceInit(part, FP[selA].id, &sa) != PRO_TK_NO_ERROR || !sa) { logf_("[auto] 411: surfA init fail"); return; }
            if (ProSurfaceInit(part, FP[selB].id, &sb) != PRO_TK_NO_ERROR || !sb) { logf_("[auto] 411: surfB init fail"); return; }
            if (ProSurfaceToGeomitem((ProSolid)part, sa, &g1) != PRO_TK_NO_ERROR ||
                ProSurfaceToGeomitem((ProSolid)part, sb, &g2) != PRO_TK_NO_ERROR) { logf_("[auto] 411: ToGeomitem fail"); return; }
            ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1);
            ProSelectionViewSet(fv->v, &s1);
            ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2);
            ProSelectionViewSet(fv->v, &s2);
        }
        g_last_nd_valid = 0;
        ab_auto_create((ProDrawing)drw, s1, s2, 0, PRO_DIM_ORNT_VERT, lx, ly, fv->v, "411");
        ab_verify_dim(drw, 41.1, ownvi);
        ab_dim_settle(drw, 41.1, "dim411", &q, &lx, &ly);   /* 轮次49：B13/B14 复检+拖动+换位 */
        ab_dim_add(lx - 7.0, q.gy0 - 8.0, lx + 7.0, q.gy1 + 8.0, "dim411");
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
    }
}

/* —�?转换类尺寸（23/9.65/23.5/2/0.25）：几何判定�?convdim 候选清单（�?[legal] 行）�?
      位置由搜索给出后调用 convdim_run（转�?隐藏逻辑不变�?—�?*/
/* ===== 轮次50：放置后复检（B13 内部长度阈�?/ B14 并行<2mm / B15 穿文字）�?拖动 �?重检 =====
   返回�?=通过（未动）�?=拖动后通过�?=拖动无效（界线不跟随）；3=到上限仍违规（回退原位�?*/
static int ab_dim_fixup(ProMdl drw, ProDimension *nd, double value, const char *name,
                        int ownvi, int orient, int dirsign)
{
    ab_rect_t cb;
    ab_rect_t outline;                 /* 真实轮廓（视�?outline，不�?4px 垫） */
    int       step, rc = 3;
    double    rtp0 = 0.0, rtp1 = 0.0;
    int       have_tp = 0;

    (void)value;
    if (ownvi < 0 || ownvi >= g_abnv) return 0;
    ab_content_box(ownvi, &cb);
    outline.x0 = g_abv[ownvi].rect.x0 + 4.0; outline.x1 = g_abv[ownvi].rect.x1 - 4.0;
    outline.y0 = g_abv[ownvi].rect.y0 + 4.0; outline.y1 = g_abv[ownvi].rect.y1 - 4.0;
    outline.tag = g_abv[ownvi].tag;
    for (step = 0; step <= 2; step++) {
        ProDimlocation loc = NULL;
        ProPoint3d a1 = { 0, 0, 0 }, a2 = { 0, 0, 0 }, w1 = { 0, 0, 0 }, w2 = { 0, 0, 0 }, tp = { 0, 0, 0 };
        ProBoolean heb = PRO_B_FALSE;
        double     ebl = 0.0;
        int        b13a = 0, b13b = 0, b14 = 0, b15 = 0, i;
        const char *edge1 = "-", *edge2 = "-";
        double     parmin = 1e9;
        double     inlen1 = 0.0, inlen2 = 0.0, danc1 = 0.0, danc2 = 0.0, allow1 = 0.0, allow2 = 0.0;
        double     sx0, sy0, sx1, sy1, vx0, vy0, vx1, vy1, ux0, uy0, ux1, uy1;

        if (ProDimensionLocationGet(nd, NULL, (ProDrawing)drw, &loc) != PRO_TK_NO_ERROR || !loc) {
            logf_("[chk50] %s: loc get failed", name);
            return 3;
        }
        ProDimlocationArrowsGet(loc, a1, a2);
        ProDimlocationWitnesslinesGet(loc, w1, w2);
        ProDimlocationTextGet(loc, &heb, tp, &ebl);
        if (!have_tp) { rtp0 = tp[0]; rtp1 = tp[1]; have_tp = 1; }
        if (orient == 0) {
            sx0 = a1[0]; sy0 = (a1[1] < a2[1]) ? a1[1] : a2[1];
            sx1 = a1[0]; sy1 = (a1[1] < a2[1]) ? a2[1] : a1[1];
            vx0 = w1[0]; vy0 = w1[1]; vx1 = a1[0]; vy1 = w1[1];
            ux0 = w2[0]; uy0 = w2[1]; ux1 = a1[0]; uy1 = w2[1];
        } else {
            sx0 = (a1[0] < a2[0]) ? a1[0] : a2[0]; sy0 = a1[1];
            sx1 = (a1[0] < a2[0]) ? a2[0] : a1[0]; sy1 = a1[1];
            vx0 = w1[0]; vy0 = w1[1]; vx1 = w1[0]; vy1 = a1[1];
            ux0 = w2[0]; uy0 = w2[1]; ux1 = w2[0]; uy1 = a2[1];
        }
        /* B13（轮�?1 裁定1）：界线在零件内部的长度 �?锚点到【实际穿出边】距�?× 1.5 + 10mm */
        inlen1 = ab_seg_internal_len(vx0, vy0, vx1, vy1, &cb);
        inlen2 = ab_seg_internal_len(ux0, uy0, ux1, uy1, &cb);
        danc1 = ab_exit_edge_dist(vx0, vy0, vx1, vy1, &outline, &edge1);
        danc2 = ab_exit_edge_dist(ux0, uy0, ux1, uy1, &outline, &edge2);
        allow1 = danc1 * 1.5 + 23.805;
        allow2 = danc2 * 1.5 + 23.805;
        b13a = (inlen1 > allow1 + 0.01);
        b13b = (inlen2 > allow2 + 0.01);
        /* B14（报告项�?.89mm 并行已由 DSH �?1 裁定接受；相交允许） */
        for (i = 0; i < g_abnseg; i++) {
            const ab_seg_t *s = &g_abseg[i];
            double          g;

            g = ab_seg_par_gap(vx0, vy0, vx1, vy1, s->x0, s->y0, s->x1, s->y1);
            if (g >= 0.0 && g < parmin) parmin = g;
            if (g > 0.05 && g < 4.76) { logf_("[chk51] %s B14(报告): wit1 parallel %.2fmm(<2mm) with '%s'", name, g / 2.3805, s->tag); }
            g = ab_seg_par_gap(ux0, uy0, ux1, uy1, s->x0, s->y0, s->x1, s->y1);
            if (g >= 0.0 && g < parmin) parmin = g;
            if (g > 0.05 && g < 4.76) { logf_("[chk51] %s B14(报告): wit2 parallel %.2fmm(<2mm) with '%s'", name, g / 2.3805, s->tag); }
        }
        /* B15：界�?尺寸线不得穿过任何尺寸文字框 */
        for (i = 0; i < g_abntb; i++) {
            if (ab_tbox_hit(vx0, vy0, vx1, vy1, &g_abtb[i]) ||
                ab_tbox_hit(ux0, uy0, ux1, uy1, &g_abtb[i]) ||
                ab_tbox_hit(sx0, sy0, sx1, sy1, &g_abtb[i])) {
                b15 = 1; logf_("[chk51] %s B15: crosses text box of '%s'", name, g_abtb[i].tag);
            }
        }
        logf_("[chk50] %s step=%d spine (%.1f,%.1f)-(%.1f,%.1f) wit1 (%.1f,%.1f)-(%.1f,%.1f) wit2 (%.1f,%.1f)-(%.1f,%.1f)",
              name, step, sx0, sy0, sx1, sy1, vx0, vy0, vx1, vy1, ux0, uy0, ux1, uy1);
        logf_("[chk51] %s step=%d internal-len: wit1=%.1fpx(%.1fmm, allow %.1fmm, exit=%s dist %.1fpx)%s  wit2=%.1fpx(%.1fmm, allow %.1fmm, exit=%s dist %.1fpx)%s  B14 minPar=%.2fmm(report)  B15=%d",
              name, step,
              inlen1, inlen1 / 2.3805, allow1 / 2.3805, edge1, danc1, b13a ? " X-B13" : " ok",
              inlen2, inlen2 / 2.3805, allow2 / 2.3805, edge2, danc2, b13b ? " X-B13" : " ok",
              (parmin < 1e8) ? parmin / 2.3805 : -1.0, b15);
        {
            int viol = b13a || b13b || b15;

            if (!viol) { rc = (step == 0) ? 0 : 1; return rc; }
            if (step == 2) { rc = 3; break; }
            {
                ProVector np;
                ProError  em;

                np[0] = tp[0] + ((orient == 0) ? (double)dirsign * 12.0 : 0.0);
                np[1] = tp[1] + ((orient == 1) ? (double)dirsign * 12.0 : 0.0);
                np[2] = tp[2];
                em = ProDrawingDimensionMove((ProDrawing)drw, nd, np);
                logf_("[chk50] %s drag step=%d: text (%.1f,%.1f) -> (%.1f,%.1f) move -> %d (%s)",
                      name, step + 1, tp[0], tp[1], np[0], np[1], (int)em, errname(em));
            }
        }
    }
    if (rc == 3) {   /* 到上限仍违规：回退到候选原位（保持可预测） */
        ProDimlocation loc = NULL;
        ProPoint3d    tp2 = { 0, 0, 0 };
        ProBoolean    hb = PRO_B_FALSE;
        double        el = 0.0;

        if (have_tp && ProDimensionLocationGet(nd, NULL, (ProDrawing)drw, &loc) == PRO_TK_NO_ERROR && loc &&
            ProDimlocationTextGet(loc, &hb, tp2, &el) == PRO_TK_NO_ERROR) {
            ProVector rv;
            double    dx = rtp0 - tp2[0], dy = rtp1 - tp2[1];

            if (fabs(dx) > 0.2 || fabs(dy) > 0.2) {
                rv[0] = rtp0; rv[1] = rtp1; rv[2] = tp2[2];
                logf_("[chk50] %s revert drag: (%.1f,%.1f) -> (%.1f,%.1f)", name, tp2[0], tp2[1], rtp0, rtp1);
                ProDrawingDimensionMove((ProDrawing)drw, nd, rv);
            }
        }
    }
    return rc;
}

/* 轮次49/50：登记本尺寸的线段（尺寸�?两条界线�? 文字�?�?供后续尺寸的 B14/B15 预检/复检 */
static void ab_dim_register(ProMdl drw, ProDimension *nd, const char *name, int orient, int ownvi)
{
    ProDimlocation loc = NULL;
    ProPoint3d    a1 = { 0, 0, 0 }, a2 = { 0, 0, 0 }, w1 = { 0, 0, 0 }, w2 = { 0, 0, 0 }, tp = { 0, 0, 0 };
    ProBoolean    heb = PRO_B_FALSE;
    double        ebl = 0.0;
    double        TH = 3.5 * 2.3805;      /* 字高兜底 3.5mm（与 SYMARKS 同源约定�?*/
    int           nchars = 0;

    /* 轮次59（用户："4 改成 4.0"）：逐尺寸小数位统一规则（显示设置，值仍�?Creo 算）�?
       整数 �?1 位（4�?.0�?�?.0�?3�?3.0）；|v|<1 �?2 位（0.25/0.35）；其余 �?1 位（63.6/41.1/23.5）；
       公差位：|tol|<0.09 �?2 位（±0.05），否则 1 位（±0.1）�?*/
    {
        double v0 = 0.0;
        int    dec = 1;

        if (ProDimensionValueGet(nd, &v0) == PRO_TK_NO_ERROR) {
            double rv = floor(v0 + 0.5);

            if (fabs(v0 - rv) < 1e-6) dec = 1;          /* 整数 �?1 位（显示�?.0�?*/
            else if (fabs(v0) < 1.0)  dec = 2;          /* 0.25 / 0.35 �?2 �?*/
            else                      dec = 1;          /* 63.6 / 41.1 / 23.5 �?1 �?*/
        }
        {
            ProDimToleranceType tt = PRO_TOL_DEFAULT;
            double up2 = 0.0, lo2 = 0.0;

            if (ProDimensionToltypeGet(nd, &tt) == PRO_TK_NO_ERROR &&
                (tt == PRO_TOL_PLUS_MINUS || tt == PRO_TOL_PLUS_MINUS_SYM || tt == PRO_TOL_LIMITS) &&
                ProDimensionToleranceGet(nd, &up2, &lo2) == PRO_TK_NO_ERROR) {
                int td = (fabs(up2) < 0.09) ? 2 : 1;

                logf_("[dec] %s tol decimals -> %d (err %d)", name, td,
                      (int)ProDimensionTolerancedecimalsSet(nd, td));
            }
        }
        {
            int rd = -1;

            logf_("[dec] %s decimals set -> %d (err %d)", name, dec, (int)ProDimensionDecimalsSet(nd, dec));
            ProDimensionDecimalsGet(nd, &rd);
            logf_("[dec] %s decimals readback = %d", name, rd);
        }
    }
    if (ProDimensionLocationGet(nd, NULL, (ProDrawing)drw, &loc) != PRO_TK_NO_ERROR || !loc) return;
    ProDimlocationArrowsGet(loc, a1, a2);
    ProDimlocationWitnesslinesGet(loc, w1, w2);
    ProDimlocationTextGet(loc, &heb, tp, &ebl);
    {
        /* 轮次51：字宽按渲染标定�?0.672×TH（实�?r50: 9.65 文字 4 字符 �?2.4px�?*/
        double cw = TH * 0.672;
        int    n1 = 0, n2 = 0;

        {
            ProLine *pl = NULL;

            if (ProDimensionTextGet(nd, &pl) == PRO_TK_NO_ERROR && pl && pl[0][0]) {
                char b8[512];

                wide_to_u8(pl[0], b8, sizeof(b8));
                n1 = (int)strlen(b8);
                logf_("[chk51] %s textGet='%s' (len=%d)", name, b8, n1);
            } else {
                logf_("[chk51] %s textGet unavailable", name);
            }
        }
        {
            wchar_t **wstrs = NULL;

            if (ProDimensionTextWstringsGet(nd, &wstrs) == PRO_TK_NO_ERROR && wstrs) {
                int cnt = 0, t;

                ProArraySizeGet((ProArray)wstrs, &cnt);   /* ProArray：按 NULL 结尾遍历会静默中�?*/
                for (t = 0; t < cnt; t++) {
                    if (wstrs[t]) {
                        n2 += (int)wcslen(wstrs[t]);
                        if (t < 4) {
                            char b8[512];

                            wide_to_u8(wstrs[t], b8, sizeof(b8));
                            logf_("[chk51] %s wstr[%d]='%s'", name, t, b8);
                        }
                    }
                }
                logf_("[chk51] %s wstrings cnt=%d total_chars=%d", name, cnt, n2);
                /* 不释放（ProWstringArrayFree 实测 fatal�?*/
            }
        }
        nchars = (n1 > 0) ? n1 : n2;
        if (nchars <= 0) nchars = 6;
        if (nchars > 16) nchars = 16;
        logf_("[chk52] %s format-string len=%d (ignored)", name, nchars);
        {
            char dbuf[64];
            int  dlen = ab_dim_display_len(nd, dbuf, sizeof(dbuf));

            if (dlen > 0) nchars = dlen;
            logf_("[chk52] %s display text='%s' -> 真字符数=%d (旧格式串口径=%d)", name, dbuf, nchars, n1 > 0 ? n1 : n2);
        }
        if (nchars <= 0) nchars = 6;
        if (nchars > 16) nchars = 16;
        if (orient == 0) {
            if (!g_ab_tbox_replace) {
                ab_seg_add(a1[0], a1[1], a1[0], a2[1], name);                 /* 尺寸线（竖） */
                ab_seg_add(w1[0], w1[1], a1[0], w1[1], name);                 /* 界线1（横�?*/
                ab_seg_add(w2[0], w2[1], a1[0], w2[1], name);                 /* 界线2（横�?*/
            }
            /* 轮次51/52：文字框"以文字点为中�?、长度用真实字符数（字宽 0.672×TH 实测标定�?*/
            if (g_ab_tbox_replace)
                ab_tbox_replace_for_tag(name, tp[0] - TH - 2.0, tp[1] - (double)nchars * cw / 2.0 - 2.0,
                                        tp[0] + 2.0,      tp[1] + (double)nchars * cw / 2.0 + 2.0);
            else
                ab_tbox_add(tp[0] - TH - 2.0, tp[1] - (double)nchars * cw / 2.0 - 2.0,
                            tp[0] + 2.0,      tp[1] + (double)nchars * cw / 2.0 + 2.0, name);
        } else {
            if (!g_ab_tbox_replace) {
                ab_seg_add(a1[0], a1[1], a2[0], a1[1], name);                 /* 尺寸线（横） */
                ab_seg_add(w1[0], w1[1], w1[0], a1[1], name);                 /* 界线1（竖�?*/
                ab_seg_add(w2[0], w2[1], w2[0], a2[1], name);                 /* 界线2（竖�?*/
            }
            if (g_ab_tbox_replace)
                ab_tbox_replace_for_tag(name, tp[0] - (double)nchars * cw / 2.0 - 2.0, tp[1] - TH - 2.0,
                                        tp[0] + (double)nchars * cw / 2.0 + 2.0, tp[1] + 2.0);
            else
                ab_tbox_add(tp[0] - (double)nchars * cw / 2.0 - 2.0, tp[1] - TH - 2.0,
                            tp[0] + (double)nchars * cw / 2.0 + 2.0, tp[1] + 2.0, name);
        }
    }
    {
        ab_tbox_t *tb = &g_abtb[g_abntb - 1];

        tb->nd = *nd; tb->orient = orient; tb->ownvi = ownvi; tb->valid = 1;
        tb->tx = tp[0]; tb->ty = tp[1];
        logf_("[chk51] %s registered: chars=%d textbox (%.1f,%.1f)-(%.1f,%.1f) dimline x/y=%.1f orient=%d",
              name, nchars, tb->x0, tb->y0, tb->x1, tb->y1,
              (orient == 0) ? a1[0] : a1[1], orient);
    }
    /* 轮次50 任务3 证据：锚点距最近轮廓的距离 vs 尺寸线位置（同侧层序应按锚点距离排） */
    if (ownvi >= 0 && ownvi < g_abnv) {
        ab_rect_t   outline;
        double      d1, d2;
        const char *side;

        outline.x0 = g_abv[ownvi].rect.x0 + 4.0; outline.x1 = g_abv[ownvi].rect.x1 - 4.0;
        outline.y0 = g_abv[ownvi].rect.y0 + 4.0; outline.y1 = g_abv[ownvi].rect.y1 - 4.0;
        d1 = ab_pt_rect_out_dist(w1[0], w1[1], &outline);
        d2 = ab_pt_rect_out_dist(w2[0], w2[1], &outline);
        side = (orient == 0) ? ((a1[0] > (outline.x0 + outline.x1) / 2.0) ? "RIGHT" : "LEFT")
                             : ((a1[1] > (outline.y0 + outline.y1) / 2.0) ? "UP" : "DOWN");
        logf_("[order50] %s side=%s anchorDepth(nearest outline edge)=%.1f/%.1f px  dimLine=(%.1f,%.1f)",
              name, side, d1, d2, a1[0], a1[1]);
    }
}

/* ===== 轮次51 裁定2：B15 终检归零 =====
   对仍存在�?某尺寸的线穿过某尺寸文字�?：沿该文字所属尺寸线方向滑动文字（ProDrawingDimensionMove
   给同一线上的新文字点），每步读回文字点/界线/尺寸线并重检；两个方向都无效则如实报告（打印前后数值）�?*/
/* 轮次52：按"�?n 条同 tag 线段"精确替换（此前只替第一条——滑字后登记被打乱、尺寸线被写过） */
static void ab_seg_replace_nth(const char *tag, int idx, double x0, double y0, double x1, double y1)
{
    int i, seen = 0;

    for (i = 0; i < g_abnseg; i++) {
        if (g_abseg[i].tag && tag && strcmp(g_abseg[i].tag, tag) == 0) {
            if (seen == idx) {
                g_abseg[i].x0 = x0; g_abseg[i].y0 = y0;
                g_abseg[i].x1 = x1; g_abseg[i].y1 = y1;
                return;
            }
            seen++;
        }
    }
}

/* 轮次52：读某尺寸的尺寸线跨度（登记的前 3 条同 tag 线段里，尺寸�?= �?0 条） */
static int ab_dim_line_span(const char *tag, int orient, double *lo, double *hi)
{
    int i, seen = 0;

    for (i = 0; i < g_abnseg; i++) {
        if (g_abseg[i].tag && tag && strcmp(g_abseg[i].tag, tag) == 0) {
            if (seen == 0) {
                if (orient == 0) {
                    *lo = g_abseg[i].y0 < g_abseg[i].y1 ? g_abseg[i].y0 : g_abseg[i].y1;
                    *hi = g_abseg[i].y0 < g_abseg[i].y1 ? g_abseg[i].y1 : g_abseg[i].y0;
                } else {
                    *lo = g_abseg[i].x0 < g_abseg[i].x1 ? g_abseg[i].x0 : g_abseg[i].x1;
                    *hi = g_abseg[i].x0 < g_abseg[i].x1 ? g_abseg[i].x1 : g_abseg[i].x0;
                }
                return (*hi - *lo) > 0.5;
            }
            seen++;
        }
    }
    return 0;
}

static int ab_b15_exc_count(void)
{
    int i, n = 0;

    for (i = 0; i < g_abntb; i++) if (g_abtb[i].valid && g_abtb[i].exc) n++;
    return n;
}

static int ab_b15_scan(int verbose, char *whoout, size_t cap)
{
    int tbi, j, n = 0, first = 1;

    for (tbi = 0; tbi < g_abntb; tbi++) {
        ab_tbox_t *tb = &g_abtb[tbi];

        if (!tb->valid) continue;
        if (tb->exc) {
            if (verbose) logf_("[b15pass] box '%s' marked as APPROVED EXCEPTION (centering kept) - skipped", tb->tag);
            continue;                                   /* 轮次54：例外项不计入违�?*/
        }
        for (j = 0; j < g_abnseg; j++) {
            if (g_abseg[j].tag && tb->tag && strcmp(g_abseg[j].tag, tb->tag) == 0) continue;  /* 自身线不�?*/
            if (ab_tbox_hit(g_abseg[j].x0, g_abseg[j].y0, g_abseg[j].x1, g_abseg[j].y1, tb)) {
                n++;
                if (verbose)
                    logf_("[b15pass] violation: '%s' line crosses text box of '%s'", g_abseg[j].tag ? g_abseg[j].tag : "?", tb->tag);
                if (first && whoout && cap > 0) {
                    sprintf_s(whoout, cap, "%s|%s", tb->tag, g_abseg[j].tag ? g_abseg[j].tag : "?");
                    first = 0;
                }
            }
        }
    }
    return n;
}

static void ab_b15_pass(ProMdl drw)
{
    int iter;

    logf_("[b15pass] === start: B15 violations = %d (approved exceptions are skipped) ===", ab_b15_scan(0, NULL, 0));
    for (iter = 0; iter < 6; iter++) {
        int tbi, fixed_any = 0;

        if (ab_b15_scan(0, NULL, 0) == 0) break;
        for (tbi = 0; tbi < g_abntb; tbi++) {
            ab_tbox_t *tb = &g_abtb[tbi];
            int        j, hit = 0, s;
            double     offy = -9999.0, offx = -9999.0;
            const char *by = "?";

            if (!tb->valid) continue;
            for (j = 0; j < g_abnseg; j++) {
                if (g_abseg[j].tag && tb->tag && strcmp(g_abseg[j].tag, tb->tag) == 0) continue;
                if (ab_tbox_hit(g_abseg[j].x0, g_abseg[j].y0, g_abseg[j].x1, g_abseg[j].y1, tb)) {
                    hit = 1; by = g_abseg[j].tag ? g_abseg[j].tag : "?";
                    /* 记录越线位置：横线取 y，竖线取 x */
                    if (fabs(g_abseg[j].y1 - g_abseg[j].y0) < 0.5) offy = g_abseg[j].y0;
                    else if (fabs(g_abseg[j].x1 - g_abseg[j].x0) < 0.5) offx = g_abseg[j].x0;
                }
            }
            if (!hit) continue;
            /* 轮次52：收�?全部越线坐标"一次性算目标位置（分两次滑会来回抖）
               —�?文字框须把所有越线都排除：只能整体移到全部越线之下或之上 */
            {
                double offs[16];
                int    noff = 0, jj2, diag = 0;
                double hh2, cur;
                double lo_o = 1e9, hi_o = -1e9, line_lo = 0.0, line_hi = 0.0;
                int    have_line;
                double cand_a, cand_b, target;
                int    ok_a, ok_b, band = 0;
                double mid_l = 0.0, mid_h = 0.0;
                const char *away_name;

                for (jj2 = 0; jj2 < g_abnseg; jj2++) {
                    if (g_abseg[jj2].tag && tb->tag && strcmp(g_abseg[jj2].tag, tb->tag) == 0) continue;
                    if (!ab_tbox_hit(g_abseg[jj2].x0, g_abseg[jj2].y0, g_abseg[jj2].x1, g_abseg[jj2].y1, tb)) continue;
                    if (fabs(g_abseg[jj2].y1 - g_abseg[jj2].y0) < 0.5) {
                        double yv = g_abseg[jj2].y0;

                        if (noff < 16) offs[noff++] = yv;
                        if (yv < lo_o) lo_o = yv;
                        if (yv > hi_o) hi_o = yv;
                    } else if (fabs(g_abseg[jj2].x1 - g_abseg[jj2].x0) < 0.5) {
                        double xv = g_abseg[jj2].x0;

                        if (noff < 16) offs[noff++] = xv;
                        if (xv < lo_o) lo_o = xv;
                        if (xv > hi_o) hi_o = xv;
                    } else {
                        diag = 1;
                    }
                }
                if (diag || noff == 0) {
                    logf_("[b15pass] %s: offender axis undecidable (diag=%d noff=%d) - skip box", tb->tag, diag, noff);
                    continue;
                }
                hh2 = (tb->orient == 0) ? ((tb->y1 - tb->y0) / 2.0) : ((tb->x1 - tb->x0) / 2.0);
                cur = (tb->orient == 0) ? tb->ty : tb->tx;
                have_line = ab_dim_line_span(tb->tag, tb->orient, &line_lo, &line_hi);
                if (have_line) { mid_l = (line_lo + line_hi) / 2.0; mid_h = (line_hi - line_lo) / 4.0; band = 1; }
                /* 两个候选：整体移到全部越线之下(lo_o-h-2) 或之�?hi_o+h+2)�?
                   "远离"= 朝越线质心的反方�?*/
                {
                    double mean_o = (lo_o + hi_o) / 2.0;
                    double down = lo_o - hh2 - 2.0, up = hi_o + hh2 + 2.0;

                    if (mean_o > cur) { cand_a = down; cand_b = up; away_name = "DOWN"; }
                    else              { cand_a = up;   cand_b = down; away_name = "UP"; }
                }
                ok_a = !band || fabs(cand_a - mid_l) <= mid_h + 0.6;
                ok_b = !band || fabs(cand_b - mid_l) <= mid_h + 0.6;
                if (ok_a) target = cand_a;
                else if (ok_b) { target = cand_b; away_name = "OPPOSITE"; }
                else {
                    /* 轮次54（DSH 裁定）：两个方向都会越出尺寸�?1/4~3/4 中段 �?**�?文字居中"，不�?*�?
                       该处 B15 记为"受裁定接受的例外"（机器判据让位给人看的居中）�?*/
                    tb->exc = 1;
                    logf_("[b15pass] %s: BOTH directions would leave the middle-half (line %.1f..%.1f mid %.1f ±%.1f; away=%.1f shift %.1f, other=%.1f shift %.1f) -> KEEP CENTERED, no slide; this B15 is recorded as an APPROVED EXCEPTION [DSH round54 ruling]",
                          tb->tag, line_lo, line_hi, mid_l, mid_h, cand_a, cand_a - cur, cand_b, cand_b - cur);
                    continue;
                }
                logf_("[b15pass] %s plan: offenders y=[", tb->tag);
                { int z; for (z = 0; z < noff; z++) logf_("  %.1f", offs[z]); logf_(" ] (lo=%.1f hi=%.1f h=%.1f) center %.1f -> %.1f (away=%s, line %.1f..%.1f, mid %.1f ±%.1f, ok_a=%d ok_b=%d)",
                      lo_o, hi_o, hh2, cur, target, away_name, line_lo, line_hi, mid_l, mid_h, ok_a, ok_b); }
                {
                    ProVector      np;
                    ProError       em;
                    ProDimlocation loc2 = NULL;
                    ProPoint3d     tp2 = { 0, 0, 0 }, a1 = { 0, 0, 0 }, a2 = { 0, 0, 0 }, w1 = { 0, 0, 0 }, w2 = { 0, 0, 0 };
                    ProBoolean     hb2 = PRO_B_FALSE;
                    double         el2 = 0.0;

                    np[0] = (tb->orient == 0) ? tb->tx : target;
                    np[1] = (tb->orient == 0) ? target : tb->ty;
                    np[2] = 0.0;
                    em = ProDrawingDimensionMove((ProDrawing)drw, &tb->nd, np);
                    if (ProDimensionLocationGet(&tb->nd, NULL, (ProDrawing)drw, &loc2) == PRO_TK_NO_ERROR && loc2 &&
                        ProDimlocationTextGet(loc2, &hb2, tp2, &el2) == PRO_TK_NO_ERROR) {
                        double hw2 = (tb->orient == 0) ? ((tb->x1 - tb->x0) / 2.0) : (hh2);

                        ProDimlocationArrowsGet(loc2, a1, a2);
                        ProDimlocationWitnesslinesGet(loc2, w1, w2);
                        logf_("[b15pass] %s slide move->%d (%s): text (%.1f,%.1f)->(%.1f,%.1f) dimline %s=%.1f wit1=(%.1f,%.1f) wit2=(%.1f,%.1f)",
                              tb->tag, (int)em, errname(em),
                              (tb->orient == 0) ? tb->tx : tb->tx, (tb->orient == 0) ? tb->ty : tb->ty, tp2[0], tp2[1],
                              (tb->orient == 0) ? "x" : "y", (tb->orient == 0) ? a1[0] : a1[1], w1[0], w1[1], w2[0], w2[1]);
                        tb->tx = tp2[0]; tb->ty = tp2[1];
                        if (tb->orient == 0) {
                            tb->y0 = tp2[1] - hh2; tb->y1 = tp2[1] + hh2;      /* x 不变，仅沿尺寸线�?*/
                        } else {
                            tb->x0 = tp2[0] - hw2; tb->x1 = tp2[0] + hw2;      /* y 不变 */
                        }
                        ab_seg_replace_nth(tb->tag, 0, (tb->orient == 0) ? a1[0] : a1[0], (tb->orient == 0) ? a1[1] : a1[1],
                                           (tb->orient == 0) ? a1[0] : a2[0], (tb->orient == 0) ? a2[1] : a1[1]);
                        ab_seg_replace_nth(tb->tag, 1, w1[0], w1[1], (tb->orient == 0) ? a1[0] : w1[0], w1[1]);
                        ab_seg_replace_nth(tb->tag, 2, w2[0], w2[1], (tb->orient == 0) ? a1[0] : w2[0], (tb->orient == 0) ? w2[1] : a2[1]);
                        if (band) {
                            logf_("[b15pass] %s centering: text center %.1f vs line-mid %.1f (band ±%.1f = 1/4 span) -> %s",
                                  tb->tag, (tb->orient == 0) ? tp2[1] : tp2[0], mid_l, mid_h,
                                  (fabs(((tb->orient == 0) ? tp2[1] : tp2[0]) - mid_l) <= mid_h + 0.6) ? "IN-MIDDLE-HALF" : "OUTSIDE (取最小位移，已说�?");
                        }
                        fixed_any = 1;
                    }
                }
            }
        }
        if (!fixed_any) { logf_("[b15pass] no further progress at iter %d", iter); break; }
    }
    logf_("[b15pass] === end: B15 violations = %d ; approved exceptions (centering kept, DSH r54) = %d ===", ab_b15_scan(0, NULL, 0), ab_b15_exc_count());
}

static void ab_dim_settle(ProMdl drw, double value, const char *name, ab_req_t *q, double *lx, double *ly)
{
    int    rc, tries = 0;
    double lx0 = *lx, ly0 = *ly;   /* 首个候选位（全部尝试失败时回退到此�?*/

    if (!g_last_nd_valid) {
        logf_("[chk50] %s: no created dim handle; skip post-check", name);
        return;
    }
    rc = ab_dim_fixup(drw, &g_last_nd, value, name, q->ownvi,
                      q->orient, ((q->orient == 0) ? ((*lx > g_abv[q->ownvi].ocx) ? +1 : -1)
                                                   : ((*ly > g_abv[q->ownvi].ocy) ? +1 : -1)));
    while ((rc == 3 || rc == 2) && tries < 2) {
        ab_req_t     q2 = *q;
        double       nlx = 0.0, nly = 0.0;
        int          nok = 0;

        tries++;
        logf_("[chk50] %s rc=%d -> try rebuild at next candidate (attempt %d)", name, rc, tries);
        ab_block_add(*lx - 9.0, *ly - 9.0, *lx + 9.0, *ly + 9.0, name);
        ab_search(&q2, &nlx, &nly, &nok);
        if (nok) {
            ProDimlocation loc = NULL;
            ProPoint3d    tp = { 0, 0, 0 };
            ProBoolean    hb = PRO_B_FALSE;
            double        el = 0.0;

            if (ProDimensionLocationGet(&g_last_nd, NULL, (ProDrawing)drw, &loc) == PRO_TK_NO_ERROR && loc &&
                ProDimlocationTextGet(loc, &hb, tp, &el) == PRO_TK_NO_ERROR) {
                ProVector np;

                np[0] = tp[0] + (nlx - *lx);
                np[1] = tp[1] + (nly - *ly);
                np[2] = tp[2];
                logf_("[chk50] %s rebuild-move -> cand (%.1f,%.1f) delta (%.1f,%.1f)", name, nlx, nly, nlx - *lx, nly - *ly);
                ProDrawingDimensionMove((ProDrawing)drw, &g_last_nd, np);
                *lx = nlx; *ly = nly;
            }
            rc = ab_dim_fixup(drw, &g_last_nd, value, name, q->ownvi,
                              q->orient, ((q->orient == 0) ? ((*lx > g_abv[q->ownvi].ocx) ? +1 : -1)
                                                           : ((*ly > g_abv[q->ownvi].ocy) ? +1 : -1)));
        } else {
            logf_("[chk50] %s rebuild: no alternative candidate", name);
            break;
        }
    }
    if ((rc == 3 || rc == 2) && (fabs(*lx - lx0) > 0.5 || fabs(*ly - ly0) > 0.5)) {
        /* 全部尝试失败 �?回退到首个候选位（位置可预测；结论如实标待裁定） */
        ProDimlocation loc = NULL;
        ProPoint3d    tp = { 0, 0, 0 };
        ProBoolean    hb = PRO_B_FALSE;
        double        el = 0.0;

        if (ProDimensionLocationGet(&g_last_nd, NULL, (ProDrawing)drw, &loc) == PRO_TK_NO_ERROR && loc &&
            ProDimlocationTextGet(loc, &hb, tp, &el) == PRO_TK_NO_ERROR) {
            ProVector np;

            np[0] = tp[0] + (lx0 - *lx);
            np[1] = tp[1] + (ly0 - *ly);
            np[2] = tp[2];
            logf_("[chk50] %s revert to first candidate (%.1f,%.1f)", name, lx0, ly0);
            ProDrawingDimensionMove((ProDrawing)drw, &g_last_nd, np);
            *lx = lx0; *ly = ly0;
        }
    }
    if (rc == 3 || rc == 2)
        logf_("[chk50] %s === FINAL: violation persists (rc=%d, drag-effective=%s) -> 跨尺�?B15 项交 b15pass 终检；停�?(%.1f,%.1f) ===",
              name, rc, (rc == 2) ? "no" : "yes", *lx, *ly);
    else
        logf_("[chk50] %s === FINAL: clean (rc=%d, drag=%s) at (%.1f,%.1f) ===",
              name, rc, (rc == 0) ? "not-needed" : "applied", *lx, *ly);
    ab_dim_register(drw, &g_last_nd, name, q->orient, q->ownvi);
    g_abnblock = 0;   /* 轮次49：封锁区只服务于本尺寸的重建尝试，不得泄漏给后续尺寸 */
}

static void ab_dim_conv(ProMdl part, ProMdl drw, double value, int vkind, int route,
                        int orient, int dir1, int dir2, double spanHalf, int ownvi, const char *name)
{
    ab_req_t q;
    double   lx, ly;
    int      ok;

    logf_("[auto] ==== dim %s (conversion value=%.4f vkind=%d route=%d) ====", name, value, vkind, route);
    memset(&q, 0, sizeof(q));
    q.orient = orient; q.dir1 = dir1; q.dir2 = dir2; q.ownvi = ownvi;
    {
        const ab_view_t *own = &g_abv[ownvi];

        if (orient == 0) {
            q.gy0 = own->ocy - spanHalf; q.gy1 = own->ocy + spanHalf;
            q.gx0 = own->rect.x0; q.gx1 = own->rect.x1;
        } else {
            q.gx0 = own->ocx - spanHalf; q.gx1 = own->ocx + spanHalf;
            q.gy0 = own->rect.y0; q.gy1 = own->rect.y1;
        }
    }
    ab_search(&q, &lx, &ly, &ok);
    if (!ok) { logf_("[auto] %s: no position; SKIP", name); return; }
    g_last_nd_valid = 0;
    convdim_run(part, drw, value, 1, lx, ly, vkind, route);
    ab_verify_dim(drw, value, ownvi);
    if (ok == 2) logf_("[auto] %s: [先放最不坏候选] least-bad candidate chosen (no fully-valid slot)", name);
    ab_dim_settle(drw, value, name, &q, &lx, &ly);          /* 轮次49：B13/B14 复检+拖动+换位 */
    {
        ab_rect_t r;

        if (orient == 0) { r.x0 = lx - 7.0; r.x1 = lx + 7.0; r.y0 = q.gy0 - 8.0; r.y1 = q.gy1 + 8.0; }
        else             { r.x0 = q.gx0 - 8.0; r.x1 = q.gx1 + 8.0; r.y0 = ly - 7.0; r.y1 = ly + 7.0; }
        ab_dim_add(r.x0, r.y0, r.x1, r.y1, name);
    }
}

/* ===== 轮次57：特征驱动建尺寸（不接收目标�?—�?值由 Creo 算）=====
   用户批评�?你为什么要根据值去检索，为什么不根据连接片的特征分析哪个是凸�?�?
   本路线彻底不按值找几何：先按几何关系把【特征】认出来，再拿特征作附件建尺寸�?

   what=1 凸点高度（②类）：【凸点顶面】↔【凸点所依附的基准面�?
     - 凸点顶面 := 达到包围�?z 上极值、且 footprint �?3mm 的面�?凸点"的几何判据）
     - 基准�?  := z 法向平面中位于凸点顶面之下（�?0.05 余量）且 z 最高的那张（凸点凸出于它）
   what=2 总厚（①类）：【达�?z 下极值的面】↔【达�?z 上极值的面�?
   两者都�?几何是否达到极�?是否相邻"为判据，不引用任何尺寸数值�?*/

#define R57_WHAT_DIMPLE 1
#define R57_WHAT_THICK  2

static int r57_find_faces(ProMdl part, int what, int *idA, int *idB, char *desc, size_t cap)
{
    ab_bbox_t bb;
    int       id;

    *idA = -1; *idB = -1;
    ab_bbox_get(part, &bb);
    if (what == R57_WHAT_DIMPLE) {
        double bestx = 1e30, bestz = -1e30, acx = 0.0, acy = 0.0;
        int    ntip = 0, nplane = 0, idB2 = -1, footB2 = -1;

        for (id = 100; id <= 6000; id++) {
            ProSurface s = NULL;
            ProGeomitemdata *gd = NULL;
            ProSurfacedata  *sd;

            if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
            if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            sd = gd->data.p_surface_data;
            {
                double ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
                double ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
                double foot = ex > ey ? ex : ey;
                double cx = (sd->xyz_min[0] + sd->xyz_max[0]) / 2.0;

                if (foot <= 3.0 && fabs(sd->xyz_max[2] - bb.z1) < 0.05) {
                    ntip++;
                    if (*idA < 0 || cx < bestx) { *idA = id; bestx = cx; acx = cx; acy = (sd->xyz_min[1] + sd->xyz_max[1]) / 2.0; }
                }
                if (sd->type == PRO_SRF_PLANE && fabs(sd->srf_shape.plane.e3[2]) > 0.99) {
                    double z = sd->srf_shape.plane.origin[2];

                    if (z < bb.z1 - 0.05 && z > bestz) { bestz = z; *idB = id; }
                    nplane++;
                }
            }
            ProGeomitemdataFree(&gd);
        }
        if (*idA < 0) { sprintf_s(desc, cap, "no small surface reaches z-top"); return 0; }
        if (*idB < 0) { sprintf_s(desc, cap, "no z-normal plane below z-top"); return 0; }
        /* 基准面取"与凸点同一凸台"的那张：�?z 档、且平面范围包含凸点 (x,y) 的平面（面积最大者） */
        for (id = 100; id <= 6000; id++) {
            ProSurface s = NULL;
            ProGeomitemdata *gd = NULL;
            ProSurfacedata  *sd;

            if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
            if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            sd = gd->data.p_surface_data;
            if (sd->type == PRO_SRF_PLANE && fabs(sd->srf_shape.plane.e3[2]) > 0.99 &&
                fabs(sd->srf_shape.plane.origin[2] - bestz) < 0.01) {
                double ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
                double ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
                double foot = ex > ey ? ex : ey;

                if (acx > sd->xyz_min[0] - 0.5 && acx < sd->xyz_max[0] + 0.5 &&
                    acy > sd->xyz_min[1] - 0.5 && acy < sd->xyz_max[1] + 0.5 && foot > footB2) {
                    idB2 = id; footB2 = foot;
                }
            }
            ProGeomitemdataFree(&gd);
        }
        if (idB2 > 0) *idB = idB2;
        sprintf_s(desc, cap, "dimple-top candidates=%d (picked id=%d at (%.2f,%.2f), leftmost) ; z-normal planes=%d ; base face=id %d z=%.3f (same-boss, foot=%.1f)",
                  ntip, *idA, acx, acy, nplane, *idB, bestz, footB2);
        return 1;
    } else {
        /* what=2 总厚：优先取【z 下极值处�?z 法向平面】（�?dDIMPLE �?�?�?NONE senses 结构一致，
           实测该结�?Creo 接受）——因�?svi �?[侧壁�?�?圆环面] 组合实测�?Creo 拒（-46）�?*/
        double footA = -1.0, footB = -1.0, footAany = -1.0;
        int    idAany = -1;

        for (id = 100; id <= 6000; id++) {
            ProSurface s = NULL;
            ProGeomitemdata *gd = NULL;
            ProSurfacedata  *sd;

            if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
            if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            sd = gd->data.p_surface_data;
            {
                double ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
                double ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
                double foot = ex > ey ? ex : ey;

                if (fabs(sd->xyz_min[2] - bb.z0) < 0.05) {
                    if (sd->type == PRO_SRF_PLANE && fabs(sd->srf_shape.plane.e3[2]) > 0.99) {
                        if (foot > footA) { *idA = id; footA = foot; }
                    }
                    if (foot > footAany) { idAany = id; footAany = foot; }
                }
                if (fabs(sd->xyz_max[2] - bb.z1) < 0.05 && foot > footB) { *idB = id; footB = foot; }
            }
            ProGeomitemdataFree(&gd);
        }
        if (*idA < 0 && idAany > 0) { *idA = idAany; footA = footAany; logf_("[r57] thick: no z-normal plane at z-min; fallback surface %d (foot=%.2f)", idAany, footAany); }
        sprintf_s(desc, cap, "z-min face id=%d foot=%.2f (z-normal plane preferred) ; z-max face id=%d foot=%.2f", *idA, footA, *idB, footB);
        return (*idA > 0 && *idB > 0) ? 1 : 0;
    }
}


/* 用两个【面】选择建尺寸；值不设——读�?Creo 算的值用于公�?登记/复检。tol_sym<=0 不加公差�?*/
static int r57_create_face_dim(ProMdl part, ProMdl drw, const ab_view_t *own, int idA, int idB,
                               int orient, double lx, double ly, double tol_sym, const char *name)
{
    ProSurface    sa = NULL, sb = NULL;
    ProGeomitem   g1, g2;
    ProSelection  sel0 = NULL, sel1 = NULL;
    ProDimAttachment *arr2 = NULL;
    ProDimSense      *sen2 = NULL;
    ProDimension      nd;
    ProVector         loc;
    ProError          ec;
    double            nv = -1.0;

    if (ProSurfaceInit(part, idA, &sa) != PRO_TK_NO_ERROR || !sa) { logf_("[r57] %s: surface %d init failed", name, idA); return 0; }
    if (ProSurfaceInit(part, idB, &sb) != PRO_TK_NO_ERROR || !sb) { logf_("[r57] %s: surface %d init failed", name, idB); return 0; }
    if (ProSurfaceToGeomitem((ProSolid)part, sa, &g1) != PRO_TK_NO_ERROR) { logf_("[r57] %s: srfA->geomitem failed", name); return 0; }
    if (ProSurfaceToGeomitem((ProSolid)part, sb, &g2) != PRO_TK_NO_ERROR) { logf_("[r57] %s: srfB->geomitem failed", name); return 0; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &sel0) != PRO_TK_NO_ERROR) return 0;
    if (ProSelectionViewSet(own->v, &sel0) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); return 0; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); return 0; }
    if (ProSelectionViewSet(own->v, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); ProSelectionFree(&sel1); return 0; }

    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr2);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen2);
    ProSelectionCopy(sel0, &arr2[0][0]); arr2[0][1] = NULL;
    ProSelectionCopy(sel1, &arr2[1][0]); arr2[1][1] = NULL;
    memset(&sen2[0], 0, sizeof(ProDimSense));
    memset(&sen2[1], 0, sizeof(ProDimSense));
    sen2[0].type = PRO_DIM_SNS_TYP_NONE; sen2[0].orient_hint = (orient == 0) ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    sen2[1].type = PRO_DIM_SNS_TYP_NONE; sen2[1].orient_hint = (orient == 0) ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
    loc[0] = lx; loc[1] = ly; loc[2] = 0.0;
    memset(&nd, 0, sizeof(nd));
    ec = ProDrawingDimensionCreate((ProDrawing)drw, arr2, sen2, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &nd);
    logf_("[r57] %s: create(idA=%d,idB=%d,orient=%d) -> %d (%s)", name, idA, idB, orient, (int)ec, errname(ec));
    ProArrayFree((ProArray *)&arr2);
    ProArrayFree((ProArray *)&sen2);
    ProSelectionFree(&sel0);
    ProSelectionFree(&sel1);
    if (ec != PRO_TK_NO_ERROR) return 0;
    /* 值由 Creo 算：读出来只�?日志/公差/登记 �?*/
    if (ProDimensionValueGet(&nd, &nv) != PRO_TK_NO_ERROR) nv = -1.0;
    logf_("[r57] %s: Creo computed value = %.4f  (expected form only; no target passed)", name, nv);
    if (tol_sym > 0.0) {
        ProName tab;
        ProError e1, e2;

        tab[0] = 0;
        ProDimensionTollabelSet(&nd, PROTOLTABLE_NONE, tab, 0);
        e1 = ProDimensionToltypeSet(&nd, PRO_TOL_PLUS_MINUS_SYM);
        e2 = ProDimensionToleranceSet(&nd, tol_sym, -tol_sym);
        logf_("[r57] %s: tol +-%.2f type->%d set->%d", name, tol_sym, (int)e1, (int)e2);
    }
    {
        ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, own->v);

        logf_("[r57] %s: show -> %d (%s)", name, (int)es, errname(es));
    }
    {
        ProVector np;

        np[0] = lx; np[1] = ly; np[2] = 0.0;
        logf_("[r57] %s: move to (%.1f,%.1f) -> %d", name, lx, ly,
              (int)ProDrawingDimensionMove((ProDrawing)drw, &nd, np));
    }
    g_last_nd = nd;
    g_last_nd_valid = 1;
    g_last_nd_value = (nv > 0.0) ? nv : -1.0;
    return 1;
}

/* what=3 凸点竖向间距（③类）：先由【凸点顶面】几何定出各凸点位置，再取最左列中上下相邻两枚，
   找到其轮廓圆/弧（圆心即凸点中心）作附�?—�?全程�?特征在何处、哪些相�?选，不引用数值�?*/
#define R57_WHAT_DIMV 3
/* what=4 料厚（⑤类）：同一 XY 投影位置、仅 z 不同的一对平行直边中 Δz 最小�?= 薄片料厚
   （本零件 = 0.25 台阶那对边；6 组同构取最靠左一组）。同样不引用任何目标值�?*/
#define R57_WHAT_SHEET 4

static int r57_find_dimv(ProMdl part, int *idA, int *idB, char *desc, size_t cap)
{
    typedef struct { double x, y; } dpt_t;
    static dpt_t  P[64];
    static double domz[64];
    int    nP = 0, id, i, j, bi = -1, bj = -1;
    double bestx = 1e30, bestdy = 1e30;
    ab_bbox_t bb;

    ab_bbox_get(part, &bb);
    /* �?1 步：凸点顶面（footprint<=3 且达�?z 上极值）�?位置去重成凸点列�?*/
    for (id = 100; id <= 6000; id++) {
        ProSurface s = NULL;
        ProGeomitemdata *gd = NULL;
        ProSurfacedata  *sd;

        if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        sd = gd->data.p_surface_data;
        {
            double ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
            double ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
            double foot = ex > ey ? ex : ey;
            double cx = (sd->xyz_min[0] + sd->xyz_max[0]) / 2.0;
            double cy = (sd->xyz_min[1] + sd->xyz_max[1]) / 2.0;

            if (foot <= 3.0 && fabs(sd->xyz_max[2] - bb.z1) < 0.05) {
                int dup = 0;

                /* 一枚凸点的圆环顶面常由两半组成（中心相�?~0.7）→ 0.8 内视作同一�?*/
                for (i = 0; i < nP; i++)
                    if (fabs(P[i].x - cx) < 0.8 && fabs(P[i].y - cy) < 0.8) { dup = 1; break; }
                if (!dup && nP < 64) { P[nP].x = cx; P[nP].y = cy; domz[nP] = sd->xyz_max[2]; nP++; }
            }
        }
        ProGeomitemdataFree(&gd);
    }
    if (nP < 2) { sprintf_s(desc, cap, "dimple tops found=%d (<2)", nP); return 0; }
    /* �?2 步：最左列（x 最小者）中，y 相邻的一�?*/
    for (i = 0; i < nP; i++) for (j = i + 1; j < nP; j++) {
        double dx = fabs(P[i].x - P[j].x), dy = fabs(P[i].y - P[j].y);
        double xmin = P[i].x < P[j].x ? P[i].x : P[j].x;

        if (dx > 0.1 || dy < 1.0) continue;
        if (xmin < bestx - 0.01 || (fabs(xmin - bestx) < 0.01 && dy < bestdy)) {
            bestx = xmin; bestdy = dy; bi = i; bj = j;
        }
    }
    if (bi < 0) { sprintf_s(desc, cap, "no adjacent dimple pair in same column (n=%d)", nP); return 0; }
    /* �?3 步：找这两枚凸点的轮廓圆/弧（圆心=凸点中心；同 z 的一对优先） */
    {
        typedef struct { int id; double x, y, z, r; } rarc_t;
        static rarc_t A[512];
        int nA = 0, ka = -1, kb = -1;

        for (id = 1; id <= 6000 && nA < 512; id++) {
            ProEdge e = NULL;
            ProGeomitem gi;
            ProGeomitemdata *gd = NULL;
            int t;

            if (ProEdgeInit(part, id, &e) != PRO_TK_NO_ERROR || !e) continue;
            if (ProEdgeToGeomitem(part, e, &gi) != PRO_TK_NO_ERROR) continue;
            if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            t = gd->data.p_curve_data->line.type;
            if (t == PRO_ENT_ARC) {
                ProArcdata *ad = &gd->data.p_curve_data->arc;

                A[nA].id = id; A[nA].x = ad->origin[0]; A[nA].y = ad->origin[1]; A[nA].z = ad->origin[2]; A[nA].r = ad->radius; nA++;
            } else if (t == PRO_ENT_CIRCLE) {
                ProCircledata *cd = &gd->data.p_curve_data->circle;

                A[nA].id = id; A[nA].x = cd->center[0]; A[nA].y = cd->center[1]; A[nA].z = cd->center[2]; A[nA].r = cd->radius; nA++;
            }
            ProGeomitemdataFree(&gd);
        }
        for (i = 0; i < nA; i++) {
            if (fabs(A[i].x - P[bi].x) < 0.5 && fabs(A[i].y - P[bi].y) < 0.5 && ka < 0) ka = i;
            if (fabs(A[i].x - P[bj].x) < 0.5 && fabs(A[i].y - P[bj].y) < 0.5) {
                /* 与已�?A �?z 者优�?*/
                if (kb < 0 || (ka >= 0 && fabs(A[i].z - A[ka].z) < 0.05 && fabs(A[kb].z - A[ka].z) >= 0.05)) kb = i;
            }
        }
        if (ka < 0 || kb < 0) { sprintf_s(desc, cap, "rim arcs not found (ka=%d kb=%d, arcs=%d)", ka, kb, nA); return 0; }
        *idA = A[ka].id; *idB = A[kb].id;
        sprintf_s(desc, cap, "dimple tops=%d; pair column x=%.2f dy=%.2f ; rim arcs idA=%d(%.2f,%.2f,%.3f r=%.2f) idB=%d(%.2f,%.2f,%.3f r=%.2f)",
                  nP, bestx, bestdy, *idA, A[ka].x, A[ka].y, A[ka].z, A[ka].r, *idB, A[kb].x, A[kb].y, A[kb].z, A[kb].r);
        return 1;
    }
}

/* what=4 料厚：同 XY 投影、仅 z 不同的一对平行直边（Δz 最�?= 料厚台阶�?*/
static int r57_find_sheet_edges(ProMdl part, int *idA, int *idB, char *desc, size_t cap)
{
    typedef struct { int id; double mx, my, mz, ux, uy, len, x0, x1; } sedge_t;
    static sedge_t E[512];
    int nE = 0, id, i, j, bi = -1, bj = -1;
    double bestdz = 1e30, bestx = 1e30;

    *idA = -1; *idB = -1;
    for (id = 1; id <= 6000 && nE < 512; id++) {
        ProEdge e = NULL;
        ProGeomitem gi;
        ProGeomitemdata *gd = NULL;
        int t;

        if (ProEdgeInit((ProSolid)part, id, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        t = gd->data.p_curve_data->line.type;
        if (t == PRO_ENT_LINE) {
            ProLinedata *ld = &gd->data.p_curve_data->line;
            double dx = ld->end2[0] - ld->end1[0];
            double dy = ld->end2[1] - ld->end1[1];
            double dz = ld->end2[2] - ld->end1[2];
            double len = sqrt(dx * dx + dy * dy + dz * dz);

            if (len > 1.0 && len < 20.0 && fabs(dz) < 0.05) {   /* 平边（上/下面上的边），非竖边 */
                E[nE].id = id;
                E[nE].mx = (ld->end1[0] + ld->end2[0]) / 2.0;
                E[nE].my = (ld->end1[1] + ld->end2[1]) / 2.0;
                E[nE].mz = (ld->end1[2] + ld->end2[2]) / 2.0;
                E[nE].ux = dx / len; E[nE].uy = dy / len; E[nE].len = len;
                E[nE].x0 = ld->end1[0] < ld->end2[0] ? ld->end1[0] : ld->end2[0];
                E[nE].x1 = ld->end1[0] < ld->end2[0] ? ld->end2[0] : ld->end1[0];
                nE++;
            }
        }
        ProGeomitemdataFree(&gd);
    }
    for (i = 0; i < nE; i++) for (j = i + 1; j < nE; j++) {
        double dz = fabs(E[i].mz - E[j].mz);
        double dx = fabs(E[i].mx - E[j].mx), dy = fabs(E[i].my - E[j].my);
        double cross = fabs(E[i].ux * E[j].uy - E[i].uy * E[j].ux);
        double xmin = E[i].mx < E[j].mx ? E[i].mx : E[j].mx;
        double ov = (E[i].x1 < E[j].x1 ? E[i].x1 : E[j].x1) - (E[i].x0 > E[j].x0 ? E[i].x0 : E[j].x0);

        if (dx > 0.05 || dy > 0.05 || dz < 0.05 || dz > 1.0 || cross > 0.02) continue;
        /* �?轮次58：边必须"垂直于视�?（本函数服务于顶部视图，视向=y）：�?y 的边�?
           该视图里投影成点 �?建出的尺寸恒�?0.000（上一版就选错�?y 向边对）�?*/
        if (fabs(E[i].uy) > 0.3 || fabs(E[j].uy) > 0.3) continue;
        /* 两条边的 x 区间必须真实重叠（中点相同但首尾错开的不算一对） */
        if (ov < 0.5) continue;
        if (dz < bestdz - 1e-4 || (fabs(dz - bestdz) < 1e-4 && xmin < bestx)) {
            bestdz = dz; bestx = xmin; bi = i; bj = j;
        }
    }
    if (bi < 0) { sprintf_s(desc, cap, "no coincident-projection edge pair (flat lines=%d)", nE); return 0; }
    *idA = E[bi].id; *idB = E[bj].id;
    sprintf_s(desc, cap, "sheet pair: edges %d & %d at (%.2f,%.2f) z=%.3f/%.3f dz=%.3f (flat lines=%d)",
              *idA, *idB, E[bi].mx, E[bi].my, E[bi].mz, E[bj].mz, bestdz, nE);
    return 1;
}

/* 用两条【边/圆弧】选择建尺寸（kind=0 �?NONE senses[直线]；kind=1 �?PNT/CENTER[圆弧]）�?
   值不设——读�?Creo 算的值。oh1/oh2/dimorient 显式给出（r57 试验结论：圆心点类竖直测�?
   必须靠顶�?dim_orient=VERT）�?*/
static int r57_create_edge_dim(ProMdl part, ProMdl drw, const ab_view_t *own, int idA, int idB,
                               int kind, ProDimOrient oh1, ProDimOrient oh2, ProDimOrient dimorient,
                               double lx, double ly, double tol_sym, const char *name)
{
    ProEdge       ea = NULL, eb = NULL;
    ProGeomitem   g1, g2;
    ProSelection  sel0 = NULL, sel1 = NULL;
    ProDimAttachment *arr2 = NULL;
    ProDimSense      *sen2 = NULL;
    ProDimension      nd;
    ProVector         loc;
    ProError          ec;
    double            nv = -1.0;

    if (ProEdgeInit((ProSolid)part, idA, &ea) != PRO_TK_NO_ERROR || !ea) { logf_("[r57] %s: edge %d init failed", name, idA); return 0; }
    if (ProEdgeInit((ProSolid)part, idB, &eb) != PRO_TK_NO_ERROR || !eb) { logf_("[r57] %s: edge %d init failed", name, idB); return 0; }
    if (ProEdgeToGeomitem((ProSolid)part, ea, &g1) != PRO_TK_NO_ERROR) return 0;
    if (ProEdgeToGeomitem((ProSolid)part, eb, &g2) != PRO_TK_NO_ERROR) return 0;
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &sel0) != PRO_TK_NO_ERROR) return 0;
    if (ProSelectionViewSet(own->v, &sel0) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); return 0; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); return 0; }
    if (ProSelectionViewSet(own->v, &sel1) != PRO_TK_NO_ERROR) { ProSelectionFree(&sel0); ProSelectionFree(&sel1); return 0; }

    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr2);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen2);
    ProSelectionCopy(sel0, &arr2[0][0]); arr2[0][1] = NULL;
    ProSelectionCopy(sel1, &arr2[1][0]); arr2[1][1] = NULL;
    memset(&sen2[0], 0, sizeof(ProDimSense));
    memset(&sen2[1], 0, sizeof(ProDimSense));
    if (kind == 1) {
        sen2[0].type = PRO_DIM_SNS_TYP_PNT; sen2[0].sense = PRO_POINT_TYP_CENTER; sen2[0].orient_hint = oh1;
        sen2[1].type = PRO_DIM_SNS_TYP_PNT; sen2[1].sense = PRO_POINT_TYP_CENTER; sen2[1].orient_hint = oh2;
    } else {
        sen2[0].type = PRO_DIM_SNS_TYP_NONE; sen2[0].orient_hint = oh1;
        sen2[1].type = PRO_DIM_SNS_TYP_NONE; sen2[1].orient_hint = oh2;
    }
    loc[0] = lx; loc[1] = ly; loc[2] = 0.0;
    memset(&nd, 0, sizeof(nd));
    ec = ProDrawingDimensionCreate((ProDrawing)drw, arr2, sen2, dimorient, loc, PRO_B_FALSE, &nd);
    logf_("[r57] %s: create(edge %d,%d kind=%d oh1=%d oh2=%d dim_orient=%d) -> %d (%s)", name, idA, idB,
          kind, (int)oh1, (int)oh2, (int)dimorient, (int)ec, errname(ec));
    ProArrayFree((ProArray *)&arr2);
    ProArrayFree((ProArray *)&sen2);
    ProSelectionFree(&sel0);
    ProSelectionFree(&sel1);
    if (ec != PRO_TK_NO_ERROR) return 0;
    if (ProDimensionValueGet(&nd, &nv) != PRO_TK_NO_ERROR) nv = -1.0;
    logf_("[r57] %s: Creo computed value = %.4f", name, nv);
    if (tol_sym > 0.0) {
        ProName tab;

        tab[0] = 0;
        ProDimensionTollabelSet(&nd, PROTOLTABLE_NONE, tab, 0);
        ProDimensionToltypeSet(&nd, PRO_TOL_PLUS_MINUS_SYM);
        ProDimensionToleranceSet(&nd, tol_sym, -tol_sym);
        logf_("[r57] %s: tol +-%.2f applied", name, tol_sym);
    }
    logf_("[r57] %s: show -> %d", name, (int)ProAnnotationShow((ProAnnotation *)&nd, NULL, own->v));
    {
        ProVector np;

        np[0] = lx; np[1] = ly; np[2] = 0.0;
        logf_("[r57] %s: move to (%.1f,%.1f) -> %d", name, lx, ly,
              (int)ProDrawingDimensionMove((ProDrawing)drw, &nd, np));
    }
    g_last_nd = nd;
    g_last_nd_valid = 1;
    g_last_nd_value = (nv > 0.0) ? nv : -1.0;
    return 1;
}

static void r57_feature_dim(ProMdl part, ProMdl drw, int what, int ownvi, int orient,
                            double spanHalf, double tol_sym, const char *name)
{
    ab_req_t       q;
    double         lx, ly;
    int            ok = 0, idA = -1, idB = -1;
    char           desc[256];
    const ab_view_t *own;

    if (ownvi < 0 || ownvi >= g_abnv) { logf_("[r57] %s: bad view idx %d", name, ownvi); return; }
    own = &g_abv[ownvi];
    logf_("[r57] ==== %s (feature-driven, what=%d view='%s' orient=%d) ====", name, what, own->tag, orient);
    if (what == R57_WHAT_DIMV) {
        if (!r57_find_dimv(part, &idA, &idB, desc, sizeof(desc))) {
            logf_("[r57] %s: feature NOT found: %s", name, desc);
            return;
        }
    } else if (what == R57_WHAT_SHEET) {
        if (!r57_find_sheet_edges(part, &idA, &idB, desc, sizeof(desc))) {
            logf_("[r57] %s: sheet-thickness pair NOT found: %s", name, desc);
            return;
        }
    } else {
        if (!r57_find_faces(part, what, &idA, &idB, desc, sizeof(desc))) {
            logf_("[r57] %s: feature faces NOT found: %s", name, desc);
            return;
        }
    }
    logf_("[r57] %s: %s", name, desc);
    memset(&q, 0, sizeof(q));
    q.orient = orient; q.dir1 = -1; q.dir2 = -1; q.ownvi = ownvi;
    if (orient == 0) {
        q.gy0 = own->ocy - spanHalf; q.gy1 = own->ocy + spanHalf;
        q.gx0 = own->rect.x0; q.gx1 = own->rect.x1;
    } else {
        q.gx0 = own->ocx - spanHalf; q.gx1 = own->ocx + spanHalf;
        q.gy0 = own->rect.y0; q.gy1 = own->rect.y1;
    }
    ab_search(&q, &lx, &ly, &ok);
    if (!ok) { logf_("[r57] %s: no position; SKIP", name); return; }
    if (ok == 2) logf_("[r57] %s: [先放最不坏候选] least-bad candidate", name);
    g_last_nd_valid = 0;
    if (what == R57_WHAT_DIMV) {
        /* 轮次57：竖直圆心距——两�?sense 都给 VERT，且顶层 dim_orient 也显�?VERT
           （试�?R57TRY|1|0..5：仅 sense 提示�?Creo 量成水平 �?0.000�?*/
        if (!r57_create_edge_dim(part, drw, own, idA, idB, 1, PRO_DIM_ORNT_VERT, PRO_DIM_ORNT_VERT,
                                 PRO_DIM_ORNT_VERT, lx, ly, tol_sym, name)) return;
    } else if (what == R57_WHAT_SHEET) {
        /* 轮次58：料厚——直线对（NONE senses�? 视图方向对应�?sense hints + 顶层 NONE
           （tvi=VERT / svi=HORIZ；与�?d025 同组附件结构�?*/
        ProDimOrient h = (orient == 0) ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;

        if (!r57_create_edge_dim(part, drw, own, idA, idB, 0, h, h,
                                 PRO_DIM_ORNT_NONE, lx, ly, tol_sym, name)) return;
    } else {
        if (!r57_create_face_dim(part, drw, own, idA, idB, orient, lx, ly, tol_sym, name)) return;
    }
    if (g_last_nd_value > 0.0)
        ab_dim_settle(drw, g_last_nd_value, name, &q, &lx, &ly);
    else
        logf_("[r57] %s: value unreadable; skip settle", name);
    if (!strcmp(name, "dDIMPLE")) { g_r58_dimple_nd = g_last_nd; g_r58_dimple_valid = 1; }
    if (!strcmp(name, "d40")) { g_r60_thick_nd = g_last_nd; g_r60_thick_valid = 1; }
    {
        ab_rect_t r;

        if (orient == 0) { r.x0 = lx - 7.0; r.x1 = lx + 7.0; r.y0 = q.gy0 - 8.0; r.y1 = q.gy1 + 8.0; }
        else             { r.x0 = q.gx0 - 8.0; r.x1 = q.gx1 + 8.0; r.y0 = ly - 7.0; r.y1 = ly + 7.0; }
        ab_dim_add(r.x0, r.y0, r.x1, r.y1, name);
    }
}


/* 轮次58�?.35（dDIMPLE）的文字沿其【竖直】尺寸线上移 dy —�?
   ProDrawingDimensionMove 沿尺寸线方向 = 只滑文字（尺寸线与界线不动，r51 已实测）�?*/
/* r60 (DSH ruling): format-override ONLY the total-thickness dim -> "4.0";
   the value stays Creo-computed because "@D" remains the token and ".0" is a literal suffix. */
static void r60_override_thick_text(ProMdl drw)
{
    wchar_t   fmt[64];
    wchar_t **strs = NULL;
    ProError  e;

    if (!g_r60_thick_valid) { logf_("[r60] thick dim handle missing; skip override"); return; }
    wcsncpy_s(fmt, 64, L"{0:@D}.0", _TRUNCATE);
    ProArrayAlloc(1, sizeof(wchar_t *), 1, (ProArray *)&strs);
    strs[0] = fmt;
    e = ProDimensionTextWstringsSet(&g_r60_thick_nd, strs);
    logf_("[r60] thick text override \x27{0:@D}.0\x27 -> %d (%s)", (int)e, errname(e));
    {
        wchar_t **w = NULL;

        if (ProDimensionTextWstringsGet(&g_r60_thick_nd, &w) == PRO_TK_NO_ERROR && w) {
            int cnt = 0;
            ProArraySizeGet((ProArray)w, &cnt);
            logf_("[r60] thick text readback cnt=%d w0=\x27%ls\x27", cnt, (cnt > 0 && w[0]) ? w[0] : L"");
        }
    }
    {
        double v = -1.0;

        if (ProDimensionValueGet(&g_r60_thick_nd, &v) == PRO_TK_NO_ERROR)
            logf_("[r60] thick value after override = %.4f (stays Creo-computed)", v);
    }
    {
        ProError er = ProDwgSheetRegenerate((ProDrawing)drw, 1);
        logf_("[r60] regen after override -> %d (%s)", (int)er, errname(er));
    }
}

static void r58_slide_dimple_text(ProMdl drw, double dy, int ownvi)
{
    ProDimlocation loc = NULL;
    ProPoint3d    tp = { 0.0, 0.0, 0.0 };
    ProBoolean    hb = PRO_B_FALSE;
    double        el = 0.0;

    if (!g_r58_dimple_valid) { logf_("[r58] slide dDIMPLE: no handle; skip"); return; }
    if (ProDimensionLocationGet(&g_r58_dimple_nd, NULL, (ProDrawing)drw, &loc) != PRO_TK_NO_ERROR || !loc) {
        logf_("[r58] slide dDIMPLE: locationGet failed"); return;
    }
    if (ProDimlocationTextGet(loc, &hb, tp, &el) != PRO_TK_NO_ERROR) {
        logf_("[r58] slide dDIMPLE: textGet failed"); return;
    }
    {
        ProVector np;

        np[0] = tp[0];
        np[1] = tp[1] + dy;
        np[2] = tp[2];
        logf_("[r58] slide dDIMPLE text (%.1f,%.1f) -> (%.1f,%.1f) => %d", tp[0], tp[1], np[0], np[1],
              (int)ProDrawingDimensionMove((ProDrawing)drw, &g_r58_dimple_nd, np));
    }
    g_ab_tbox_replace = 1;
    ab_dim_register(drw, &g_r58_dimple_nd, "dDIMPLE", 0, ownvi);
    g_ab_tbox_replace = 0;
}


/* ===== r57 debug: R57TRY|<what>|<variant> - trial dim; logs Creo-computed value ===== */
static void r57_try(ProMdl part, ProMdl drw, int what, int v)
{
    int      fvi = -1, svi = -1, tvi = -1, i;
    int      idA = -1, idB = -1;
    char     desc[256];
    const ab_view_t *own = NULL;
    ab_req_t q;
    double   lx, ly;
    int      ok = 0;
    char     nm[64];

    for (i = 0; i < g_abnv; i++) {
        if (g_abv[i].isfront) fvi = i;
        if (g_abv[i].isside) svi = i;
        if (g_abv[i].istop) tvi = i;
    }
    if (what == 1) {
        /* dDIMV 变体�?=VERT/VERT  1=NONE/NONE  2=VERT/NONE  3=HORIZ/HORIZ
           4=VERT/VERT+dimVERT  5=NONE/NONE+dimVERT  6=VERT/VERT+dimHORIZ（对照） */
        ProDimOrient h1 = PRO_DIM_ORNT_VERT, h2 = PRO_DIM_ORNT_VERT, do_ = PRO_DIM_ORNT_NONE;

        if (v == 1) { h1 = PRO_DIM_ORNT_NONE; h2 = PRO_DIM_ORNT_NONE; }
        else if (v == 2) { h1 = PRO_DIM_ORNT_VERT; h2 = PRO_DIM_ORNT_NONE; }
        else if (v == 3) { h1 = PRO_DIM_ORNT_HORIZ; h2 = PRO_DIM_ORNT_HORIZ; }
        else if (v == 4) { do_ = PRO_DIM_ORNT_VERT; }
        else if (v == 5) { h1 = PRO_DIM_ORNT_NONE; h2 = PRO_DIM_ORNT_NONE; do_ = PRO_DIM_ORNT_VERT; }
        else if (v == 6) { do_ = PRO_DIM_ORNT_HORIZ; }
        if (fvi < 0) { logf_("[try] no front view"); return; }
        own = &g_abv[fvi];
        if (!r57_find_dimv(part, &idA, &idB, desc, sizeof(desc))) { logf_("[try] dimv find failed: %s", desc); return; }
        logf_("[try] dimv v=%d %s", v, desc);
        sprintf_s(nm, sizeof(nm), "trydimv%d", v);
        memset(&q, 0, sizeof(q));
        q.orient = 0; q.dir1 = -1; q.dir2 = -1; q.ownvi = fvi;
        q.gy0 = own->ocy - 12.0; q.gy1 = own->ocy + 12.0;
        q.gx0 = own->rect.x0 - 60.0; q.gx1 = own->rect.x0 - 6.0;
        ab_search(&q, &lx, &ly, &ok);
        if (!ok) { logf_("[try] dimv v=%d no position", v); return; }
        r57_create_edge_dim(part, drw, own, idA, idB, 1, h1, h2, do_, lx, ly, 0.0, nm);
        return;
    }
    if (what == 2) {
        /* 总厚变体�?=tvi/VERT/[z法向平面@zmin �?凸点顶面] 1=svi/HORIZ/[同] 2=svi/HORIZ/[侧壁 �?凸点顶面] */
        if (!r57_find_faces(part, R57_WHAT_THICK, &idA, &idB, desc, sizeof(desc))) { logf_("[try] thick find failed: %s", desc); return; }
        logf_("[try] thick v=%d %s", v, desc);
        if (v == 2) {
            /* 复用之前失败的组合：�?idA 换成 z 下极值处 footprint 最大的任意面（=侧壁面） */
            int id, best = -1;
            double bf = -1.0;
            ab_bbox_t bb;

            ab_bbox_get(part, &bb);
            for (id = 100; id <= 6000; id++) {
                ProSurface s = NULL;
                ProGeomitemdata *gd = NULL;

                if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
                if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) { if (gd) ProGeomitemdataFree(&gd); continue; }
                {
                    double ex = fabs(gd->data.p_surface_data->xyz_max[0] - gd->data.p_surface_data->xyz_min[0]);
                    double ey = fabs(gd->data.p_surface_data->xyz_max[1] - gd->data.p_surface_data->xyz_min[1]);
                    double foot = ex > ey ? ex : ey;

                    if (fabs(gd->data.p_surface_data->xyz_min[2] - bb.z0) < 0.05 && foot > bf) { best = id; bf = foot; }
                }
                ProGeomitemdataFree(&gd);
            }
            if (best > 0) idA = best;
            logf_("[try] thick v=2 use wall id=%d", idA);
        }
        if (v == 0) {
            if (tvi < 0) { logf_("[try] no top view"); return; }
            own = &g_abv[tvi];
            sprintf_s(nm, sizeof(nm), "trythk%d", v);
            memset(&q, 0, sizeof(q));
            q.orient = 0; q.dir1 = -1; q.dir2 = -1; q.ownvi = tvi;
            q.gy0 = own->ocy - 8.0; q.gy1 = own->ocy + 8.0;
            q.gx0 = own->rect.x1 + 6.0; q.gx1 = own->rect.x1 + 70.0;
            ab_search(&q, &lx, &ly, &ok);
            if (!ok) { logf_("[try] thick v=0 no position"); return; }
            r57_create_face_dim(part, drw, own, idA, idB, 0, lx, ly, 0.0, nm);
            return;
        }
        if (svi < 0) { logf_("[try] no side view"); return; }
        own = &g_abv[svi];
        sprintf_s(nm, sizeof(nm), "trythk%d", v);
        memset(&q, 0, sizeof(q));
        q.orient = 1; q.dir1 = -1; q.dir2 = -1; q.ownvi = svi;
        q.gx0 = own->ocx - 14.0; q.gx1 = own->ocx + 14.0;
        q.gy0 = own->rect.y0 - 60.0; q.gy1 = own->rect.y1 + 60.0;
        ab_search(&q, &lx, &ly, &ok);
        if (!ok) { logf_("[try] thick v=%d no position", v); return; }
        r57_create_face_dim(part, drw, own, idA, idB, 1, lx, ly, 0.0, nm);
        return;
    }
    logf_("[try] unknown what=%d", what);
}

static void autobuild_run(ProMdl part, ProMdl drw, const wchar_t *mode)
{
    int do_all = 1, do636 = 1, do411 = 1;
    int fvi = -1, svi = -1, tvi = -1, i;

    if (mode[0] == L'6') { do_all = 0; do411 = 0; }
    else if (mode[0] == L'4') { do_all = 0; do636 = 0; }

    /* r59 (user: 4 -> 4.0): enable trailing_zeros at RUNTIME (drawing display only; metric.dtl untouched) */
    {
        ProName opt; ProLine val, rd;
        wcsncpy_s(opt, PRO_NAME_SIZE, L"trailing_zeros", _TRUNCATE);
        wcsncpy_s(val, PRO_LINE_SIZE, L"yes", _TRUNCATE);
        logf_("[dec] drawing option trailing_zeros=yes -> %d", (int)ProDrawingSetupOptionSet((ProDrawing)drw, opt, val));
        rd[0] = 0;
        logf_("[dec] trailing_zeros readback: %d v=%ls", (int)ProDrawingSetupOptionGet((ProDrawing)drw, opt, rd), rd);
    }
    ab_bbox_get(part, &g_abbb);
    logf_("[auto] bbox x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]",
          g_abbb.x0, g_abbb.x1, g_abbb.y0, g_abbb.y1, g_abbb.z0, g_abbb.z1);
    ab_occ_reset();
    if (!ab_collect_views(drw)) { logf_("[auto] no views; abort"); return; }
    for (i = 0; i < g_abnv; i++) {
        if (g_abv[i].isfront) fvi = i;
        if (g_abv[i].isside) svi = i;
        if (g_abv[i].istop) tvi = i;
    }
    logf_("[auto] view indices: front=%d side=%d top=%d", fvi, svi, tvi);
    if (fvi < 0) { logf_("[auto] no front view; abort"); return; }
    ab_furn_add(557.0, 76.0, 990.0, 214.0, "titleblock");
    ab_furn_add(70.0, 96.0, 380.0, 214.0, "technote");   /* r58：技术要求块移到图框左下角（标题栏左侧） */
    ab_furn_add(93.0, 710.0, 547.0, 766.0, "toltable");
    logf_("[auto] furniture: titleblock x[557..990] y[76..214]; technote(LL) x[70..380] y[96..214]; toltable x[93..547] y[710..766]; sheet margin 20px");
    if (do_all) {
        /* 轮次55（用户裁定四类）：① 最大外�?63.6/41.1/4.0 �?凸点高度 0.25 �?凸点间隙 23/23.5/2
           + �?凸台十字中心线（间距尺寸一律锚在十字线上）�?.65 撤销（不属四类）�?*/
        boss_cross_dims(part, drw, fvi, 1);            /* 23（十字线锚定�?*/
        if (do636) ab_dim_636(part, drw, fvi);
        logf_("[auto] d965 REMOVED (r55 user ruling: right-edge notch spacing is not one of the four categories)");
        boss_cross_dims(part, drw, fvi, 2);            /* 23.5（十字线锚定�? 凸点竖向间距�?*/
        if (do411) ab_dim_411(part, drw, fvi);
        ab_dim_conv(part, drw, 2.0, 0, 6, 1, -1, 0, 34.0, fvi, "d2");
        /* 轮次57：① 总厚改为特征驱动（z 极值两端的"�?直接作附件）——不再按值搜线对�?
           �?d40（route5 值搜）注释保留：值搜在凸点尖（球�?torus）一侧无线边可配，故一直失败�?*/
        if (svi >= 0) r57_feature_dim(part, drw, R57_WHAT_THICK, svi, 1, 8.0, 0.0, "d40");
        else logf_("[auto] d40 skipped (no side view)");
        /* r60: format-override the total thickness text to "4.0" (value token preserved) */
        r60_override_thick_text(drw);
        /* 轮次57：③ 凸点竖向间距（相邻两枚凸点轮廓弧圆心距；特征驱动�?*/
        r57_feature_dim(part, drw, R57_WHAT_DIMV, fvi, 0, 10.0, 0.05, "dDIMV");
        /* 轮次57：② 凸点高度（凸点顶�?�?其依附的基准面；值由 Creo 算）——替换旧�?0.25 值搜尺寸
           （旧 0.25 = 两层 0.25 薄片的台阶厚，锚错了特征；本行由 d025 改为特征驱动�?*/
        if (tvi >= 0) r57_feature_dim(part, drw, R57_WHAT_DIMPLE, tvi, 0, 6.0, 0.05, "dDIMPLE");
        else logf_("[auto] dDIMPLE skipped (no top view)");
        /* 轮次58（用户要�?料厚 0.25 也要�?）：�?料厚 —�?料厚台阶边对（特征驱动），无公差 */
        if (tvi >= 0) r57_feature_dim(part, drw, R57_WHAT_SHEET, tvi, 0, 6.0, 0.0, "d025s");
        else logf_("[auto] d025s skipped (no top view)");
        /* 轮次58：顶部视图两�?z 尺寸的竖排文字会互压（各 60px 高、同�?10px 条带）—�?
           �?0.35 的文字沿其竖直尺寸线上移 34px（ProDrawingDimensionMove 沿向 = 只动文字，界线不动）�?*/
        r58_slide_dimple_text(drw, 34.0, tvi);
    } else if (!do636) {
        ab_dim_411(part, drw, fvi);
    } else {
        ab_dim_636(part, drw, fvi);
    }
    ab_b15_pass(drw);   /* r51 裁定2：B15 终检归零（文字滑动） */
    logf_("[auto] AUTOBUILD done");
}

/* ===== 轮次46：符号库标记（yat/jizhun）—�?只读定义读取 + 变量文本 + 试验实例 ===== */
static void sym_dump_vartexts(ProDtlsyminstdata data, const char *tag)
{
    ProDtlvartext *vts = NULL;
    ProError       e = ProDtlsyminstdataVartextsCollect(data, &vts);

    logf_("[sym] vartextsCollect(%s) -> %d (%s)", tag, (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR && vts) {
        int n = 0, i;

        ProArraySizeGet((ProArray)vts, &n);
        logf_("[sym] vartext count=%d", n);
        for (i = 0; i < n; i++) {
            ProLine pr, va;
            char    a8[PRO_LINE_SIZE * 4], b8[PRO_LINE_SIZE * 4];

            pr[0] = 0; va[0] = 0;
            if (ProDtlvartextDataGet(vts[i], pr, va) == PRO_TK_NO_ERROR) {
                wide_to_u8(pr, a8, sizeof(a8));
                wide_to_u8(va, b8, sizeof(b8));
                logf_("[sym] vt[%d] prompt='%s' value='%s'", i, a8, b8);
            }
        }
        ProArrayFree((ProArray *)&vts);
    }
}

static void symvar_run(ProMdl drw, int idx)
{
    ProDtlsyminst *insts = NULL;
    int cnt = 0;

    if (ProDrawingDtlsyminstsCollect((ProDrawing)drw, PRO_VALUE_UNUSED, &insts) != PRO_TK_NO_ERROR || !insts) {
        logf_("[symvar] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)insts, &cnt);
    logf_("[symvar] count=%d request=%d", cnt, idx);
    if (idx < 0 || idx >= cnt) { logf_("[symvar] index out of range"); ProArrayFree((ProArray *)&insts); return; }
    {
        ProDtlsyminstdata d = NULL;
        ProDtlsymdef      def;
        ProDtlsymdefdata  dd = NULL;
        ProName           nm, path;
        ProLineEnvelope   env;
        int               id = -1;
        double            h = -1.0, dh = -1.0;
        ProDtlsymdefattachType at = (ProDtlsymdefattachType)0;

        nm[0] = 0; path[0] = 0;
        if (ProDtlsyminstDataGet(&insts[idx], PRODISPMODE_NUMERIC, &d) != PRO_TK_NO_ERROR || !d) {
            logf_("[symvar] dataGet failed");
            ProArrayFree((ProArray *)&insts);
            return;
        }
        ProDtlsyminstdataIdGet(d, &id);
        if (ProDtlsyminstdataDefGet(d, &def) == PRO_TK_NO_ERROR &&
            ProDtlsymdefDataGet(&def, &dd) == PRO_TK_NO_ERROR && dd) {
            ProDtlsymdefdataNameGet(dd, nm);
            ProDtlsymdefdataPathGet(dd, path);
            ProDtlsymdefdataScaledheightGet(dd, &dh);
        }
        ProDtlsyminstdataScaledheightGet(d, &h);
        ProDtlsyminstdataAttachtypeGet(d, &at);
        env[0][0] = env[0][1] = -9999.0; env[3][0] = env[3][1] = -9999.0;
        ProDtlsyminstEnvelopeGet(&insts[idx], env);
        {
            char n8[256], p8[PRO_PATH_SIZE * 2];

            wide_to_u8(nm, n8, sizeof(n8));
            wide_to_u8(path, p8, sizeof(p8));
            logf_("[symvar] idx=%d id=%d name='%s' path='%s' attachtype=%d defHeight=%.3f instHeight=%.3f env=(%.1f,%.1f)-(%.1f,%.1f)",
                 idx, id, n8, p8, (int)at, dh, h, env[0][0], env[0][1], env[3][0], env[3][1]);
        }
        {
            ProDtlattach  att = NULL;
            ProDtlattachType aty = (ProDtlattachType)0;
            ProView       av = NULL;
            ProVector     loc = { 0,0,0 };
            ProSelection  sel = NULL;

            if (ProDtlsyminstdataAttachmentGet(d, &att) == PRO_TK_NO_ERROR && att &&
                ProDtlattachGet(att, &aty, &av, loc, &sel) == PRO_TK_NO_ERROR) {
                logf_("[symvar] attach type=%d view=%p loc=(%.3f,%.3f,%.3f) sel=%p", (int)aty, (void *)av, loc[0], loc[1], loc[2], (void *)sel);
                if (sel) {
                    ProModelitem mi;
                    ProPoint3d   p = { -9999,-9999,-9999 };

                    memset(&mi, 0, sizeof(mi));
                    if (ProSelectionModelitemGet(sel, &mi) == PRO_TK_NO_ERROR)
                        logf_("[symvar] attach selitem type=%d id=%d owner=%p", (int)mi.type, (int)mi.id, (void *)mi.owner);
                    if (ProSelectionPoint3dGet(sel, p) == PRO_TK_NO_ERROR)
                        logf_("[symvar] attach point=(%.3f,%.3f,%.3f)", p[0], p[1], p[2]);
                }
            } else {
                logf_("[symvar] attachment get failed");
            }
        }
        sym_dump_vartexts(d, "symvar");
    }
    ProArrayFree((ProArray *)&insts);
}

/* SYMHEIGHT|<index>|<height>: change one symbol instance's scaled height.
   Needed because instHeight is fixed at creation time and does NOT follow
   the drawing text_height afterwards: placing a roughness symbol while
   text_height=5.0 yields instHeight 44.964 instead of the genuine 31.475
   (44.964/31.475 = 5.0/3.5). */
static void symheight_run(ProMdl drw, int idx, double h)
{
    ProDtlsyminst *insts = NULL;
    int cnt = 0;

    if (ProDrawingDtlsyminstsCollect((ProDrawing)drw, PRO_VALUE_UNUSED, &insts) != PRO_TK_NO_ERROR || !insts) {
        logf_("[symheight] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)insts, &cnt);
    logf_("[symheight] count=%d request=%d targetHeight=%.3f", cnt, idx, h);
    if (idx < 0 || idx >= cnt) { logf_("[symheight] index out of range"); ProArrayFree((ProArray *)&insts); return; }
    {
        ProDtlsyminstdata d = NULL;
        ProLineEnvelope   env0;
        double            h0 = -1.0, h1 = -1.0;

        if (ProDtlsyminstDataGet(&insts[idx], PRODISPMODE_NUMERIC, &d) != PRO_TK_NO_ERROR || !d) {
            logf_("[symheight] dataGet failed");
            ProArrayFree((ProArray *)&insts);
            return;
        }
        env0[0][0] = env0[0][1] = -9999; env0[3][0] = env0[3][1] = -9999;
        ProDtlsyminstEnvelopeGet(&insts[idx], env0);
        ProDtlsyminstdataScaledheightGet(d, &h0);
        logf_("[symheight] before instHeight=%.3f bbox=(%.2f,%.2f)-(%.2f,%.2f)",
              h0, env0[0][0], env0[0][1], env0[3][0], env0[3][1]);
        {
            ProError eh = ProDtlsyminstdataScaledheightSet(d, h);
            logf_("[symheight] scaledheightSet(%.3f) -> %d (%s)", h, (int)eh, errname(eh));
        }
        {
            ProError em = ProDtlsyminstModify(&insts[idx], d);
            logf_("[symheight] modify -> %d (%s)", (int)em, errname(em));
        }
        ProDtlsyminstDraw(&insts[idx]);
        ProDtlsyminstdataFree(d);
        /* read back */
        if (ProDtlsyminstDataGet(&insts[idx], PRODISPMODE_NUMERIC, &d) == PRO_TK_NO_ERROR && d) {
            ProDtlsyminstdataScaledheightGet(d, &h1);
            env0[0][0] = env0[0][1] = -9999; env0[3][0] = env0[3][1] = -9999;
            ProDtlsyminstEnvelopeGet(&insts[idx], env0);
            logf_("[symheight] AFTER instHeight=%.3f bbox=(%.2f,%.2f)-(%.2f,%.2f)",
                  h1, env0[0][0], env0[0][1], env0[3][0], env0[3][1]);
            ProDtlsyminstdataFree(d);
        }
    }
    ProArrayFree((ProArray *)&insts);
}
static void symmove_run(ProMdl drw, int idx, double x, double y)
{
    ProDtlsyminst *insts = NULL;
    int cnt = 0;

    if (ProDrawingDtlsyminstsCollect((ProDrawing)drw, PRO_VALUE_UNUSED, &insts) != PRO_TK_NO_ERROR || !insts) {
        logf_("[symmove] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)insts, &cnt);
    logf_("[symmove] count=%d request=%d target=(%.2f,%.2f)", cnt, idx, x, y);
    if (idx < 0 || idx >= cnt) { logf_("[symmove] index out of range"); ProArrayFree((ProArray *)&insts); return; }
    {
        ProDtlsyminstdata d = NULL;
        ProLineEnvelope   env0, env1;
        ProView           fv;
        ProVector         loc;
        ProDtlattach      att = NULL;

        if (ProDtlsyminstDataGet(&insts[idx], PRODISPMODE_NUMERIC, &d) != PRO_TK_NO_ERROR || !d) {
            logf_("[symmove] dataGet failed");
            ProArrayFree((ProArray *)&insts);
            return;
        }
        env0[0][0] = env0[0][1] = -9999; env0[3][0] = env0[3][1] = -9999;
        ProDtlsyminstEnvelopeGet(&insts[idx], env0);
        logf_("[symmove] before bbox=(%.2f,%.2f)-(%.2f,%.2f)", env0[0][0], env0[0][1], env0[3][0], env0[3][1]);
        fv = sym_front_view(drw);
        loc[0] = x; loc[1] = y; loc[2] = 0.0;
        if (!fv || ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) != PRO_TK_NO_ERROR || !att) {
            logf_("[symmove] attach alloc failed");
            ProArrayFree((ProArray *)&insts);
            return;
        }
        ProDtlsyminstdataAttachmentSet(d, att);
        ProDtlsyminstdataAttachtypeSet(d, PROSYMDEFATTACHTYPE_FREE);
        {
            ProError em = ProDtlsyminstModify(&insts[idx], d);
            logf_("[symmove] modify -> %d (%s)", (int)em, errname(em));
        }
        ProDtlsyminstDraw(&insts[idx]);
        logf_("[symmove] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
        env1[0][0] = env1[0][1] = -9999; env1[3][0] = env1[3][1] = -9999;
        ProDtlsyminstEnvelopeGet(&insts[idx], env1);
        logf_("[symmove] after bbox=(%.2f,%.2f)-(%.2f,%.2f)", env1[0][0], env1[0][1], env1[3][0], env1[3][1]);
    }
    ProArrayFree((ProArray *)&insts);
}

static ProView sym_front_view(ProMdl drw)
{
    ProView *views = NULL;
    ProView  fv = NULL;
    int      vc = 0, i;

    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) return NULL;
    ProArraySizeGet((ProArray)views, &vc);
    for (i = 0; i < vc; i++) {
        ProName nm;

        nm[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
        if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { fv = views[i]; break; }
    }
    ProArrayFree((ProArray *)&views);
    return fv;
}

/* ===== 轮次46：SYMPROBE —�?符号库定位探针（检索矩�?+ 调色板回退 + 变量文本 + 试验实例�?===== */
static void symprobe_run(ProMdl drw)
{
    ProDtlsymdef def;
    int          got = 0;

    memset(&def, 0, sizeof(def));
    {
        static const wchar_t *subs[4] = { L"yat", L".",
                                          L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\symbols\\yat",
                                          L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\symbols" };
        static const wchar_t *nms[2] = { L"jizhun", L"jizhun.sym" };
        int i, j, k;

        for (i = 0; i < 4 && !got; i++) {
            for (j = 0; j < 2 && !got; j++) {
                for (k = 0; k < 2 && !got; k++) {
                    ProError e;

                    memset(&def, 0, sizeof(def));
                    if (k == 0)
                        e = ProDrawingSystemDtlsymdefRetrieve(drw, (wchar_t *)subs[i], (wchar_t *)nms[j],
                                                              PRO_VALUE_UNUSED, PRO_B_TRUE, &def);
                    else
                        e = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)subs[i], (wchar_t *)nms[j],
                                                        1, PRO_B_TRUE, &def);
                    logf_("[sym] matrix fn=%s sub=%ls name=%ls ver=%s -> %d (%s)",
                          k == 0 ? "SYS" : "PLAIN", subs[i], nms[j], k == 0 ? "latest" : "1", (int)e, errname(e));
                    if (e == PRO_TK_NO_ERROR) got = 1;
                }
            }
        }
    }
    if (!got) {
        ProMdl  pal = NULL;
        ProError erp = ProMdlRetrieve((wchar_t *)L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\templates\\symbol_palette.drw",
                                      PRO_MDLFILE_DRAWING, &pal);

        logf_("[sym] palette retrieve -> %d (%s)", (int)erp, errname(erp));
        if (erp == PRO_TK_NO_ERROR && pal) {
            ProDtlsyminst *insts = NULL;
            ProError       ecol = ProDrawingDtlsyminstsCollect((ProDrawing)pal, PRO_VALUE_UNUSED, &insts);

            logf_("[sym] palette collect -> %d (%s)", (int)ecol, errname(ecol));
            if (ecol == PRO_TK_NO_ERROR && insts) {
                int n = 0, i;

                ProArraySizeGet((ProArray)insts, &n);
                logf_("[sym] palette instances = %d", n);
                for (i = 0; i < n; i++) {
                    ProDtlsyminstdata dd = NULL;

                    if (ProDtlsyminstDataGet(&insts[i], PRODISPMODE_NUMERIC, &dd) == PRO_TK_NO_ERROR && dd) {
                        ProDtlsymdef dx;
                        ProName      nm;

                        memset(&dx, 0, sizeof(dx));
                        nm[0] = 0;
                        if (ProDtlsyminstdataDefGet(dd, &dx) == PRO_TK_NO_ERROR) {
                            ProDtlsymdefdata ddx = NULL;

                            if (ProDtlsymdefDataGet(&dx, &ddx) == PRO_TK_NO_ERROR && ddx)
                                ProDtlsymdefdataNameGet(ddx, nm);
                        }
                        {
                            char a8[PRO_NAME_SIZE * 4];

                            wide_to_u8(nm, a8, sizeof(a8));
                            logf_("[sym] pal inst[%d] def='%s'", i, a8);
                            if (!got && _wcsicmp(nm, L"jizhun") == 0) {
                                def = dx; got = 1;
                                sym_dump_vartexts(dd, "palette-instance");
                            }
                        }
                    }
                }
                ProArrayFree((ProArray *)&insts);
            }
        }
    } else {
        logf_("[sym] def obtained via retrieve matrix");
    }
    if (!got) { logf_("[sym] *** def NOT FOUND (matrix + palette both failed) ***"); return; }
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) == PRO_TK_NO_ERROR && id) {
            ProDtlsyminstdataDefSet(id, &def);
            sym_dump_vartexts(id, "data-after-defset");
            {
                ProView     fv = sym_front_view(drw);
                ProVector   loc;
                ProDtlattach att = NULL;

                loc[0] = 120.0; loc[1] = 120.0; loc[2] = 0.0;
                if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                    ProDtlsyminstdataAttachmentSet(id, att);
                    ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
                }
                ProDtlsyminstdataScaledheightSet(id, 3.0);
                {
                    ProDtlsyminst inst;
                    ProError      ec;

                    memset(&inst, 0, sizeof(inst));
                    ec = ProDtlsyminstCreate(drw, id, &inst);
                    logf_("[sym] Create(temp @120,120) -> %d (%s)", (int)ec, errname(ec));
                    if (ec == PRO_TK_NO_ERROR) {
                        ProError es = ProAnnotationShow((ProAnnotation *)&inst, NULL, fv);

                        logf_("[sym] Show -> %d (%s)", (int)es, errname(es));
                        {
                            ProDtlsyminstdata cd = NULL;

                            if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd)
                                sym_dump_vartexts(cd, "instance");
                        }
                        {
                            ProDimension dim;
                            ProError     ed;

                            memset(&dim, 0, sizeof(dim));
                            ed = ProDtlsyminstDimattachGet(&inst, &dim);
                            logf_("[sym] DimattachGet -> %d (%s)", (int)ed, errname(ed));
                        }
                    }
                }
                if (att) ProDtlattachFree(&att);
            }
        }
    }
}

/* ===== 轮次53：SYMMARK —�?放置符号库标记（yat/b_sym v1，用户指�?�?�?符号）；
   变量名不预设：先读实例默�?vartext �?prompt，按同名设置编号值，并读回打印�?===== */
static void symmark_run(ProMdl drw, double x, double y, const wchar_t *value, double dimv)
{
    ProDtlsymdef def;
    ProError     er;
    wchar_t      prompt[PRO_LINE_SIZE];

    (void)dimv;
    prompt[0] = 0;
    wcscpy_s(prompt, PRO_LINE_SIZE, L"A");
    memset(&def, 0, sizeof(def));
    er = PRO_TK_E_NOT_FOUND;
    /* 轮次53：b_sym.sym 在库里没�?.N 版本后缀（文件头 VERS 0）⇒ 版本号要挨个试；
       jizhun 的用法（version=1）不适用于它。逐一打印尝试结果�?*/
    {
        /* r76: 多条 pro_symbol_dir 使短分类 'yat' 失效�?4）；优先用完整路径分类�?
           实测成功�?..\symbols\yat + b_sym�?..\symbols\yat\ + b_sym�?
           ...\symbols + yat\b_sym。短名仅作兜底�?*/
        struct { const wchar_t *cat; const wchar_t *nm; int ver; } cand[] = {
            { L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\symbols\\yat",  L"b_sym", 1 },
            { L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\symbols\\yat\\", L"b_sym", 1 },
            { L"D:\\PiCreoClient\\PiConfig\\CreoSTD\\symbols",       L"yat\\b_sym", 1 },
            { L"yat", L"b_sym", 1 }, { L"yat", L"b_sym", 0 }, { L"yat", L"b_sym", PRO_VALUE_UNUSED },
            { L"yat", L"b_sym.sym", 1 }, { L"yat", L"b_sym.sym", 0 }, { L"yat", L"b_sym.sym", PRO_VALUE_UNUSED },
            { L"yat", L"jizhun", 1 },                       /* 兜底：仍失败则用 jizhun 并如实记�?*/
        };
        int ci;

        for (ci = 0; ci < (int)(sizeof(cand) / sizeof(cand[0])); ci++) {
            memset(&def, 0, sizeof(def));
            er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)cand[ci].cat, (wchar_t *)cand[ci].nm, cand[ci].ver, PRO_B_TRUE, &def);
            logf_("[sym] retrieve %ls/%ls ver=%d -> %d (%s)", cand[ci].cat, cand[ci].nm, cand[ci].ver, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR) break;
        }
    }
    if (er != PRO_TK_NO_ERROR) { logf_("[sym] *** no symbol definition could be retrieved ***"); return; }
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) != PRO_TK_NO_ERROR || !id) { logf_("[sym] data alloc failed"); return; }
        ProDtlsyminstdataDefSet(id, &def);
        /* 先读符号自带的变量（默认 prompt/value），确定要设置哪个变�?*/
        {
            ProDtlvartext *vts = NULL;
            ProError       ev = ProDtlsyminstdataVartextsCollect(id, &vts);

            logf_("[sym] [b_sym] vartextsCollect -> %d (%s)", (int)ev, errname(ev));
            if (ev == PRO_TK_NO_ERROR && vts) {
                int n = 0, i;

                ProArraySizeGet((ProArray)vts, &n);
                logf_("[sym] [b_sym] default vartext count=%d", n);
                for (i = 0; i < n; i++) {
                    ProLine pr, va;
                    char    a8[PRO_LINE_SIZE * 4], b8[PRO_LINE_SIZE * 4];

                    pr[0] = 0; va[0] = 0;
                    if (ProDtlvartextDataGet(vts[i], pr, va) == PRO_TK_NO_ERROR) {
                        wide_to_u8(pr, a8, sizeof(a8));
                        wide_to_u8(va, b8, sizeof(b8));
                        logf_("[sym] [b_sym] def-vt[%d] prompt='%s' value='%s'", i, a8, b8);
                        if (i == 0 && pr[0]) wcscpy_s(prompt, PRO_LINE_SIZE, pr);   /* 第一个变量名 = 编号�?*/
                    }
                }
                /* 不释放（ProWstringArrayFree/…Free 实测 fatal�?*/
            }
        }
        if (value && value[0]) {
            /* 轮次53：b_sym 的变量名是数�?'1'（实测读�?vt[0] prompt='1'），jizhun �?'A'�?
               变量名无法从定义里读到（def vartextsCollect 返回 -4）⇒ 对候选变量名逐个加值，
               不匹配的多余项被忽略（实�?Add 返回 0、显示只认定义里那个变量）�?*/
            static const wchar_t *prompts[4] = { L"1", L"A", L"b", L"B" };
            int pi;

            for (pi = 0; pi < 4; pi++) {
                ProDtlvartext vt = NULL;
                char          p8[PRO_LINE_SIZE * 4];

                wide_to_u8((wchar_t *)prompts[pi], p8, sizeof(p8));
                if (ProDtlvartextAlloc((wchar_t *)prompts[pi], (wchar_t *)value, &vt) == PRO_TK_NO_ERROR && vt) {
                    ProError ea = ProDtlsyminstdataVartextAdd(id, vt);

                    logf_("[sym] vartextAdd %s='%ls' -> %d (%s)", p8, value, (int)ea, errname(ea));
                    /* �?Free：Add 之后由实例持有（实测 Free 后再 Create 异常退出） */
                } else {
                    logf_("[sym] vartext alloc failed (prompt %s)", p8);
                }
            }
        }
        {
            ProView     fv = sym_front_view(drw);
            ProVector   loc;
            ProDtlattach att = NULL;

            loc[0] = x; loc[1] = y; loc[2] = 0.0;
            if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                ProDtlsyminstdataAttachmentSet(id, att);
                ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
                logf_("[sym] attach FREE @(%.1f,%.1f) ok", x, y);
            } else {
                logf_("[sym] attach alloc failed (fv=%d)", fv ? 1 : 0);
            }
            {
                ProError eh = ProDtlsyminstdataScaledheightSet(id, 3.0);

                logf_("[sym] heightSet 3.0 -> %d (%s)", (int)eh, errname(eh));
            }
            {
                ProDtlsyminst inst;
                ProError      ec;

                memset(&inst, 0, sizeof(inst));
                ec = ProDtlsyminstCreate(drw, id, &inst);
                logf_("[sym] create @(%.1f,%.1f) -> %d (%s)", x, y, (int)ec, errname(ec));
                if (ec == PRO_TK_NO_ERROR) {
                    ProError e1 = ProAnnotationShow((ProAnnotation *)&inst, NULL, fv);

                    logf_("[sym] show(annotation) -> %d (%s)", (int)e1, errname(e1));
                    if (e1 != PRO_TK_NO_ERROR) {
                        ProError e2 = ProDtlsyminstShow(&inst);

                        logf_("[sym] show(syminst) -> %d (%s)", (int)e2, errname(e2));
                    }
                    {
                        ProDtlsyminstdata cd = NULL;

                        if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd)
                            sym_dump_vartexts(cd, "mark");
                    }
                    {
                        ProDimension dim;
                        ProError     ed;

                        memset(&dim, 0, sizeof(dim));
                        ed = ProDtlsyminstDimattachGet(&inst, &dim);
                        logf_("[sym] dimattachGet -> %d (%s)", (int)ed, errname(ed));
                    }
                }
            }
            /* �?ProDtlattachFree(&att)：附件由实例数据持有，Free 后再操作会异常中�?*/
        }
        /* �?ProDtlsyminstdataFree(id)：创建后实例持有数据（与 D2 工作流一致，避免悬空�?*/
    }
}

/* ===== 轮次46：SYMARKS —�?重要控制项标记（编号依次递增 1/2；放�?regen+repaint�?===== */
typedef struct {
    double     target;
    ProDrawing drw;
    double     ax, ay, bx, by;   /* 尺寸线（箭头位置�?*/
    double     tx, ty;           /* 文字位置（ProDimlocationTextGet；与箭头同系的模型坐标） */
    double     cx, cy;           /* 轮次52：文字框中心（ProDrawingDimensionPosGet 读回�?*/
    int        has_center;       /* 1 = cx/cy 可信 */
    int        has_arrow, has_text;
    int        orient;           /* 1=横排（文字在线上方向右） 0=竖排（文字在线左侧向上） */
    int        nchars;           /* 尺寸文字字符数（轮次52：按显示文本=�?公差 计算�?*/
    int        use_rect;         /* 1 = 仅接受箭头落�?rect(±tol) 内的实例（避开隐藏的模型尺寸实例） */
    ab_rect_t  rect;
    double     tol;
    /* 轮次57：特征驱动尺寸的值由 Creo 算（事先不知道），符号定位改用【属性】匹配：
       match_tol=1 �?只收"±值为 tolval 的对称公�?尺寸；use_orient �?再按尺寸线方向过滤�?*/
    int        match_tol;
    double     tolval;
    int        use_orient, match_orient;
} symdim_ctx_t;

static ProError symmark_dim_cb(ProDimension *dim, ProError status, ProAppData app)
{
    symdim_ctx_t *c = (symdim_ctx_t *)app;
    double        v = 0.0;

    (void)status;
    if (c->has_arrow && c->has_text) return PRO_TK_NO_ERROR;
    if (c->match_tol) {
        ProDimToleranceType tt = PRO_TOL_DEFAULT;
        double up = 0.0, lo = 0.0;

        if (ProDimensionToltypeGet(dim, &tt) != PRO_TK_NO_ERROR || tt != PRO_TOL_PLUS_MINUS_SYM)
            return PRO_TK_NO_ERROR;
        if (ProDimensionToleranceGet(dim, &up, &lo) != PRO_TK_NO_ERROR)
            return PRO_TK_NO_ERROR;
        if (fabs(fabs(up) - c->tolval) > 0.001) return PRO_TK_NO_ERROR;
    } else {
        if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
        if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    }
    {
        ProDimlocation loc = NULL;

        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc) {
            ProPoint3d a1 = { -9999.0, -9999.0, -9999.0 }, a2 = { -9999.0, -9999.0, -9999.0 };
            ProPoint3d tp = { -9999.0, -9999.0, -9999.0 };
            ProBoolean heb = PRO_B_FALSE;
            double     ebl = 0.0;

            ProDimlocationArrowsGet(loc, a1, a2);
            if (c->use_rect) {
                double d1 = ab_pt_rect_dist(a1[0], a1[1], &c->rect);
                double d2 = ab_pt_rect_dist(a2[0], a2[1], &c->rect);

                if (d1 > c->tol && d2 > c->tol) {
                    logf_("[sym]  dim v=%.4f skipped (arrows (%.1f,%.1f)/(%.1f,%.1f) far from view)", v, a1[0], a1[1], a2[0], a2[1]);
                    return PRO_TK_NO_ERROR;
                }
            }
            {
                int o = (fabs(a1[0] - a2[0]) >= fabs(a1[1] - a2[1])) ? 1 : 0;

                if (c->use_orient && o != c->match_orient) {
                    logf_("[sym]  dim skipped: orient=%d wanted %d (arrows (%.1f,%.1f)/(%.1f,%.1f))", o, c->match_orient, a1[0], a1[1], a2[0], a2[1]);
                    return PRO_TK_NO_ERROR;
                }
            }
            c->ax = a1[0]; c->ay = a1[1]; c->bx = a2[0]; c->by = a2[1];
            c->has_arrow = 1;
            /* 主轴判据：箭头间的较大分量方�?= 尺寸线方向（微距尺寸 Δx/Δy 都小时亦可靠�?*/
            c->orient = (fabs(c->ax - c->bx) >= fabs(c->ay - c->by)) ? 1 : 0;
            logf_("[sym]  [iso] v=%.4f arrows ok (%.1f,%.1f)/(%.1f,%.1f) orient=%d; calling textGet", v, c->ax, c->ay, c->bx, c->by, c->orient);
            {
                ProError et = ProDimlocationTextGet(loc, &heb, tp, &ebl);

                logf_("[sym]  [iso] textGet -> %d (%s) pnt=(%.1f,%.1f) elbow=%d", (int)et, errname(et), tp[0], tp[1], (int)heb);
                if (et == PRO_TK_NO_ERROR) {
                    c->tx = tp[0]; c->ty = tp[1];
                    c->has_text = 1;
                }
            }
            logf_("[sym]  [iso] calling wstringsGet");
            /* 轮次52：字符数改用"显示文本"长度（API �?wstrings/TextGet 都是格式�?{0:@D}，不能用�?*/
            {
                char dbuf[64];

                if (ab_dim_display_len(dim, dbuf, sizeof(dbuf)) > 0) {
                    c->nchars = (int)strlen(dbuf);
                    logf_("[sym]  [iso] display text='%s' chars=%d", dbuf, c->nchars);
                }
            }
            /* 轮次52：文字中心（ProDrawingDimensionPosGet 文档�?文字框中�?；与 TextGet 点比对合理性） */
            {
                ProVector pos;

                if (ProDrawingDimensionPosGet((ProDrawing)c->drw, dim, pos) == PRO_TK_NO_ERROR) {
                    double dlt = fabs(pos[0] - c->tx) + fabs(pos[1] - c->ty);

                    logf_("[sym]  [iso] posGet=(%.1f,%.1f,%.1f) vs textPnt=(%.1f,%.1f) delta=%.1f", pos[0], pos[1], pos[2], c->tx, c->ty, dlt);
                    if (dlt < 80.0) { c->cx = pos[0]; c->cy = pos[1]; c->has_center = 1; }
                }
            }
            if (c->nchars <= 0) c->nchars = (c->target == 0.25) ? 9 : 6;
            logf_("[sym]  dim v=%.4f arrows (%.1f,%.1f)/(%.1f,%.1f) textPnt (%.1f,%.1f) center=(%.1f,%.1f) hasC=%d elbow=%d orient=%d chars=%d",
                  v, c->ax, c->ay, c->bx, c->by, c->tx, c->ty, c->cx, c->cy, c->has_center, (int)heb, c->orient, c->nchars);
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 位置 = 文字框周�?可用空间搜索"�?
   轮次52（DSH 裁定）：目标改为"紧贴"——距�?�?1×TH，取最小可行；方向顺文字排�?
   （横�?�?右方；竖�?�?下方优先、其次右方）。文字框�?中心 + 真尺�?建模�?
   字符�?0.672×TH（渲染实测标定），长�?= 真显示字符数（�?公差，不再用格式串）�?*/
static void sym_place_mark(ProMdl drw, const symdim_ctx_t *c, int ownvi, const wchar_t *value, double TH, const char *name, const char *own_tag)
{
    ab_rect_t tb;
    double    cw  = TH * 0.672;                    /* 字符宽（渲染实测标定�?*/
    double    w   = (double)c->nchars * cw;        /* 文字长度 */
    double    mw  = 10.0, mh = 13.0;               /* 符号本体半宽/半高（渲染实测标定的几何量） */
    double    dmin = 2.0, dmax = TH;               /* 轮次52：紧贴区�?[2px, 1×TH] */
    double    cxc, cyc;                            /* 文字框中�?*/
    int       best_valid = 0;
    double    bx2 = 0.0, by2 = 0.0, bd = 1e9, bscore = 1e9;

    /* 轮次52：中心优先取 PosGet（文档：文字框中心）；不可信时退�?TextGet �?+ 对称外扩 */
    if (c->has_center) { cxc = c->cx; cyc = c->cy; }
    else               { cxc = c->tx; cyc = c->ty; }
    if (c->orient == 1) {
        tb.x0 = cxc - w / 2.0 - 2.0; tb.x1 = cxc + w / 2.0 + 2.0;
        tb.y0 = cyc - TH / 2.0 - 2.0; tb.y1 = cyc + TH / 2.0 + 2.0;
    } else {
        tb.x0 = cxc - TH / 2.0 - 2.0; tb.x1 = cxc + TH / 2.0 + 2.0;
        tb.y0 = cyc - w / 2.0 - 2.0;  tb.y1 = cyc + w / 2.0 + 2.0;
    }
    if (!c->has_center) {                          /* 无中心数据：按老约定外扩覆盖两�?*/
        if (c->orient == 1) { tb.y0 = c->ty - TH - 2.0; tb.y1 = c->ty + TH + 2.0; }
        else                { tb.x0 = c->tx - TH - 2.0; tb.x1 = c->tx + TH + 2.0; }
    }
    tb.tag = "text";
    logf_("[sym] %s textbox x[%.1f..%.1f] y[%.1f..%.1f] (pnt %.1f,%.1f center %.1f,%.1f hasC=%d orient=%d chars=%d TH=%.1f len=%.1f)",
          name, tb.x0, tb.x1, tb.y0, tb.y1, c->tx, c->ty, cxc, cyc, c->has_center, c->orient, c->nchars, TH, w);
    logf_("[sym] %s tight rule: valid gap [%.1f..%.1f]px (2px..1xTH=%.1fpx)", name, dmin, dmax, TH);
    {
        static const int    DX[8] = { 1, 0, -1, 0, 1, 1, -1, -1 };
        static const int    DY[8] = { 0, 1, 0, -1, -1, 1, 1, -1 };
        static const double OFF[6] = { 2.0, 3.0, 4.0, 5.0, 6.0, 8.0 };
        int di, oi;

        for (oi = 0; oi < 6; oi++) {
            for (di = 0; di < 8; di++) {
                double      mx, my, d, gd, g;
                ab_rect_t   mb;
                int         valid = 1, i;
                char        why[72];
                const char *who = "-";
                double      dirpen = 0.0;

                why[0] = 0;
                mx = (DX[di] > 0) ? tb.x1 + OFF[oi] + mw : ((DX[di] < 0) ? tb.x0 - OFF[oi] - mw : (tb.x0 + tb.x1) / 2.0);
                my = (DY[di] > 0) ? tb.y1 + OFF[oi] + mh : ((DY[di] < 0) ? tb.y0 - OFF[oi] - mh : (tb.y0 + tb.y1) / 2.0);
                mb.x0 = mx - mw; mb.x1 = mx + mw; mb.y0 = my - mh; mb.y1 = my + mh; mb.tag = "mk";
                d = ab_rect_gap(&mb, &tb);
                if (d < dmin - 0.01 || d > dmax + 0.01) {
                    valid = 0; sprintf_s(why, sizeof(why), "gap %.1f outside [%.1f..%.1f]", d, dmin, dmax);
                }
                /* 轮次52 方向偏好：横排→右方；竖排→下方优先、其次右�?*/
                if (c->orient == 1) {
                    if (!(DX[di] > 0 && DY[di] == 0)) dirpen = 3.0;
                } else {
                    if (DX[di] == 0 && DY[di] < 0) dirpen = 0.0;
                    else if (DX[di] > 0 && DY[di] == 0) dirpen = 1.0;
                    else dirpen = 3.0;
                }
                if (valid && (mb.x0 < 83.3 || mb.x1 > 964.0 || mb.y0 < 102.5 || mb.y1 > 738.2)) {
                    valid = 0; sprintf_s(why, sizeof(why), "off-sheet(margin10mm)");
                }
                if (valid) {
                    for (i = 0; i < g_abnv && valid; i++) {
                        g = ab_rect_gap(&mb, &g_abv[i].rect);
                        if (i == ownvi) {
                            /* 轮次54：只否决"深侵入零�?；浅侵入�?~25px）留给后面的 clamp-outside-part
                               做最小平移出界（这样"文字右方"这类紧贴候选能活下来）�?*/
                            if (g < -25.0) { valid = 0; sprintf_s(why, sizeof(why), "deep inside own view %.1f", g); }
                        } else if (g < 6.0) {
                            valid = 0; sprintf_s(why, sizeof(why), "close to view '%s' %.1f", g_abv[i].tag, g);
                        }
                    }
                }
                if (valid) {
                    g = ab_gap_furn(&mb, &who);
                    if (g < 6.0) { valid = 0; sprintf_s(why, sizeof(why), "close to '%s' %.1f", who, g); }
                }
                if (valid) {
                    /* 轮次54：跳�?符号自己标注的那根尺�?的矩形（DSH 裁定：靠近自身尺寸本来就该允许） */
                    gd = ab_gap_dims_ex(&mb, own_tag, &who);
                    if (gd < 6.0) { valid = 0; sprintf_s(why, sizeof(why), "close to dim '%s' %.1f", who, gd); }
                }
                logf_("[pos] %s cand dir=(%d,%d) off=%.0f loc=(%.1f,%.1f) gap=%.1f valid=%d %s",
                      name, DX[di], DY[di], OFF[oi], mx, my, d, valid, why[0] ? why : "-");
                if (valid) {
                    double score = d + dirpen;

                    if (score < bscore - 1e-6) { bscore = score; bd = d; bx2 = mx; by2 = my; best_valid = 1; }
                }
            }
        }
    }
    if (!best_valid) { logf_("[sym] %s *** no valid mark position ***", name); return; }
    /* 轮次54：若符号框压�?*零件轮廓**（outline，非其外�?4px），�?背离零件"的轴做最小平移使其出界；
       平移后重新校验其它约束（视图/家具/其它尺寸/图幅），过不了就退回原候选并如实报告�?*/
    {
        ab_rect_t own;
        double    ov;

        own.x0 = g_abv[ownvi].rect.x0 + 4.0; own.x1 = g_abv[ownvi].rect.x1 - 4.0;
        own.y0 = g_abv[ownvi].rect.y0 + 4.0; own.y1 = g_abv[ownvi].rect.y1 - 4.0;
        own.tag = g_abv[ownvi].tag;
        {
            ab_rect_t mb;

            mb.x0 = bx2 - mw; mb.x1 = bx2 + mw; mb.y0 = by2 - mh; mb.y1 = by2 + mh; mb.tag = "mk";
            ov = ab_rect_gap(&mb, &own);
        }
        if (ov < 0.5) {
            double shift = 0.0;
            ab_rect_t mb;

            mb.x0 = bx2 - mw; mb.x1 = bx2 + mw; mb.y0 = by2 - mh; mb.y1 = by2 + mh; mb.tag = "mk";
            if (cyc < (own.y0 + own.y1) / 2.0) shift = own.y0 - mb.y1 - 0.5;         /* 零件在上 �?符号下移 */
            else                               shift = own.y1 - mb.y0 + 0.5;         /* 零件在下 �?符号上移 */
            {
                double ny = by2 + shift, nx = bx2;
                ab_rect_t nb;
                int       ok = 1, i;
                const char *who = "-";

                nb.x0 = nx - mw; nb.x1 = nx + mw; nb.y0 = ny - mh; nb.y1 = ny + mh; nb.tag = "mk";
                if (nb.x0 < 83.3 || nb.x1 > 964.0 || nb.y0 < 102.5 || nb.y1 > 738.2) ok = 0;
                if (ok) for (i = 0; i < g_abnv && ok; i++) {
                    double g2 = ab_rect_gap(&nb, &g_abv[i].rect);

                    if (i == ownvi) { if (g2 < -4.0) ok = 0; }   /* 自身视图�?outline�?rect-4）判：出界即�?*/
                    else if (g2 < 6.0) ok = 0;
                }
                if (ok) { if (ab_gap_furn(&nb, &who) < 6.0) ok = 0; }
                if (ok) { if (ab_gap_dims_ex(&nb, own_tag, &who) < 6.0) ok = 0; }
                logf_("[sym] %s clamp-outside-part: overlap %.1fpx -> shift dy=%.1f to (%.1f,%.1f) %s",
                      name, ov, shift, nx, ny, ok ? "OK" : "*** rejected, keep original ***");
                if (ok) { by2 = ny; }
            }
        }
    }
    logf_("[sym] %s chosen (%.1f,%.1f) sym-to-text gap=%.1fpx (%.2fmm, rule <=1xTH=%.1fpx): OK",
          name, bx2, by2, bd, bd / 2.3805, TH);
    logf_("[sym] %s measured endpoints: text box edge (%.1f,%.1f) -> symbol edge (%.1f,%.1f)",
          name, (bx2 > (tb.x0 + tb.x1) / 2.0) ? tb.x1 : tb.x0, cyc, (bx2 > (tb.x0 + tb.x1) / 2.0) ? (bx2 - mw) : (bx2 + mw), by2);
    symmark_run(drw, bx2, by2, value, 0.0);
    logf_("[sym] %s final plan distance sym-to-text = %.1f px", name, bd);
}

static void symmarks_run(ProMdl part, ProMdl drw)
{
    symdim_ctx_t c2, cdv, cdp;
    double       TH;
    int          fvi = -1, tvi = -1, i;

    (void)part;
    logf_("[sym] ==== SYMARKS v2 (text-anchored placement; number by order) ====");
    {
        ProLine val;
        char    b8[64];
        double  mm = 0.0;

        val[0] = 0;
        if (ProDrawingSetupOptionGet((ProDrawing)drw, (wchar_t *)L"draw_text_height", val) == PRO_TK_NO_ERROR && val[0]) {
            wide_to_u8(val, b8, sizeof(b8));
            mm = atof(b8);
            if (mm > 0.5 && mm < 20.0) { TH = mm * 2.3805; logf_("[sym] draw_text_height='%s' -> TH=%.1f px", b8, TH); }
            else { TH = 3.5 * 2.3805; logf_("[sym] draw_text_height='%s' unusable -> TH=%.1f px (fallback)", b8, TH); }
        } else {
            TH = 3.5 * 2.3805;
            logf_("[sym] draw_text_height unavailable -> TH=%.1f px (fallback 3.5mm)", TH);
        }
    }
    memset(&c2, 0, sizeof(c2));
    c2.target = 2.0; c2.drw = (ProDrawing)drw;
    c2.use_orient = 1; c2.match_orient = 1;          /* 横排 2±0.05 �?mark#1 */
    memset(&cdv, 0, sizeof(cdv));
    cdv.match_tol = 1; cdv.tolval = 0.05; cdv.drw = (ProDrawing)drw;
    cdv.use_orient = 1; cdv.match_orient = 0;        /* 竖排 ±0.05（前视图�? 凸点竖向间距 �?mark#2 */
    memset(&cdp, 0, sizeof(cdp));
    cdp.match_tol = 1; cdp.tolval = 0.05; cdp.drw = (ProDrawing)drw;
    cdp.use_orient = 1; cdp.match_orient = 0;        /* 竖排 ±0.05（顶部视图）= 凸点高度 �?mark#3 */
    for (i = 0; i < g_abnv; i++) {
        if (g_abv[i].isfront) fvi = i;
        if (g_abv[i].istop) tvi = i;
    }
    if (fvi >= 0) {
        c2.use_rect = 1; c2.rect = g_abv[fvi].rect; c2.rect.y0 -= 55.0; c2.rect.y1 += 55.0; c2.rect.x1 += 90.0; c2.tol = 6.0;
        cdv.use_rect = 1; cdv.rect = g_abv[fvi].rect;
        cdv.rect.x0 -= 90.0; cdv.rect.y0 -= 60.0; cdv.rect.x1 += 90.0; cdv.rect.y1 += 60.0; cdv.tol = 8.0;
    }
    if (tvi >= 0) {
        cdp.use_rect = 1; cdp.rect = g_abv[tvi].rect;
        cdp.rect.x0 -= 60.0; cdp.rect.y0 -= 60.0; cdp.rect.x1 += 60.0; cdp.rect.y1 += 60.0; cdp.tol = 8.0;
    }
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, symmark_dim_cb, NULL, (ProAppData)&c2);
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, symmark_dim_cb, NULL, (ProAppData)&cdv);
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, symmark_dim_cb, NULL, (ProAppData)&cdp);
    logf_("[sym] dim(2.0,f) arrow=%d text=%d orient=%d | dim(pitch,f) arrow=%d text=%d orient=%d | dim(dimple-h,t) arrow=%d text=%d orient=%d",
          c2.has_arrow, c2.has_text, c2.orient, cdv.has_arrow, cdv.has_text, cdv.orient,
          cdp.has_arrow, cdp.has_text, cdp.orient);
    if (c2.has_arrow && c2.has_text) sym_place_mark(drw, &c2, fvi, L"1", TH, "mark#1", "d2");
    else logf_("[sym] mark#1 SKIP (dim data incomplete)");
    if (cdv.has_arrow && cdv.has_text && fvi >= 0) sym_place_mark(drw, &cdv, fvi, L"2", TH, "mark#2", "dDIMV");
    else logf_("[sym] mark#2 SKIP (dim data incomplete)");
    if (cdp.has_arrow && cdp.has_text && tvi >= 0) sym_place_mark(drw, &cdp, tvi, L"3", TH, "mark#3", "dDIMPLE");
    else logf_("[sym] mark#3 SKIP (dim data incomplete)");
    {
        ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);
        ProError ew = ProWindowRepaint(-1);

        logf_("[sym] final regen=%d (%s) repaint=%d (%s)", (int)eg, errname(eg), (int)ew, errname(ew));
    }
    logf_("[sym] SYMARKS done");
}

/* ===== 轮次46：SYMDBG|x|y —�?符号可见性诊断（定义信息/可见性标�?Draw/Regen 后查看） ===== */
static void symdbg_run(ProMdl drw, double x, double y)
{
    ProDtlsymdef def;
    ProError     er;

    memset(&def, 0, sizeof(def));
    er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)L"yat", (wchar_t *)L"jizhun", 1, PRO_B_TRUE, &def);
    logf_("[sym] dbg retrieve v1 -> %d (%s)", (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR) return;
    {
        ProDtlsymdefdata dd = NULL;

        if (ProDtlsymdefDataGet(&def, &dd) == PRO_TK_NO_ERROR && dd) {
            ProName nm;
            ProPath pp;
            double  h = -1.0;
            ProDtlsymdefdataHeighttype ht = 0;
            char    a8[PRO_NAME_SIZE * 4], b8[PRO_PATH_SIZE * 4];

            nm[0] = 0; pp[0] = 0;
            ProDtlsymdefdataNameGet(dd, nm);
            ProDtlsymdefdataPathGet(dd, pp);
            ProDtlsymdefdataHeightGet(dd, &h);
            ProDtlsymdefdataHeighttypeGet(dd, &ht);
            wide_to_u8(nm, a8, sizeof(a8));
            wide_to_u8(pp, b8, sizeof(b8));
            logf_("[sym] dbg def name='%s' path='%s' height=%.3f heighttype=%d", a8, b8, h, (int)ht);
            {
                ProDtlsymdefattach *atts = NULL;

                if (ProDtlsymdefdataAttachGet(dd, &atts) == PRO_TK_NO_ERROR && atts) {
                    int n = 0;

                    ProArraySizeGet((ProArray)atts, &n);
                    logf_("[sym] dbg def attach count=%d", n);
                    ProArrayFree((ProArray *)&atts);
                }
            }
        }
    }
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) != PRO_TK_NO_ERROR || !id) return;
        ProDtlsyminstdataDefSet(id, &def);
        {
            ProDtlvartext vt = NULL;

            if (ProDtlvartextAlloc((wchar_t *)L"A", (wchar_t *)L"9", &vt) == PRO_TK_NO_ERROR && vt)
                logf_("[sym] dbg vartextAdd -> %d (%s)", (int)ProDtlsyminstdataVartextAdd(id, vt), errname(PRO_TK_NO_ERROR));
        }
        {
            ProView     fv = sym_front_view(drw);
            ProVector   loc;
            ProDtlattach att = NULL;

            loc[0] = x; loc[1] = y; loc[2] = 0.0;
            if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                ProDtlsyminstdataAttachmentSet(id, att);
                ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
            }
            ProDtlsyminstdataScaledheightSet(id, 3.0);
            {
                double hs = -1.0, hg = -1.0;
                ProError es1 = ProDtlsyminstdataScaledheightGet(id, &hs);
                ProError es2 = ProDtlsyminstdataHeightGet(id, &hg);

                logf_("[sym] dbg height scaledGet(%d)=%.3f heightGet(%d)=%.3f", (int)es1, hs, (int)es2, hg);
            }
            {
                ProDtlsyminst inst;
                ProError      ec;

                memset(&inst, 0, sizeof(inst));
                ec = ProDtlsyminstCreate(drw, id, &inst);
                logf_("[sym] dbg create @(%.1f,%.1f) -> %d (%s)", x, y, (int)ec, errname(ec));
                if (ec == PRO_TK_NO_ERROR) {
                    {
                        ProError e1 = ProAnnotationShow((ProAnnotation *)&inst, NULL, fv);
                        ProError e2 = ProDtlsyminstShow(&inst);
                        ProError e3 = ProDtlsyminstDraw(&inst);

                        logf_("[sym] dbg show(ann)=%d show(inst)=%d draw=%d", (int)e1, (int)e2, (int)e3);
                    }
                    {
                        ProDtlsyminstdata cd = NULL;

                        if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd) {
                            ProBoolean disp = PRO_B_FALSE, inv = PRO_B_TRUE;

                            logf_("[sym] dbg isDisplayed -> %d (%d)", (int)ProDtlsyminstdataIsDisplayed(cd, &disp), (int)disp);
                            logf_("[sym] dbg isInvisible -> %d (%d)", (int)ProDtlsyminstdataIsInvisible(cd, &inv), (int)inv);
                        }
                    }
                    {
                        ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);

                        logf_("[sym] dbg regen -> %d (%s)", (int)eg, errname(eg));
                    }
                    {
                        ProError ew = ProWindowRepaint(-1);

                        logf_("[sym] dbg repaint -> %d (%s)", (int)ew, errname(ew));
                    }
                }
            }
            if (att) ProDtlattachFree(&att);
        }
    }
}

static void syminfo_run(ProMdl drw, const wchar_t *subdir, const wchar_t *name){
    ProDtlsymdef def;
    ProError     er;

    memset(&def, 0, sizeof(def));
    er = ProDrawingSystemDtlsymdefRetrieve(drw, (wchar_t *)subdir, (wchar_t *)name,
                                           PRO_VALUE_UNUSED, PRO_B_TRUE, &def);
    logf_("[sym] Retrieve %ls/%ls -> %d (%s)", subdir, name, (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR) return;
    {
        ProDtlsymdefdata dd = NULL;

        if (ProDtlsymdefDataGet(&def, &dd) == PRO_TK_NO_ERROR && dd) {
            ProName nm;
            ProPath pp;

            nm[0] = 0; pp[0] = 0;
            ProDtlsymdefdataNameGet(dd, nm);
            ProDtlsymdefdataPathGet(dd, pp);
            {
                char a8[PRO_NAME_SIZE * 4], b8[PRO_PATH_SIZE * 4];

                wide_to_u8(nm, a8, sizeof(a8));
                wide_to_u8(pp, b8, sizeof(b8));
                logf_("[sym] def name='%s' path='%s'", a8, b8);
            }
            {
                double                    h = -1.0;
                ProDtlsymdefdataHeighttype ht = 0;

                ProDtlsymdefdataHeightGet(dd, &h);
                ProDtlsymdefdataHeighttypeGet(dd, &ht);
                logf_("[sym] def height=%.3f heighttype=%d", h, (int)ht);
            }
            {
                ProDtlsymdefattach *atts = NULL;

                if (ProDtlsymdefdataAttachGet(dd, &atts) == PRO_TK_NO_ERROR && atts) {
                    int n = 0;

                    ProArraySizeGet((ProArray)atts, &n);
                    logf_("[sym] def attach count=%d", n);
                    ProArrayFree((ProArray *)&atts);
                } else {
                    logf_("[sym] def attach list empty/none");
                }
            }
        }
    }
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) == PRO_TK_NO_ERROR && id) {
            ProDtlsyminstdataDefSet(id, &def);
            sym_dump_vartexts(id, "data-after-defset");
            {
                ProView     fv = sym_front_view(drw);
                ProVector   loc;
                ProDtlattach att = NULL;

                loc[0] = 120.0; loc[1] = 120.0; loc[2] = 0.0;
                if (fv && ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, fv, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                    ProDtlsyminstdataAttachmentSet(id, att);
                    ProDtlsyminstdataAttachtypeSet(id, PROSYMDEFATTACHTYPE_FREE);
                }
                ProDtlsyminstdataScaledheightSet(id, 3.0);
                {
                    ProDtlsyminst inst;
                    ProError      ec;

                    memset(&inst, 0, sizeof(inst));
                    ec = ProDtlsyminstCreate(drw, id, &inst);
                    logf_("[sym] Create(temp free @120,120) -> %d (%s)", (int)ec, errname(ec));
                    if (ec == PRO_TK_NO_ERROR) {
                        ProError es = ProAnnotationShow((ProAnnotation *)&inst, NULL, fv);

                        logf_("[sym] Show -> %d (%s)", (int)es, errname(es));
                        {
                            ProDtlsyminstdata cd = NULL;

                            if (ProDtlsyminstDataGet(&inst, PRODISPMODE_NUMERIC, &cd) == PRO_TK_NO_ERROR && cd)
                                sym_dump_vartexts(cd, "instance");
                        }
                        {
                            ProDimension dim;
                            ProError     ed;

                            memset(&dim, 0, sizeof(dim));
                            ed = ProDtlsyminstDimattachGet(&inst, &dim);
                            logf_("[sym] DimattachGet -> %d (%s)", (int)ed, errname(ed));
                        }
                    }
                }
                if (att) ProDtlattachFree(&att);
            }
        }
    }
}

/* ===== 轮次47：VIEWMOVE|<idx>|<cx>|<cy> —�?�?ProDrawingViewMove 把视图平移到目标 outline 中心 ===== */
static void viewmove_run(ProMdl drw, int vi, double cx, double cy)
{
    ProView *views = NULL;
    int      vc = 0;

    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[vmove] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    if (vi < 0 || vi >= vc) {
        logf_("[vmove] bad index %d (vc=%d)", vi, vc);
        ProArrayFree((ProArray *)&views);
        return;
    }
    {
        ProName    nm;
        char       n8[64];
        ProPoint3d out[2], out2[2];
        double     ocx, ocy;

        nm[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
        wide_to_u8(nm, n8, sizeof(n8));
        out[0][0] = out[0][1] = out[0][2] = -9999.0;
        out[1][0] = out[1][1] = out[1][2] = -9999.0;
        ProDrawingViewOutlineGet((ProDrawing)drw, views[vi], out);
        ocx = (out[0][0] + out[1][0]) / 2.0;
        ocy = (out[0][1] + out[1][1]) / 2.0;
        logf_("[vmove] BEFORE view[%d] '%s' outline x[%.1f..%.1f] y[%.1f..%.1f] c=(%.1f,%.1f) -> target(%.1f,%.1f)",
              vi, n8, out[0][0], out[1][0], out[0][1], out[1][1], ocx, ocy, cx, cy);
        {
            ProVector mv;
            ProError  em;

            mv[0] = cx - ocx; mv[1] = cy - ocy; mv[2] = 0.0;
            em = ProDrawingViewMove((ProDrawing)drw, views[vi], mv);
            logf_("[vmove] ProDrawingViewMove d=(%.1f,%.1f) -> %d (%s)", mv[0], mv[1], (int)em, errname(em));
        }
        {
            ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);

            logf_("[vmove] regen -> %d (%s)", (int)eg, errname(eg));
        }
        out2[0][0] = out2[0][1] = out2[0][2] = -9999.0;
        out2[1][0] = out2[1][1] = out2[1][2] = -9999.0;
        ProDrawingViewOutlineGet((ProDrawing)drw, views[vi], out2);
        logf_("[vmove] AFTER  view[%d] '%s' outline x[%.1f..%.1f] y[%.1f..%.1f] c=(%.1f,%.1f)",
              vi, n8, out2[0][0], out2[1][0], out2[0][1], out2[1][1],
              (out2[0][0] + out2[1][0]) / 2.0, (out2[0][1] + out2[1][1]) / 2.0);
    }
    ProArrayFree((ProArray *)&views);
}

/* ===== 轮次38：AGEO|<�? —�?只读打印该值尺寸的箭头头位�?+ 界线端点（模型坐标） ===== */
typedef struct { double target; ProDrawing drw; int n; } ageo_ctx_t;

static ProError ageo_cb(ProDimension *dim, ProError status, ProAppData app)
{
    ageo_ctx_t *c = (ageo_ctx_t *)app;
    double      v = 0.0;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    {
        ProDimlocation loc = NULL;

        if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc) {
            ProPoint3d a1 = { -9999.0, -9999.0, -9999.0 }, a2 = { -9999.0, -9999.0, -9999.0 };
            ProPoint3d w1 = { -9999.0, -9999.0, -9999.0 }, w2 = { -9999.0, -9999.0, -9999.0 };
            ProError   ea = ProDimlocationArrowsGet(loc, a1, a2);
            ProError   ew = ProDimlocationWitnesslinesGet(loc, w1, w2);
            double     daa = sqrt((a1[0] - a2[0]) * (a1[0] - a2[0]) + (a1[1] - a2[1]) * (a1[1] - a2[1]) + (a1[2] - a2[2]) * (a1[2] - a2[2]));
            double     dww = sqrt((w1[0] - w2[0]) * (w1[0] - w2[0]) + (w1[1] - w2[1]) * (w1[1] - w2[1]) + (w1[2] - w2[2]) * (w1[2] - w2[2]));
            double     d1w1 = sqrt((a1[0] - w1[0]) * (a1[0] - w1[0]) + (a1[1] - w1[1]) * (a1[1] - w1[1]) + (a1[2] - w1[2]) * (a1[2] - w1[2]));
            double     d2w2 = sqrt((a2[0] - w2[0]) * (a2[0] - w2[0]) + (a2[1] - w2[1]) * (a2[1] - w2[1]) + (a2[2] - w2[2]) * (a2[2] - w2[2]));

            logf_("[ageo] id=%d v=%.4f", (int)dim->id, v);
            logf_("[ageo]   arrows(%d) a1=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) |a1-a2|=%.3f",
                  (int)ea, a1[0], a1[1], a1[2], a2[0], a2[1], a2[2], daa);
            logf_("[ageo]   witns(%d)  w1=(%.3f,%.3f,%.3f) w2=(%.3f,%.3f,%.3f) |w1-w2|=%.3f",
                  (int)ew, w1[0], w1[1], w1[2], w2[0], w2[1], w2[2], dww);
            logf_("[ageo]   dist a1->w1 = %.3f ; a2->w2 = %.3f", d1w1, d2w2);
            c->n++;
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 诊断：列出图纸里的注释数�?+ 每个注释的显示状态和首行文本（排查“标了但看不见”）�?
   diag_notes_of 可对任意图纸句柄调用；DIAG|<图名> 用它做活体对照�?*/
static void diag_notes_of(ProMdl drw, const char *tag)
{
    {
        ProDtlnote *notes = NULL;
        ProError ec = ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes);
        int cnt = -1;
        logf_("[%s] dtlnotesCollect -> %d (%s)", tag, (int)ec, errname(ec));
        if (ec == PRO_TK_NO_ERROR && notes) {
            int i;
            ProArraySizeGet((ProArray)notes, &cnt);
            logf_("[%s] note count = %d", tag, cnt);
            for (i = 0; i < cnt && i < 60; i++) {
                ProDtlnotedata nd = NULL;
                ProBoolean disp = PRO_B_FALSE;
                char line8[300];
                line8[0] = 0;
                if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) == PRO_TK_NO_ERROR && nd) {
                    ProDtlnotedataIsDisplayed(nd, &disp);
                    {
                        ProDtlnoteline *lines = NULL;
                        if (ProDtlnotedataLinesCollect(nd, &lines) == PRO_TK_NO_ERROR && lines) {
                            int lc = 0;
                            ProArraySizeGet((ProArray)lines, &lc);
                            if (lc > 0) {
                                ProDtlnotetext *txts = NULL;
                                if (ProDtlnotelineTextsCollect(lines[0], &txts) == PRO_TK_NO_ERROR && txts) {
                                    int tc = 0;
                                    ProArraySizeGet((ProArray)txts, &tc);
                                    if (tc > 0) {
                                        ProLine s;
                                        if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR)
                                            wide_to_u8(s, line8, sizeof(line8));
                                    }
                                }
                            }
                        }
                    }
                }
                logf_("[%s] note[%d] displayed=%d text='%s'", tag, i, (int)disp, line8);
            }
            ProArrayFree((ProArray *)&notes);
        } else {
            logf_("[%s] no note array", tag);
        }
    }
}

static void diag_notes(void)
{
    ProMdl drw = inc_drw();

    logf_("[diag] drawing=%p", (void *)drw);
    if (!drw) return;
    diag_notes_of(drw, "diag");
}

/* 坐标探针：放一条单行注�?"MARKk (x,y)"，用于反推图纸注释的坐标约定 */
static void add_probe_note(ProMdl drw, double x, double y, int k)
{
    ProDtlnotedata data = NULL;
    ProDtlattach   attach = NULL;
    ProDtlnote     note;
    ProVector      loc;
    ProError       e;
    wchar_t        ws[64];

    swprintf_s(ws, 64, L"MARK%d (%.0f,%.0f)", k, x, y);
    loc[0] = x; loc[1] = y; loc[2] = 0.0;

    e = ProDtlnotedataAlloc(drw, &data);
    if (e != PRO_TK_NO_ERROR) { logf_("[coords] dataAlloc -> %d", (int)e); return; }
    {
        ProDtlnotetext text = NULL;
        ProDtlnoteline line = NULL;
        ProLine        s;
        wcsncpy_s(s, PRO_LINE_SIZE, ws, _TRUNCATE);
        ProDtlnotetextAlloc(&text);
        ProDtlnotetextStringSet(text, s);
        ProDtlnotelineAlloc(&line);
        ProDtlnotelineTextAdd(line, text);
        ProDtlnotedataLineAdd(data, line);
    }
    e = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &attach);
    if (e == PRO_TK_NO_ERROR) ProDtlnotedataAttachmentSet(data, attach);
    ProDtlnotedataDisplayedSet(data, PRO_B_TRUE);   /* must set, default is hidden */
    e = ProDtlnoteCreate(drw, NULL, data, &note);
    logf_("[coords] create '%ls' -> %d (%s)", ws, (int)e, errname(e));
    if (attach) ProDtlattachFree(attach);
    ProDtlnotedataFree(data);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 把文件内容以 Base64 输出�?stderr（明文通道：授权进程内读密文文�?-> 文本流出�?*/
/* NOTEAT|x|y|text: create a free single-line plain-text note at (x,y) */
static void noteat_run(ProMdl drw, double x, double y, const wchar_t *text)
{
    ProDtlnotedata data = NULL;
    ProDtlattach   attach = NULL;
    ProDtlnote     note;
    ProVector      loc;
    ProError       e;

    loc[0] = x; loc[1] = y; loc[2] = 0.0;
    e = ProDtlnotedataAlloc(drw, &data);
    if (e != PRO_TK_NO_ERROR) { logf_("[noteat] dataAlloc -> %d", (int)e); return; }
    {
        ProDtlnotetext t = NULL;
        ProDtlnoteline line = NULL;
        ProLine        s;

        wcsncpy_s(s, PRO_LINE_SIZE, text, _TRUNCATE);
        ProDtlnotetextAlloc(&t);
        ProDtlnotetextStringSet(t, s);
        ProDtlnotelineAlloc(&line);
        ProDtlnotelineTextAdd(line, t);
        ProDtlnotedataLineAdd(data, line);
    }
    /* 2026-11-05: ProDtlattachAlloc 的 location 对 FREE 附着是【屏幕坐标】
       （SDK: "For drawing objects ... this should be in screen coordinates"）。
       原来直接传纸面坐标 -> 注记全落到 (0,8.3) 左下角。
       用 ProDrawingSheetTrfGet 的矩阵把 纸面 -> 屏幕。 */
    {
        ProMatrix trf;
        ProName   ssz;
        int r0, c0;
        for (r0 = 0; r0 < 4; r0++) for (c0 = 0; c0 < 4; c0++) trf[r0][c0] = (r0 == c0) ? 1.0 : 0.0;
        ssz[0] = 0;
        if (ProDrawingSheetTrfGet((ProDrawing)drw, 1, ssz, trf) == PRO_TK_NO_ERROR) {
            ProVector loc2;
            loc2[0] = x * trf[0][0] + y * trf[0][1] + trf[0][2];
            loc2[1] = x * trf[1][0] + y * trf[1][1] + trf[1][2];
            loc2[2] = 0.0;
            logf_("[noteat] trf (%.1f,%.1f)->(%.1f,%.1f)", x, y, loc2[0], loc2[1]);
            loc[0] = loc2[0]; loc[1] = loc2[1]; loc[2] = 0.0;
        } else {
            logf_("[noteat] SheetTrfGet failed, keep paper coords");
        }
    }
    e = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &attach);
    if (e == PRO_TK_NO_ERROR) ProDtlnotedataAttachmentSet(data, attach);
    ProDtlnotedataDisplayedSet(data, PRO_B_TRUE);
    e = ProDtlnoteCreate(drw, NULL, data, &note);
    logf_("[noteat] create '%ls' @paper(%.1f,%.1f) -> %d (%s)", text, x, y, (int)e, errname(e));
    if (attach) ProDtlattachFree(attach);
    ProDtlnotedataFree(data);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 2026-11-05: 把已有注记挪到纸面 (x,y)。
   ProDtlnotedataAttachmentGet 取原附着 -> 改 location -> AttachmentSet ->
   ProDtlnoteModify 提交。location 是屏幕坐标，所以要先做 纸面->屏幕 变换。 */
static void note_move_run(ProMdl drw, int id, double x, double y)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i;
    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[movenote] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata nd = NULL;
        int nid = -1;
        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        ProDtlnotedataIdGet(nd, &nid);
        if (nid != id) { ProDtlnotedataFree(nd); continue; }

        {
            ProDtlattach att = NULL;
            ProVector   loc;
            ProMatrix   trf;
            ProName     ssz;
            int r0, c0;
            for (r0 = 0; r0 < 4; r0++) for (c0 = 0; c0 < 4; c0++) trf[r0][c0] = (r0 == c0) ? 1.0 : 0.0;
            ssz[0] = 0;
            if (ProDrawingSheetTrfGet((ProDrawing)drw, 1, ssz, trf) != PRO_TK_NO_ERROR) {
                logf_("[movenote] SheetTrfGet failed");
            }
            loc[0] = x * trf[0][0] + y * trf[0][1] + trf[0][2];
            loc[1] = x * trf[1][0] + y * trf[1][1] + trf[1][2];
            loc[2] = 0.0;

            if (ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &att) == PRO_TK_NO_ERROR && att) {
                ProError es = ProDtlnotedataAttachmentSet(nd, att);
                ProError em = ProDtlnoteModify(&notes[i], NULL, nd);
                logf_("[movenote] id=%d -> paper(%.1f,%.1f) screen(%.1f,%.1f) set=%d modify=%d (%s)",
                      id, x, y, loc[0], loc[1], (int)es, (int)em, errname(em));
                ProDtlattachFree(att);
            } else {
                logf_("[movenote] attachAlloc failed");
            }
        }
        ProDtlnotedataFree(nd);
        break;
    }
    ProArrayFree((ProArray *)&notes);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

static void dump_file_b64(const wchar_t *path)
{
    static const char *TBL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE   *f = _wfopen(path, L"rb");
    long    sz, i;
    unsigned char *buf;
    int     col = 0;
    char    line[200];

    if (!f) { logf_("[b64] cannot open file"); return; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64L * 1024 * 1024) { fclose(f); logf_("[b64] bad size %ld", sz); return; }
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); logf_("[b64] read fail"); return; }
    fclose(f);

    logf_("-----BEGIN-FILE-B64----- %ld bytes", sz);
    for (i = 0; i < sz; i += 3) {
        unsigned char b0 = buf[i];
        unsigned char b1 = (i + 1 < sz) ? buf[i + 1] : 0;
        unsigned char b2 = (i + 2 < sz) ? buf[i + 2] : 0;
        int rem = (int)(sz - i);
        line[col++] = TBL[b0 >> 2];
        line[col++] = TBL[((b0 & 0x03) << 4) | (b1 >> 4)];
        line[col++] = (rem > 1) ? TBL[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        line[col++] = (rem > 2) ? TBL[b2 & 0x3F] : '=';
        if (col >= 120) { line[col] = 0; logf_("%s", line); col = 0; }
    }
    if (col) { line[col] = 0; logf_("%s", line); }
    logf_("-----END-FILE-B64-----");
    free(buf);
}

/* 参数表导出到日志（明文通道：导�?CSV 后进程内读回打印）——PARAMS 命令�?*/
static void dump_params_to_log(ProMdl mdl, const wchar_t *tag)
{
    ProModelitem   owner;
    ProParamColumn cols = (ProParamColumn)(
        PRO_PARAM_NAME_COLUMN | PRO_PARAM_TYPE_COLUMN | PRO_PARAM_VALUE_COLUMN |
        PRO_PARAM_DESIGNATE_COLUMN | PRO_PARAM_ACCESS_COLUMN | PRO_PARAM_SOURCE_COLUMN |
        PRO_PARAM_DESCRIPTION_COLUMN | PRO_PARAM_UNIT_COLUMN | PRO_PARAM_OWNER_COLUMN |
        PRO_PARAM_OWNER_TYPE_COLUMN | PRO_PARAM_REPORTINBOM_COLUMN);
    ProPath    p;
    char       p8[PRO_PATH_SIZE * 4];
    SYSTEMTIME st;

    if (ProMdlToModelitem(mdl, &owner) != PRO_TK_NO_ERROR) {
        logf_("[params] ProMdlToModelitem failed");
        return;
    }
    GetLocalTime(&st);
    swprintf_s(p, PRO_PATH_SIZE,
               L"D:\\creo-agent\\sync-toolkit\\work\\params_%ls_%02u%02u%02u.csv",
               tag, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    {
        ProError e = ProParameterTableExport(&owner, mdl, 0, PRO_PARAMTABLE_EXPORT_CSV, p, cols);
        wide_to_u8(p, p8, sizeof(p8));
        logf_("[params] export '%s' -> %d (%s)", p8, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) dump_text_file(p8, "PARAMS-CSV");
    }
}

/* 公差在线实验：给指定值的模型尺寸设公差并回读（P0c：排�?显示=设定�?�?�?*/
typedef struct { double target; int ty; double up, lo; } tolset_ctx_t;

static ProError dim_tol_cb(ProDimension *dim, ProError status, ProAppData app)
{
    tolset_ctx_t *c = (tolset_ctx_t *)app;
    double v = 0.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    {
        double d = v - c->target;
        if (d < 0) d = -d;
        if (d > 0.02) return PRO_TK_NO_ERROR;
    }
    {
        ProDimToleranceType pt = (ProDimToleranceType)0;
        double pu = -999, pl = -999;
        ProError et1, et2;
        ProDimensionToltypeGet(dim, &pt);
        ProDimensionToleranceGet(dim, &pu, &pl);
        logf_("[tolset] v=%.4f PRE : type=%d up=%.4f lo=%.4f", v, (int)pt, pu, pl);
        et1 = ProDimensionToltypeSet(dim, (ProDimToleranceType)c->ty);
        et2 = ProDimensionToleranceSet(dim, c->up, c->lo);
        logf_("[tolset]   set type=%d up=%.4f lo=%.4f -> t%d s%d", c->ty, c->up, c->lo, (int)et1, (int)et2);
        pt = (ProDimToleranceType)0; pu = -999; pl = -999;
        ProDimensionToltypeGet(dim, &pt);
        ProDimensionToleranceGet(dim, &pu, &pl);
        logf_("[tolset] v=%.4f POST: type=%d up=%.4f lo=%.4f", v, (int)pt, pu, pl);
    }
    return PRO_TK_NO_ERROR;
}

/* r138: DIMTOL|<id>|<ty>|<up>|<lo> —�?按图纸尺�?id 直接设公差（不依赖零件尺寸） */
/* DUMPDIMS: list all model dimensions so the drawing can quote real design values */
typedef struct { int n; } dumpdims_ctx_t;

static ProError dumpdims_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dumpdims_ctx_t *c = (dumpdims_ctx_t *)app;
    double v = 0.0;
    ProName sym;
    ProDimensiontype ty = (ProDimensiontype)0;

    (void)status;
    sym[0] = 0;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    ProDimensionSymbolGet(dim, sym);
    ProDimensionTypeGet(dim, &ty);
    logf_("[dims] id=%d sym='%ls' type=%d value=%.4f", dim->id, sym, (int)ty, v);
    c->n++;
    return PRO_TK_NO_ERROR;
}

typedef struct { int target_id; int ty; double up, lo; int hits; } dimtol_ctx_t;

static ProError dimtol_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dimtol_ctx_t *c = (dimtol_ctx_t *)app;
    (void)status;
    if ((int)dim->id != c->target_id) return PRO_TK_NO_ERROR;
    {
        ProError e0 = ProDimensionTollabelSet(dim, PROTOLTABLE_NONE, L"", 0);
        ProError et1 = ProDimensionToltypeSet(dim, (ProDimToleranceType)c->ty);
        ProError et2 = ProDimensionToleranceSet(dim, c->up, c->lo);
        int      rt = -1;
        double   ru = 0.0, rl = 0.0;
        char     db[64] = "-";

        ProDimensionToltypeGet(dim, (ProDimToleranceType *)&rt);
        ProDimensionToleranceGet(dim, &ru, &rl);
        ab_dim_display_len(dim, db, sizeof(db));
        logf_("[dimtol] id=%d set type=%d up=%.4f lo=%.4f -> label=%d(%s) t=%d(%s) s=%d(%s)",
             (int)dim->id, c->ty, c->up, c->lo, (int)e0, errname(e0), (int)et1, errname(et1), (int)et2, errname(et2));
        logf_("[dimtol] id=%d readback tol=%d up=%.4f lo=%.4f display='%s'", (int)dim->id, rt, ru, rl, db);
        c->hits++;
    }
    return PRO_TK_NO_ERROR;
}

/* r138: DIMTEXT|<id>|<fmt> —�?用格式串覆盖图纸尺寸文本（@D 仍为 Creo 计算值） */
/* r139: DIMDEC|<id>|<n> -- set one drawing dimension's decimal places (display only) */
typedef struct { int target_id; int dec; int hits; } dimdec_ctx_t;

static ProError dimdec_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dimdec_ctx_t *c = (dimdec_ctx_t *)app;
    (void)status;
    if ((int)dim->id != c->target_id) return PRO_TK_NO_ERROR;
    {
        ProError e1 = ProDimensionDecimalsSet(dim, c->dec);
        int      rd = -99;
        char     db[64] = "-";

        ProDimensionDecimalsGet(dim, &rd);
        ab_dim_display_len(dim, db, sizeof(db));
        logf_("[dimdec] id=%d set %d -> rd=%d (err=%d %s) display='%s'",
             (int)dim->id, c->dec, rd, (int)e1, errname(e1), db);
        c->hits++;
    }
    return PRO_TK_NO_ERROR;
}

typedef struct { int target_id; wchar_t fmt[256]; int hits; } dimtext_ctx_t;

static ProError dimtext_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dimtext_ctx_t *c = (dimtext_ctx_t *)app;
    (void)status;
    if ((int)dim->id != c->target_id) return PRO_TK_NO_ERROR;
    {
        wchar_t **arr = NULL;
        ProError e;
        ProArrayAlloc(1, sizeof(wchar_t *), 1, (ProArray *)&arr);
        arr[0] = c->fmt;
        e = ProDimensionTextWstringsSet(dim, arr);
        logf_("[dimtext] id=%d fmt='%ls' set -> %d (%s)", (int)dim->id, c->fmt, (int)e, errname(e));
        c->hits++;
    }
    return PRO_TK_NO_ERROR;
}

/* 关键尺寸文本位置 dump（排版收敛用�?*/
static ProError dim_textpos_cb(ProDimension *dim, ProError status, ProAppData app)
{
    ProDrawing drw = (ProDrawing)app;
    double     v = -1.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (!value_in_keep_list(v)) return PRO_TK_NO_ERROR;
    {
        ProDimlocation loc = NULL;
        ProBoolean     he = PRO_B_FALSE;
        ProPoint3d     p;
        double         el = 0.0;

        p[0] = p[1] = p[2] = -9999.0;
        if (ProDimensionLocationGet(dim, NULL, drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, p, &el);
        logf_("[textpos] %.4f text=(%.2f,%.2f) elbow=%d", v, p[0], p[1], (int)he);
    }
    return PRO_TK_NO_ERROR;
}

/* 公差标签诊断/摘除（P0c 深挖：模型尺寸可能挂�?ISO/DIN 通用公差表上�?
   regen 会把表值重新写�?�?图面始终显示 ±0.2/±0.1。摘除标签后显式值应生效。） */
typedef struct { double target; int mode; int ty; double up, lo; } tolf_ctx_t;   /* mode 0=dump 1=free+set */

static ProError dim_tolfree_cb(ProDimension *dim, ProError status, ProAppData app)
{
    tolf_ctx_t *c = (tolf_ctx_t *)app;
    double v = 0.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (c->mode == 0 && !value_in_keep_list(v)) return PRO_TK_NO_ERROR;
    {
        double d = v - c->target;
        if (d < 0) d = -d;
        if (c->mode == 1 && d > 0.02) return PRO_TK_NO_ERROR;
    }
    {
        ProToleranceTable   ttab = PROTOLTABLE_NONE;
        ProName             tab;
        int                 col = -1;
        ProDimToleranceType pt = (ProDimToleranceType)0;
        double              pu = -999, pl = -999;
        ProError            el;

        tab[0] = 0;
        el = ProDimensionTollabelGet(dim, &ttab, tab, &col);
        ProDimensionToltypeGet(dim, &pt);
        ProDimensionToleranceGet(dim, &pu, &pl);
        logf_("[tolfree] v=%.4f labelGet->%d(%s) ttab=%d col=%d ; type=%d up=%.4f lo=%.4f",
              v, (int)el, errname(el), (int)ttab, col, (int)pt, pu, pl);
        if (c->mode == 1) {
            ProError es = ProDimensionTollabelSet(dim, PROTOLTABLE_NONE, tab, 0);
            ProError et1, et2;
            logf_("[tolfree]   labelSet(NONE) -> %d (%s)", (int)es, errname(es));
            et1 = ProDimensionToltypeSet(dim, (ProDimToleranceType)c->ty);
            et2 = ProDimensionToleranceSet(dim, c->up, c->lo);
            logf_("[tolfree]   set type=%d up=%.4f lo=%.4f -> t%d s%d", c->ty, c->up, c->lo, (int)et1, (int)et2);
            pt = (ProDimToleranceType)0; pu = -999; pl = -999;
            ProDimensionToltypeGet(dim, &pt);
            ProDimensionToleranceGet(dim, &pu, &pl);
            logf_("[tolfree] v=%.4f POST: type=%d up=%.4f lo=%.4f", v, (int)pt, pu, pl);
        }
    }
    return PRO_TK_NO_ERROR;
}

/* ---- P0b：标题栏填充。实测真人图纸（c021006-016-00）标题栏 5 格是**字面文本注释**
   �?.0327 / 麦拉�?/ 绝缘PC / C021006-016-00 / INSULATION_PP_2P_2），不是参数引用�?
   快速出图模板给的是占位符字面文�?"&CNAME" �?—�?所以唯一正确修法=直接改注释文本�?---- */
static int edit_note_text_match(ProMdl drw, const wchar_t *from, const wchar_t *to)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i, hits = 0;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[editnote] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata nd = NULL;
        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        {
            ProDtlnoteline *lines = NULL;
            if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) continue;
            {
                int lc = 0;
                ProArraySizeGet((ProArray)lines, &lc);
                /* 2026-11-05: 原来只处理 lc==1 的单行注记，而标题栏 &MASS[.4]
                   / &CNAME / &DRWNUM 所在注记是多行的，整块被跳过 ->
                   图面一直显示未解析的占位符。改为遍历所有行，逐段全等匹配。 */
                {
                    int li;
                    for (li = 0; li < lc; li++) {
                    ProDtlnotetext *txts = NULL;
                    if (ProDtlnotelineTextsCollect(lines[li], &txts) == PRO_TK_NO_ERROR && txts) {
                        int tc = 0;
                        ProArraySizeGet((ProArray)txts, &tc);
                        if (tc >= 1) {
                            ProLine s;
                            s[0] = 0;
                            if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR && !wcscmp(s, from)) {
                                ProLine    nt;
                                ProBoolean ro = PRO_B_FALSE;
                                ProError   es, em;
                                wcsncpy_s(nt, PRO_LINE_SIZE, to, _TRUNCATE);
                                ProDtlnotedataReadonlyGet(nd, &ro);
                                if (ro) {
                                    ProError ero = ProDtlnotedataReadonlySet(nd, PRO_B_FALSE);
                                    logf_("[editnote]   readonly->FALSE %d (%s)", (int)ero, errname(ero));
                                }
                                /* 整行文本替换：为该行建立一个全新的纯文本对象数组（对参数型
                                   文本也有效；StringSet �?&MASS[.4] 这类参数型文本实测无效） */
                                {
                                    ProDtlnotetext  tnew = NULL;
                                    ProDtlnotetext *arr = NULL;
                                    ProDtlnotetextAlloc(&tnew);
                                    ProDtlnotetextStringSet(tnew, nt);
                                    ProArrayAlloc(1, sizeof(ProDtlnotetext), 1, (ProArray *)&arr);
                                    arr[0] = tnew;
                                    es = ProDtlnotelineTextsSet(lines[li], arr);
                                    logf_("[editnote] note[%d] line[%d] '%ls'->'%ls' textsSet=%d (%s)", i, li, from, to, (int)es, errname(es));
                                }
                                /* 提交（收集到�?notedata 是工作副本） */
                                em = ProDtlnoteModify(&notes[i], NULL, nd);
                                logf_("[editnote]   modify -> %d (%s)", (int)em, errname(em));
                                /* 验证：重读数据库文本（只认落库的值） */
                                {
                                    ProDtlnotedata nd2 = NULL;
                                    if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd2) == PRO_TK_NO_ERROR && nd2) {
                                        ProDtlnoteline *l2 = NULL;
                                        if (ProDtlnotedataLinesCollect(nd2, &l2) == PRO_TK_NO_ERROR && l2) {
                                            int lc2 = 0;
                                            ProArraySizeGet((ProArray)l2, &lc2);
                                            if (lc2 >= 1) {
                                                ProDtlnotetext *t2 = NULL;
                                                if (ProDtlnotelineTextsCollect(l2[0], &t2) == PRO_TK_NO_ERROR && t2) {
                                                    int tc2 = 0;
                                                    ProArraySizeGet((ProArray)t2, &tc2);
                                                    if (tc2 >= 1) {
                                                        ProLine s2;
                                                        s2[0] = 0;
                                                        if (ProDtlnotetextStringGet(t2[0], s2) == PRO_TK_NO_ERROR) {
                                                            char v8[300];
                                                            wide_to_u8(s2, v8, sizeof(v8));
                                                            logf_("[editnote]   verify text now: '%s'", v8);
                                                            if (!wcscmp(s2, to)) hits++;
                                                        }
                                                    }
                                                    ProArrayFree((ProArray *)&t2);
                                                }
                                            }
                                            ProArrayFree((ProArray *)&l2);
                                        }
                                    }
                                }
                            }
                        }
                        ProArrayFree((ProArray *)&txts);
                    }
                    }   /* for li: 2026-11-05 added */
                }
                ProArrayFree((ProArray *)&lines);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
    return hits;
}

/* ===== 轮次46：NOTEDUMP / NOTEFIX2 — 多行注释全量转储 + 子串匹配整行替换 ===== */
static void note_dump_all(ProMdl drw)
{
    ProDtlnote *notes = NULL;
    int         cnt = 0, i;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[notedump] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    logf_("[notedump] total notes=%d", cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata nd = NULL;

        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        {
            ProDtlnoteline *lines = NULL;

            if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) continue;
            {
                int lc = 0, li;

                ProArraySizeGet((ProArray)lines, &lc);
                for (li = 0; li < lc; li++) {
                    ProDtlnotetext *txts = NULL;

                    if (ProDtlnotelineTextsCollect(lines[li], &txts) != PRO_TK_NO_ERROR || !txts) continue;
                    {
                        int tc = 0;

                        ProArraySizeGet((ProArray)txts, &tc);
                        if (tc >= 1) {
                            ProLine s;
                            char    a8[400];

                            s[0] = 0;
                            if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR) {
                                wide_to_u8(s, a8, sizeof(a8));
                                logf_("[notedump] note[%d] line[%d] text='%s'", i, li, a8);
                            }
                        }
                        ProArrayFree((ProArray *)&txts);
                    }
                }
                ProArrayFree((ProArray *)&lines);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
}

/* 轮次46：多行注释无法就地改（textsSet/modify 实测不落库）——改�?复制全部�?命中行换文本"重建，原附件、删�?*/
static int note_rebuild_legend(ProMdl drw, const wchar_t *contains, const wchar_t *to)
{
    ProDtlnote *notes = NULL;
    int         cnt = 0, i;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) return 0;
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata  nd = NULL;
        ProDtlnoteline *lines = NULL;
        int             li, lc = 0, hitline = -1;

        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) continue;
        ProArraySizeGet((ProArray)lines, &lc);
        for (li = 0; li < lc; li++) {
            ProDtlnotetext *txts = NULL;

            if (ProDtlnotelineTextsCollect(lines[li], &txts) != PRO_TK_NO_ERROR || !txts) continue;
            {
                int tc = 0;

                ProArraySizeGet((ProArray)txts, &tc);
                if (tc >= 1) {
                    ProLine s;

                    s[0] = 0;
                    if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR && wcsstr(s, contains)) hitline = li;
                }
                ProArrayFree((ProArray *)&txts);
            }
        }
        if (hitline >= 0) {
            ProDtlattach   att = NULL;
            ProDtlnotedata d2 = NULL;
            ProDtlnote     note2;
            ProError       ea = ProDtlnotedataAttachmentGet(nd, &att);
            int            lj;

            logf_("[note2] found note[%d] line[%d] (attachGet=%d %s); rebuilding", i, hitline, (int)ea, errname(ea));
            if (ProDtlnotedataAlloc(drw, &d2) != PRO_TK_NO_ERROR || !d2) break;
            for (lj = 0; lj < lc; lj++) {
                ProLine        src;
                ProDtlnotetext tnew = NULL;
                ProDtlnoteline lnew = NULL;

                src[0] = 0;
                if (lj == hitline) {
                    wcsncpy_s(src, PRO_LINE_SIZE, to, _TRUNCATE);
                } else {
                    ProDtlnotetext *txts2 = NULL;

                    if (ProDtlnotelineTextsCollect(lines[lj], &txts2) == PRO_TK_NO_ERROR && txts2) {
                        int tc2 = 0;

                        ProArraySizeGet((ProArray)txts2, &tc2);
                        if (tc2 >= 1) ProDtlnotetextStringGet(txts2[0], src);
                        ProArrayFree((ProArray *)&txts2);
                    }
                }
                ProDtlnotetextAlloc(&tnew);
                ProDtlnotetextStringSet(tnew, src);
                ProDtlnotelineAlloc(&lnew);
                ProDtlnotelineTextAdd(lnew, tnew);
                ProDtlnotedataLineAdd(d2, lnew);
            }
            if (att) ProDtlnotedataAttachmentSet(d2, att);
            ProDtlnotedataDisplayedSet(d2, PRO_B_TRUE);
            {
                ProError ec = ProDtlnoteCreate(drw, NULL, d2, &note2);

                logf_("[note2] noteCreate -> %d (%s)", (int)ec, errname(ec));
                ProDtlnotedataFree(d2);
                if (ec == PRO_TK_NO_ERROR) {
                    ProError ed = ProDtlnoteDelete(&notes[i], NULL);

                    logf_("[note2] noteDelete(old) -> %d (%s)", (int)ed, errname(ed));
                }
            }
            ProArrayFree((ProArray *)&lines);
            ProArrayFree((ProArray *)&notes);
            return 1;
        }
        ProArrayFree((ProArray *)&lines);
    }
    ProArrayFree((ProArray *)&notes);
    return 0;
}

static int note_fix_sub(ProMdl drw, const wchar_t *contains, const wchar_t *to){
    ProDtlnote *notes = NULL;
    int         cnt = 0, i, hits = 0;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[notefix] collect failed");
        return 0;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata nd = NULL;

        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        {
            ProDtlnoteline *lines = NULL;

            if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) continue;
            {
                int lc = 0, li;

                ProArraySizeGet((ProArray)lines, &lc);
                for (li = 0; li < lc; li++) {
                    ProDtlnotetext *txts = NULL;

                    if (ProDtlnotelineTextsCollect(lines[li], &txts) != PRO_TK_NO_ERROR || !txts) continue;
                    {
                        int tc = 0;

                        ProArraySizeGet((ProArray)txts, &tc);
                        if (tc >= 1) {
                            ProLine s;

                            s[0] = 0;
                            if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR && wcsstr(s, contains)) {
                                ProLine       nt;
                                ProBoolean    ro = PRO_B_FALSE;
                                ProDtlnotetext tnew = NULL;
                                ProDtlnotetext *arr = NULL;
                                ProError      es, em;
                                char          a8[400];

                                wide_to_u8(s, a8, sizeof(a8));
                                logf_("[notefix] note[%d] line[%d] matched: '%s'", i, li, a8);
                                wcsncpy_s(nt, PRO_LINE_SIZE, to, _TRUNCATE);
                                ProDtlnotedataReadonlyGet(nd, &ro);
                                if (ro) ProDtlnotedataReadonlySet(nd, PRO_B_FALSE);
                                ProDtlnotetextAlloc(&tnew);
                                ProDtlnotetextStringSet(tnew, nt);
                                ProArrayAlloc(1, sizeof(ProDtlnotetext), 1, (ProArray *)&arr);
                                arr[0] = tnew;
                                es = ProDtlnotelineTextsSet(lines[li], arr);
                                em = ProDtlnoteModify(&notes[i], NULL, nd);
                                logf_("[notefix]   textsSet=%d (%s) modify=%d (%s)", (int)es, errname(es), (int)em, errname(em));
                                {
                                    ProDtlnotedata nd2 = NULL;

                                    if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd2) == PRO_TK_NO_ERROR && nd2) {
                                        ProDtlnoteline *l2 = NULL;

                                        if (ProDtlnotedataLinesCollect(nd2, &l2) == PRO_TK_NO_ERROR && l2) {
                                            int lc2 = 0;

                                            ProArraySizeGet((ProArray)l2, &lc2);
                                            if (li < lc2) {
                                                ProDtlnotetext *t2 = NULL;

                                                if (ProDtlnotelineTextsCollect(l2[li], &t2) == PRO_TK_NO_ERROR && t2) {
                                                    int tc2 = 0;

                                                    ProArraySizeGet((ProArray)t2, &tc2);
                                                    if (tc2 >= 1) {
                                                        ProLine s2;
                                                        char    b8[400];

                                                        s2[0] = 0;
                                                        if (ProDtlnotetextStringGet(t2[0], s2) == PRO_TK_NO_ERROR) {
                                                            wide_to_u8(s2, b8, sizeof(b8));
                                                            logf_("[notefix]   verify now: '%s'", b8);
                                                            if (!wcscmp(s2, to)) hits++;
                                                        }
                                                    }
                                                    ProArrayFree((ProArray *)&t2);
                                                }
                                            }
                                            ProArrayFree((ProArray *)&l2);
                                        }
                                    }
                                }
                                ProArrayFree((ProArray *)&arr);
                            }
                        }
                        ProArrayFree((ProArray *)&txts);
                    }
                }
                ProArrayFree((ProArray *)&lines);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
    return hits;
}

static void get_part_string_param(ProMdl part, const wchar_t *pname, wchar_t *out, int outsz){
    ProModelitem owner;
    ProParameter p;
    ProParamvalue pv;

    out[0] = 0;
    if (ProMdlToModelitem(part, &owner) != PRO_TK_NO_ERROR) return;
    if (ProParameterInit(&owner, (wchar_t *)pname, &p) != PRO_TK_NO_ERROR) return;
    memset(&pv, 0, sizeof(pv));
    /* 管线写入用的�?ProParameterWithUnitsCreate —�?读取必须用带单位接口�?
       ProParameterValueGet 对带单位参数会失败（实测三值全空） */
    if (ProParameterValueWithUnitsGet(&p, &pv, NULL) != PRO_TK_NO_ERROR) return;
    if (pv.type == PRO_PARAM_STRING) wcsncpy_s(out, outsz, pv.value.s_val, _TRUNCATE);
}

static double get_mass_value(ProMdl part)
{
    ProMassProperty mp;
    double m = -1.0;

    if (ProSolidMassPropertyGet((ProSolid)part, NULL, &mp) == PRO_TK_NO_ERROR && mp.mass > 0) {
        m = mp.mass;
        logf_("[filltb] mass(Get) = %.6g vol=%.3f", m, mp.volume);
    } else if (ProSolidMassPropertyWithDensityGet((ProSolid)part, NULL, PRO_MP_DENS_USE_IF_MISSING,
                                                  8.2e-6, &mp) == PRO_TK_NO_ERROR && mp.mass > 0) {
        /* NB109 密度 8.2e3 kg/m^3（材料库 金属\NB109.mtl）；模型单位 mm/kg�?
           API 直接乘体�?mm^3)，故密度换算�?kg/mm^3 = 8.2e-6 */
        m = mp.mass;
        logf_("[filltb] mass(WithDensity 8.2e-6) = %.6g vol=%.3f", m, mp.volume);
    } else {
        logf_("[filltb] mass: both APIs failed");
    }
    return m;
}

/* 2026-11-05: 把图框里 4 个 '***' 空位按【坐标】填上实际值。
   实测位置（A3 图框格式化后，A3 = 420x297mm x 2.3805 = 1000x707 图纸单位）：
     id  0  env=(141.1,737.4)-(120.9,746.9)  左上角（倒置）图号位
     id  3  env=(918.5,106.4)-(938.7, 96.9)  右下 图号
     id 13  env=(918.5,151.6)-(938.7,142.1)  右下 名称
     id 21  env=(800.7,184.4)-(818.4,176.0)  名称栏
   做法：找出 text 全为 '***' 的注记，删掉它，用 noteat_run 在同一位置
   重建一条自由文本（noteat_run 内部走 ProDtlattachAlloc(FREE)，
   所以不会像裸 ProDtlnoteCreate 那样落到 (0,0)）。 */
static void fill_star_slots(ProMdl drw, const wchar_t *drwnum,
                            const wchar_t *mass_s, const wchar_t *cname)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i, done = 0;
    double  anchors[8][3];   /* cx, cy, slot */
    int     na = 0;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) {
        logf_("[stars] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)notes, &cnt);
    logf_("[stars] scanning %d notes for '***' slots", cnt);

    /* 先收集所有空位坐标（删完再建，避免边遍历边删） */
    for (i = 0; i < cnt; i++) {
        wchar_t txt[512];
        ProLineEnvelope env;
        inc_note_text(&notes[i], txt, 512);
        if (wcscmp(txt, L"***") != 0) continue;
        env[0][0] = env[0][1] = env[3][0] = env[3][1] = 0.0;
        if (ProDtlnoteLineEnvelopeGet(&notes[i], 0, env) != PRO_TK_NO_ERROR) continue;
        if (na >= 8) break;
        anchors[na][0] = (env[0][0] + env[3][0]) * 0.5;
        anchors[na][1] = (env[0][1] + env[3][1]) * 0.5;
        anchors[na][2] = (double)i;      /* 暂存索引，稍后按它删 */
        logf_("[stars] slot %d id=%d center=(%.1f,%.1f)",
              na, (int)notes[i].id, anchors[na][0], anchors[na][1]);
        na++;
    }
    if (na == 0) { ProArrayFree((ProArray *)&notes); logf_("[stars] no '***' slot"); return; }

    /* 逐个：定位 -> 选填值 -> 删旧 -> 原位新建 */
    for (i = 0; i < na; i++) {
        int idx = (int)anchors[i][2];
        double cx = anchors[i][0], cy = anchors[i][1];
        const wchar_t *fill;
        ProError ed = ProDtlnoteDelete((ProDrawing)drw, &notes[idx]);
        logf_("[stars] slot %d delete -> %d (%s)", i, (int)ed, errname(ed));
        if (ed != PRO_TK_NO_ERROR) continue;

        if (cy > 700.0)                    fill = drwnum;                       /* 左上倒置图号 */
        else if (cy > 160.0)               fill = (cname && cname[0]) ? cname : drwnum;
        else if (cy > 120.0 && cx > 780.0) fill = mass_s;                       /* 重量 */
        else                               fill = drwnum;                       /* 右下图号 */
        if (!fill || !fill[0]) continue;

        noteat_run(drw, cx, cy, fill);
        {
            char t8[600];
            wide_to_u8(fill, t8, sizeof(t8));
            logf_("[stars] slot %d (%.1f,%.1f) filled '%s'", i, cx, cy, t8);
        }
        done++;
    }
    ProArrayFree((ProArray *)&notes);
    logf_("[stars] filled %d/%d slots", done, na);
}
static void fill_title_block(ProMdl drw, ProMdl part)
{
    wchar_t cname[PRO_NAME_SIZE], mstd[PRO_NAME_SIZE], drwnum[PRO_NAME_SIZE], mass_s[64];
    double  m;

    cname[0] = mstd[0] = drwnum[0] = mass_s[0] = 0;
    get_part_string_param(part, L"PTC_COMMON_NAME", cname, PRO_NAME_SIZE);
    if (!cname[0]) get_part_string_param(part, L"CNAME", cname, PRO_NAME_SIZE);
    get_part_string_param(part, L"MSTD", mstd, PRO_NAME_SIZE);
    get_part_string_param(part, L"DRWNUM", drwnum, PRO_NAME_SIZE);
    m = get_mass_value(part);
    if (m >= 0) swprintf_s(mass_s, 64, L"%.4f", m);
    logf_("[filltb] cname='%ls' mstd='%ls' drwnum='%ls' mass='%ls'", cname, mstd, drwnum, mass_s);
    if (cname[0])  edit_note_text_match(drw, L"&CNAME", cname);
    if (mstd[0])   edit_note_text_match(drw, L"&MSTD", mstd);
    if (drwnum[0]) edit_note_text_match(drw, L"&DRWNUM", drwnum);   /* 两处：名称栏 + 左上倒置 */
    if (mass_s[0]) edit_note_text_match(drw, L"&MASS[.4]", mass_s);
    /* 2026-11-05: 本机图框里没有 &CNAME/&MASS 这类占位符，而是 4 个 '***' 空位，
       位置各不相同（名称栏 / 图号栏 / 左上角 / 中间栏），只能按坐标逐个填。
       空位坐标取自 STATE 实测 env 的右下角（Creo 注记 env[0]=左上 env[3]=右下，
       见 note[0] id=0 env=(141.1,737.4)-(120.9,746.9) 这类倒挂值，用中点更稳）。 */
    if (drwnum[0]) fill_star_slots(drw, drwnum, mass_s, cname);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 公差重显实验：设公差 -> 擦除该尺�?-> 重新显示（测试图面公差文本是否跟随） */
typedef struct { double target; int ty; double up, lo; ProDrawing drw; ProView view; } restol_ctx_t;

static ProError dim_restol_cb(ProDimension *dim, ProError status, ProAppData app)
{
    restol_ctx_t *c = (restol_ctx_t *)app;
    double v = 0.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    {
        double d = v - c->target;
        if (d < 0) d = -d;
        if (d > 0.02) return PRO_TK_NO_ERROR;
    }
    {
        ProError ee = ProAnnotationErase((ProAnnotation *)dim, NULL, c->view);
        ProError et1, et2, es;
        logf_("[restol] %.4f erase -> %d (%s)", v, (int)ee, errname(ee));
        et1 = ProDimensionToltypeSet(dim, (ProDimToleranceType)c->ty);
        et2 = ProDimensionToleranceSet(dim, c->up, c->lo);
        logf_("[restol]   set type=%d up=%.4f lo=%.4f -> t%d s%d", c->ty, c->up, c->lo, (int)et1, (int)et2);
        es = ProAnnotationShow((ProAnnotation *)dim, NULL, c->view);
        logf_("[restol] %.4f show -> %d (%s)", v, (int)es, errname(es));
    }
    return PRO_TK_NO_ERROR;
}

/* 挂接结构探针：dump 指定值尺寸的 attachments / sense（总长参考尺寸的构造样本） */
typedef struct { double target; } attach_ctx_t;

static ProError dim_attach_cb(ProDimension *dim, ProError status, ProAppData app)
{
    attach_ctx_t *c = (attach_ctx_t *)app;
    double v = 0.0;

    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    {
        double d = v - c->target;
        if (d < 0) d = -d;
        if (d > 0.02) return PRO_TK_NO_ERROR;
    }
    logf_("[attach] dim id=%d value=%.4f", (int)dim->id, v);
    {
        ProAnnotationPlane plane;
        ProDimAttachment *att = NULL;
        ProDimSense *sense = NULL;
        ProDimOrient orient;
        ProError e = ProDimensionAttachmentsGet(dim, &plane, &att, &sense, &orient);
        logf_("[attach] AttachmentsGet -> %d (%s) orient=%d", (int)e, errname(e), (int)orient);
        if (e == PRO_TK_NO_ERROR && att) {
            int n = 0, i;
            ProArraySizeGet((ProArray)att, &n);
            logf_("[attach] attachment pairs=%d", n);
            for (i = 0; i < n && i < 4; i++) {
                int k;
                for (k = 0; k < 2; k++) {
                    ProModelitem mi;
                    ProError em = ProSelectionModelitemGet(att[i][k], &mi);
                    logf_("[attach]   pair%d sel%d: -> %d type=%d id=%d", i, k, (int)em, (int)mi.type, (int)mi.id);
                }
                if (sense) {
                    logf_("[attach]   pair%d sense: type=%d orient_hint=%d", i,
                          (int)sense[i].type, (int)sense[i].orient_hint);
                }
            }
            ProArrayFree((ProArray *)&att);
            if (sense) ProArrayFree((ProArray *)&sense);
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 旧版（面附件）保留备查：实测 ProDimensionCreate -> -2（BAD_INPUTS），未被调用 */
static void add_total_dim_model_faceold(ProMdl drw, ProMdl part)
{
    ProPoint3d bb[2];
    double     bestL = 1e30, bestR = 1e30;
    int        sidL = -1, sidR = -1;
    int        id;

    bb[0][0] = bb[0][1] = bb[0][2] = -9999.0;
    bb[1][0] = bb[1][1] = bb[1][2] = -9999.0;
    if (ProSolidOutlineGet((ProSolid)part, bb) != PRO_TK_NO_ERROR) { logf_("[totm] bbox failed"); return; }
    for (id = 100; id <= 4000; id++) {   /* id>=100 跳过基准�?*/
        ProSurface s = NULL;
        if (ProSurfaceInit((ProMdl)part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        {
            ProGeomitemdata *gd = NULL;
            if (ProSurfaceDataGet(s, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_surface_data) {
                ProSurfacedata *sd = gd->data.p_surface_data;
                if (sd->type == PRO_SRF_PLANE) {
                    double nx = sd->srf_shape.plane.e3[0];
                    if (nx > 0.99 || nx < -0.99) {
                        double x0 = sd->srf_shape.plane.origin[0];
                        double dl = x0 - bb[0][0]; if (dl < 0) dl = -dl;
                        double dr = x0 - bb[1][0]; if (dr < 0) dr = -dr;
                        if (dl < bestL) { bestL = dl; sidL = id; }
                        if (dr < bestR) { bestR = dr; sidR = id; }
                    }
                }
            }
            if (gd) ProGeomitemdataFree(&gd);
        }
    }
    logf_("[totm] faces: L=%d (d=%.2f) R=%d (d=%.2f)", sidL, bestL, sidR, bestR);
    if (sidL < 0 || sidR < 0) return;
    {
        ProSurface         ls = NULL, rs = NULL;
        ProGeomitem        gi1, gi2;
        ProSelection       s1 = NULL, s2 = NULL;
        ProDimAttachment  *att_arr = NULL;
        ProDimSense       *sense_arr = NULL;
        ProAnnotationPlane plane;      /* 值结构体（同 ProDtlnote�?*/
        ProDimension       nd;
        ProPoint3d         loc;
        ProError           e;

        memset(&plane, 0, sizeof(plane));
        memset(&nd, 0, sizeof(nd));
        ProSurfaceInit((ProMdl)part, sidL, &ls);
        ProSurfaceInit((ProMdl)part, sidR, &rs);
        if (ProSurfaceToGeomitem((ProSolid)part, ls, &gi1) != PRO_TK_NO_ERROR) return;
        if (ProSurfaceToGeomitem((ProSolid)part, rs, &gi2) != PRO_TK_NO_ERROR) return;
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &s1) != PRO_TK_NO_ERROR) return;
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); return; }
        e = ProAnnotationplaneFlatToScreenCreate((ProSolid)part, PRO_B_TRUE, &plane);
        logf_("[totm] flatToScreen plane -> %d (%s)", (int)e, errname(e));
        if (e != PRO_TK_NO_ERROR) {
            ProView fv = NULL;
            if (ProViewNameToView((ProMdl)part, L"FRONT", &fv) != PRO_TK_NO_ERROR)
                ProViewNameToView((ProMdl)part, L"front", &fv);
            if (fv) {
                e = ProAnnotationplaneFromViewCreate((ProSolid)part, fv, &plane);
                logf_("[totm] fromView plane -> %d (%s)", (int)e, errname(e));
            }
        }
        ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&att_arr);
        ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sense_arr);
        att_arr[0][0] = s1;
        att_arr[0][1] = s2;
        memset(&sense_arr[0], 0, sizeof(ProDimSense));
        sense_arr[0].type = PRO_DIM_SNS_TYP_NONE;
        sense_arr[0].orient_hint = PRO_DIM_ORNT_HORIZ;
        loc[0] = (bb[0][0] + bb[1][0]) / 2.0;    /* 初始位置：零件上方（模型坐标�?*/
        loc[1] = bb[1][1] + 12.0;
        loc[2] = (bb[0][2] + bb[1][2]) / 2.0;
        memset(&nd, 0, sizeof(nd));
        e = ProDimensionCreate((ProSolid)part, PRO_REF_DIMENSION, &plane, att_arr, sense_arr,
                               PRO_DIM_ORNT_HORIZ, loc, &nd);
        logf_("[totm] ProDimensionCreate -> %d (%s)", (int)e, errname(e));
        ProArrayFree((ProArray *)&att_arr);
        ProArrayFree((ProArray *)&sense_arr);
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
        if (e == PRO_TK_NO_ERROR) {
            double  nv = -1.0;
            ProView fv = NULL;
            ProDimensionValueGet(&nd, &nv);
            logf_("[totm] created dim value=%.4f", nv);
            {
                ProView *views = NULL;
                if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                    int vc = 0, vi;
                    ProArraySizeGet((ProArray)views, &vc);
                    for (vi = 0; vi < vc; vi++) {
                        ProName nm;
                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                        if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { fv = views[vi]; break; }
                    }
                    ProArrayFree((ProArray *)&views);
                }
            }
            if (fv) {
                ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, fv);
                logf_("[totm] show on front -> %d (%s)", (int)es, errname(es));
            }
        }
    }
}

static void add_total_refdim(ProMdl drw, ProMdl part, double loc_x, double loc_y);   /* 阶梯B（图纸侧），定义在后 */

/* 总长 63.6（轮�?1 阶梯A）：模型侧参考尺寸（PRO_REF_DIMENSION，绝不建驱动尺寸）�?
   A1 平面：ActiveGet 优先 �?FromView(FRONT) �?FlatToScreen�?
   A2 附件/朝向：ProArrayAlloc �?ProArray�?
   A3 附件：两条竖直长边（内侧一�?= x�?.05 / 65.65，实测扫描）�?
   A4 location：两端点中点上方 +17（落在注释平面上，不�?(0,0,0)）�?
   A 失败 �?阶梯 B：图纸侧边对（add_total_refdim，ref_dim=PRO_B_FALSE，loc=(385.4,597.2)）�?*/
static void add_total_dim_model(ProMdl drw, ProMdl part)
{
    double     ux[160];
    int        uix[160], uc = 0, hid, q, r;
    ProPoint3d bb[2];

    bb[0][0] = bb[0][1] = bb[0][2] = -9999.0;
    bb[1][0] = bb[1][1] = bb[1][2] = -9999.0;
    ProSolidOutlineGet((ProSolid)part, bb);
    for (hid = 1; hid <= 3000 && uc < 160; hid++) {
        ProEdge  e = NULL;
        Pro3dPnt *pts = NULL;
        int      cnt = 0, t;
        double   exmin = 1e30, exmax = -1e30, eymin = 1e30, eymax = -1e30;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeTessellationGet(e, &pts, NULL, NULL, NULL, &cnt) != PRO_TK_NO_ERROR || !pts || cnt < 2) continue;
        for (t = 0; t < cnt; t++) {
            if (pts[t][0] < exmin) exmin = pts[t][0];
            if (pts[t][0] > exmax) exmax = pts[t][0];
            if (pts[t][1] < eymin) eymin = pts[t][1];
            if (pts[t][1] > eymax) eymax = pts[t][1];
        }
        ProArrayFree((ProArray *)&pts);
        if ((eymax - eymin) <= 20.0 || (exmax - exmin) > 2.0) continue;
        /* �?轮次13 强制检查：ProCurvedata 是联合体，非直线�?.line �?= 垃圾
           （上一�?z=+70.66 越界疑似此因）。只�?PRO_ENT_LINE 才可作候选�?*/
        {
            ProGeomitem      gi0;
            ProGeomitemdata *gd0 = NULL;
            int              ltype = -1;

            if (ProEdgeToGeomitem((ProSolid)part, e, &gi0) == PRO_TK_NO_ERROR &&
                ProGeomitemdataGet(&gi0, &gd0) == PRO_TK_NO_ERROR && gd0 && gd0->data.p_curve_data)
                ltype = gd0->data.p_curve_data->line.type;
            if (gd0) ProGeomitemdataFree(&gd0);
            logf_("[totm]   edge %d curve type=%d (line=%d)", hid, ltype, (int)PRO_ENT_LINE);
            if (ltype != PRO_ENT_LINE) continue;
        }
        {
            double cx = (exmin + exmax) / 2.0;
            int    dup = 0;
            for (q = 0; q < uc; q++) { double d = ux[q] - cx; if (d < 0) d = -d; if (d < 0.5) { dup = 1; break; } }
            if (!dup) { ux[uc] = cx; uix[uc] = hid; uc++; }
        }
    }
    for (q = 0; q < uc; q++)
        for (r = q + 1; r < uc; r++)
            if (ux[r] < ux[q]) { double td = ux[q]; int ti = uix[q]; ux[q] = ux[r]; uix[q] = uix[r]; ux[r] = td; uix[r] = ti; }
    logf_("[totm] vertical long LINE edges: %d", uc);
    for (q = 0; q < uc; q++) logf_("[totm]   x=%.2f edge=%d", ux[q], uix[q]);
    if (uc < 2) {
        logf_("[totm] not enough edges -> ladder B");
        add_total_refdim(drw, part, 385.4, 597.2);
        return;
    }
    {
        int          li = uc / 2 - 1, ri = uc / 2;   /* 最内一对（实测 = 2.05/65.65�?*/
        ProEdge      le = NULL, re = NULL;
        Pro3dPnt    *lpts = NULL, *rpts = NULL;
        int          lcnt = 0, rcnt = 0, t;
        double       lymin = 1e30, lymax = -1e30, lz = 0.0;
        double       rymin = 1e30, rymax = -1e30, rz = 0.0;
        ProGeomitem  gi1, gi2;
        ProSelection s1 = NULL, s2 = NULL;
        ProDimAttachment *att_arr = NULL;
        ProDimSense      *sense_arr = NULL;
        ProAnnotationPlane plane;
        ProPoint3d   loc;
        ProDimension nd;
        ProError     e;

        memset(&plane, 0, sizeof(plane));
        memset(&nd, 0, sizeof(nd));
        ProEdgeInit((ProSolid)part, uix[li], &le);
        ProEdgeInit((ProSolid)part, uix[ri], &re);
        if (ProEdgeTessellationGet(le, &lpts, NULL, NULL, NULL, &lcnt) == PRO_TK_NO_ERROR && lpts) {
            for (t = 0; t < lcnt; t++) {
                if (lpts[t][1] < lymin) lymin = lpts[t][1];
                if (lpts[t][1] > lymax) lymax = lpts[t][1];
                lz += lpts[t][2];
            }
            lz /= (lcnt > 0 ? (double)lcnt : 1.0);
            ProArrayFree((ProArray *)&lpts);
        }
        if (ProEdgeTessellationGet(re, &rpts, NULL, NULL, NULL, &rcnt) == PRO_TK_NO_ERROR && rpts) {
            for (t = 0; t < rcnt; t++) {
                if (rpts[t][1] < rymin) rymin = rpts[t][1];
                if (rpts[t][1] > rymax) rymax = rpts[t][1];
                rz += rpts[t][2];
            }
            rz /= (rcnt > 0 ? (double)rcnt : 1.0);
            ProArrayFree((ProArray *)&rpts);
        }
        /* A1 注释平面阶梯 */
        e = ProAnnotationplaneActiveGet((ProMdl)part, &plane);
        logf_("[totm] A1 ActiveGet -> %d (%s)", (int)e, errname(e));
        if (e != PRO_TK_NO_ERROR) {
            ProView fv0 = NULL;
            if (ProViewNameToView((ProMdl)part, L"FRONT", &fv0) != PRO_TK_NO_ERROR)
                ProViewNameToView((ProMdl)part, L"front", &fv0);
            if (fv0) {
                e = ProAnnotationplaneFromViewCreate((ProSolid)part, fv0, &plane);
                logf_("[totm] A1 FromView -> %d (%s)", (int)e, errname(e));
            }
        }
        if (e != PRO_TK_NO_ERROR) {
            e = ProAnnotationplaneFlatToScreenCreate((ProSolid)part, PRO_B_TRUE, &plane);
            logf_("[totm] A1 FlatToScreen -> %d (%s)", (int)e, errname(e));
        }
        if (ProEdgeToGeomitem((ProSolid)part, le, &gi1) != PRO_TK_NO_ERROR) return;
        if (ProEdgeToGeomitem((ProSolid)part, re, &gi2) != PRO_TK_NO_ERROR) return;
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &s1) != PRO_TK_NO_ERROR) return;
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); return; }
        ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&att_arr);
        ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sense_arr);
        att_arr[0][0] = s1;
        att_arr[0][1] = s2;
        memset(&sense_arr[0], 0, sizeof(ProDimSense));
        sense_arr[0].type = PRO_DIM_SNS_TYP_NONE;
        sense_arr[0].orient_hint = PRO_DIM_ORNT_HORIZ;
        /* A4'（轮�?2，单点改动）：loc = 两端边【曲线数据中点】的中点�?
           全程�?ProGeomitemdataGet（模型空间，axis_dtl_cb 已验证）�?
           绝不�?ProEdgeTessellationGet —�?其坐标不在模型空间（上一�?loc 越界即此因）�?*/
        {
            ProPoint3d pL, pR;
            ProGeomitemdata *gd = NULL;

            pL[0] = pL[1] = pL[2] = -9999.0;
            pR[0] = pR[1] = pR[2] = -9999.0;
            if (ProGeomitemdataGet(&gi1, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_curve_data) {
                ProCurvedata *cd = gd->data.p_curve_data;
                pL[0] = (cd->line.end1[0] + cd->line.end2[0]) / 2.0;
                pL[1] = (cd->line.end1[1] + cd->line.end2[1]) / 2.0;
                pL[2] = (cd->line.end1[2] + cd->line.end2[2]) / 2.0;
                ProGeomitemdataFree(&gd);
            }
            gd = NULL;
            if (ProGeomitemdataGet(&gi2, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_curve_data) {
                ProCurvedata *cd = gd->data.p_curve_data;
                pR[0] = (cd->line.end1[0] + cd->line.end2[0]) / 2.0;
                pR[1] = (cd->line.end1[1] + cd->line.end2[1]) / 2.0;
                pR[2] = (cd->line.end1[2] + cd->line.end2[2]) / 2.0;
                ProGeomitemdataFree(&gd);
            }
            logf_("[totm] edge mids: L=(%.2f,%.2f,%.2f) R=(%.2f,%.2f,%.2f)", pL[0], pL[1], pL[2], pR[0], pR[1], pR[2]);
            loc[0] = (pL[0] + pR[0]) / 2.0;
            loc[1] = (pL[1] + pR[1]) / 2.0;
            loc[2] = (pL[2] + pR[2]) / 2.0;
        }
        logf_("[totm] model-space loc=(%.2f,%.2f,%.2f)  [bbox x2.05..65.65 y147..189 z-70..-60]", loc[0], loc[1], loc[2]);
        e = ProDimensionCreate((ProSolid)part, PRO_REF_DIMENSION, &plane, att_arr, sense_arr,
                               PRO_DIM_ORNT_HORIZ, loc, &nd);
        logf_("[totm] A ProDimensionCreate -> %d (%s)", (int)e, errname(e));
        ProArrayFree((ProArray *)&att_arr);
        ProArrayFree((ProArray *)&sense_arr);
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
        if (e == PRO_TK_NO_ERROR) {
            double  nv = -1.0;
            ProView fv = NULL;
            ProDimensionValueGet(&nd, &nv);
            logf_("[totm] A created dim value=%.4f", nv);
            {
                ProView *views = NULL;
                if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                    int vc = 0, vi;
                    ProArraySizeGet((ProArray)views, &vc);
                    for (vi = 0; vi < vc; vi++) {
                        ProName nm;
                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                        if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { fv = views[vi]; break; }
                    }
                    ProArrayFree((ProArray *)&views);
                }
            }
            if (fv) {
                ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, fv);
                logf_("[totm] A show on front -> %d (%s)", (int)es, errname(es));
            }
            return;
        }
        logf_("[totm] A failed -> ladder B (drawing-side edges)");
        add_total_refdim(drw, part, 385.4, 597.2);
    }
}

/* ============ 终版：主动尺寸摆�?place_dims ============
   规则（轮�?8 规范）：
   1) 收集所有已显示关键尺寸的文本位�?P
   2) 归属视图 = 中心�?P 最近的视图；侧 = P 相对视图矩形在哪个方向（�?�?�?右）
   3) �?(视图,�? 一组内按值升序分层（小在内、大在外），层距 17 单位�?mm�?
   4) 水平尺寸�?y �?x；垂直尺寸改 x �?y
   5) �?之后绝不�?regen（regen 会把文本拉回�?*/
typedef struct {
    ProDimension *dim;          /* 仅收集趟记录（不跨回调使用） */
    double v, tx, ty;           /* �?/ 当前文本位置 */
    double nx, ny;              /* 目标位置 */
    int    vidx, side, layer;   /* side: 0=�?1=�?2=�?3=�?*/
    int    done;                /* 第二趟是否已移动 */
} placedim_t;

typedef struct {
    ProDrawing drw;
    ProSolid   solid;
    ProView   *views;
    int        vc;
    double     vx0[16], vy0[16], vx1[16], vy1[16];
    placedim_t items[64];
    int        n;
} place_ctx_t;

static ProError place_collect_cb(ProDimension *dim, ProError status, ProAppData app)
{
    place_ctx_t *c = (place_ctx_t *)app;
    double       v = 0.0;

    /* �?必须真正显示在图纸上（返�?NOT_DISPLAYED 的直接跳过；之前�?bug�?
       收集到了未显示的 0.25 实例并对它做 Move �?崩） */
    {
        ProView dv = NULL;
        int     found = -1, q;

        if (ProDrawingDimensionViewGet(c->drw, dim, &dv) != PRO_TK_NO_ERROR || !dv)
            return PRO_TK_NO_ERROR;
        for (q = 0; q < c->vc; q++) {
            if (c->views[q] == dv) { found = q; break; }
        }
        if (found < 0) return PRO_TK_NO_ERROR;   /* 视图不在表内：不�?*/
        if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
        if (!value_in_keep_list(v)) return PRO_TK_NO_ERROR;
        {
            int r;
            for (r = 0; r < c->n; r++) {
                double d = c->items[r].v - v;
                if (d < 0) d = -d;
                if (d < 0.02) return PRO_TK_NO_ERROR;   /* 同值去�?*/
            }
        }
        {
            ProDimlocation loc = NULL;
            ProBoolean    he = PRO_B_FALSE;
            ProPoint3d    p;
            double        el = 0.0;
            p[0] = p[1] = p[2] = -9999.0;
            if (ProDimensionLocationGet(dim, NULL, c->drw, &loc) == PRO_TK_NO_ERROR && loc)
                ProDimlocationTextGet(loc, &he, p, &el);
            if (p[0] < -9998.0 || c->n >= 64) return PRO_TK_NO_ERROR;
            c->items[c->n].dim = dim;
            c->items[c->n].v = v;
            c->items[c->n].tx = p[0];
            c->items[c->n].ty = p[1];
            c->items[c->n].vidx = found;   /* 真实归属视图下标 */
            c->items[c->n].side = -1;      /* 待算 */
            c->items[c->n].layer = 0;
            c->n++;
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 第二趟：�?visit 回调内用“新鲜”的尺寸指针执行移动（与命令通道 MOVE| 同款模式�?
   已验证不会崩；跨回调保存�?ProDimension* 会失效，是上一版只�?1 根的根因）�?*/
typedef struct {
    ProDrawing drw;
    place_ctx_t *c;
} place_move_ctx_t;

static ProError place_move_cb(ProDimension *dim, ProError status, ProAppData app)
{
    place_move_ctx_t *m = (place_move_ctx_t *)app;
    place_ctx_t      *c = m->c;
    double            v = 0.0;
    int               i;

    /* 与收集趟相同的“已显示”过�?*/
    {
        ProView dv = NULL;
        if (ProDrawingDimensionViewGet(m->drw, dim, &dv) != PRO_TK_NO_ERROR || !dv)
            return PRO_TK_NO_ERROR;
    }
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    for (i = 0; i < c->n; i++) {
        double d = c->items[i].v - v;
        if (d < 0) d = -d;
        if (d > 0.02) continue;
        if (c->items[i].done) return PRO_TK_NO_ERROR;
        {
            ProVector vec;
            ProError  e;
            vec[0] = c->items[i].nx;
            vec[1] = c->items[i].ny;
            vec[2] = 0.0;
            e = ProDrawingDimensionMove(m->drw, dim, vec);
            logf_("[place] %.4f v%d side%d layer%d  (%.1f,%.1f) -> (%.1f,%.1f)  move->%d(%s)",
                  v, c->items[i].vidx, c->items[i].side, c->items[i].layer,
                  c->items[i].tx, c->items[i].ty, c->items[i].nx, c->items[i].ny,
                  (int)e, errname(e));
            {
                /* 自检：重读文本位置（回调内指针新鲜） */
                ProDimlocation loc = NULL;
                ProBoolean    he = PRO_B_FALSE;
                ProPoint3d    p;
                double        el = 0.0;
                p[0] = p[1] = p[2] = -9999.0;
                if (ProDimensionLocationGet(dim, NULL, m->drw, &loc) == PRO_TK_NO_ERROR && loc)
                    ProDimlocationTextGet(loc, &he, p, &el);
                logf_("[place]   verify text now (%.1f,%.1f)", p[0], p[1]);
            }
        }
        c->items[i].done = 1;
        return PRO_TK_NO_ERROR;
    }
    return PRO_TK_NO_ERROR;
}

static void place_dims(ProMdl drw, ProMdl part)
{
    place_ctx_t c;
    place_move_ctx_t mv;
    int         i, q;
    const double step = 17.0;   /* 7mm */

    memset(&c, 0, sizeof(c));
    c.drw = (ProDrawing)drw;
    c.solid = (ProSolid)part;
    for (i = 0; i < 64; i++) c.items[i].vidx = -1;
    if (ProDrawingViewsCollect((ProDrawing)drw, &c.views) != PRO_TK_NO_ERROR || !c.views) return;
    ProArraySizeGet((ProArray)c.views, &c.vc);
    if (c.vc > 16) c.vc = 16;
    for (i = 0; i < c.vc; i++) {
        ProPoint3d box[2];
        box[0][0] = box[0][1] = box[0][2] = -9999.0;
        box[1][0] = box[1][1] = box[1][2] = -9999.0;
        ProDrawingViewOutlineGet((ProDrawing)drw, c.views[i], box);
        c.vx0[i] = box[0][0]; c.vy0[i] = box[0][1];
        c.vx1[i] = box[1][0]; c.vy1[i] = box[1][1];
    }
    /* —�?第一趟：收集 + 算归�?�?�?目标（不改动任何东西�?—�?*/
    ProSolidDimensionVisit(c.solid, PRO_B_FALSE, place_collect_cb, NULL, &c);
    logf_("[place] collected %d dims", c.n);
    for (i = 0; i < c.n; i++) {
        int bv = c.items[i].vidx;
        if (bv < 0) {
            double best = 1e18;
            bv = 0;
            for (q = 0; q < c.vc; q++) {
                double cx = (c.vx0[q] + c.vx1[q]) / 2.0;
                double cy = (c.vy0[q] + c.vy1[q]) / 2.0;
                double d = (c.items[i].tx - cx) * (c.items[i].tx - cx) + (c.items[i].ty - cy) * (c.items[i].ty - cy);
                if (d < best) { best = d; bv = q; }
            }
            c.items[i].vidx = bv;
        }
        {
            double t = c.items[i].ty, x = c.items[i].tx;
            double dU = t - c.vy1[bv];
            double dD = c.vy0[bv] - t;
            double dL = c.vx0[bv] - x;
            double dR = x - c.vx1[bv];
            if (dU >= 0.0 || dD >= 0.0 || dL >= 0.0 || dR >= 0.0) {
                double bestd = 1e18;
                int    bs = 0;
                if (dU >= 0.0 && dU < bestd) { bestd = dU; bs = 0; }
                if (dD >= 0.0 && dD < bestd) { bestd = dD; bs = 1; }
                if (dL >= 0.0 && dL < bestd) { bestd = dL; bs = 2; }
                if (dR >= 0.0 && dR < bestd) { bestd = dR; bs = 3; }
                c.items[i].side = bs;
            } else {
                double aU = c.vy1[bv] - t, aD = t - c.vy0[bv], aL = x - c.vx0[bv], aR = c.vx1[bv] - x;
                double m2 = aU; int bs = 0;
                if (aD < m2) { m2 = aD; bs = 1; }
                if (aL < m2) { m2 = aL; bs = 2; }
                if (aR < m2) { m2 = aR; bs = 3; }
                c.items[i].side = bs;
            }
        }
    }
    for (i = 0; i < c.n; i++) {
        int layer = 0;
        for (q = 0; q < c.n; q++) {
            if (q == i) continue;
            if (c.items[q].vidx != c.items[i].vidx || c.items[q].side != c.items[i].side) continue;
            {
                double d = c.items[q].v - c.items[i].v;
                if (d < 0) d = -d;
                if (c.items[q].v < c.items[i].v || (d < 0.02 && q < i)) layer++;
            }
        }
        c.items[i].layer = layer;
    }
    for (i = 0; i < c.n; i++) {
        double nx = c.items[i].tx, ny = c.items[i].ty;
        switch (c.items[i].side) {
        case 0: ny = c.vy1[c.items[i].vidx] + step + step * c.items[i].layer; break;
        case 1: ny = c.vy0[c.items[i].vidx] - step - step * c.items[i].layer; break;
        case 2: nx = c.vx0[c.items[i].vidx] - step - step * c.items[i].layer; break;
        default: nx = c.vx1[c.items[i].vidx] + step + step * c.items[i].layer; break;
        }
        c.items[i].nx = nx;
        c.items[i].ny = ny;
        c.items[i].done = 0;
        logf_("[place] target %.4f v%d side%d layer%d  (%.1f,%.1f) -> (%.1f,%.1f)",
              c.items[i].v, c.items[i].vidx, c.items[i].side, c.items[i].layer,
              c.items[i].tx, c.items[i].ty, nx, ny);
    }
    /* —�?第二趟：统一移动（回调内用新鲜指针） —�?*/
    mv.drw = (ProDrawing)drw;
    mv.c = &c;
    ProSolidDimensionVisit(c.solid, PRO_B_FALSE, place_move_cb, NULL, &mv);
    logf_("[place] done");
}

/* P1：总长参考尺寸。枚举竖直长边（span>20、x 恒定）→ �?x 排序 �?
   从最内侧一对开始试建（视图只显示零件本体；外侧的料带边不在视图内会 BAD_INPUTS）�?*/
static void add_total_refdim(ProMdl drw, ProMdl part, double loc_x, double loc_y)
{
    double ux[160];
    int    uix[160], uc = 0, hid, q, r;

    for (hid = 1; hid <= 3000 && uc < 160; hid++) {
        ProEdge e = NULL;
        Pro3dPnt *pts = NULL;
        int cnt = 0, t;
        double exmin = 1e30, exmax = -1e30, eymin = 1e30, eymax = -1e30;

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeTessellationGet(e, &pts, NULL, NULL, NULL, &cnt) != PRO_TK_NO_ERROR || !pts || cnt < 2) continue;
        for (t = 0; t < cnt; t++) {
            if (pts[t][0] < exmin) exmin = pts[t][0];
            if (pts[t][0] > exmax) exmax = pts[t][0];
            if (pts[t][1] < eymin) eymin = pts[t][1];
            if (pts[t][1] > eymax) eymax = pts[t][1];
        }
        ProArrayFree((ProArray *)&pts);
        if ((eymax - eymin) <= 20.0 || (exmax - exmin) > 2.0) continue;   /* 只看竖直长边 */
        /* �?轮次13 强制检查：只有 PRO_ENT_LINE 才可作候选（联合体误读防护） */
        {
            ProGeomitem      gi0;
            ProGeomitemdata *gd0 = NULL;
            int              ltype = -1;

            if (ProEdgeToGeomitem((ProSolid)part, e, &gi0) == PRO_TK_NO_ERROR &&
                ProGeomitemdataGet(&gi0, &gd0) == PRO_TK_NO_ERROR && gd0 && gd0->data.p_curve_data)
                ltype = gd0->data.p_curve_data->line.type;
            if (gd0) ProGeomitemdataFree(&gd0);
            logf_("[tot]   edge %d curve type=%d (line=%d)", hid, ltype, (int)PRO_ENT_LINE);
            if (ltype != PRO_ENT_LINE) continue;
        }
        {
            double cx = (exmin + exmax) / 2.0;
            int    dup = 0;
            for (q = 0; q < uc; q++) { double d = ux[q] - cx; if (d < 0) d = -d; if (d < 0.5) { dup = 1; break; } }
            if (!dup) { ux[uc] = cx; uix[uc] = hid; uc++; }
        }
    }
    logf_("[tot] vertical long LINE edges: %d", uc);
    for (q = 0; q < uc; q++) logf_("[tot]   x=%.2f edge=%d", ux[q], uix[q]);
    for (q = 0; q < uc; q++)          /* 升序排序 */
        for (r = q + 1; r < uc; r++)
            if (ux[r] < ux[q]) {
                double td = ux[q];
                int    ti = uix[q];
                ux[q] = ux[r]; uix[q] = uix[r];
                ux[r] = td;    uix[r] = ti;
            }
    if (uc < 2) { logf_("[tot] not enough vertical edges"); return; }
    {
        int      off;
        ProView  front = NULL;
        ProView *views = NULL;

        /* 找主视图（选择器需要带视图上下文：ProSelectionViewSet�?*/
        if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
            int vc = 0, vi;
            ProArraySizeGet((ProArray)views, &vc);
            for (vi = 0; vi < vc; vi++) {
                ProName nm;
                nm[0] = 0;
                ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[vi]; break; }
            }
            ProArrayFree((ProArray *)&views);
        }
        logf_("[tot] front view -> %p", (void *)front);

        for (off = 0; off <= uc / 2; off++) {
            int li = uc / 2 - 1 - off;
            int ri = uc / 2 + off;
            ProEdge           le = NULL, re = NULL;
            ProGeomitem       gi1, gi2;
            ProSelection      s1 = NULL, s2 = NULL;
            ProDimAttachment *att_arr = NULL;
            ProDimSense      *sense_arr = NULL;
            ProDimension      nd;
            ProPoint3d        loc;
            ProError          e;

            if (li < 0 || ri >= uc) break;
            logf_("[tot] try pair x=%.2f (e%d) .. x=%.2f (e%d)", ux[li], uix[li], ux[ri], uix[ri]);
            ProEdgeInit((ProSolid)part, uix[li], &le);
            ProEdgeInit((ProSolid)part, uix[ri], &re);
            if (ProEdgeToGeomitem((ProSolid)part, le, &gi1) != PRO_TK_NO_ERROR) continue;
            if (ProEdgeToGeomitem((ProSolid)part, re, &gi2) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &s1) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &s2) != PRO_TK_NO_ERROR) {
                ProSelectionFree(&s1);
                continue;
            }
            if (front) {
                ProSelectionViewSet(front, s1);
                ProSelectionViewSet(front, s2);
            }
            /* 文档要求 ProArray 形式的附�?朝向数组 */
            ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&att_arr);
            ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sense_arr);
            att_arr[0][0] = s1;
            att_arr[0][1] = s2;
            memset(&sense_arr[0], 0, sizeof(ProDimSense));
            sense_arr[0].type = PRO_DIM_SNS_TYP_NONE;
            sense_arr[0].orient_hint = PRO_DIM_ORNT_HORIZ;
            loc[0] = loc_x; loc[1] = loc_y; loc[2] = 0.0;
            memset(&nd, 0, sizeof(nd));
            /* 文档：ref_dim=PRO_B_FALSE 创建 PRO_REF_DIMENSION（参考尺寸），TRUE 是普通尺�?*/
            e = ProDrawingDimensionCreate((ProDrawing)drw, att_arr, sense_arr, PRO_DIM_ORNT_HORIZ, loc, PRO_B_FALSE, &nd);
            logf_("[tot] create -> %d (%s)", (int)e, errname(e));
            ProArrayFree((ProArray *)&att_arr);
            ProArrayFree((ProArray *)&sense_arr);
            ProSelectionFree(&s1);
            ProSelectionFree(&s2);
            if (e == PRO_TK_NO_ERROR) {
                double nv = -1;
                ProDimensionValueGet(&nd, &nv);
                logf_("[tot] created dim value=%.4f", nv);
                break;
            }
        }
    }
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* P1 v3：总长参考尺寸（"面版"）。扫 X 法向平面 �?从最外一对开始试建参考尺�?
   （ref_dim=PRO_B_FALSE �?PRO_REF_DIMENSION 参考尺寸，不驱动模型）�?*/
static void add_total_refdim_faces(ProMdl drw, ProMdl part, double loc_x, double loc_y)
{
    double px[64];
    int    pids[64], pc = 0, id, q, r;

    for (id = 1; id <= 4000 && pc < 64; id++) {
        ProSurface s = NULL;
        if (ProSurfaceInit((ProMdl)part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        {
            ProGeomitemdata *gd = NULL;
            if (ProSurfaceDataGet(s, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_surface_data) {
                ProSurfacedata *sd = gd->data.p_surface_data;
                if (sd->type == PRO_SRF_PLANE) {
                    double nx = sd->srf_shape.plane.e3[0];
                    double px0 = sd->srf_shape.plane.origin[0];
                    if (nx > 0.99 || nx < -0.99) {
                        int dup = 0;
                        for (q = 0; q < pc; q++) { double d = px[q] - px0; if (d < 0) d = -d; if (d < 0.3) { dup = 1; break; } }
                        if (!dup) { px[pc] = px0; pids[pc] = id; pc++; }
                    }
                }
            }
            if (gd) ProGeomitemdataFree(&gd);
        }
    }
    logf_("[totf] X-normal planes: %d", pc);
    for (q = 0; q < pc; q++) logf_("[totf]   x=%.2f surf=%d", px[q], pids[q]);
    for (q = 0; q < pc; q++)          /* 升序 */
        for (r = q + 1; r < pc; r++)
            if (px[r] < px[q]) { double td = px[q]; int ti = pids[q]; px[q] = px[r]; pids[q] = pids[r]; px[r] = td; pids[r] = ti; }
    if (pc < 2) return;
    {
        int off;
        for (off = 0; off <= pc / 2; off++) {
            int li = off;
            int ri = pc - 1 - off;
            ProSurface        ls = NULL, rs = NULL;
            ProGeomitem       gi1, gi2;
            ProSelection      s1 = NULL, s2 = NULL;
            ProDimAttachment *att_arr = NULL;
            ProDimSense      *sense_arr = NULL;
            ProDimension      nd;
            ProPoint3d        loc;
            ProError          e;
            ProView           front = NULL;
            ProView          *views = NULL;

            if (li >= ri) break;
            ProSurfaceInit((ProMdl)part, pids[li], &ls);
            ProSurfaceInit((ProMdl)part, pids[ri], &rs);
            if (!ls || !rs) continue;
            if (ProSurfaceToGeomitem((ProSolid)part, ls, &gi1) != PRO_TK_NO_ERROR) continue;
            if (ProSurfaceToGeomitem((ProSolid)part, rs, &gi2) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi1, &s1) != PRO_TK_NO_ERROR) continue;
            if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &s2) != PRO_TK_NO_ERROR) { ProSelectionFree(&s1); continue; }
            if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                int vc2 = 0, vi;
                ProArraySizeGet((ProArray)views, &vc2);
                for (vi = 0; vi < vc2; vi++) {
                    ProName nm;
                    nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                    if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[vi]; break; }
                }
                ProArrayFree((ProArray *)&views);
            }
            if (front) { ProSelectionViewSet(front, s1); ProSelectionViewSet(front, s2); }
            ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&att_arr);
            ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sense_arr);
            att_arr[0][0] = s1;
            att_arr[0][1] = s2;
            memset(&sense_arr[0], 0, sizeof(ProDimSense));
            sense_arr[0].type = PRO_DIM_SNS_TYP_NONE;
            sense_arr[0].orient_hint = PRO_DIM_ORNT_HORIZ;
            loc[0] = loc_x; loc[1] = loc_y; loc[2] = 0.0;
            memset(&nd, 0, sizeof(nd));
            logf_("[totf] try x=%.2f..%.2f (surf %d / %d) @(%.0f,%.0f)", px[li], px[ri], pids[li], pids[ri], loc_x, loc_y);
            e = ProDrawingDimensionCreate((ProDrawing)drw, att_arr, sense_arr, PRO_DIM_ORNT_HORIZ, loc, PRO_B_FALSE, &nd);
            logf_("[totf] create -> %d (%s)", (int)e, errname(e));
            ProArrayFree((ProArray *)&att_arr);
            ProArrayFree((ProArray *)&sense_arr);
            ProSelectionFree(&s1);
            ProSelectionFree(&s2);
            if (e == PRO_TK_NO_ERROR) {
                double nv = -1;
                ProDimensionValueGet(&nd, &nv);
                logf_("[totf] created dim value=%.4f", nv);
                break;
            }
        }
    }
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* &MASS[.4] �?参数�?文本，StringSet/Modify 均无效（实测）；改为删除旧注记、原挂接位置重建纯文�?*/
static int replace_note_with_text(ProMdl drw, const wchar_t *from, const wchar_t *to)
{
    ProDtlnote *notes = NULL;
    int cnt = 0, i;

    if (ProDrawingDtlnotesCollect((ProDrawing)drw, NULL, 1, &notes) != PRO_TK_NO_ERROR || !notes) return 0;
    ProArraySizeGet((ProArray)notes, &cnt);
    for (i = 0; i < cnt; i++) {
        ProDtlnotedata nd = NULL;
        if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) != PRO_TK_NO_ERROR || !nd) continue;
        {
            ProDtlnoteline *lines = NULL;
            if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) continue;
            {
                int lc = 0;
                ProArraySizeGet((ProArray)lines, &lc);
                if (lc == 1) {
                    ProDtlnotetext *txts = NULL;
                    if (ProDtlnotelineTextsCollect(lines[0], &txts) == PRO_TK_NO_ERROR && txts) {
                        int tc = 0;
                        ProArraySizeGet((ProArray)txts, &tc);
                        if (tc >= 1) {
                            ProLine s;
                            s[0] = 0;
                            if (ProDtlnotetextStringGet(txts[0], s) == PRO_TK_NO_ERROR && !wcscmp(s, from)) {
                                ProDtlattach     att = NULL;
                                ProDtlattachType ty = PRO_DTLATTACHTYPE_FREE;
                                ProView          vw = NULL;
                                ProVector        loc;
                                ProSelection     ap = NULL;
                                ProError         e;

                                loc[0] = loc[1] = loc[2] = 0.0;
                                if (ProDtlnotedataAttachmentGet(nd, &att) == PRO_TK_NO_ERROR && att)
                                    ProDtlattachGet(att, &ty, &vw, loc, &ap);
                                logf_("[replnote] '%ls' at (%.1f,%.1f) type=%d", from, loc[0], loc[1], (int)ty);
                                ProArrayFree((ProArray *)&txts);
                                ProArrayFree((ProArray *)&lines);
                                {
                                    ProError ed = ProDtlnoteDelete(&notes[i], NULL);
                                    logf_("[replnote] delete -> %d (%s)", (int)ed, errname(ed));
                                }
                                ProArrayFree((ProArray *)&notes);
                                {
                                    ProDtlnotedata d2 = NULL;
                                    ProDtlattach   a2 = NULL;
                                    ProDtlnote     note2;
                                    ProLine        s2;
                                    wcsncpy_s(s2, PRO_LINE_SIZE, to, _TRUNCATE);
                                    if (ProDtlnotedataAlloc(drw, &d2) != PRO_TK_NO_ERROR) return 0;
                                    {
                                        ProDtlnotetext text = NULL;
                                        ProDtlnoteline line = NULL;
                                        ProDtlnotetextAlloc(&text);
                                        ProDtlnotetextStringSet(text, s2);
                                        ProDtlnotelineAlloc(&line);
                                        ProDtlnotelineTextAdd(line, text);
                                        ProDtlnotedataLineAdd(d2, line);
                                    }
                                    e = ProDtlattachAlloc(PRO_DTLATTACHTYPE_FREE, NULL, loc, NULL, &a2);
                                    if (e == PRO_TK_NO_ERROR) ProDtlnotedataAttachmentSet(d2, a2);
                                    ProDtlnotedataDisplayedSet(d2, PRO_B_TRUE);
                                    e = ProDtlnoteCreate(drw, NULL, d2, &note2);
                                    logf_("[replnote] create '%ls' -> %d (%s)", to, (int)e, errname(e));
                                    if (a2) ProDtlattachFree(a2);
                                    ProDtlnotedataFree(d2);
                                }
                                return 1;
                            }
                        }
                        ProArrayFree((ProArray *)&txts);
                    }
                }
                ProArrayFree((ProArray *)&lines);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
    return 0;
}

/* P3：中心线。把零件坐标系的轴在各视图里“显示”出来（图纸里轴=点画线中心线�?*/
typedef struct { ProDrawing drw; ProView *views; int vc; int front_first; } axline_ctx_t;

static ProError axis_show_cb(ProAxis axis, ProError status, ProAppData app)
{
    axline_ctx_t *c = (axline_ctx_t *)app;
    int i;

    for (i = 0; i < c->vc; i++) {
        ProError e = ProAnnotationShow((ProAnnotation *)axis, NULL, c->views[i]);
        logf_("[axis] show %p view%d -> %d (%s)", (void *)axis, i, (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) break;
    }
    return PRO_TK_NO_ERROR;
}

/* P3 安全版：�?-> geomitem -> 3D选择(带视�? -> ProDrawingAxisToDtlaxis 生成图纸�?点画�?�?
   （旧实现�?ProAxis 强转 ProAnnotation �?Show，触发致命错�?+ 管线自重启，已废弃。）
   定位：视图轮�?outline) �?模型包围�?bbox) 做线性映射（实测 transform API �?outline
   不同系，映射更可靠）：sx = out0x + (mx-bbox0x)*(out1x-out0x)/(bbox1x-bbox0x)�?*/
typedef struct {
    ProDrawing drw; ProSolid solid; ProView front;
    ProPoint3d out0, out1, box0, box1;
    int ok, fail;
} axdtl_ctx_t;

static ProError axis_dtl_cb(ProAxis axis, ProError status, ProAppData app)
{
    axdtl_ctx_t *c = (axdtl_ctx_t *)app;
    ProGeomitem  gi;
    ProSelection s3d = NULL, s2d = NULL;
    ProError     e;

    e = ProAxisToGeomitem(c->solid, axis, &gi);
    if (e != PRO_TK_NO_ERROR) { logf_("[axisdtl] ToGeomitem -> %d (%s)", (int)e, errname(e)); c->fail++; return PRO_TK_NO_ERROR; }
    e = ProSelectionAlloc(NULL, (ProModelitem *)&gi, &s3d);
    if (e != PRO_TK_NO_ERROR) { logf_("[axisdtl] selAlloc -> %d (%s)", (int)e, errname(e)); c->fail++; return PRO_TK_NO_ERROR; }
    if (c->front) ProSelectionViewSet(c->front, s3d);
    {
        ProGeomitemdata *gd = NULL;
        ProPoint3d       loc;

        loc[0] = loc[1] = loc[2] = 0.0;
        if (ProGeomitemdataGet(&gi, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_curve_data) {
            ProCurvedata *cd = gd->data.p_curve_data;
            /* �?直线：取两端中点（union 直读 .line；type 值仅记录�?*/
            loc[0] = (cd->line.end1[0] + cd->line.end2[0]) / 2.0;
            loc[1] = (cd->line.end1[1] + cd->line.end2[1]) / 2.0;
            loc[2] = (cd->line.end1[2] + cd->line.end2[2]) / 2.0;
            logf_("[axisdtl] axis type=%d mid(model)=(%.1f,%.1f,%.1f)", cd->line.type, loc[0], loc[1], loc[2]);
            ProGeomitemdataFree(&gd);
        } else {
            logf_("[axisdtl] no curve data; loc=(0,0,0)");
        }
        /* 模型坐标 -> 视图轮廓坐标（线性映射；outline 与尺�?loc 同系�?*/
        {
            double bw = c->box1[0] - c->box0[0];
            double bh = c->box1[1] - c->box0[1];
            if (bw > 1e-6 && bh > 1e-6 && c->out0[0] > -9998.0) {
                ProPoint3d p2;
                p2[0] = c->out0[0] + (loc[0] - c->box0[0]) * (c->out1[0] - c->out0[0]) / bw;
                p2[1] = c->out0[1] + (loc[1] - c->box0[1]) * (c->out1[1] - c->out0[1]) / bh;
                logf_("[axisdtl]   bbox->outline = (%.2f, %.2f)", p2[0], p2[1]);
                loc[0] = p2[0]; loc[1] = p2[1]; loc[2] = 0.0;
            } else {
                logf_("[axisdtl]   no outline/bbox; use raw point");
            }
        }
        e = ProDrawingAxisToDtlaxis(c->drw, s3d, loc, &s2d);
        logf_("[axisdtl] AxisToDtlaxis @(%.1f,%.1f) -> %d (%s)", loc[0], loc[1], (int)e, errname(e));
        if (e == PRO_TK_NO_ERROR) {
            c->ok++;
            if (s2d) ProSelectionFree(&s2d);
        } else {
            c->fail++;
        }
    }
    ProSelectionFree(&s3d);
    return PRO_TK_NO_ERROR;
}

/* ---- 会话内补建基准轴（不存盘�?---
   模型里凸点没有轴 �?扫圆柱面 �?建“穿过圆柱面”的基准轴（PRO_DTMAXIS_CONSTR_TYPE_THRU）�?
   轴随后由 PRO_AXIS 显示机制画成中心线。仅内存生效，模型文件不被改动�?*/
static int create_axis_thru_surface(ProSolid part, ProSurface surf, const char *tag)
{
    ProElement   root = NULL, e_type = NULL, e_consarr = NULL, e_cons = NULL, e_ct = NULL, e_cr = NULL;
    ProGeomitem  gi;
    ProSelection sel_surf = NULL, sel_model = NULL;
    ProArray     carr = NULL;
    ProElement   kids[2], ck[2];
    ProFeature   feat;
    ProErrorlist errs;
    ProError     e;

    memset(&feat, 0, sizeof(feat));
    memset(&errs, 0, sizeof(errs));
    if (ProSurfaceToGeomitem(part, surf, &gi) != PRO_TK_NO_ERROR) { logf_("[mkaxis] %s: ToGeomitem failed", tag); return 0; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&gi, &sel_surf) != PRO_TK_NO_ERROR) { logf_("[mkaxis] %s: selSurface failed", tag); return 0; }
    ProSelectionAlloc(NULL, NULL, &sel_model);   /* 空选择 = 当前模型 */

    ProElementAlloc(PRO_E_FEATURE_TREE, &root);
    ProElementAlloc(PRO_E_FEATURE_TYPE, &e_type);
    ProElementIntegerSet(e_type, PRO_FEAT_DATUM_AXIS);
    ProElementAlloc(PRO_E_DTMAXIS_CONSTR_TYPE, &e_ct);
    ProElementIntegerSet(e_ct, PRO_DTMAXIS_CONSTR_TYPE_THRU);
    ProElementAlloc(PRO_E_DTMAXIS_CONSTR_REF, &e_cr);
    ProElementReferenceSet(e_cr, sel_surf);
    ck[0] = e_ct;
    ck[1] = e_cr;
    ProElementAlloc(PRO_E_DTMAXIS_CONSTRAINT, &e_cons);
    ProElementChildrenSet(e_cons, NULL, ck, 2);
    ProArrayObjectAdd(&carr, -1, 1, &e_cons);
    ProElementAlloc(PRO_E_DTMAXIS_CONSTRAINTS, &e_consarr);
    ProElementArraySet(e_consarr, NULL, (ProElement *)carr);
    kids[0] = e_type;
    kids[1] = e_consarr;
    ProElementChildrenSet(root, NULL, kids, 2);

    e = ProFeatureWithoptionsCreate(sel_model, root, NULL, 0, &feat, &errs);
    logf_("[mkaxis] %s: create -> %d (%s) feat.id=%d", tag, (int)e, errname(e), (int)feat.id);
    if (sel_surf) ProSelectionFree(&sel_surf);
    if (sel_model) ProSelectionFree(&sel_model);
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* 扫描圆柱面：CYLSCAN 打日志；range>0 时对半径在范围内的圆柱面建轴 */
typedef struct { double rmin, rmax; } cylscan_ctx_t;

static void cylscan(ProSolid part, double rmin, double rmax)
{
    int id, made = 0, seen = 0;

    for (id = 1; id <= 4000; id++) {
        ProSurface s = NULL;
        if (ProSurfaceInit((ProMdl)part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        {
            ProGeomitemdata *gd = NULL;
            if (ProSurfaceDataGet(s, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_surface_data) {
                ProSurfacedata *sd = gd->data.p_surface_data;
                if (sd->type == PRO_SRF_CYL) {
                    double r = sd->srf_shape.cylinder.radius;
                    double ox = sd->srf_shape.cylinder.origin[0];
                    double oy = sd->srf_shape.cylinder.origin[1];
                    double oz = sd->srf_shape.cylinder.origin[2];
                    seen++;
                    logf_("[cyl] id=%d r=%.3f origin=(%.2f,%.2f,%.2f)", id, r, ox, oy, oz);
                    if (rmin > 0.0 && r >= rmin && r <= rmax) {
                        char tag[64];
                        sprintf_s(tag, sizeof(tag), "cyl%d", id);
                        if (create_axis_thru_surface(part, s, tag)) made++;
                    }
                }
            }
            if (gd) ProGeomitemdataFree(&gd);
        }
    }
    logf_("[cyl] total cylinder faces=%d, axes created=%d", seen, made);
}

/* ===== 轮次57：特征驱动探针（纯几何识别，不引用任何尺寸值）=====
   用户批评：一直是"值驱�?（先假定该是 0.25/0.35，再搜哪个几何对等值）——错�?
   改为特征驱动：先按几何属性认零件上的特征（凸�?凸台/基准�?外形极值）�?
   尺寸�?特征"定义，值让 Creo 算�?
   本命令只做【识别与枚举】：面（类型+范围+参数）、小面片（凸点候选）�?
   圆柱/圆锥面（凸台候选）、z 法向平面�?z 档分组、小�?弧边（凸点轮廓）�?*/

static const char *srf_typename(ProSrftype t)
{
    switch (t) {
    case PRO_SRF_PLANE:   return "PLANE";
    case PRO_SRF_CYL:     return "CYL";
    case PRO_SRF_CONE:    return "CONE";
    case PRO_SRF_TORUS:   return "TORUS";
    case PRO_SRF_COONS:   return "COONS";
    case PRO_SRF_SPL:     return "SPL";
    case PRO_SRF_FIL:     return "FIL";
    case PRO_SRF_RUL:     return "RUL";
    case PRO_SRF_REV:     return "REV";
    case PRO_SRF_TABCYL:  return "TABCYL";
    case PRO_SRF_B_SPL:   return "B_SPL";
    case PRO_SRF_FOREIGN: return "FOREIGN";
    case PRO_SRF_CYL_SPL: return "CYL_SPL";
    case PRO_SRF_SPL2DER: return "SPL2DER";
    default:              return "OTHER";
    }
}

typedef struct { double x, y, z, foot; int n; double zlo, zhi; const char *tname; } feat_grp_t;

static void featid_run(ProMdl part)
{
    ab_bbox_t  bb;
    feat_grp_t grp[160];
    int        ngrp = 0;
    int        npl = 0, ncyl = 0, ncone = 0, ntor = 0, nspl = 0, nother = 0;
    int        id;

    ab_bbox_get(part, &bb);
    logf_("[feat] ==== FEATURE INVENTORY (pure geometry / NO dim values) ====");
    logf_("[feat] bbox x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]  L=%.3f W=%.3f T=%.3f",
          bb.x0, bb.x1, bb.y0, bb.y1, bb.z0, bb.z1,
          bb.x1 - bb.x0, bb.y1 - bb.y0, bb.z1 - bb.z0);

    for (id = 100; id <= 6000; id++) {
        ProSurface      s = NULL;
        ProGeomitemdata *gd = NULL;
        ProSurfacedata  *sd;
        double          ex, ey, ez, foot, cx, cy, cz;

        if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
        if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        sd = gd->data.p_surface_data;
        ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
        ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
        ez = fabs(sd->xyz_max[2] - sd->xyz_min[2]);
        foot = ex > ey ? ex : ey;
        cx = (sd->xyz_min[0] + sd->xyz_max[0]) / 2.0;
        cy = (sd->xyz_min[1] + sd->xyz_max[1]) / 2.0;
        cz = (sd->xyz_min[2] + sd->xyz_max[2]) / 2.0;

        if (sd->type == PRO_SRF_PLANE) {
            npl++;
            logf_("[feat] srf %4d PLANE  ext=(%.2f,%.2f,%.2f) org=(%.3f,%.3f,%.3f) n=(%.3f,%.3f,%.3f)",
                  id, ex, ey, ez,
                  sd->srf_shape.plane.origin[0], sd->srf_shape.plane.origin[1], sd->srf_shape.plane.origin[2],
                  sd->srf_shape.plane.e3[0], sd->srf_shape.plane.e3[1], sd->srf_shape.plane.e3[2]);
        } else if (sd->type == PRO_SRF_CYL) {
            ncyl++;
            logf_("[feat] srf %4d CYL    r=%.3f ext=(%.2f,%.2f,%.2f) org=(%.3f,%.3f,%.3f) ax=(%.3f,%.3f,%.3f)",
                  id, sd->srf_shape.cylinder.radius, ex, ey, ez,
                  sd->srf_shape.cylinder.origin[0], sd->srf_shape.cylinder.origin[1], sd->srf_shape.cylinder.origin[2],
                  sd->srf_shape.cylinder.e3[0], sd->srf_shape.cylinder.e3[1], sd->srf_shape.cylinder.e3[2]);
        } else if (sd->type == PRO_SRF_CONE) {
            ncone++;
            logf_("[feat] srf %4d CONE   alpha=%.4f ext=(%.2f,%.2f,%.2f) org=(%.3f,%.3f,%.3f) ax=(%.3f,%.3f,%.3f)",
                  id, sd->srf_shape.cone.alpha, ex, ey, ez,
                  sd->srf_shape.cone.origin[0], sd->srf_shape.cone.origin[1], sd->srf_shape.cone.origin[2],
                  sd->srf_shape.cone.e3[0], sd->srf_shape.cone.e3[1], sd->srf_shape.cone.e3[2]);
        } else if (sd->type == PRO_SRF_TORUS) {
            ntor++;
            logf_("[feat] srf %4d TORUS  r1=%.3f r2=%.3f ext=(%.2f,%.2f,%.2f) org=(%.3f,%.3f,%.3f) ax=(%.3f,%.3f,%.3f)",
                  id, sd->srf_shape.torus.radius1, sd->srf_shape.torus.radius2, ex, ey, ez,
                  sd->srf_shape.torus.origin[0], sd->srf_shape.torus.origin[1], sd->srf_shape.torus.origin[2],
                  sd->srf_shape.torus.e3[0], sd->srf_shape.torus.e3[1], sd->srf_shape.torus.e3[2]);
        } else {
            if (sd->type == PRO_SRF_SPL || sd->type == PRO_SRF_B_SPL || sd->type == PRO_SRF_CYL_SPL ||
                sd->type == PRO_SRF_SPL2DER || sd->type == PRO_SRF_REV || sd->type == PRO_SRF_RUL ||
                sd->type == PRO_SRF_TABCYL) nspl++; else nother++;
            logf_("[feat] srf %4d %-7s ext=(%.2f,%.2f,%.2f) c=(%.3f,%.3f,%.3f) z[%.3f..%.3f]",
                  id, srf_typename(sd->type), ex, ey, ez, cx, cy, cz,
                  sd->xyz_min[2], sd->xyz_max[2]);
        }

        /* 小面片（凸点候选）：footprint <= 3mm，任意类型都�?*/
        if (foot <= 3.0 && ez <= 3.0) {
            int g, hitg = -1;
            for (g = 0; g < ngrp; g++) {
                if (fabs(grp[g].x - cx) < 1.0 && fabs(grp[g].y - cy) < 1.0) { hitg = g; break; }
            }
            if (hitg < 0 && ngrp < 160) {
                hitg = ngrp++;
                grp[hitg].x = cx; grp[hitg].y = cy; grp[hitg].z = cz;
                grp[hitg].foot = foot; grp[hitg].n = 0;
                grp[hitg].zlo = sd->xyz_min[2]; grp[hitg].zhi = sd->xyz_max[2];
                grp[hitg].tname = srf_typename(sd->type);
            }
            if (hitg >= 0) {
                grp[hitg].n++;
                if (sd->xyz_min[2] < grp[hitg].zlo) grp[hitg].zlo = sd->xyz_min[2];
                if (sd->xyz_max[2] > grp[hitg].zhi) grp[hitg].zhi = sd->xyz_max[2];
                if (foot > grp[hitg].foot) grp[hitg].foot = foot;
                logf_("[feat]   small srf %4d %-7s ext=(%.2f,%.2f,%.2f) c=(%.3f,%.3f,%.3f) z[%.3f..%.3f]",
                      id, srf_typename(sd->type), ex, ey, ez, cx, cy, cz, sd->xyz_min[2], sd->xyz_max[2]);
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[feat] surface counts: planes=%d cyl=%d cone=%d torus=%d spl/rev/rul=%d other=%d",
          npl, ncyl, ncone, ntor, nspl, nother);

    /* 小面片分组汇总（凸点候选簇�?*/
    {
        int g;
        logf_("[feat] --- small-surface groups (footprint<=3mm, xy within 1mm) : %d ---", ngrp);
        for (g = 0; g < ngrp; g++) {
            logf_("[feat] grp[%2d] c=(%.3f,%.3f) n=%d z[%.3f..%.3f] maxfoot=%.2f firsttype=%s",
                  g, grp[g].x, grp[g].y, grp[g].n, grp[g].zlo, grp[g].zhi,
                  grp[g].foot, grp[g].tname);
        }
    }

    /* 小圆/弧边扫描（凸点轮廓、孔、凸台圆�?*/
    {
        int nsmall = 0, nbig = 0;
        logf_("[feat] --- circle/arc edges ---");
        for (id = 1; id <= 6000; id++) {
            ProEdge e = NULL;
            ProGeomitem gi;
            ProGeomitemdata *gd = NULL;
            int t;

            if (ProEdgeInit(part, id, &e) != PRO_TK_NO_ERROR || !e) continue;
            if (ProEdgeToGeomitem(part, e, &gi) != PRO_TK_NO_ERROR) continue;
            if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            t = gd->data.p_curve_data->line.type;
            if (t == PRO_ENT_CIRCLE) {
                ProCircledata *cd = &gd->data.p_curve_data->circle;
                if (cd->radius <= 3.0) {
                    logf_("[feat] circ %4d r=%.3f c=(%.3f,%.3f,%.3f) n=(%.3f,%.3f,%.3f)",
                          id, cd->radius, cd->center[0], cd->center[1], cd->center[2],
                          cd->norm_axis_unit_vect[0], cd->norm_axis_unit_vect[1], cd->norm_axis_unit_vect[2]);
                    nsmall++;
                } else {
                    logf_("[feat] circ %4d r=%.3f c=(%.3f,%.3f,%.3f) n=(%.3f,%.3f,%.3f)",
                          id, cd->radius, cd->center[0], cd->center[1], cd->center[2],
                          cd->norm_axis_unit_vect[0], cd->norm_axis_unit_vect[1], cd->norm_axis_unit_vect[2]);
                    nbig++;
                }
            } else if (t == PRO_ENT_ARC) {
                ProArcdata *ad = &gd->data.p_curve_data->arc;
                if (ad->radius <= 3.0) {
                    logf_("[feat] arc  %4d r=%.3f org=(%.3f,%.3f,%.3f) a[%.2f..%.2f]",
                          id, ad->radius, ad->origin[0], ad->origin[1], ad->origin[2],
                          ad->start_angle, ad->end_angle);
                    nsmall++;
                }
            }
            ProGeomitemdataFree(&gd);
        }
        logf_("[feat] circle/arc edges: r<=3 -> %d, r>3 -> %d", nsmall, nbig);
    }

    /* z 法向平面�?z 档分组（基准�?台阶面清点） */
    {
        double zl[32];
        int    zc = 0, n;
        logf_("[feat] --- z-normal PLANE levels (|nz|>0.99) ---");
        for (n = 0; n < 32; n++) zl[n] = -9999.0;
        for (id = 100; id <= 6000; id++) {
            ProSurface s = NULL;
            ProGeomitemdata *gd = NULL;

            if (ProSurfaceInit(part, id, &s) != PRO_TK_NO_ERROR || !s) continue;
            if (ProSurfaceDataGet(s, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
                if (gd) ProGeomitemdataFree(&gd);
                continue;
            }
            {
                ProSurfacedata *sd = gd->data.p_surface_data;
                if (sd->type == PRO_SRF_PLANE && fabs(sd->srf_shape.plane.e3[2]) > 0.99) {
                    double z = sd->srf_shape.plane.origin[2];
                    int k, dup = 0;
                    for (k = 0; k < zc; k++) if (fabs(zl[k] - z) < 0.01) { dup = 1; break; }
                    if (!dup && zc < 32) {
                        double ex = fabs(sd->xyz_max[0] - sd->xyz_min[0]);
                        double ey = fabs(sd->xyz_max[1] - sd->xyz_min[1]);
                        zl[zc++] = z;
                        logf_("[feat] zlevel %d: z=%.3f  first id=%d nz=%.3f ext=(%.2f,%.2f)",
                              zc, z, id, sd->srf_shape.plane.e3[2], ex, ey);
                    }
                }
            }
            ProGeomitemdataFree(&gd);
        }
        logf_("[feat] distinct z-normal plane levels: %d", zc);
    }
}

/* EINFO|id1|id2|... ：逐条打边几何（线=端点；圆=中心/半径/法向；弧=原点/半径�?*/
static void einfo_run(ProMdl part, const wchar_t *arg)
{
    const wchar_t *p = arg;

    while (p && *p) {
        int id = (int)wcstol(p, NULL, 10);
        ProEdge e = NULL;
        ProGeomitem gi;
        ProGeomitemdata *gd = NULL;

        if (id > 0 &&
            ProEdgeInit(part, id, &e) == PRO_TK_NO_ERROR && e &&
            ProEdgeToGeomitem(part, e, &gi) == PRO_TK_NO_ERROR &&
            ProGeomitemdataGet(&gi, &gd) == PRO_TK_NO_ERROR && gd && gd->data.p_curve_data) {
            int t = gd->data.p_curve_data->line.type;

            if (t == PRO_ENT_LINE) {
                ProLinedata *ld = &gd->data.p_curve_data->line;
                double dx = ld->end2[0] - ld->end1[0];
                double dy = ld->end2[1] - ld->end1[1];
                double dz = ld->end2[2] - ld->end1[2];
                logf_("[einfo] %4d LINE   end1=(%.3f,%.3f,%.3f) end2=(%.3f,%.3f,%.3f) len=%.3f dz=%+.3f dir=(%.3f,%.3f,%.3f)",
                      id, ld->end1[0], ld->end1[1], ld->end1[2], ld->end2[0], ld->end2[1], ld->end2[2],
                      sqrt(dx * dx + dy * dy + dz * dz), dz, dx, dy, dz);
            } else if (t == PRO_ENT_CIRCLE) {
                ProCircledata *cd = &gd->data.p_curve_data->circle;
                logf_("[einfo] %4d CIRCLE c=(%.3f,%.3f,%.3f) r=%.3f n=(%.3f,%.3f,%.3f)",
                      id, cd->center[0], cd->center[1], cd->center[2], cd->radius,
                      cd->norm_axis_unit_vect[0], cd->norm_axis_unit_vect[1], cd->norm_axis_unit_vect[2]);
            } else if (t == PRO_ENT_ARC) {
                ProArcdata *ad = &gd->data.p_curve_data->arc;
                logf_("[einfo] %4d ARC    org=(%.3f,%.3f,%.3f) r=%.3f v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)",
                      id, ad->origin[0], ad->origin[1], ad->origin[2], ad->radius,
                      ad->vector1[0], ad->vector1[1], ad->vector1[2],
                      ad->vector2[0], ad->vector2[1], ad->vector2[2]);
            } else {
                logf_("[einfo] %4d type=%d (non line/circle/arc)", id, t);
            }
            ProGeomitemdataFree(&gd);
        } else {
            logf_("[einfo] %4d init/retrieve failed", id);
        }
        p = wcschr(p, L'|');
        if (!p) break;
        p++;
    }
}

/* EDGEOWN|<id1>|<id2>|...：查每条边由**哪个特征**创建（2026-11-11 新增）
   用途：建尺寸前排除复制几何/面组产生的「鬼边」。
   起因：XEDGE/BBOX 只能按坐标找极值边，而复制曲面的边和真实轮廓的 x 坐标完全一样，
         坐标上无法区分 => 尺寸值算对了，引线却落在空中（"标空"）。
   做法：ProGeomitemFeatureGet 拿到创建该边的特征，再取特征类型名逐个看。*/
static void edgeown_run(ProMdl part, const wchar_t *arg)
{
    const wchar_t *p = arg;
    int n = 0, nbad = 0;

    logf_("[edgeown] ==== edge owner (which feature created each edge) ====");
    while (p && *p) {
        int         id = (int)wcstol(p, NULL, 10);
        ProEdge     e = NULL;
        ProGeomitem gi;
        ProFeature  feat;
        ProName     tn;
        int         fnum = -1;
        char        tn8[256];

        tn[0] = 0;
        tn8[0] = 0;
        if (id > 0 &&
            ProEdgeInit(part, id, &e) == PRO_TK_NO_ERROR && e &&
            ProEdgeToGeomitem(part, e, &gi) == PRO_TK_NO_ERROR) {
            ProError ef = ProGeomitemFeatureGet(&gi, &feat);
            ProError et = PRO_TK_GENERAL_ERROR;
            if (ef == PRO_TK_NO_ERROR) {
                et = ProFeatureTypenameGet(&feat, tn);
                ProFeatureNumberGet(&feat, &fnum);
            }
            wide_to_u8(tn, tn8, sizeof(tn8));
            logf_("[edgeown] %4d feat='%s' num=%d (get_feat=%d type=%d)", id, tn8, fnum, (int)ef, (int)et);
            n++;
        } else {
            logf_("[edgeown] %4d init failed", id);
            nbad++;
        }
        p = wcschr(p, L'|');
        if (!p) break;
        p++;
    }
    logf_("[edgeown] total=%d bad=%d", n, nbad);
}

/* ---- P3：给 6 个凸点画点画线十字中心线（图纸级 draft entity，screen 坐标�?
   凸点圆心（模型坐标，来自 CYLSCAN 实测）：x∈{10.85,33.85,56.85}，y∈{156.06,179.56}�?
   节距 23.0×23.5 与图上尺寸吻合。屏幕坐�?= 主视图轮�?+ 模型包围盒线性映射�?---- */
static int mk_dtl_line(ProMdl drw, ProView view, double x1, double y1, double x2, double y2)
{
    ProDtlentitydata data = NULL;
    ProDtlentity     ent;
    ProCurvedata     cd;
    ProName          fnt;
    ProError         e;
    int              ec;

    memset(&cd, 0, sizeof(cd));
    cd.line.type = PRO_ENT_LINE;   /* 实测：轴曲线�?line.type=2 = PRO_ENT_LINE */
    cd.line.end1[0] = x1; cd.line.end1[1] = y1; cd.line.end1[2] = 0.0;
    cd.line.end2[0] = x2; cd.line.end2[1] = y2; cd.line.end2[2] = 0.0;
    if (ProDtlentitydataAlloc(drw, &data) != PRO_TK_NO_ERROR) return 0;
    ProDtlentitydataCurveSet(data, &cd);
    ProDtlentitydataViewSet(data, view);
    ProDtlentitydataWidthSet(data, 0.15);
    {
        static const wchar_t *fnames[3] = { L"centerline", L"phantomfont", L"CENTERLINE" };
        int k, okfont = -1;
        e = PRO_TK_GENERAL_ERROR;
        ec = PRO_TK_GENERAL_ERROR;
        for (k = 0; k < 3; k++) {
            ProError eset;
            ProDtlentitydata dx = NULL;
            ProName fr;
            char fr8[96];
            wcsncpy_s(fnt, PRO_NAME_SIZE, fnames[k], _TRUNCATE);
            eset = ProDtlentitydataFontSet(data, fnt);
            logf_("[cross] font '%ls' set -> %d (%s)", fnames[k], (int)eset, errname(eset));
            ec = ProDtlentityCreate(drw, NULL, data, &ent);
            logf_("[cross] create with '%ls' -> %d (%s)", fnames[k], (int)ec, errname(ec));
            e = eset;
            if (ec != PRO_TK_NO_ERROR) continue;
            fr[0] = 0;
            if (ProDtlentityDataGet(&ent, NULL, &dx) == PRO_TK_NO_ERROR && dx) {
                ProDtlentitydataFontGet(dx, fr);
                ProDtlentitydataFree(dx);
            }
            fr8[0] = 0;
            wide_to_u8(fr, fr8, sizeof(fr8));
            logf_("[cross] font readback '%s' for requested '%ls'", fr8, fnames[k]);
            if (_wcsicmp(fr, fnames[k]) == 0) { okfont = k; break; }
            logf_("[cross] font mismatch; delete entity, try next name");
            ProDtlentityDelete(&ent, NULL);
            ec = PRO_TK_GENERAL_ERROR;
        }
        if (ec != PRO_TK_NO_ERROR) {
            ProName f2;
            f2[0] = 0;
            ProDtlentitydataFontSet(data, f2);
            ec = ProDtlentityCreate(drw, NULL, data, &ent);
            logf_("[cross] fallback empty font -> create %d (%s)", (int)ec, errname(ec));
        }
        logf_("[cross] line (%.1f,%.1f)-(%.1f,%.1f) font '%ls' ok=%d create->%d (%s)",
              x1, y1, x2, y2, (okfont >= 0 ? fnames[okfont] : L"<empty>"), okfont,
              (int)ec, errname(ec));
    }
    ProDtlentitydataFree(data);
    return (ec == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* split a dxf-style circle list: union-find root */
static int bc_root(int *par, int i)
{
    while (par[i] != i) { par[i] = par[par[i]]; i = par[i]; }
    return i;
}

/* CROSSES[|ax|ay] - boss cross-hair centre lines as draft entities in the front view.
   ax / ay = model axis shown as paper x / paper y (0=X 1=Y 2=Z), default 0|1 (= the
   PiCreo quickdraw front view: paper x <- model X, paper y <- model Y). The VM-built
   front view of the la=2 parts needs 2|0 (paper x <- model Z, paper y <- model X).
   Boss centres come from geometry only (2026-10-28 rework, work order "r_1boss"):

     1) collect every ARC / CIRCLE edge centre + radius (ids 1..6000, same walk as
        r57_find_*);
     2) per radius band (0.02mm quantisation): merge centres closer than 0.6mm
        (union-find, chained - one boss stack carries rings at several depths, each
        step < 0.6mm, while neighbouring bosses are > 20mm apart) and keep only the
        centres carrying >= 3 edges of that band: a real ring, not a stray arc
        origin (the flat-pattern body of these parts contributes many of those);
     3) inside a band pick the longest run of ring centres that is collinear
        (<= 0.5mm off the line) and equidistant (adjacent spacing sd < 0.5) - that
        is the boss array.  Bands compete on run length, ties go to the larger
        radius (the boss rim rather than an inner fillet ring);
     4) rows parallel to the winning line are added when they repeat its spacing
        slots (2x3 arrays); a row needs >= 2 members;
     5) a band with exactly 2 ring centres further apart than 2*r also counts
        (two-boss parts);
     6) only when nothing above matches does it fall back to the old rule
        "radius carried by the most deduplicated centres".

   Why the old version failed on nb109_1 (capsule shaped, 1x3 bosses): it gated every
   centre cluster on cnt >= max_cluster/2 and only then voted on the radius.  The
   end-cap cluster of the first boss collects 276 edges, so the gate became 138 and
   dropped the two plain bosses (26 edges each); the radius vote then saw a single
   centre and the tie-break took the largest radius present there (19.2869, the crown
   arc) - one cross instead of three. */
static void boss_centerlines(ProMdl drw, ProMdl part, int ax, int ay)
{
    typedef struct { double p[3]; double r; } bc_t;
    static bc_t   A[1600];
    static int    PAR[1600];
    static double rcx[240], rcy[240], rcz[240];
    static int    rcn[240];
    static double rct[64];
    static int    rci[64];
    static double spc[64], spc2[64];
    static double BOSS[200][3];
    static double bprj[200];
    ProView       *views = NULL;
    ProView        front = NULL;
    ProPoint3d     out[2];
    ProPoint3d     box[2];
    int            nA = 0, nP = 0;
    int            i, j, k, m, id;
    int            bestCnt = 0, bestBand = -1, bestNc2 = 0;
    double         bestR = 0.0, bestSp = 0.0, bestTol = 0.0;
    double         bux = 0.0, buy = 0.0, buz = 0.0;
    double         bax = 0.0, bay = 0.0, baz = 0.0;
    double         half, kx, ky, x0, x1, y0, y1;
    double         minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    int            made = 0, clamped = 0;

    if (ax < 0 || ax > 2) ax = 0;
    if (ay < 0 || ay > 2) ay = 1;

    /* 1) every ARC / CIRCLE edge -> centre + radius */
    for (id = 1; id <= 6000 && nA < 1600; id++) {
        ProEdge          e = NULL;
        ProGeomitem      gi;
        ProGeomitemdata *gd = NULL;
        int              t;

        if (ProEdgeInit((ProSolid)part, id, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        t = gd->data.p_curve_data->line.type;
        if (t == PRO_ENT_CIRCLE) {
            ProCircledata *cd = &gd->data.p_curve_data->circle;
            A[nA].p[0] = cd->center[0]; A[nA].p[1] = cd->center[1]; A[nA].p[2] = cd->center[2];
            A[nA].r = cd->radius; nA++;
        } else if (t == PRO_ENT_ARC) {
            ProArcdata *ad = &gd->data.p_curve_data->arc;
            A[nA].p[0] = ad->origin[0]; A[nA].p[1] = ad->origin[1]; A[nA].p[2] = ad->origin[2];
            A[nA].r = ad->radius; nA++;
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[cross] geom scan: %d arc/circle edges (ax=%d ay=%d)", nA, ax, ay);
    if (nA < 2) { logf_("[cross] no arc/circle edges"); return; }

    /* 2)..5) per radius band: ring centres -> collinear + equidistant array */
    for (i = 0; i < nA; i++) {
        double bandr = A[i].r;
        int    nC = 0, dup = 0, bandN = 0;
        double bandSd = 0.0, bandSp = 0.0;
        double lux = 0.0, luy = 0.0, luz = 0.0;
        double lax = 0.0, lay = 0.0, laz = 0.0;

        for (m = 0; m < i; m++) if (fabs(A[m].r - bandr) < 0.02) { dup = 1; break; }
        if (dup) continue;

        /* union-find over this band only */
        for (m = 0; m < nA; m++) PAR[m] = m;
        for (m = 0; m < nA; m++) {
            int    ra, rb;
            double dx, dy, dz;

            if (fabs(A[m].r - bandr) >= 0.02) continue;
            for (k = m + 1; k < nA; k++) {
                if (fabs(A[k].r - bandr) >= 0.02) continue;
                dx = A[m].p[0] - A[k].p[0];
                dy = A[m].p[1] - A[k].p[1];
                dz = A[m].p[2] - A[k].p[2];
                if (dx * dx + dy * dy + dz * dz >= 0.36) continue;
                ra = bc_root(PAR, m);
                rb = bc_root(PAR, k);
                if (ra != rb) PAR[ra] = rb;
            }
        }
        /* centroids; only rings backed by >= 3 edges of this band */
        for (m = 0; m < nA && nC < 240; m++) {
            int    root, n;
            double sx = 0.0, sy = 0.0, sz = 0.0;

            if (fabs(A[m].r - bandr) >= 0.02) continue;
            root = bc_root(PAR, m);
            if (root != m) continue;
            n = 0;
            for (k = 0; k < nA; k++) {
                if (fabs(A[k].r - bandr) >= 0.02) continue;
                if (bc_root(PAR, k) != root) continue;
                sx += A[k].p[0]; sy += A[k].p[1]; sz += A[k].p[2]; n++;
            }
            if (n < 3) continue;
            rcx[nC] = sx / n; rcy[nC] = sy / n; rcz[nC] = sz / n; rcn[nC] = n; nC++;
        }
        if (nC < 2) continue;

        /* 3) every candidate line, then the longest uniform run on it */
        for (m = 0; m < nC; m++) {
            for (k = m + 1; k < nC; k++) {
                double dx = rcx[k] - rcx[m], dy = rcy[k] - rcy[m], dz = rcz[k] - rcz[m];
                double len = sqrt(dx * dx + dy * dy + dz * dz);
                double ux, uy, uz;
                int    cnt = 0, a, b, lo, hi;

                if (len < 2.0) continue;
                ux = dx / len; uy = dy / len; uz = dz / len;
                for (a = 0; a < nC; a++) {
                    double wx = rcx[a] - rcx[m], wy = rcy[a] - rcy[m], wz = rcz[a] - rcz[m];
                    double t = wx * ux + wy * uy + wz * uz;
                    double oxx = wx - t * ux, oyy = wy - t * uy, ozz = wz - t * uz;

                    if (sqrt(oxx * oxx + oyy * oyy + ozz * ozz) > 0.5) continue;
                    if (cnt < 64) { rct[cnt] = t; rci[cnt] = a; cnt++; }
                }
                if (cnt < 2) continue;
                for (a = 1; a < cnt; a++) {
                    double tv = rct[a];
                    int    iv = rci[a];

                    b = a - 1;
                    while (b >= 0 && rct[b] > tv) { rct[b + 1] = rct[b]; rci[b + 1] = rci[b]; b--; }
                    rct[b + 1] = tv; rci[b + 1] = iv;
                }
                spc[0] = 0.0; spc2[0] = 0.0;
                for (a = 1; a < cnt; a++) {
                    double g = rct[a] - rct[a - 1];
                    spc[a] = spc[a - 1] + g;
                    spc2[a] = spc2[a - 1] + g * g;
                }
                for (lo = 0; lo + 2 < cnt; lo++) {
                    for (hi = lo + 2; hi < cnt; hi++) {
                        int    ng = hi - lo;
                        double sum = spc[hi] - spc[lo];
                        double sq = spc2[hi] - spc2[lo];
                        double mean = sum / ng;
                        double var = sq / ng - mean * mean;
                        double sd = (var > 0.0) ? sqrt(var) : 0.0;

                        if (mean < 0.8) continue;          /* ring stack, not separate bosses */
                        if (sd >= 0.5) continue;
                        if ((hi - lo + 1) > bandN) {
                            bandN = hi - lo + 1;
                            bandSd = sd;
                            bandSp = mean;
                            lux = ux; luy = uy; luz = uz;
                            lax = rcx[m] + rct[lo] * ux;
                            lay = rcy[m] + rct[lo] * uy;
                            laz = rcz[m] + rct[lo] * uz;
                        }
                    }
                }
            }
        }
        if (bandN < 2) continue;
        /* a band only counts as a boss array with >= 3 collinear centres, or with
           exactly two rings standing further apart than two diameters */
        if (bandN < 3 && !(nC == 2 && bandSp > 2.0 * bandr)) continue;
        logf_("[cross] band r=%.4f ring-centres=%d run=%d spacing=%.4f sd=%.4f",
             bandr, nC, bandN, bandSp, bandSd);
        if (bandN > bestCnt || (bandN == bestCnt && bandr > bestR)) {
            bestCnt = bandN; bestBand = i; bestR = bandr; bestSp = bandSp;
            bestNc2 = nC;
            bux = lux; buy = luy; buz = luz;
            bax = lax; bay = lay; baz = laz;
        }
    }
    if (bestBand >= 0)
        logf_("[cross] boss array: band r=%.4f bestN=%d spacing=%.4f dir=(%.3f,%.3f,%.3f)",
             bestR, bestCnt, bestSp, bux, buy, buz);

    if (bestBand >= 0) {
        /* 4) the winning line + rows that repeat its spacing slots */
        double bandr = A[bestBand].r;
        int    nC = 0;

        for (m = 0; m < nA; m++) PAR[m] = m;
        for (m = 0; m < nA; m++) {
            int    ra, rb;
            double dx, dy, dz;

            if (fabs(A[m].r - bandr) >= 0.02) continue;
            for (k = m + 1; k < nA; k++) {
                if (fabs(A[k].r - bandr) >= 0.02) continue;
                dx = A[m].p[0] - A[k].p[0];
                dy = A[m].p[1] - A[k].p[1];
                dz = A[m].p[2] - A[k].p[2];
                if (dx * dx + dy * dy + dz * dz >= 0.36) continue;
                ra = bc_root(PAR, m); rb = bc_root(PAR, k);
                if (ra != rb) PAR[ra] = rb;
            }
        }
        for (m = 0; m < nA && nC < 240; m++) {
            int    root, n;
            double sx = 0.0, sy = 0.0, sz = 0.0;

            if (fabs(A[m].r - bandr) >= 0.02) continue;
            root = bc_root(PAR, m);
            if (root != m) continue;
            n = 0;
            for (k = 0; k < nA; k++) {
                if (fabs(A[k].r - bandr) >= 0.02) continue;
                if (bc_root(PAR, k) != root) continue;
                sx += A[k].p[0]; sy += A[k].p[1]; sz += A[k].p[2]; n++;
            }
            if (n < 3) continue;
            rcx[nC] = sx / n; rcy[nC] = sy / n; rcz[nC] = sz / n; rcn[nC] = n; nC++;
        }

        nP = 0;
        for (m = 0; m < nC && nP < 200; m++) {
            double wx = rcx[m] - bax, wy = rcy[m] - bay, wz = rcz[m] - baz;
            double t = wx * bux + wy * buy + wz * buz;
            double oxx = wx - t * bux, oyy = wy - t * buy, ozz = wz - t * buz;

            if (sqrt(oxx * oxx + oyy * oyy + ozz * ozz) > 0.5) continue;
            BOSS[nP][0] = rcx[m]; BOSS[nP][1] = rcy[m]; BOSS[nP][2] = rcz[m];
            bprj[nP] = t;
            logf_("[cross] boss[%d] model=(%.3f,%.3f,%.3f) rings=%d line", nP, rcx[m], rcy[m], rcz[m], rcn[m]);
            nP++;
        }
        /* rows: perpendicular offsets that repeat at least two of the line's slots */
        {
            double rowo[24][3];
            int    rowc[24];
            int    nRow = 0, q, r2;

            for (q = 0; q < nC; q++) {
                double wx = rcx[q] - bax, wy = rcy[q] - bay, wz = rcz[q] - baz;
                double t = wx * bux + wy * buy + wz * buz;
                double oxx = wx - t * bux, oyy = wy - t * buy, ozz = wz - t * buz;
                int    hit = -1, matched = 0;

                if (sqrt(oxx * oxx + oyy * oyy + ozz * ozz) <= 0.5) continue;
                for (m = 0; m < nP; m++) if (fabs(bprj[m] - t) < 0.5) { matched = 1; break; }
                if (!matched) continue;
                for (m = 0; m < nRow; m++)
                    if (fabs(rowo[m][0] - oxx) < 0.5 && fabs(rowo[m][1] - oyy) < 0.5 &&
                        fabs(rowo[m][2] - ozz) < 0.5) { hit = m; break; }
                if (hit < 0 && nRow < 24) {
                    hit = nRow++;
                    rowo[hit][0] = oxx; rowo[hit][1] = oyy; rowo[hit][2] = ozz; rowc[hit] = 0;
                }
                if (hit >= 0) rowc[hit]++;
            }
            for (r2 = 0; r2 < nRow; r2++) {
                int added = 0;

                if (rowc[r2] < 2) { logf_("[cross] row[%d] skipped (%d slot)", r2, rowc[r2]); continue; }
                for (q = 0; q < nC && nP < 200; q++) {
                    double wx = rcx[q] - bax, wy = rcy[q] - bay, wz = rcz[q] - baz;
                    double t = wx * bux + wy * buy + wz * buz;
                    double oxx = wx - t * bux, oyy = wy - t * buy, ozz = wz - t * buz;
                    int    matched = 0;

                    if (fabs(oxx - rowo[r2][0]) > 0.5) continue;
                    if (fabs(oyy - rowo[r2][1]) > 0.5) continue;
                    if (fabs(ozz - rowo[r2][2]) > 0.5) continue;
                    for (m = 0; m < nP; m++)
                        if (fabs(bprj[m] - t) < 0.5 &&
                            fabs(BOSS[m][0] - rcx[q]) < 0.3 && fabs(BOSS[m][1] - rcy[q]) < 0.3) { matched = 1; break; }
                    if (matched) continue;
                    BOSS[nP][0] = rcx[q]; BOSS[nP][1] = rcy[q]; BOSS[nP][2] = rcz[q];
                    bprj[nP] = t;
                    logf_("[cross] boss[%d] model=(%.3f,%.3f,%.3f) rings=%d row%d", nP, rcx[q], rcy[q], rcz[q], rcn[q], r2);
                    nP++; added++;
                }
                logf_("[cross] row[%d] offset=(%.2f,%.2f,%.2f) slots=%d added=%d",
                     r2, rowo[r2][0], rowo[r2][1], rowo[r2][2], rowc[r2], added);
            }
        }
        (void)bestNc2;
    } else {
        /* 6) fallback: the radius carried by the most deduplicated centres */
        static double RR[400];
        static int    RC[400];
        int           nR = 0, bestc = 0;
        double        rdom = 0.0;

        logf_("[cross] no collinear/equidistant boss array - fallback to dominant radius");
        for (i = 0; i < nA; i++) {
            int dup = 0, hit = -1, nC = 0, root;

            for (m = 0; m < i; m++) if (fabs(A[m].r - A[i].r) < 0.02) { dup = 1; break; }
            if (dup) continue;
            for (m = 0; m < nR; m++) if (fabs(RR[m] - A[i].r) < 0.02) { hit = m; break; }
            if (hit < 0 && nR < 400) { hit = nR; RR[nR] = A[i].r; RC[nR] = 0; nR++; }

            for (m = 0; m < nA; m++) PAR[m] = m;
            for (m = 0; m < nA; m++) {
                int    ra, rb;
                double dx, dy, dz;

                if (fabs(A[m].r - A[i].r) >= 0.02) continue;
                for (k = m + 1; k < nA; k++) {
                    if (fabs(A[k].r - A[i].r) >= 0.02) continue;
                    dx = A[m].p[0] - A[k].p[0];
                    dy = A[m].p[1] - A[k].p[1];
                    dz = A[m].p[2] - A[k].p[2];
                    if (dx * dx + dy * dy + dz * dz >= 0.36) continue;
                    ra = bc_root(PAR, m); rb = bc_root(PAR, k);
                    if (ra != rb) PAR[ra] = rb;
                }
            }
            for (m = 0; m < nA; m++) {
                int    n;
                double sx = 0.0, sy = 0.0, sz = 0.0;

                if (fabs(A[m].r - A[i].r) >= 0.02) continue;
                root = bc_root(PAR, m);
                if (root != m) continue;
                n = 0;
                for (k = 0; k < nA; k++) {
                    if (fabs(A[k].r - A[i].r) >= 0.02) continue;
                    if (bc_root(PAR, k) != root) continue;
                    sx += A[k].p[0]; sy += A[k].p[1]; sz += A[k].p[2]; n++;
                }
                if (n < 2) continue;
                nC++;
            }
            if (hit >= 0) RC[hit] = nC;
        }
        for (m = 0; m < nR; m++)
            if (RC[m] > bestc || (RC[m] == bestc && RR[m] > rdom)) { bestc = RC[m]; rdom = RR[m]; }
        logf_("[cross] fallback dominant radius=%.4f on %d centres", rdom, bestc);
        nP = 0;
        for (i = 0; i < nA && nP < 200; i++) {
            int d2 = 0;

            if (fabs(A[i].r - rdom) > 0.30) continue;
            for (m = 0; m < nP; m++)
                if (fabs(BOSS[m][0] - A[i].p[0]) < 0.6 && fabs(BOSS[m][1] - A[i].p[1]) < 0.6 &&
                    fabs(BOSS[m][2] - A[i].p[2]) < 0.6) { d2 = 1; break; }
            if (d2) continue;
            BOSS[nP][0] = A[i].p[0]; BOSS[nP][1] = A[i].p[1]; BOSS[nP][2] = A[i].p[2];
            logf_("[cross] boss[%d] model=(%.3f,%.3f,%.3f) r=%.3f (fallback)",
                 nP, A[i].p[0], A[i].p[1], A[i].p[2], A[i].r);
            nP++;
        }
    }
    if (nP < 1) { logf_("[cross] no boss centres found"); return; }

    /* 6) front view outline + model bbox, then map on the requested axes */
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) return;
    {
        int vc = 0, vi;
        ProArraySizeGet((ProArray)views, &vc);
        for (vi = 0; vi < vc; vi++) {
            ProName nm;
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[vi]; break; }
        }
        ProArrayFree((ProArray *)&views);
    }
    if (!front) { logf_("[cross] no front view"); return; }
    memset(out, 0, sizeof(out));
    memset(box, 0, sizeof(box));
    out[0][0] = out[0][1] = out[0][2] = -9999.0;
    out[1][0] = out[1][1] = out[1][2] = -9999.0;
    box[0][0] = box[0][1] = box[0][2] = -9999.0;
    box[1][0] = box[1][1] = box[1][2] = -9999.0;
    ProDrawingViewOutlineGet((ProDrawing)drw, front, out);
    ProSolidOutlineGet((ProSolid)part, box);
    if (out[0][0] < -9998.0 || box[0][0] < -9998.0) { logf_("[cross] outline/bbox failed"); return; }

    x0 = box[0][ax]; x1 = box[1][ax];
    y0 = box[0][ay]; y1 = box[1][ay];
    if (x1 - x0 < 1e-6 || y1 - y0 < 1e-6) {
        logf_("[cross] degenerate model extent on axes %d/%d (%.4f x %.4f)", ax, ay, x1 - x0, y1 - y0);
        return;
    }
    kx = (out[1][0] - out[0][0]) / (x1 - x0);
    ky = (out[1][1] - out[0][1]) / (y1 - y0);
    half = 17.0 * (kx / 2.3805);      /* half length scales with the view, as before */
    {
        double wv = out[1][0] - out[0][0], hv = out[1][1] - out[0][1];
        double lim = 0.35 * ((wv < hv) ? wv : hv);
        if (half > lim) { logf_("[cross] half clamped %.2f -> %.2f (view %.1f x %.1f)", half, lim, wv, hv); half = lim; }
    }
    logf_("[cross] map ax=%d ay=%d kx=%.3f ky=%.3f outline x[%.1f..%.1f] y[%.1f..%.1f] model[%d]=[%.2f..%.2f] model[%d]=[%.2f..%.2f]",
          ax, ay, kx, ky, out[0][0], out[1][0], out[0][1], out[1][1], ax, x0, x1, ay, y0, y1);

    for (i = 0; i < nP; i++) {
        double sx = out[0][0] + (BOSS[i][ax] - x0) * kx;
        double sy = out[0][1] + (BOSS[i][ay] - y0) * ky;
        double hh = half, hv = half;          /* per cross, so nothing pokes outside */
        double roomx = ((sx - out[0][0]) < (out[1][0] - sx)) ? (sx - out[0][0]) : (out[1][0] - sx);
        double roomy = ((sy - out[0][1]) < (out[1][1] - sy)) ? (sy - out[0][1]) : (out[1][1] - sy);

        roomx -= 0.5; roomy -= 0.5;
        if (hh > roomx) { hh = roomx; clamped++; }
        if (hv > roomy) { hv = roomy; clamped++; }
        if (hh < 0.3) hh = 0.3;
        if (hv < 0.3) hv = 0.3;

        logf_("[cross] boss[%d] paper=(%.1f,%.1f) half=%.2f/%.2f", i, sx, sy, hh, hv);
        made += mk_dtl_line(drw, front, sx, sy - hv, sx, sy + hv);
        made += mk_dtl_line(drw, front, sx - hh, sy, sx + hh, sy);
        if (sx - hh < minx) minx = sx - hh;
        if (sx + hh > maxx) maxx = sx + hh;
        if (sy - hv < miny) miny = sy - hv;
        if (sy + hv > maxy) maxy = sy + hv;
    }
    logf_("[cross] done, lines created=%d (bosses=%d) xrange[%.1f..%.1f] yrange[%.1f..%.1f] inside=%d clamped=%d",
          made, nP, minx, maxx, miny, maxy,
          (minx >= out[0][0] - 0.5 && maxx <= out[1][0] + 0.5 &&
           miny >= out[0][1] - 0.5 && maxy <= out[1][1] + 0.5), clamped);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* ===== 轮次55：十字中心线清点（CROSSINFO�? 尺寸附件读回（DATTACH�?===== */
static void crossinfo_run(ProMdl drw)
{
    ProDtlentity *ents = NULL;
    int           n = 0, i;

    if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents) {
        logf_("[cross] collect failed");
        return;
    }
    ProArraySizeGet((ProArray)ents, &n);
    logf_("[cross] draft entities n=%d", n);
    for (i = 0; i < n && i < 80; i++) {
        ProDtlentitydata d = NULL;
        int             id = -1, cons = 0;
        ProView         v = NULL;
        ProCurvedata    cd;
        ProBoolean      b = PRO_B_FALSE;
        ProName         fnt;
        double          w = -1.0;
        char            f8[96];

        if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
        memset(&cd, 0, sizeof(cd));
        ProDtlentitydataIdGet(d, &id);
        fnt[0] = 0;
        ProDtlentitydataFontGet(d, fnt);
        ProDtlentitydataWidthGet(d, &w);
        f8[0] = 0;
        wide_to_u8(fnt, f8, sizeof(f8));
        ProDtlentitydataViewGet(d, &v);
        if (ProDtlentitydataIsConstruction(d, &b) == PRO_TK_NO_ERROR) cons = (int)b;
        if (ProDtlentitydataCurveGet(d, &cd) == PRO_TK_NO_ERROR && cd.line.type == PRO_ENT_LINE) {
            double dx = cd.line.end2[0] - cd.line.end1[0];
            double dy = cd.line.end2[1] - cd.line.end1[1];
            double len = sqrt(dx * dx + dy * dy);
            char   v8[64] = "-";

            if (v) { ProName nm; nm[0] = 0; ProDrawingViewNameGet((ProDrawing)drw, v, nm); wide_to_u8(nm, v8, sizeof(v8)); }
            logf_("[cross] ent[%d] id=%d cons=%d view='%s' font='%s' width=%.3f line (%.1f,%.1f)-(%.1f,%.1f) len=%.1f",
                  i, id, cons, v8, f8, w, cd.line.end1[0], cd.line.end1[1], cd.line.end2[0], cd.line.end2[1], len);
        } else {
            logf_("[cross] ent[%d] id=%d cons=%d (not a line; type=%d)", i, id, cons, (int)cd.line.type);
        }
    }
    ProArrayFree((ProArray *)&ents);
}

typedef struct { double target; ProDrawing drw; } dattach_ctx_t;

static ProError dattach_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dattach_ctx_t *c = (dattach_ctx_t *)app;
    double         v = 0.0;

    (void)status;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    logf_("[dattach] dim v=%.4f id=%d ------------------------------", v, (int)dim->id);
    {
        ProDimAttachment *arr = NULL;

        ProAnnotationPlane plane2;
        memset(&plane2, 0, sizeof(plane2));
        ProDimSense      *senses2 = NULL;
        ProDimOrient      ohint2 = PRO_DIM_ORNT_NONE;
        ProError          eg = PRO_TK_GENERAL_ERROR;
        const char       *src = "ProDimensionAttachmentsGet";

        /* 图模式创建的尺寸（AUTOBUILD/EDIM）附件要�?ProDrawingDimAttachpointsGet 才读得到�?
           ProDimensionAttachmentsGet 对它们恒返回空表（本�?63.6 对照�?n=0 即此因）�?*/
        if (c->drw)
            eg = ProDrawingDimAttachpointsGet(c->drw, dim, &arr, &senses2);
        if (eg == PRO_TK_NO_ERROR && arr) {
            src = "ProDrawingDimAttachpointsGet";
        } else {
            arr = NULL; senses2 = NULL;
            eg = ProDimensionAttachmentsGet(dim, &plane2, &arr, &senses2, &ohint2);
            src = "ProDimensionAttachmentsGet";
        }
        if (eg == PRO_TK_NO_ERROR && arr) {
            int n = 0, i;

            ProArraySizeGet((ProArray)arr, &n);
            logf_("[dattach]   src=%s attachments n=%d", src, n);
            for (i = 0; i < n; i++) {
                int j;

                for (j = 0; j < 2; j++) {
                    ProSelection sel = arr[i][j];
                    ProModelitem mi;
                    ProPoint3d   pt = { -9999.0, -9999.0, -9999.0 };
                    ProError     ep = ProSelectionPoint3dGet(sel, pt);

                    memset(&mi, 0, sizeof(mi));
                    if (ProSelectionModelitemGet(sel, &mi) == PRO_TK_NO_ERROR)
                        logf_("[dattach]   att[%d][%d] type=%d id=%d owner=%d  point=(%.3f,%.3f,%.3f) ptErr=%d",
                              i, j, (int)mi.type, (int)mi.id, (int)mi.owner, pt[0], pt[1], pt[2], (int)ep);
                    else
                        logf_("[dattach]   att[%d][%d] modelitemGet failed; point=(%.3f,%.3f,%.3f) ptErr=%d", i, j, pt[0], pt[1], pt[2], (int)ep);
                }
            }
            ProDimattachmentarrayFree(arr);
        } else {
            logf_("[dattach]   AttachmentsGet failed (%d %s) src=%s", (int)eg, errname(eg), src);
        }
    }
    return PRO_TK_NO_ERROR;
}

static void dattach_run(ProMdl drw, double value)
{
    dattach_ctx_t c;

    c.target = value; c.drw = (ProDrawing)drw;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dattach_cb, NULL, (ProAppData)&c);
}

/* DMOVE|<value>|<x>|<y>：把指定值的尺寸文字沿图纸移动到 (x,y)（用于避让文字碰撞） */
typedef struct { double target, x, y; ProDrawing drw; ProDimension hit; int found; } dmove_ctx_t;

static ProError dmove_cb(ProDimension *dim, ProError status, ProAppData app)
{
    dmove_ctx_t *c = (dmove_ctx_t *)app;
    double v = 0.0;

    (void)status;
    if (c->found) return PRO_TK_NO_ERROR;
    if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (v < c->target - 0.02 || v > c->target + 0.02) return PRO_TK_NO_ERROR;
    c->hit = *dim; c->found = 1;
    return PRO_TK_NO_ERROR;
}

static void dmove_run(ProMdl drw, double value, double x, double y)
{
    dmove_ctx_t c;

    memset(&c, 0, sizeof(c));
    c.target = value; c.x = x; c.y = y; c.drw = (ProDrawing)drw;
    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dmove_cb, NULL, (ProAppData)&c);
    if (!c.found) { logf_("[dmove] value %.4f not found", value); return; }
    {
        ProView view = NULL;
        ProPoint3d pos;
        ProError ev, em, es;
        ProBoolean sh = PRO_B_FALSE;
        ProPoint3d tp = { -9999.0, -9999.0, -9999.0 };
        ProDimlocation loc = NULL;
        ProBoolean he = PRO_B_FALSE;
        double el = 0.0;

        ev = ProDrawingDimensionViewGet((ProDrawing)drw, &c.hit, &view);
        pos[0] = x; pos[1] = y; pos[2] = 0.0;
        em = ProDimensionMove(&c.hit, pos);
        es = ProAnnotationShow((ProAnnotation *)&c.hit, NULL, view);
        ProDwgSheetRegenerate((ProDrawing)drw, 1);
        ProAnnotationIsShown((ProAnnotation *)&c.hit, (ProDrawing)drw, &sh);
        if (ProDimensionLocationGet(&c.hit, NULL, (ProDrawing)drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, tp, &el);
        logf_("[dmove] v=%.4f viewGet=%d move(%.1f,%.1f)=%d (%s) show=%d (%s) shown=%d text=(%.1f,%.1f)",
              value, (int)ev, x, y, (int)em, errname(em), (int)es, errname(es), (int)sh, tp[0], tp[1]);
    }
}

/* ===== 轮次55：按用户裁定，凸�?间距"尺寸一律锚在【十字中心线】上 =====
   - �?draft 实体里按几何位置找目标十字线（竖�?同一 x / 横线=同一 y）；
   - �?ProModelitemInit(drw, id, PRO_DRAFT_ENTITY) + ProSelectionAlloc 造成附件�?
   - 界线从十字线端点（在凸台本体之外）引出，不再穿凸台�?*/
/* Measure the bounding box of the short cross lines boss_centerlines drew.
   Returns the number of line entities seen; the caller derives the boss grid
   centre and half-spacing from the box instead of assuming a 56-unit span. */
static int cross_extent(ProMdl drw, double *xmin, double *xmax, double *ymin, double *ymax)
{
    ProDtlentity *ents = NULL;
    int           n = 0, i, hits = 0;

    if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents)
        return 0;
    ProArraySizeGet((ProArray)ents, &n);
    for (i = 0; i < n; i++) {
        ProDtlentitydata d = NULL;
        ProCurvedata    cd;
        int             k;

        if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
        memset(&cd, 0, sizeof(cd));
        if (ProDtlentitydataCurveGet(d, &cd) != PRO_TK_NO_ERROR || cd.line.type != PRO_ENT_LINE) continue;
        {
            /* 2026-10-30: boss crosses are drawn as point-dash arms whose ends can
               be slightly off-axis and whose length scales with the view (139 units
               on nb109_5). Accept any near-axis line shorter than 200 and let the
               per-cross midpoint filter below reject frames/leaders. */
            double dx = fabs(cd.line.end2[0] - cd.line.end1[0]);
            double dy = fabs(cd.line.end2[1] - cd.line.end1[1]);
            if ((dx > 200.0) || (dy > 200.0)) continue;
            if (dx > 0.05 && dy > 0.05) {
                /* not axis aligned: allow up to 8% slope */
                double big = (dx > dy) ? dx : dy;
                double sft = (dx > dy) ? dy : dx;
                if (sft > big * 0.08) continue;
            }
        }
        for (k = 0; k < 2; k++) {
            double px = k ? cd.line.end2[0] : cd.line.end1[0];
            double py = k ? cd.line.end2[1] : cd.line.end1[1];
            if (px < *xmin) *xmin = px;
            if (px > *xmax) *xmax = px;
            if (py < *ymin) *ymin = py;
            if (py > *ymax) *ymax = py;
        }
        hits++;
    }
    ProArrayFree((ProArray *)&ents);
    return hits;
}

/* Collect every cross arm of the wanted direction inside a box around the grid
   centre. Returns the count and fills ids + midpoints, so the caller can pick
   the widest pair instead of guessing an offset. */
static int cross_list(ProMdl drw, int want_vertical, double cx, double cy, double tol,
                      int *ids, double *xs, double *ys, int cap)
{
    ProDtlentity *ents = NULL;
    int           n = 0, i, out = 0;

    if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents)
        return 0;
    ProArraySizeGet((ProArray)ents, &n);
    for (i = 0; i < n && out < cap; i++) {
        ProDtlentitydata d = NULL;
        ProCurvedata    cd;
        int             id = -1;

        if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
        memset(&cd, 0, sizeof(cd));
        if (ProDtlentitydataCurveGet(d, &cd) != PRO_TK_NO_ERROR || cd.line.type != PRO_ENT_LINE) continue;
        {
            double dx = cd.line.end2[0] - cd.line.end1[0];
            double dy = cd.line.end2[1] - cd.line.end1[1];
            double mx = (cd.line.end1[0] + cd.line.end2[0]) / 2.0;
            double my = (cd.line.end1[1] + cd.line.end2[1]) / 2.0;
            int    isv = fabs(dx) < fabs(dy);

            if (want_vertical != isv) continue;
            if (fabs(mx - cx) > tol || fabs(my - cy) > tol) {
                logf_("[xlist] skip id=%d v=%d mid=(%.1f,%.1f) d=(%.1f,%.1f) tol=%.1f",
                      -1, want_vertical, mx, my, fabs(mx - cx), fabs(my - cy), tol);
                continue;
            }
            {   /* cross arms scale with the view; 200 covers nb109_5 (139 units) */
                double len = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
                if (len > 200.0) continue;
            }
            ProDtlentitydataIdGet(d, &id);
            ids[out] = id; xs[out] = mx; ys[out] = my; out++;
        }
    }
    ProArrayFree((ProArray *)&ents);
    return out;
}

static int find_cross_id(ProMdl drw, int want_vertical, double cx, double cy, double tol, char *why, size_t cap)
{
    ProDtlentity *ents = NULL;
    int           n = 0, i, found = -1;
    double        bestd = 1e9;

    if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents) {
        sprintf_s(why, cap, "collect failed");
        return -1;
    }
    ProArraySizeGet((ProArray)ents, &n);
    for (i = 0; i < n; i++) {
        ProDtlentitydata d = NULL;
        ProCurvedata    cd;
        int             id = -1;

        if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
        memset(&cd, 0, sizeof(cd));
        if (ProDtlentitydataCurveGet(d, &cd) != PRO_TK_NO_ERROR || cd.line.type != PRO_ENT_LINE) continue;
        ProDtlentitydataIdGet(d, &id);
        {
            double dx = cd.line.end2[0] - cd.line.end1[0];
            double dy = cd.line.end2[1] - cd.line.end1[1];
            double mx = (cd.line.end1[0] + cd.line.end2[0]) / 2.0;
            double my = (cd.line.end1[1] + cd.line.end2[1]) / 2.0;
            int    isv = fabs(dx) < fabs(dy);          /* 竖线 = |dx|<|dy| */

            if (want_vertical != isv) continue;
            if (want_vertical) {
                if (fabs(mx - cx) > tol || fabs(my - cy) > tol * 2.0) continue;
            } else {
                if (fabs(my - cy) > tol || fabs(mx - cx) > tol * 2.0) continue;
            }
            {
                double dd = fabs(mx - cx) + fabs(my - cy);

                if (dd < bestd) { bestd = dd; found = id; }
            }
        }
    }
    ProArrayFree((ProArray *)&ents);
    if (found < 0) sprintf_s(why, cap, "no cross line near (%.1f,%.1f) vert=%d", cx, cy, want_vertical);
    else           sprintf_s(why, cap, "found id=%d d=%.2f", found, bestd);
    return found;
}

static void edgescan_run(ProMdl part, double rmin, double rmax)
{
    struct { int id, type; double cx, cy, cz, r; } U[64];
    int nU = 0, hid;
    for (hid = 1; hid <= 8000; hid++) {
        ProEdge e = NULL;
        ProGeomitem gi;
        ProGeomitemdata *gd = NULL;
        int t;
        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        t = gd->data.p_curve_data->line.type;
        if (t == PRO_ENT_CIRCLE || t == PRO_ENT_ARC) {
            double cx, cy, cz, r;
            if (t == PRO_ENT_CIRCLE) {
                cx = gd->data.p_curve_data->circle.center[0];
                cy = gd->data.p_curve_data->circle.center[1];
                cz = gd->data.p_curve_data->circle.center[2];
                r  = gd->data.p_curve_data->circle.radius;
            } else {
                cx = gd->data.p_curve_data->arc.origin[0];
                cy = gd->data.p_curve_data->arc.origin[1];
                cz = gd->data.p_curve_data->arc.origin[2];
                r  = gd->data.p_curve_data->arc.radius;
            }
            if (r >= rmin && r <= rmax) {
                int dup = -1, k;
                /* 全量打印：同一圆心可能有前后两面多条（CIRCLE/ARC），只留一条会选到背面看不见的 */
                logf_("[edgescan] raw id=%d type=%s center=(%.3f,%.3f,%.3f) r=%.4f",
                      hid, (t == PRO_ENT_CIRCLE) ? "CIRCLE" : "ARC", cx, cy, cz, r);
                for (k = 0; k < nU; k++) {
                    if (fabs(U[k].cx - cx) < 0.05 && fabs(U[k].cy - cy) < 0.05) { dup = k; break; }
                }
                if (dup < 0 && nU < 64) {
                    U[nU].id = hid; U[nU].type = t; U[nU].cx = cx; U[nU].cy = cy; U[nU].cz = cz; U[nU].r = r;
                    logf_("[edgescan] id=%d type=%s center=(%.2f,%.2f,%.2f) r=%.4f",
                          hid, (t == PRO_ENT_CIRCLE) ? "CIRCLE" : "ARC", cx, cy, cz, r);
                    nU++;
                } else if (dup >= 0 && t == PRO_ENT_CIRCLE && U[dup].type != PRO_ENT_CIRCLE) {
                    U[dup].id = hid; U[dup].type = t; U[dup].cz = cz;
                    logf_("[edgescan]   -> unique slot replaced with CIRCLE id=%d z=%.3f", hid, cz);
                }
            }
        }
        ProGeomitemdataFree(&gd);
    }
    logf_("[edgescan] unique=%d (r %.2f..%.2f)", nU, rmin, rmax);
    {
        int a, b;
        for (a = 0; a < nU; a++) {
            for (b = a + 1; b < nU; b++) {
                double dx = U[b].cx - U[a].cx, dy = U[b].cy - U[a].cy;
                if (fabs(dy) < 0.5 && fabs(dx) > 0.5)
                    logf_("[edgescan] pair (%d,%d) dx=%.4f", U[a].id, U[b].id, dx);
                if (fabs(dx) < 0.5 && fabs(dy) > 0.5)
                    logf_("[edgescan] pair (%d,%d) dy=%.4f", U[a].id, U[b].id, dy);
            }
        }
    }
}

static int mk_cross_sel(ProMdl drw, int id, ProView view, ProSelection *out)
{
    ProModelitem mi;

    memset(&mi, 0, sizeof(mi));
    if (ProModelitemInit(drw, id, PRO_DRAFT_ENTITY, &mi) != PRO_TK_NO_ERROR) return 0;
    if (ProSelectionAlloc(NULL, &mi, out) != PRO_TK_NO_ERROR || !*out) return 0;
    ProSelectionViewSet(view, out);
    return 1;
}

/* 用两根十字线建一根尺寸（附件=两条 draft 线；位置�?ab_search 搜索；公差按 conv_apply_tol 同规则） */
static void cross_dim_create(ProMdl drw, ProView view, int idA, int idB, int orient, int dir1, int dir2,
                             double spanHalf, double value, const char *tag)
{
    ProSelection sA = NULL, sB = NULL;
    ab_req_t     q;
    double       lx = 0.0, ly = 0.0;
    int          ok = 0;

    if (!mk_cross_sel(drw, idA, view, &sA) || !mk_cross_sel(drw, idB, view, &sB)) {
        logf_("[xdim] %s: cross sel failed (ids %d,%d)", tag, idA, idB);
        return;
    }
    /* 位置：用与几何路线相同的搜索（ownvi 由调用方定，这里直接�?front view 建包络） */
    memset(&q, 0, sizeof(q));
    q.orient = orient; q.dir1 = dir1; q.dir2 = dir2; q.ownvi = 0;
    {
        /* ownvi=0 假定主视图（调用方保证） */
        const ab_view_t *own = &g_abv[0];

        if (orient == 0) { q.gy0 = own->ocy - spanHalf; q.gy1 = own->ocy + spanHalf; q.gx0 = own->rect.x0; q.gx1 = own->rect.x1; }
        else             { q.gx0 = own->ocx - spanHalf; q.gx1 = own->ocx + spanHalf; q.gy0 = own->rect.y0; q.gy1 = own->rect.y1; }
    }
    ab_search(&q, &lx, &ly, &ok);
    if (!ok) {
        logf_("[xdim] %s: no position; fallback (385.7,570)", tag);
        lx = 385.7; ly = 570.0;
    }
    {
        ProDimAttachment *arr = NULL;
        ProDimSense      *sen = NULL;
        ProVector         loc;
        ProDimension      nd;
        ProError          ec;

        ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
        ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
        ProSelectionCopy(sA, &arr[0][0]); arr[0][1] = NULL;
        ProSelectionCopy(sB, &arr[1][0]); arr[1][1] = NULL;
        memset(&sen[0], 0, sizeof(ProDimSense));
        memset(&sen[1], 0, sizeof(ProDimSense));
        sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = (orient == 0) ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;
        sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = sen[0].orient_hint;
        loc[0] = lx; loc[1] = ly; loc[2] = 0.0;
        memset(&nd, 0, sizeof(nd));
        ec = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, loc, PRO_B_FALSE, &nd);
        logf_("[xdim] %s create @(%.1f,%.1f) attach=cross(%d,%d) -> %d (%s)", tag, lx, ly, idA, idB, (int)ec, errname(ec));
        if (ec == PRO_TK_NO_ERROR) {
            double nv = -1.0;

            if (ProDimensionValueGet(&nd, &nv) == PRO_TK_NO_ERROR)
                logf_("[xdim] %s *** Creo computed = %.4f (expect %.4f) ***", tag, nv, value);
            conv_apply_tol(&nd, value);
            {
                ProError es = ProAnnotationShow((ProAnnotation *)&nd, NULL, view);

                logf_("[xdim] %s show -> %d (%s)", tag, (int)es, errname(es));
            }
            g_last_nd = nd; g_last_nd_valid = 1; g_last_nd_value = value;
            ab_dim_register((ProMdl)drw, &nd, tag, orient, 0);
            /* 轮次55 修正：交叉尺寸也要登�?矩形"，否则后续尺寸的净距规则看不见它（实测 63.6 曾贴�?6px�?*/
            if (orient == 0) ab_dim_add(lx - 7.0, q.gy0 - 8.0, lx + 7.0, q.gy1 + 8.0, tag);
            else             ab_dim_add(q.gx0 - 8.0, ly - 7.0, q.gx1 + 8.0, ly + 7.0, tag);
        }
        ProArrayFree((ProArray *)&arr);
        ProArrayFree((ProArray *)&sen);
    }
    ProSelectionFree(&sA);
    ProSelectionFree(&sB);
}

/* 用户裁定后的凸台间距尺寸�?3（横向，上排两根竖十字）/ 23.5（竖向，中列两根横十字） */
static void boss_cross_dims(ProMdl part, ProMdl drw, int fvi, int mode)
{
    char why[80];
    int  id1, id2;

    (void)part; (void)fvi;
    {
        /* 2026-10-29: the anchor grid used to be "front view centre +/- 28".
           That only works when the crosses are spread 56 units apart, which is
           the nb109_3 layout. boss_centerlines now picks its scan plane from the
           bbox, so the crosses sit wherever the bosses really project - derive
           the grid from their actual bounding box instead of assuming a span. */
        ProView *views = NULL;
        int vc = 0, vi;
        double cx = 300.0, cy = 520.0;
        if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
            ProArraySizeGet((ProArray)views, &vc);
            for (vi = 0; vi < vc; vi++) {
                ProName nm;
                nm[0] = 0;
                ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) {
                    ProOutline o;
                    if (ProDrawingViewOutlineGet((ProDrawing)drw, views[vi], o) == PRO_TK_NO_ERROR) {
                        cx = (o[0][0] + o[1][0]) / 2.0;
                        cy = (o[0][1] + o[1][1]) / 2.0;
                    }
                    break;
                }
            }
            ProArrayFree((ProArray *)&views);
        }
        /* Measure the real cross extent from the drawing entities. */
        {
            double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
            int    hits = cross_extent(drw, &xmin, &xmax, &ymin, &ymax);
            if (hits >= 2) {
                cx = (xmin + xmax) / 2.0;
                cy = (ymin + ymax) / 2.0;
                g_cross_dx = (xmax - xmin) / 2.0;   /* half the boss spacing */
                g_cross_dy = (ymax - ymin) / 2.0;
                logf_("[xdim] cross extent x[%.1f..%.1f] y[%.1f..%.1f] n=%d -> centre (%.1f,%.1f) dx=%.1f dy=%.1f",
                      xmin, xmax, ymin, ymax, hits, cx, cy, g_cross_dx, g_cross_dy);
            } else {
                g_cross_dx = 28.0;
                g_cross_dy = 28.0;
                logf_("[xdim] cross extent not measurable (hits=%d); keep view-centre anchor", hits);
            }
        }
        g_xdim_cx = cx; g_xdim_cy = cy;
        logf_("[xdim] front anchor center=(%.1f,%.1f)", cx, cy);
    }
    if (mode != 2) {
        /* 23: the two boss crosses furthest apart along X. Enumerate every
           vertical cross arm in the grid box and take the widest pair; a
           fixed-offset lookup cannot work because the arms are only a few
           units long and their exact spacing depends on the projection. */
        id1 = id2 = -1;
        {
            int a, b;
            double best = -1.0;
            int nmax = 0;
            int grid[64];
            double gx[64], gy[64];
            /* 2026-10-30: tol must reach the outermost cross, not the half-span.
               nb109_5 crosses sit at +/-43.3 from the grid centre, so pass the
               full measured extent plus a margin. */
            nmax = cross_list(drw, 1, g_xdim_cx, g_xdim_cy, g_cross_dx * 2.0 + 8.0, grid, gx, gy, 64);
            logf_("[xdim] d23 vertical arms found=%d", nmax);
            /* 2026-10-30: the arms of ONE cross sit at the same X, so "furthest
               pair" picks two columns but they can be the SAME cross (span ~0).
               Group by X first (tolerance 2 paper units), keep one arm per group,
               then take the two closest distinct columns - that is the boss pitch. */
            {
                double cols[64]; int colid[64]; int nc = 0,n, found_dup;
                for (n = 0; n < nmax; n++) {
                    int q; found_dup = 0;
                    for (q = 0; q < nc; q++) if (fabs(cols[q] - gx[n]) < 2.0) { found_dup = 1; break; }
                    if (!found_dup && nc < 64) { cols[nc] = gx[n]; colid[nc] = grid[n]; nc++; }
                }
                logf_("[xdim] d23 distinct columns=%d", nc);
                for (n = 1; n < nc; n++) {
                    double d = fabs(cols[n] - cols[n-1]);
                    if (best < 0.0 || d < best) { best = d; id1 = colid[n-1]; id2 = colid[n]; }
                }
            }
            logf_("[xdim] d23 pair ids %d/%d span=%.2f", id1, id2, best);
        }
    }
    if (mode == 1) {
        if (id1 >= 0 && id2 >= 0)
            cross_dim_create(drw, g_abv[0].v, id1, id2, 1, +1, 0, 30.0, 23.0, "d23");
        return;
    }
    /* 23.5: the two boss crosses furthest apart along Y */
    id1 = id2 = -1;
    {
        int a, b;
        double best = -1.0;
        int nmax = 0;
        int grid[64];
        double gx[64], gy[64];
        nmax = cross_list(drw, 0, g_xdim_cx, g_xdim_cy, g_cross_dy * 2.0 + 8.0, grid, gx, gy, 64);
        logf_("[xdim] d235 horizontal arms found=%d", nmax);
        for (a = 0; a < nmax; a++)
            for (b = a + 1; b < nmax; b++) {
                double d = fabs(gy[a] - gy[b]);
                if (d > best) { best = d; id1 = grid[a]; id2 = grid[b]; }
            }
        logf_("[xdim] d235 pair ids %d/%d span=%.2f", id1, id2, best);
    }
    if (id1 >= 0 && id2 >= 0)
        cross_dim_create(drw, g_abv[0].v, id1, id2, 0, +1, 0, 28.0, 23.5, "d235");
}

/* 轮次55：DTLSYM 清点（可选）——这里保留占�?*/
static void axis_lines_to_views(ProMdl drw, ProMdl part)
{
    /* 中心�?= 显示模型的轴（PRO_AXIS 注解）。零坐标运算：投影由 Creo 自己做�?
       （AxisToDtlaxis 路线 4 �?ok=0，已放弃；保留上面的 axis_dtl_cb 以备后用。） */
    ProView *views = NULL;
    int      vc = 0, i;

    (void)part;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[axisshow] viewsCollect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    for (i = 0; i < vc; i++) {
        ProName nm;
        ProError e;
        nm[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
        /* 只在主视图显示轴：其他视图（俯视等）轴上投影成贯穿长线，人样没有这种 */
        if (!(wcsstr(nm, L"front") || wcsstr(nm, L"FRONT"))) {
            logf_("[axisshow] view[%d] '%ls' skipped (non-front)", i, nm);
            continue;
        }
        e = ProAnnotationByViewShow((ProDrawing)drw, views[i], PRO_AXIS);
        logf_("[axisshow] view[%d] '%ls' PRO_AXIS -> %d (%s)", i, nm, (int)e, errname(e));
    }
    ProArrayFree((ProArray *)&views);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* P3：右上角 1:2 轴测图（先给零件�?'3d' 视图再用 ProDrawingGeneralviewCreate�?*/
/* axiso eye direction, overridable via ISO|x|y|scale|wx,wy,wz:
   default looks from -X (boss face), +Y (top), +Z (right end). */
static double g_iso_dir[3] = { 0.752, -0.451, -0.481 };

/* 2026-11-04: Creo reads the orient matrix by COLUMN, i.e. m[r][c] is the
   component of paper axis c (x / y / line-of-sight) along model axis r.
   The old code wrote the paper axes as ROWS, which is the transpose, so the
   long axis Z contributed 0.577 to paper x AND 0.577 to paper y -> the iso
   view collapsed into one 45-degree stroke (nb109_1.drw.5, top right).
   Evidence: VIEWADD|probeF passes rows {x<-Z, y<-X, dir<-Y}, yet its outline
   comes back 46.2 x 151.4 = 19.44 x 63.6, i.e. paper x<-Y, paper y<-Z. */
static void iso_basis(ProMatrix m)
{
    double wv[3], uv[3], vv[3], n;
    int    i;

    for (i = 0; i < 3; i++) wv[i] = g_iso_dir[i];
    n = sqrt(wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]);
    if (n < 1e-9) { wv[0] = 0.752; wv[1] = -0.451; wv[2] = -0.481; n = 1.0; }
    for (i = 0; i < 3; i++) wv[i] /= n;
    /* u = normalize(w x Yup): long axis Z stays horizontal on paper */
    uv[0] = -wv[2]; uv[1] = 0.0; uv[2] = wv[0];
    n = sqrt(uv[0] * uv[0] + uv[2] * uv[2]);
    if (n < 1e-9) { uv[0] = 0.0; uv[1] = 0.0; uv[2] = 1.0; n = 1.0; }
    uv[0] /= n; uv[1] = 0.0; uv[2] /= n;
    /* v = u x w: same handedness as the working views (x<-Z, y<-Y, dir<-+X) */
    vv[0] = uv[1] * wv[2] - uv[2] * wv[1];
    vv[1] = uv[2] * wv[0] - uv[0] * wv[2];
    vv[2] = uv[0] * wv[1] - uv[1] * wv[0];
    for (i = 0; i < 16; i++) ((double *)m)[i] = 0.0;
    for (i = 0; i < 3; i++) {
        m[i][0] = uv[i];   /* column 0 = paper x   */
        m[i][1] = vv[i];   /* column 1 = paper y   */
        m[i][2] = wv[i];   /* column 2 = view dir  */
    }
    m[3][3] = 1.0;
    logf_("[iso] basis u=(%.3f,%.3f,%.3f) v=(%.3f,%.3f,%.3f) w=(%.3f,%.3f,%.3f) dots uv=%.4f uw=%.4f vw=%.4f",
          uv[0], uv[1], uv[2], vv[0], vv[1], vv[2], wv[0], wv[1], wv[2],
          uv[0] * vv[0] + uv[1] * vv[1] + uv[2] * vv[2],
          uv[0] * wv[0] + uv[1] * wv[1] + uv[2] * wv[2],
          vv[0] * wv[0] + vv[1] * wv[1] + vv[2] * wv[2]);
}

static void add_iso_view(ProMdl drw, ProMdl part, double px, double py, double scale)
{
    ProMatrix iso;
    ProView   v = NULL;
    ProError  e;

    iso_basis(iso);
    v = NULL;
    e = ProMdlDisplay(drw);
    logf_("[iso] A display -> %d (%s)", (int)e, errname(e));
    /* 注意：不要调 ProViewMatrixSet/ProViewStore —�?p_view=NULL 表示“当前活动图纸视图”，
       实测会把 顶部_1 的朝向污染成轴测方向（变成大斜视图）。手推矩阵直接传�?
       GeneralviewCreate 即可（create->0 已实测）�?*/
    /* 先清掉旧的轴测图（识别规则：视图比例 <0.9；我们的轴测�?0.5，投影视�?1.0�?
       注意：主视图/侧视�?俯视图也�?general 类型，绝不能按类型清 —�?按比例清�?*/
    {
        ProView *views = NULL;
        if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
            int vc = 0, i;
            ProArraySizeGet((ProArray)views, &vc);
            for (i = 0; i < vc; i++) {
                double vscale = -1.0;
                ProName nm;
                nm[0] = 0;
                ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                /* 2026-11-04: scale<0.9 alone is not enough - a view created with
                   scale 1.0 still reports 1.0, so the old (bad) iso view survived
                   and the new one was added on top of it. Also match the default
                   auto name new_view_N, which no real view ever uses here. */
                if (ProDrawingViewScaleGet((ProDrawing)drw, views[i], &vscale) == PRO_TK_NO_ERROR &&
                    ((vscale > 0.0 && vscale < 0.9) || !wcsncmp(nm, L"new_view", 8))) {
                    ProError ee;
                    ee = ProDrawingViewErase((ProDrawing)drw, views[i]);
                    logf_("[iso] erase old iso view '%ls' (scale=%.2f) -> %d (%s)", nm, vscale, (int)ee, errname(ee));
                }
            }
            ProArrayFree((ProArray *)&views);
        }
    }
    {
        /* 2026-11-04: no longer overwrite the computed basis with the part's
           stored '3d' view matrix - its row/column convention is unverified
           and that is exactly what produced the degenerate iso. Only report. */
        ProView v3 = NULL;
        if (ProViewNameToView((ProSolid)part, L"3d", &v3) == PRO_TK_NO_ERROR && v3)
            logf_("[iso] B part has a '3d' view; still using computed basis");
        else
            logf_("[iso] B no '3d' view; use computed column-major basis");
    }
    {
        ProView    nv = NULL;
        ProPoint3d pos;
        pos[0] = px; pos[1] = py; pos[2] = 0.0;
        logf_("[iso] C create @(%.0f,%.0f) scale=%.2f ...", px, py, scale);
        e = ProDrawingGeneralviewCreate((ProDrawing)drw, (ProSolid)part, 1, PRO_B_FALSE, pos, scale, iso, &nv);
        logf_("[iso] D GeneralviewCreate -> %d (%s)", (int)e, errname(e));
        /* 注：比例 1:2 的标注由轴测图视图自身显示（上次渲染已证实），不再手工建注释 */
        (void)e;
    }
}

/* P2：三视图紧凑布局。report=只打印；do_move=按包围盒计算目标并移动（ProDrawingViewOriginSet�?*/
/* DETAIL|parentIdx|cx|cy|r|px|py|scale �?create a detail view. */
static void detail_run(ProMdl drw, ProMdl part, int pidx, double cx, double cy, double r,
                       double px, double py, double scale)
{
    ProView *views = NULL;
    int      vc = 0;
    ProView  parent = NULL;
    ProMatrix mat;
    int      hid, best = -1;
    double   bestd = 1e30;
    ProGeomitem bestGi;
    ProPoint3d  bestMid = { 0,0,0 };
    char     nm8[128];
    ProPoint3d loc = { 0,0,0 };

    if (!part || ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[detail] viewsCollect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    if (pidx < 0 || pidx >= vc) { logf_("[detail] parent idx %d out of %d", pidx, vc); ProArrayFree((ProArray *)&views); return; }
    parent = views[pidx];
    {
        ProName pn;
        pn[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, parent, pn);
        wide_to_u8(pn, nm8, sizeof(nm8));
        logf_("[detail] parent[%d]='%s'", pidx, nm8);
    }
    ProArrayFree((ProArray *)&views);

    if (ProDrawingViewTransformGet((ProDrawing)drw, parent, PRO_B_TRUE, mat) != PRO_TK_NO_ERROR) {
        logf_("[detail] transformGet failed");
        return;
    }
    memset(&bestGi, 0, sizeof(bestGi));
    for (hid = 1; hid <= 3000; hid++) {
        ProEdge e = NULL;
        ProGeomitem gi;
        ProGeomitemdata *gd = NULL;
        ProPoint3d mid = { 0,0,0 }, out = { 0,0,0 };

        if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
        if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
            if (gd) ProGeomitemdataFree(&gd);
            continue;
        }
        {
            int t = gd->data.p_curve_data->line.type;

            if (t == PRO_ENT_LINE) {
                mid[0] = (gd->data.p_curve_data->line.end1[0] + gd->data.p_curve_data->line.end2[0]) / 2.0;
                mid[1] = (gd->data.p_curve_data->line.end1[1] + gd->data.p_curve_data->line.end2[1]) / 2.0;
                mid[2] = (gd->data.p_curve_data->line.end1[2] + gd->data.p_curve_data->line.end2[2]) / 2.0;
            } else if (t == PRO_ENT_ARC) {
                mid[0] = gd->data.p_curve_data->arc.origin[0];
                mid[1] = gd->data.p_curve_data->arc.origin[1];
                mid[2] = gd->data.p_curve_data->arc.origin[2];
            } else if (t == PRO_ENT_CIRCLE) {
                mid[0] = gd->data.p_curve_data->circle.center[0];
                mid[1] = gd->data.p_curve_data->circle.center[1];
                mid[2] = gd->data.p_curve_data->circle.center[2];
            } else {
                ProGeomitemdataFree(&gd);
                continue;
            }
        }
        ProGeomitemdataFree(&gd);
        if (ProPntTrfEval(mid, mat, out) != PRO_TK_NO_ERROR) continue;
        {
            double d = sqrt((out[0] - cx) * (out[0] - cx) + (out[1] - cy) * (out[1] - cy));
            if (d < bestd) { bestd = d; best = hid; bestGi = gi; bestMid[0] = mid[0]; bestMid[1] = mid[1]; bestMid[2] = mid[2]; }
        }
    }
    if (best < 0) { logf_("[detail] no model edge found"); return; }
    logf_("[detail] nearest edge id=%d d=%.2f model=(%.2f,%.2f,%.2f)", best, bestd, bestMid[0], bestMid[1], bestMid[2]);
    {
        ProSelection sel = NULL;
        int N = 25, i;
        double *par = (double *)calloc((size_t)N, sizeof(double));
        ProPoint3d *pnt = (ProPoint3d *)calloc((size_t)N, sizeof(ProPoint3d));
        ProCurvedata cd;
        ProError ec;

        if (ProSelectionAlloc(NULL, (ProModelitem *)&bestGi, &sel) != PRO_TK_NO_ERROR || !sel) {
            logf_("[detail] selection alloc failed");
            free(par); free(pnt);
            return;
        }
        ProSelectionViewSet(parent, &sel);
        for (i = 0; i < N; i++) {
            double a = 2.0 * 3.14159265358979323846 * (i % 24) / 24.0;
            pnt[i][0] = cx + r * cos(a);
            pnt[i][1] = cy + r * sin(a);
            pnt[i][2] = 0.0;
            par[i] = (double)i;
        }
        memset(&cd, 0, sizeof(cd));
        cd.spline.type = PRO_ENT_SPLINE;
        cd.spline.par_arr = par;
        cd.spline.pnt_arr = pnt;
        cd.spline.tan_arr = NULL;
        cd.spline.num_points = N;
        loc[0] = px; loc[1] = py; loc[2] = 0.0;
        {
            ProView nv = NULL;

            ec = ProDrawingViewDetailCreate((ProDrawing)drw, parent, NULL, &cd, loc, &nv);
            logf_("[detail] create(paper spline c=(%.1f,%.1f) r=%.1f -> (%.1f,%.1f) sc=%.2f) -> %d (%s)",
                  cx, cy, r, px, py, scale, (int)ec, errname(ec));
            if (ec != PRO_TK_NO_ERROR) {
                for (i = 0; i < N; i++) {
                    double a = 2.0 * 3.14159265358979323846 * i / (double)N;
                    pnt[i][0] = bestMid[0] + (r / 2.3805) * cos(a);
                    pnt[i][1] = bestMid[1] + (r / 2.3805) * sin(a);
                    pnt[i][2] = bestMid[2];
                }
                nv = NULL;
                ec = ProDrawingViewDetailCreate((ProDrawing)drw, parent, NULL, &cd, loc, &nv);
                logf_("[detail] retry(model spline c=(%.2f,%.2f) r=%.2f) -> %d (%s)",
                      bestMid[0], bestMid[1], r / 2.3805, (int)ec, errname(ec));
            }
            if (ec == PRO_TK_NO_ERROR && nv) {
                ProName vn;
                ProPoint3d out2[2];
                double sc = -1.0;

                ProDrawingViewDetailBoundarySet((ProDrawing)drw, nv, PRO_DETAIL_BOUNDARY_CIRCLE, PRO_B_TRUE);
                logf_("[detail] scaleSet %.3f -> %d", scale, (int)ProDrawingViewScaleSet((ProDrawing)drw, nv, scale));
                ProDrawingViewOriginSet((ProDrawing)drw, nv, loc, NULL);
                logf_("[detail] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
                vn[0] = 0;
                ProDrawingViewNameGet((ProDrawing)drw, nv, vn);
                if (ProDrawingViewScaleGet((ProDrawing)drw, nv, &sc) != PRO_TK_NO_ERROR) sc = -1.0;
                out2[0][0] = out2[0][1] = -9999; out2[1][0] = out2[1][1] = -9999;
                ProDrawingViewOutlineGet((ProDrawing)drw, nv, out2);
                {
                    char v8[128];
                    wide_to_u8(vn, v8, sizeof(v8));
                    logf_("[detail] new view name='%s' scale=%.3f outline=(%.1f,%.1f)-(%.1f,%.1f)",
                          v8, sc, out2[0][0], out2[0][1], out2[1][0], out2[1][1]);
                }
                {
                    ProView *v2 = NULL;
                    int n2 = 0;
                    if (ProDrawingViewsCollect((ProDrawing)drw, &v2) == PRO_TK_NO_ERROR && v2) {
                        ProArraySizeGet((ProArray)v2, &n2);
                        ProArrayFree((ProArray *)&v2);
                    }
                    logf_("[detail] view count now = %d", n2);
                }
            }
        }
        ProSelectionFree(&sel);
        free(par); free(pnt);
    }
}

typedef struct {
    ProDrawing   drw;
    ProView      parent;
    ProSelection sel;
    ProCurvedata *cd;
    ProPoint3d   loc;
    ProView      view;
    ProError     err;
    HANDLE       done;
} detail_thr_t;

static DWORD WINAPI detail_thread_proc(LPVOID p)
{
    detail_thr_t *t = (detail_thr_t *)p;

    t->err = ProDrawingViewDetailCreate(t->drw, t->parent, t->sel, t->cd, t->loc, &t->view);
    SetEvent(t->done);
    return 0;
}

/* DETAIL2|parent|cx|cy|r|px|py|scale �?circle boundary + draft point ref, threaded create. */
static void detail2_run(ProMdl drw, ProMdl part, int pidx, double cx, double cy, double r,
                        double px, double py, double scale)
{
    ProView *views = NULL;
    int      vc = 0;
    ProView  parent = NULL;
    ProDtlentitydata ed = NULL;
    ProCurvedata    pcd, ccd;
    ProDtlentity    ent;
    ProDtlentitydata ed2 = NULL;
    int             eid = -1;
    ProModelitem    mi;
    ProSelection    sel = NULL;
    ProPoint3d      loc = { 0,0,0 };
    detail_thr_t    th;
    HANDLE          thd = NULL;

    (void)part;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) { logf_("[detail2] viewsCollect failed"); return; }
    ProArraySizeGet((ProArray)views, &vc);
    if (pidx < 0 || pidx >= vc) { logf_("[detail2] idx out of range"); ProArrayFree((ProArray *)&views); return; }
    parent = views[pidx];
    ProArrayFree((ProArray *)&views);

    memset(&pcd, 0, sizeof(pcd));
    pcd.point.type = PRO_ENT_POINT;
    pcd.point.position[0] = cx; pcd.point.position[1] = cy; pcd.point.position[2] = 0.0;
    if (ProDtlentitydataAlloc(drw, &ed) != PRO_TK_NO_ERROR || !ed) { logf_("[detail2] dataAlloc failed"); return; }
    logf_("[detail2] curveSet -> %d", (int)ProDtlentitydataCurveSet(ed, &pcd));
    logf_("[detail2] viewSet -> %d", (int)ProDtlentitydataViewSet(ed, parent));
    memset(&ent, 0, sizeof(ent));
    {
        ProError ee = ProDtlentityCreate(drw, NULL, ed, &ent);
        logf_("[detail2] entityCreate(center point) -> %d (%s)", (int)ee, errname(ee));
    }
    if (ProDtlentityDataGet(&ent, NULL, &ed2) == PRO_TK_NO_ERROR && ed2)
        ProDtlentitydataIdGet(ed2, &eid);
    logf_("[detail2] center point id=%d", eid);
    if (eid < 0) return;
    memset(&mi, 0, sizeof(mi));
    ProModelitemInit(drw, eid, PRO_DRAFT_ENTITY, &mi);
    if (ProSelectionAlloc(NULL, &mi, &sel) != PRO_TK_NO_ERROR || !sel) { logf_("[detail2] sel failed"); return; }
    ProSelectionViewSet(parent, &sel);
    logf_("[detail2] selection made (view set)");

    memset(&ccd, 0, sizeof(ccd));
    ccd.circle.type = PRO_ENT_CIRCLE;
    ccd.circle.center[0] = cx; ccd.circle.center[1] = cy; ccd.circle.center[2] = 0.0;
    ccd.circle.norm_axis_unit_vect[0] = 0.0; ccd.circle.norm_axis_unit_vect[1] = 0.0; ccd.circle.norm_axis_unit_vect[2] = 1.0;
    ccd.circle.radius = r;
    loc[0] = px; loc[1] = py; loc[2] = 0.0;

    memset(&th, 0, sizeof(th));
    th.drw = (ProDrawing)drw; th.parent = parent; th.sel = sel; th.cd = &ccd; th.loc[0] = px; th.loc[1] = py; th.loc[2] = 0.0;
    th.done = CreateEvent(NULL, TRUE, FALSE, NULL);
    thd = CreateThread(NULL, 0, detail_thread_proc, &th, 0, NULL);
    logf_("[detail2] thread=%p started; waiting 10s", (void *)thd);
    if (thd && WaitForSingleObject(th.done, 10000) == WAIT_OBJECT_0) {
        logf_("[detail2] thread returned err=%d (%s) view=%p", (int)th.err, errname(th.err), (void *)th.view);
        if (th.err == PRO_TK_NO_ERROR && th.view) {
            ProName vn; ProPoint3d out2[2]; double sc = -1.0; ProView *v2 = NULL; int n2 = 0;

            ProDrawingViewDetailBoundarySet((ProDrawing)drw, th.view, PRO_DETAIL_BOUNDARY_CIRCLE, PRO_B_TRUE);
            logf_("[detail2] scaleSet %.2f -> %d", scale, (int)ProDrawingViewScaleSet((ProDrawing)drw, th.view, scale));
            ProDrawingViewOriginSet((ProDrawing)drw, th.view, th.loc, NULL);
            logf_("[detail2] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
            vn[0] = 0; ProDrawingViewNameGet((ProDrawing)drw, th.view, vn);
            if (ProDrawingViewScaleGet((ProDrawing)drw, th.view, &sc) != PRO_TK_NO_ERROR) sc = -1;
            out2[0][0] = out2[0][1] = -9999; out2[1][0] = out2[1][1] = -9999;
            ProDrawingViewOutlineGet((ProDrawing)drw, th.view, out2);
            {
                char v8[128]; wide_to_u8(vn, v8, sizeof(v8));
                logf_("[detail2] new view '%s' scale=%.3f outline=(%.1f,%.1f)-(%.1f,%.1f)",
                      v8, sc, out2[0][0], out2[0][1], out2[1][0], out2[1][1]);
            }
            if (ProDrawingViewsCollect((ProDrawing)drw, &v2) == PRO_TK_NO_ERROR && v2) {
                ProArraySizeGet((ProArray)v2, &n2);
                ProArrayFree((ProArray *)&v2);
            }
            logf_("[detail2] view count = %d", n2);
        }
    } else {
        logf_("[detail2] *** create BLOCKED >10s (thread left running) ***");
    }
    if (th.done) CloseHandle(th.done);
    ProSelectionFree(&sel);
}

/* DBOUND|idx|r_paper �?replace a detail view boundary with a circle of r paper units
   around the view's reference point (local space: 0,0=ref point, unit=inch in top model). */
static void dbound_run(ProMdl drw, int idx, double rpaper)
{
    ProView *views = NULL;
    int vc = 0;
    ProView v = NULL;
    ProCurvedata cd;
    double rlocal = rpaper / (2.3805 * 25.4);
    ProError e;

    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) { logf_("[dbound] collect failed"); return; }
    ProArraySizeGet((ProArray)views, &vc);
    if (idx < 0 || idx >= vc) { logf_("[dbound] idx out of range"); ProArrayFree((ProArray *)&views); return; }
    v = views[idx];
    ProArrayFree((ProArray *)&views);
    memset(&cd, 0, sizeof(cd));
    {
        int N = 25, i;
        double *par = (double *)calloc((size_t)N, sizeof(double));
        ProPoint3d *pnt = (ProPoint3d *)calloc((size_t)N, sizeof(ProPoint3d));

        for (i = 0; i < N; i++) {
            double a = 2.0 * 3.14159265358979323846 * (i % 24) / 24.0;
            pnt[i][0] = rlocal * cos(a);
            pnt[i][1] = rlocal * sin(a);
            pnt[i][2] = 0.0;
            par[i] = (double)i;
        }
        cd.spline.type = PRO_ENT_SPLINE;
        cd.spline.par_arr = par;
        cd.spline.pnt_arr = pnt;
        cd.spline.tan_arr = NULL;
        cd.spline.num_points = N;
        e = ProDrawingViewDetailCurvedataSet((ProDrawing)drw, v, &cd);
        free(par); free(pnt);
    }
    logf_("[dbound] idx=%d r_paper=%.2f -> r_local=%.5f in -> %d (%s)", idx, rpaper, rlocal, (int)e, errname(e));
    logf_("[dbound] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
    {
        ProPoint3d out[2]; double sc = -1;
        out[0][0] = out[0][1] = -9999; out[1][0] = out[1][1] = -9999;
        ProDrawingViewOutlineGet((ProDrawing)drw, v, out);
        if (ProDrawingViewScaleGet((ProDrawing)drw, v, &sc) != PRO_TK_NO_ERROR) sc = -1;
        logf_("[dbound] outline=(%.1f,%.1f)-(%.1f,%.1f) scale=%.2f", out[0][0], out[0][1], out[1][0], out[1][1], sc);
    }
}

static void add_native_ortho_view(ProDrawing drw, ProSolid part,
                                  double px, double py, double scale,
                                  const double m[4][4], const wchar_t *name)
{
    ProMatrix mat;
    ProPoint3d pos;
    ProView nv = NULL;
    ProError e;
    memcpy(mat, m, sizeof(ProMatrix));
    pos[0] = px; pos[1] = py; pos[2] = 0.0;
    e = ProDrawingGeneralviewCreate(drw, part, 1, PRO_B_FALSE, pos, scale, mat, &nv);
    logf_("[nview] create '%ls' @(%.1f,%.1f) sc=%.2f -> %d (%s) view=%p",
          name, px, py, scale, (int)e, errname(e), (void *)nv);
    if (e == PRO_TK_NO_ERROR && nv) {
        ProError en = ProDrawingViewNameSet(drw, nv, (wchar_t *)name);
        logf_("[nview] nameSet '%ls' -> %d (%s)", name, (int)en, errname(en));
    }
}

typedef struct { ProMdl want; int found; int seen; } inc_winfind_ctx_t;

static ProError inc_winfind_cb(int wid, ProAppData app)
{
    inc_winfind_ctx_t *c = (inc_winfind_ctx_t *)app;
    ProMdl m = NULL;
    ProName nm;
    ProError e = ProWindowMdlGet(wid, &m);
    nm[0] = 0;
    if (m) ProMdlNameGet(m, nm);
    logf_("[win] id=%d mdlGet=%d (%s) mdl=%p name='%ls'%s",
          wid, (int)e, errname(e), (void *)m, nm, (m == c->want) ? "  <== drawing" : "");
    c->seen++;
    if (e == PRO_TK_NO_ERROR && m && m == c->want) c->found = wid;
    return PRO_TK_NO_ERROR;
}

static int inc_find_drawing_window(ProMdl drw)
{
    inc_winfind_ctx_t c;
    c.want = drw; c.found = 0; c.seen = 0;
    ProWindowsVisit(inc_winfind_cb, (ProAppData)&c);
    logf_("[win] visit seen=%d found=%d", c.seen, c.found);
    return c.found;
}

static void inc_show_drawing(ProMdl drw)
{
    int wid;
    ProError ed = PRO_TK_NO_ERROR;
    ProError ea = PRO_TK_GENERAL_ERROR;
    ProError ec = PRO_TK_GENERAL_ERROR;
    if (!drw) { logf_("[show] no drawing"); return; }
    wid = inc_find_drawing_window(drw);
    if (wid <= 0) {
        ed = ProMdlDisplay(drw);
        logf_("[show] ProMdlDisplay -> %d (%s)", (int)ed, errname(ed));
        wid = inc_find_drawing_window(drw);
    }
    if (wid > 0) {
        ea = ProWindowActivate(wid);
        ec = ProWindowCurrentSet(wid);
        ProWindowRepaint(wid);
    }
    logf_("[show] wid=%d activate -> %d (%s) currentSet -> %d (%s)",
          wid, (int)ea, errname(ea), (int)ec, errname(ec));
}
static void layout_report(ProMdl drw, int do_move)
{
    ProView *views = NULL;
    int      vc = 0, i;
    int      i_front = -1;
    ProPoint3d f_org;
    ProPoint3d f_out0, f_out1;

    f_org[0] = f_org[1] = -9999.0;
    f_out0[0] = f_out0[1] = f_out1[0] = f_out1[1] = -9999.0;
    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
        logf_("[layout] viewsCollect failed");
        return;
    }
    ProArraySizeGet((ProArray)views, &vc);
    for (i = 0; i < vc; i++) {
        ProName    nm;
        ProPoint3d org, out[2];
        ProSelection sr = NULL;
        double     vsc = -1.0;

        nm[0] = 0;
        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
        org[0] = org[1] = org[2] = -9999.0;
        ProDrawingViewOriginGet((ProDrawing)drw, views[i], org, &sr);
        out[0][0] = out[0][1] = out[0][2] = -9999.0;
        out[1][0] = out[1][1] = out[1][2] = -9999.0;
        ProDrawingViewOutlineGet((ProDrawing)drw, views[i], out);
        ProDrawingViewScaleGet((ProDrawing)drw, views[i], &vsc);
        logf_("[layout] view[%d] '%ls': scale=%.3f origin=(%.1f,%.1f) outline x[%.1f..%.1f] y[%.1f..%.1f]",
              i, nm, vsc, org[0], org[1], out[0][0], out[1][0], out[0][1], out[1][1]);
        if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) {
            i_front = i;
            f_org[0] = org[0]; f_org[1] = org[1];
            f_out0[0] = out[0][0]; f_out0[1] = out[0][1];
            f_out1[0] = out[1][0]; f_out1[1] = out[1][1];
        }
    }
    if (!do_move || i_front < 0) {
        ProArrayFree((ProArray *)&views);
        return;
    }
    {
        /* 目标：其余视图与主视图间�?~75 单位（≈30mm），右视图水平对齐、俯视图垂直对齐�?
           注意两套空间：outline（logical screen）与 OriginSet 位置�?2.381 倍（实测标定�?
           �?评审记录 坐标系对照表）。轴测图（scale<0.9）一律不参与�?*/
        const double gap = 75.0;
        const double k_org_out = 2.381;   /* Δoutline = 2.381 × Δorigin */
        for (i = 0; i < vc; i++) {
            ProName    nm;
            ProPoint3d org, out[2], norg;
            ProSelection sr = NULL;
            double     dx = 0.0, dy = 0.0, vscale = -1.0;

            if (i == i_front) continue;
            if (ProDrawingViewScaleGet((ProDrawing)drw, views[i], &vscale) == PRO_TK_NO_ERROR &&
                vscale > 0.0 && vscale < 0.9) {
                logf_("[layout] skip iso view (scale=%.2f)", vscale);
                continue;
            }
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            org[0] = org[1] = org[2] = -9999.0;
            ProDrawingViewOriginGet((ProDrawing)drw, views[i], org, &sr);
            ProDrawingViewOutlineGet((ProDrawing)drw, views[i], out);
            if (out[0][0] < -9998.0 || org[0] < -9998.0) continue;
            {
                int is_right = (out[0][0] > (f_out0[0] + f_out1[0]) / 2.0);
                if (is_right) {
                    dx = (f_out1[0] + gap) - out[0][0];              /* 左缘对齐主视图右�?间隙 */
                    dy = (f_out0[1] + f_out1[1]) / 2.0 - (out[0][1] + out[1][1]) / 2.0;  /* 垂直居中 */
                } else {
                    dy = (f_out0[1] - gap) - out[1][1];              /* 顶缘对齐主视图下�?间隙 */
                    dx = (f_out0[0] + f_out1[0]) / 2.0 - (out[0][0] + out[1][0]) / 2.0;  /* 水平居中 */
                }
                /* outline 空间的位移换算到 OriginSet 位置空间 */
                norg[0] = org[0] + dx / k_org_out;
                norg[1] = org[1] + dy / k_org_out;
                norg[2] = 0.0;
                logf_("[layout] move '%ls' outline d(%.1f,%.1f) -> org +%.1f,%.1f", nm, dx, dy, dx / k_org_out, dy / k_org_out);
                {
                    ProError em = ProDrawingViewOriginSet((ProDrawing)drw, views[i], norg, NULL);
                    logf_("[layout]   OriginSet -> %d (%s)", (int)em, errname(em));
                }
            }
        }
    }
    ProArrayFree((ProArray *)&views);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 运行时命令通道：每 tick 检�?work\cmd_now.txt（首�?一条命令），结果打�?stderr�?
   MACRO|<宏文�? | STAT | SAVE | MKFRONT|<�?-2>|<flip 0/1> | EXIT */
#define CMD_FILE L"cmd_now.txt"
static wchar_t g_cmd_file[PRO_PATH_SIZE] = L"";
static const wchar_t *cmd_file_path(void)
{
    if (g_cmd_file[0] == 0) {
        if (GetEnvironmentVariableW(L"SYNC_CMD_FILE", g_cmd_file, PRO_PATH_SIZE) == 0)
            wcscpy_s(g_cmd_file, PRO_PATH_SIZE, CMD_FILE);
        logf_("[cmd] channel = '%ls'", g_cmd_file);
    }
    return g_cmd_file;
}
static wchar_t g_cmd_last[2048] = L"";

/* ---------- generic boundary dimensions (human style: pick outer edges) ---------- */
static ProView find_view_by_key(ProDrawing drw, int kind /*0=main,1=top,2=left*/)
{
    ProView *views = NULL;
    ProView v = NULL;
    int vc = 0, i;
    if (ProDrawingViewsCollect(drw, &views) != PRO_TK_NO_ERROR || !views) return NULL;
    ProArraySizeGet((ProArray)views, &vc);
    for (i = 0; i < vc; i++) {
        ProName nm;
        nm[0] = 0;
        ProDrawingViewNameGet(drw, views[i], nm);
        if (kind == 1) {
            if (wcsstr(nm, L"\u9876\u90E8") || wcsstr(nm, L"\u4FEF\u89C6")) { v = views[i]; break; }
        } else if (kind == 2) {
            if (wcsstr(nm, L"\u5DE6\u4FA7") || wcsstr(nm, L"\u5DE6\u89C6")) { v = views[i]; break; }
        } else {
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT") || wcsstr(nm, L"\u4E3B\u89C6")) { v = views[i]; break; }
        }
    }
    ProArrayFree((ProArray *)&views);
    return v;
}

static int pick_extreme_edge(ProSolid part, Pro3dPnt *box, int axis, int want_max,
                             int other_axis, double other_min_span)
{
    int hid, best = -1;
    double bestv = want_max ? -1e30 : 1e30;
    for (hid = 1; hid <= 5000; hid++) {
        ProEdge e = NULL;
        Pro3dPnt *pts = NULL;
        int cnt = 0, t, k;
        double mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
        if (ProEdgeInit(part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
        if (ProEdgeTessellationGet(e, &pts, NULL, NULL, NULL, &cnt) != PRO_TK_NO_ERROR || !pts || cnt < 2) continue;
        for (t = 0; t < cnt; t++) {
            for (k = 0; k < 3; k++) {
                if (pts[t][k] < mn[k]) mn[k] = pts[t][k];
                if (pts[t][k] > mx[k]) mx[k] = pts[t][k];
            }
        }
        ProArrayFree((ProArray *)&pts);
        if (mn[0] < box[0][0] - 0.4 || mx[0] > box[1][0] + 0.4) continue;
        if (mn[1] < box[0][1] - 0.4 || mx[1] > box[1][1] + 0.4) continue;
        if (mn[2] < box[0][2] - 0.4 || mx[2] > box[1][2] + 0.4) continue;
        if (mx[other_axis] - mn[other_axis] < other_min_span) continue;
        {
            double v = want_max ? mx[axis] : mn[axis];
            if ((want_max && v > bestv) || (!want_max && v < bestv)) { bestv = v; best = hid; }
        }
    }
    return best;
}

static void dim_two_edges(ProMdl drw, ProMdl part, ProView view, int id1, int id2,
                          int orient /*0=H,1=V*/, double x, double y, const char *tag)
{
    ProEdge e1 = NULL, e2 = NULL;
    ProGeomitem g1, g2;
    ProSelection *att = NULL, s1 = NULL, s2 = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProVector pos;
    ProDimOrient oh = orient ? PRO_DIM_ORNT_VERT : PRO_DIM_ORNT_HORIZ;

    if (!view) { logf_("[bdim] %s: no view", tag); return; }
    if (ProEdgeInit((ProSolid)part, id1, &e1) != PRO_TK_NO_ERROR || !e1) { logf_("[bdim] %s: edge %d", tag, id1); return; }
    if (ProEdgeInit((ProSolid)part, id2, &e2) != PRO_TK_NO_ERROR || !e2) { logf_("[bdim] %s: edge %d", tag, id2); return; }
    if (ProEdgeToGeomitem((ProSolid)part, e1, &g1) != PRO_TK_NO_ERROR) { logf_("[bdim] %s: gi1", tag); return; }
    if (ProEdgeToGeomitem((ProSolid)part, e2, &g2) != PRO_TK_NO_ERROR) { logf_("[bdim] %s: gi2", tag); return; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g1, &s1) != PRO_TK_NO_ERROR) { logf_("[bdim] %s: sel1", tag); return; }
    ProSelectionViewSet(view, &s1);
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g2, &s2) != PRO_TK_NO_ERROR) { logf_("[bdim] %s: sel2", tag); return; }
    ProSelectionViewSet(view, &s2);
    ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s1, &arr[0][0]); arr[0][1] = NULL;
    ProSelectionCopy(s2, &arr[1][0]); arr[1][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    memset(&sen[1], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = oh;
    sen[1].type = PRO_DIM_SNS_TYP_NONE; sen[1].orient_hint = oh;
    pos[0] = x; pos[1] = y; pos[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError ec = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, pos, PRO_B_FALSE, &dim);
        logf_("[bdim] %s edges %d/%d create@(%.1f,%.1f) -> %d (%s)", tag, id1, id2, x, y, (int)ec, errname(ec));
        if (ec == PRO_TK_NO_ERROR) {
            double v = -1.0;
            if (ProDimensionValueGet(&dim, &v) == PRO_TK_NO_ERROR)
                logf_("[bdim] %s *** value = %.4f ***", tag, v);
            logf_("[bdim] %s show -> %d (%s)", tag, (int)ProAnnotationShow((ProAnnotation *)&dim, NULL, view), errname(PRO_TK_NO_ERROR));
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s1);
    ProSelectionFree(&s2);
}

static void dim_one_edge(ProMdl drw, ProMdl part, ProView view, int id, double x, double y, const char *tag)
{
    ProEdge e = NULL;
    ProGeomitem g;
    ProSelection s = NULL;
    ProDimAttachment *arr = NULL;
    ProDimSense *sen = NULL;
    ProDimension dim;
    ProVector pos;

    if (!view) { logf_("[d1] %s: no view", tag); return; }
    if (ProEdgeInit((ProSolid)part, id, &e) != PRO_TK_NO_ERROR || !e) { logf_("[d1] %s: edge %d", tag, id); return; }
    if (ProEdgeToGeomitem((ProSolid)part, e, &g) != PRO_TK_NO_ERROR) { logf_("[d1] %s: gi", tag); return; }
    if (ProSelectionAlloc(NULL, (ProModelitem *)&g, &s) != PRO_TK_NO_ERROR) { logf_("[d1] %s: sel", tag); return; }
    ProSelectionViewSet(view, &s);
    ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&arr);
    ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sen);
    ProSelectionCopy(s, &arr[0][0]); arr[0][1] = NULL;
    memset(&sen[0], 0, sizeof(ProDimSense));
    sen[0].type = PRO_DIM_SNS_TYP_NONE; sen[0].orient_hint = PRO_DIM_ORNT_NONE;
    pos[0] = x; pos[1] = y; pos[2] = 0.0;
    memset(&dim, 0, sizeof(dim));
    {
        ProError ec = ProDrawingDimensionCreate((ProDrawing)drw, arr, sen, PRO_DIM_ORNT_NONE, pos, PRO_B_FALSE, &dim);
        logf_("[d1] %s edge %d create@(%.1f,%.1f) -> %d (%s)", tag, id, x, y, (int)ec, errname(ec));
        if (ec == PRO_TK_NO_ERROR) {
            double v = -1.0;
            if (ProDimensionValueGet(&dim, &v) == PRO_TK_NO_ERROR) logf_("[d1] %s *** value = %.4f ***", tag, v);
            ProAnnotationShow((ProAnnotation *)&dim, NULL, view);
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
    ProSelectionFree(&s);
}

static void tote_dims(ProMdl drw, ProMdl part, double x, double y)
{
    Pro3dPnt box[2];
    int el, er, eb, et;
    ProView view = find_view_by_key((ProDrawing)drw, 0);
    if (!view) { logf_("[tote] no main view"); return; }
    if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) { logf_("[tote] bbox failed"); return; }
    {
        int wid = -1;
        ProMdlDisplay(drw);
        ProMdlWindowGet(drw, &wid);
        if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
    }
    el = pick_extreme_edge((ProSolid)part, box, 2, 0, 1, 8.0);
    er = pick_extreme_edge((ProSolid)part, box, 2, 1, 1, 8.0);
    eb = pick_extreme_edge((ProSolid)part, box, 1, 0, 2, 20.0);
    et = pick_extreme_edge((ProSolid)part, box, 1, 1, 2, 20.0);
    logf_("[tote] edges L=%d R=%d B=%d T=%d", el, er, eb, et);
    if (el > 0 && er > 0) dim_two_edges(drw, part, view, el, er, 0, x, y, "len");
    if (eb > 0 && et > 0) dim_two_edges(drw, part, view, eb, et, 1, x - 95.0, y, "wid");
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* r153: read-only feature inventory (counts only, never touches the model) */
typedef struct { int total, datum, solid; } featn_t;
static ProError featn_visit(ProFeature *feat, ProError status, ProAppData app)
{
    featn_t *c = (featn_t *)app;
    ProFeattype t = (ProFeattype)0;
    if (!c || !feat) return PRO_TK_NO_ERROR;
    c->total++;
    if (ProFeatureTypeGet(feat, &t) == PRO_TK_NO_ERROR) {
        if (t == PRO_FEAT_DATUM || t == PRO_FEAT_DATUM_AXIS || t == PRO_FEAT_DATUM_POINT ||
            t == PRO_FEAT_DATUM_SURF || t == PRO_FEAT_DATUM_QUILT) c->datum++;
        else c->solid++;
    }
    return PRO_TK_NO_ERROR;
}

static void featn_run(ProMdl part, const wchar_t *tag)
{
    featn_t c;
    ProError ev;
    Pro3dPnt box[2];
    memset(&c, 0, sizeof(c));
    ev = ProSolidFeatVisit((ProSolid)part, featn_visit, NULL, &c);
    logf_("[featn] ==== '%ls' total=%d solid=%d datum=%d (visit=%d %s)",
          tag, c.total, c.solid, c.datum, (int)ev, errname(ev));
    if (ProSolidOutlineGet((ProSolid)part, box) == PRO_TK_NO_ERROR)
        logf_("[featn] bbox L=%.3f W=%.3f T=%.3f x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]",
              box[1][0] - box[0][0], box[1][1] - box[0][1], box[1][2] - box[0][2],
              box[0][0], box[1][0], box[0][1], box[1][1], box[0][2], box[1][2]);
    else
        logf_("[featn] bbox unavailable");
}

/* r153: put the displayed part into standard orientation, fit, repaint */
static void viewstd_run(ProMdl part, const wchar_t *tag)
{
    ProError er, ef, ew, ed;
    int wid = -1;
    ed = ProMdlDisplay(part);
    er = ProViewReset(part, NULL);
    if (er != PRO_TK_NO_ERROR) {
        ProView v = NULL;
        ProError ev = ProViewRetrieve(part, (wchar_t *)L"DEFAULT", &v);
        logf_("[viewstd] reset failed (%d); retrieve DEFAULT -> %d (%s)", (int)er, (int)ev, errname(ev));
    }
    ef = ProViewRefit(part, NULL);
    if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR && wid > 0) {
        ProWindowActivate(wid);
        ProWindowCurrentSet(wid);
    }
    ew = ProWindowRepaint(-1);
    logf_("[viewstd] '%ls' display=%d reset=%d refit=%d repaint=%d win=%d",
          tag, (int)ed, (int)er, (int)ef, (int)ew, wid);
}

/* r153: 3/4 view with the longest bbox axis horizontal on screen */
static void viewiso_run2(ProMdl part, const wchar_t *tag, int transp)
{
    Pro3dPnt box[2];
    double ext[3], vv[3], r[3], u[3];
    double n, dot;
    int i, la = 0, r1 = -1, r2 = -1, wa = 0, th = 0, wid = -1;
    ProMatrix m;
    ProError em, ef, ew;

    if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) {
        logf_("[viewiso] bbox failed");
        return;
    }
    for (i = 0; i < 3; i++) ext[i] = box[1][i] - box[0][i];
    for (i = 1; i < 3; i++) if (ext[i] > ext[la]) la = i;
    for (i = 0; i < 3; i++) {
        if (i == la) continue;
        if (r1 < 0) r1 = i; else r2 = i;
    }
    wa = (ext[r1] >= ext[r2]) ? r1 : r2;
    th = (wa == r1) ? r2 : r1;

    memset(vv, 0, sizeof(vv));
    vv[la] = 0.32; vv[wa] = 0.62; vv[th] = 0.72;
    n = sqrt(vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2]);
    for (i = 0; i < 3; i++) vv[i] /= n;

    dot = vv[la];
    memset(r, 0, sizeof(r));
    r[la] = 1.0;
    for (i = 0; i < 3; i++) r[i] -= dot * vv[i];
    n = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (i = 0; i < 3; i++) r[i] /= n;

    u[0] = vv[1] * r[2] - vv[2] * r[1];
    u[1] = vv[2] * r[0] - vv[0] * r[2];
    u[2] = vv[0] * r[1] - vv[1] * r[0];

    memset(m, 0, sizeof(m));
    for (i = 0; i < 3; i++) {
        if (transp) { m[i][0] = r[i]; m[i][1] = u[i]; m[i][2] = vv[i]; }
        else        { m[0][i] = r[i]; m[1][i] = u[i]; m[2][i] = vv[i]; }
    }
    m[3][3] = 1.0;

    em = ProViewMatrixSet(part, NULL, m);
    ef = ProViewRefit(part, NULL);
    if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR && wid > 0) {
        ProWindowActivate(wid);
        ProWindowCurrentSet(wid);
    }
    ew = ProWindowRepaint(-1);
    logf_("[viewiso] '%ls' transp=%d ext=%.2f/%.2f/%.2f la=%d wa=%d th=%d set=%d refit=%d repaint=%d win=%d",
          tag, transp, ext[0], ext[1], ext[2], la, wa, th, (int)em, (int)ef, (int)ew, wid);
}

/* r153: standard orientation, then level the longest axis and tilt to show thickness.
   sign=+1/-1 selects the ProViewRotate direction (calibrated at runtime). */
static void vshot_run(ProMdl part, const wchar_t *tag, int sign)
{
    Pro3dPnt box[2];
    ProMatrix m;
    double L[3], sx, sy, ang;
    int i, la = 0, wid = -1;
    ProError er, eg, rot1, rot2, ef, ew;

    if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) { logf_("[vshot] bbox fail"); return; }
    for (i = 1; i < 3; i++)
        if ((box[1][i] - box[0][i]) > (box[1][la] - box[0][la])) la = i;

    er = ProViewReset(part, NULL);
    eg = ProViewMatrixGet(part, NULL, m);
    memset(L, 0, sizeof(L)); L[la] = 1.0;
    sx = L[0] * m[0][0] + L[1] * m[0][1] + L[2] * m[0][2];
    sy = L[0] * m[1][0] + L[1] * m[1][1] + L[2] * m[1][2];
    ang = atan2(sy, sx) * 180.0 / 3.14159265358979323846;
    while (ang > 90.0) ang -= 180.0;
    while (ang < -90.0) ang += 180.0;
    rot1 = ProViewRotate(part, NULL, PRO_Z_ROTATION, -ang * sign);
    rot2 = ProViewRotate(part, NULL, PRO_X_ROTATION, 18.0);
    ef = ProViewRefit(part, NULL);
    if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR && wid > 0) {
        ProWindowActivate(wid);
        ProWindowCurrentSet(wid);
    }
    ew = ProWindowRepaint(-1);
    logf_("[vshot] '%ls' sign=%d la=%d ang=%.2f reset=%d matget=%d rotZ=%d rotX=%d refit=%d repaint=%d win=%d",
          tag, sign, la, ang, (int)er, (int)eg, (int)rot1, (int)rot2, (int)ef, (int)ew, wid);
}

/* r153: standard orientation, then level the longest axis in screen space by
   composing a screen rotation onto the real view matrix (Get -> rotate -> Set). */
static void vlevel_run(ProMdl part, const wchar_t *tag, double tilt_deg)
{
    Pro3dPnt box[2];
    ProMatrix m, rot, out;
    double L[3], sx, sy, ang, c, s, ct, st;
    int i, a, b, k, la = 0, wid = -1;
    ProError er, eg, es, ef, ew;

    if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) { logf_("[vlevel] bbox fail"); return; }
    for (i = 1; i < 3; i++)
        if ((box[1][i] - box[0][i]) > (box[1][la] - box[0][la])) la = i;

    er = ProViewReset(part, NULL);
    eg = ProViewMatrixGet(part, NULL, m);
    if (eg != PRO_TK_NO_ERROR) { logf_("[vlevel] matrixGet failed (%d)", (int)eg); return; }

    memset(L, 0, sizeof(L)); L[la] = 1.0;
    sx = L[0] * m[0][0] + L[1] * m[0][1] + L[2] * m[0][2];
    sy = L[0] * m[1][0] + L[1] * m[1][1] + L[2] * m[1][2];
    ang = atan2(sy, sx) * 180.0 / 3.14159265358979323846;
    while (ang > 90.0) ang -= 180.0;
    while (ang < -90.0) ang += 180.0;

    c = cos(-ang * 3.14159265358979323846 / 180.0);
    s = sin(-ang * 3.14159265358979323846 / 180.0);
    ct = cos(tilt_deg * 3.14159265358979323846 / 180.0);
    st = sin(tilt_deg * 3.14159265358979323846 / 180.0);

    /* rot = Rx(tilt) * Rz(-ang), both in screen space */
    memset(rot, 0, sizeof(rot));
    rot[0][0] = c;      rot[0][1] = -s;
    rot[1][0] = ct * s; rot[1][1] = ct * c; rot[1][2] = -st;
    rot[2][0] = st * s; rot[2][1] = st * c; rot[2][2] = ct;
    rot[3][3] = 1.0;

    memset(out, 0, sizeof(out));
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++) {
            double acc = 0.0;
            for (k = 0; k < 4; k++) acc += rot[a][k] * m[k][b];
            out[a][b] = acc;
        }
    /* ProViewMatrixSet rejects scaling/origin shift: strip the 4th column and
       renormalize each rotation row */
    for (a = 0; a < 3; a++) {
        double nl = sqrt(out[a][0] * out[a][0] + out[a][1] * out[a][1] + out[a][2] * out[a][2]);
        if (nl > 1e-12) { for (b = 0; b < 3; b++) out[a][b] /= nl; }
        out[a][3] = 0.0;
    }
    out[3][0] = out[3][1] = out[3][2] = 0.0;
    out[3][3] = 1.0;

    logf_("[vlevel] M0=[%.4f %.4f %.4f %.4f] M1=[%.4f %.4f %.4f %.4f] M2=[%.4f %.4f %.4f %.4f]",
          m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
          m[2][0], m[2][1], m[2][2], m[2][3]);

    es = ProViewMatrixSet(part, NULL, out);
    ef = ProViewRefit(part, NULL);
    if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR && wid > 0) {
        ProWindowActivate(wid);
        ProWindowCurrentSet(wid);
    }
    ew = ProWindowRepaint(-1);
    logf_("[vlevel] '%ls' la=%d ang=%.2f tilt=%.1f reset=%d get=%d set=%d refit=%d repaint=%d win=%d",
          tag, la, ang, tilt_deg, (int)er, (int)eg, (int)es, (int)ef, (int)ew, wid);
}

/* 2026-11-13 (006 asm drawing): <name>.prt.* / <name>.asm.* exists in the
   current directory?  Used by NATIVEDRW2 to pick the right model type. */
static int mdl_ver_exists(const wchar_t *name, const wchar_t *ext)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%ls.%ls.*", name, ext);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

static void run_cmd_channel(void)
{
    FILE  *f;
    char   buf[4096];
    size_t n;
    wchar_t w[2048];
    wchar_t *nl;

    f = _wfopen(cmd_file_path(), L"rb");
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n == 0) return;
    if (!u8_to_wide(buf, w, 2048)) return;
    nl = wcschr(w, L'\n');
    if (nl) *nl = 0;
    if (w[0] == 0 || !wcscmp(w, g_cmd_last)) return;   /* 同一命令不重复执�?*/
    wcscpy_s(g_cmd_last, 2048, w);
    logf_("[cmd] >> %ls", w);

    if (!wcsncmp(w, L"MACRO|", 6)) {
        run_macro_text(w + 6, "cmd-macro");
    } else if (!wcscmp(w, L"STAT")) {
        ProMdl cur = NULL;
        ProError ec = ProMdlCurrentGet(&cur);
        ProName nm; char c8[200];
        nm[0] = 0;
        if (ec == PRO_TK_NO_ERROR && cur) ProMdlNameGet(cur, nm);
        wide_to_u8(nm, c8, sizeof(c8));
        logf_("[cmd] STAT current='%s' (get -> %d %s)", c8, (int)ec, errname(ec));
        if (ec == PRO_TK_NO_ERROR && cur) {
            ProMdlType mt;
            ProMdlTypeGet(cur, &mt);
            logf_("[cmd] STAT type=%d", (int)mt);
            if (mt == PRO_MDL_PART || mt == PRO_MDL_ASSEMBLY) {
                ProLine *vns = NULL, *valt = NULL; int vc = 0, vi;
                Pro3dPnt box[2];
                if (ProViewNamesGet(cur, &vns, &valt, &vc) == PRO_TK_NO_ERROR) {
                    for (vi = 0; vi < vc; vi++) {
                        char v8[200]; wide_to_u8(vns[vi], v8, sizeof(v8));
                        logf_("[cmd] STAT view[%d]='%s'", vi, v8);
                    }
                    /* 不调�?ProWstringArrayFree（会触发 fatal error，见 ensure_front_view 注释�?*/
                }
                if (ProSolidOutlineGet((ProSolid)cur, box) == PRO_TK_NO_ERROR)
                    logf_("[cmd] STAT bbox %.2f x %.2f x %.2f",
                          box[1][0]-box[0][0], box[1][1]-box[0][1], box[1][2]-box[0][2]);
            }
        }
    } else if (!wcsncmp(w, L"LOADDRAW|", 9)) {
        /* r93: LOADDRAW|<dir>|<modelname> —�?从磁盘载入已保存图纸（不重建、不保存�?*/
        wchar_t *bar = wcschr(w + 9, L'|');
        if (!bar) {
            logf_("[load] bad args (need dir|name)");
        } else {
            ProPath dir;
            ProName nm;
            size_t dl = (size_t)(bar - (w + 9));
            ProError ed, er;
            if (dl >= PRO_PATH_SIZE) dl = PRO_PATH_SIZE - 1;
            wmemcpy(dir, w + 9, dl); dir[dl] = 0;
            wcsncpy_s(nm, PRO_NAME_SIZE, bar + 1, _TRUNCATE);
            ed = ProDirectoryChange(dir);
            logf_("[load] chdir '%ls' -> %d (%s)", dir, (int)ed, errname(ed));
            {
                ProMdl drw = NULL;
                er = ProMdlnameRetrieve(nm, PRO_MDLFILE_DRAWING, &drw);
                logf_("[load] retrieve '%ls' -> %d (%s)", nm, (int)er, errname(er));
                if (er == PRO_TK_NO_ERROR && drw) {
                    int wid = -1;
                    ProError edpy = ProMdlDisplay(drw);
                    ProError ew = ProWindowCurrentGet(&wid);
                    if (wid > 0) ProWindowActivate(wid);
                    wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nm, _TRUNCATE);
                    logf_("[load] display -> %d win=%d cur->%d; g_def_model='%ls'",
                          (int)edpy, wid, (int)ew, g_def_model);
                }
            }
        }
    } else if (!wcsncmp(w, L"SAVEAS|", 7)) {
        /* r92: ProMdlnameBackup(ProMdl handle, ProPath backup_dir) —�?handle 必须有效�?
           backup_dir 必须是【目录】。旧实现�?NULL+文件路径 �?-19 (OTHER)�?*/
        ProMdl drw = g_loaded_drw;
        ProError er = PRO_TK_NO_ERROR;
        if (!drw) er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] SAVEAS drw=%p retrieve('%ls') -> %d (%s)", (void *)drw, g_def_model, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            ProPath p;
            ProError eb;
            DWORD attrs;
            size_t plen;
            wcsncpy_s(p, PRO_PATH_SIZE, w + 7, _TRUNCATE);
            /* 去掉末尾多余的分隔符/空白（根目录 D:\ 保留�?*/
            plen = wcslen(p);
            while (plen > 3 && (p[plen-1] == L'\\' || p[plen-1] == L'/' || p[plen-1] == L' '))
                p[--plen] = 0;
            attrs = GetFileAttributesW(p);
            logf_("[cmd] SAVEAS dir='%ls' attrs=0x%08X exists=%d isdir=%d",
                 p, (unsigned)attrs, (attrs != INVALID_FILE_ATTRIBUTES),
                 ((attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0);
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                logf_("[cmd] SAVEAS ABORT: backup_dir is not an existing directory");
            } else {
                prep_save_regen();
                eb = ProMdlnameBackup(drw, p);
                logf_("[cmd] SAVEAS ProMdlnameBackup(drw,'%ls') -> %d (%s)", p, (int)eb, errname(eb));
            }
        }
    } else if (!wcsncmp(w, L"VIEWONLY|", 9)) {
        ProPath path;
        ProMdl drw = NULL;
        wcsncpy_s(path, PRO_PATH_SIZE, w + 9, _TRUNCATE);
        {
            ProError er = ProDrawingViewonlyOpen(path, &drw);
            logf_("[viewonly] '%ls' -> %d (%s) handle=%p", path, (int)er, errname(er), (void *)drw);
            if (er == PRO_TK_NO_ERROR && drw) {
                int wid = -1;
                ProError ed = ProMdlDisplay(drw);
                ProError ew = ProWindowCurrentGet(&wid);
                if (wid > 0) ProWindowActivate(wid);
                g_loaded_drw = drw;
                logf_("[viewonly] display=%d win=%d cur=%d", (int)ed, wid, (int)ew);
            }
        }
    } else if (!wcscmp(w, L"STATELOADED")) {
        logf_("[stateloaded] handle=%p", (void *)g_loaded_drw);
        if (g_loaded_drw) {
            ddims_run(g_loaded_drw);
            inc_state_views((ProDrawing)g_loaded_drw);
            inc_state_notes((ProDrawing)g_loaded_drw);
            inc_state_syms((ProDrawing)g_loaded_drw);
        }
    } else if (!wcsncmp(w, L"PDFLOADED|", 10)) {
        ProPath out;
        ProPDFOptions opts = NULL;
        wcsncpy_s(out, PRO_PATH_SIZE, w + 10, _TRUNCATE);
        logf_("[pdfloaded] handle=%p out='%ls'", (void *)g_loaded_drw, out);
        if (g_loaded_drw) {
            ProError ep;
            ProPDFoptionsAlloc(&opts);
            ep = ProPDFExport(g_loaded_drw, out, opts);
            logf_("[pdfloaded] export -> %d (%s)", (int)ep, errname(ep));
            if (opts) ProPDFoptionsFree(opts);
            dump_file_b64(out);
        }
    } else if (!wcscmp(w, L"STATECUR")) {
        ProMdl cur = NULL;
        ProError ec = ProMdlCurrentGet(&cur);
        logf_("[statecur] current -> %d (%s) handle=%p", (int)ec, errname(ec), (void *)cur);
        if (ec == PRO_TK_NO_ERROR && cur) {
            ProMdlfileType ft = PRO_MDLFILE_PART;
            ProMdlFiletypeGet(cur, &ft);
            logf_("[statecur] filetype=%d", (int)ft);
            if (ft == PRO_MDLFILE_DRAWING) {
                ddims_run(cur);
                inc_state_views((ProDrawing)cur);
                inc_state_notes((ProDrawing)cur);
                inc_state_syms((ProDrawing)cur);
            }
        }
    } else if (!wcsncmp(w, L"PDFCUR|", 7)) {
        ProMdl cur = NULL;
        ProError ec = ProMdlCurrentGet(&cur);
        ProPath out;
        ProPDFOptions opts = NULL;
        wcsncpy_s(out, PRO_PATH_SIZE, w + 7, _TRUNCATE);
        logf_("[pdfcur] current -> %d (%s) handle=%p out='%ls'", (int)ec, errname(ec), (void *)cur, out);
        if (ec == PRO_TK_NO_ERROR && cur) {
            ProError ep;
            ProPDFoptionsAlloc(&opts);
            ep = ProPDFExport(cur, out, opts);
            logf_("[pdfcur] export -> %d (%s)", (int)ep, errname(ep));
            if (opts) ProPDFoptionsFree(opts);
            dump_file_b64(out);
        }
    } else if (!wcscmp(w, L"MACRODIAG")) {
        /* r118: diagnose runtime SDCurrentDrawing -26 with full window/model state */
        ProMdl part = NULL, wm = NULL, cur = NULL;
        ProName nm;
        int wid = -1;
        ProError ep = retrieve_model_any(g_def_model, &part);
        logf_("[mdiag] part retrieve('%ls') -> %d part=%p", g_def_model, (int)ep, (void *)part);
        if (ep == PRO_TK_NO_ERROR && part) {
            ProError ed = ProMdlDisplay(part);
            ProError ew = ProWindowCurrentGet(&wid);
            ProError ea = (wid > 0) ? ProWindowActivate(wid) : PRO_TK_GENERAL_ERROR;
            ProError es = (wid > 0) ? ProWindowCurrentSet(wid) : PRO_TK_GENERAL_ERROR;
            logf_("[mdiag] display=%d winGet=%d win=%d activate=%d currentSet=%d",
                  (int)ed, (int)ew, wid, (int)ea, (int)es);
            nm[0] = 0;
            if (wid > 0 && ProWindowMdlGet(wid, &wm) == PRO_TK_NO_ERROR && wm) ProMdlNameGet(wm, nm);
            logf_("[mdiag] window model='%ls' handle=%p", nm, (void *)wm);
            nm[0] = 0;
            { ProError ec = ProMdlCurrentGet(&cur); if (ec == PRO_TK_NO_ERROR && cur) ProMdlNameGet(cur, nm);
              logf_("[mdiag] current model get=%d name='%ls' handle=%p", (int)ec, nm, (void *)cur); }
            {
                ProError l1 = ProMacroLoad(L"~ Command `SDCurrentDrawing`;");
                ProError x1 = ProMacroExecute();
                logf_("[mdiag] A plain: load=%d exec=%d", (int)l1, (int)x1);
            }
            if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
            {
                ProError l2 = ProMacroLoad(L"~ Command `SDCurrentDrawing`;");
                ProError x2 = ProMacroExecute();
                logf_("[mdiag] B after windowSet: load=%d exec=%d", (int)l2, (int)x2);
            }
            {
                ProError l3 = ProMacroLoad(L"~ Command `SDStdDrawing`;");
                ProError x3 = ProMacroExecute();
                logf_("[mdiag] E SDStdDrawing: load=%d exec=%d", (int)l3, (int)x3);
                ProError l4 = ProMacroLoad(L"~ Command `SDCustomDrawing`;");
                ProError x4 = ProMacroExecute();
                logf_("[mdiag] E SDCustomDrawing: load=%d exec=%d", (int)l4, (int)x4);
            }
        }
    } else if (!wcsncmp(w, L"LOADPART|", 9)) {
        /* r117: LOADPART|<dir>|<name> �?open the part after a clean Creo start */
        wchar_t *bar = wcschr(w + 9, L'|');
        if (bar) {
            ProPath dir;
            ProName nm;
            ProMdl part = NULL;
            size_t dl = (size_t)(bar - (w + 9));
            if (dl >= PRO_PATH_SIZE) dl = PRO_PATH_SIZE - 1;
            wmemcpy(dir, w + 9, dl); dir[dl] = 0;
            wcsncpy_s(nm, PRO_NAME_SIZE, bar + 1, _TRUNCATE);
            {
                ProError ed = ProDirectoryChange(dir);
                ProError er = ProMdlnameRetrieve(nm, PRO_MDLFILE_PART, &part);
                logf_("[loadpart] chdir '%ls' -> %d (%s); retrieve '%ls' -> %d (%s) part=%p",
                      dir, (int)ed, errname(ed), nm, (int)er, errname(er), (void *)part);
                if (er == PRO_TK_NO_ERROR && part) {
                    int wid = -1;
                    ProError edp = ProMdlDisplay(part);
                    ProError ews = PRO_TK_NO_ERROR;
                    ProWindowCurrentGet(&wid);
                    if (wid > 0) {
                        ProWindowActivate(wid);
                        ews = ProWindowCurrentSet(wid);
                    }
                    wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nm, _TRUNCATE);
                    logf_("[loadpart] display=%d win=%d currentSet=%d g_def_model='%ls'",
                          (int)edp, wid, (int)ews, g_def_model);
                }
            }
        }
    } else if (!wcsncmp(w, L"NEWDRW|", 7)) {
        wchar_t nn[64], tn[64], pn[64];
        if (swscanf_s(w + 7, L"%63[^|]|%63[^|]|%63s", nn, 64, tn, 64, pn, 64) == 3) {
            ProMdlnameShortdata nd;
            ProDrawing ndrw = NULL;
            ProError ec;
            wcsncpy_s(nd.name, PRO_NAME_SIZE, pn, _TRUNCATE);
            wcscpy_s(nd.type, PRO_MDLEXTENSION_SIZE, L"prt");
            ec = ProDrawingFromTemplateCreate(nn, tn, &nd, PRODWGCREATE_DISPLAY_DRAWING, &ndrw, NULL);
            logf_("[newdrw] create('%ls','%ls','%ls') -> %d (%s) drw=%p",
                  nn, tn, pn, (int)ec, errname(ec), (void *)ndrw);
            if (ec == PRO_TK_NO_ERROR && ndrw) {
                int wid = -1;
                ProMdlDisplay((ProMdl)ndrw);
                ProWindowCurrentGet(&wid);
                if (wid > 0) ProWindowActivate(wid);
                wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nn, _TRUNCATE);
                g_loaded_drw = (ProMdl)ndrw;
            }
        } else logf_("[newdrw] bad args");    } else if (!wcsncmp(w, L"NATIVEDRW2|", 11)) {
        /* r112: successor ProDrawingFromTemplateCreate with ProMdlnameShortdata
           2026-11-13: optional 4th field <prt|asm>; empty = auto-detect on disk */
        wchar_t nn[64], tn[64], pn[64], ty[8];
        ty[0] = 0;
        if (swscanf_s(w + 11, L"%63[^|]|%63[^|]|%63[^|]|%7s", nn, 64, tn, 64, pn, 64, ty, 8) >= 2) {
            ProMdlnameShortdata nd;
            ProDrawing drw = NULL;
            wcsncpy_s(nd.name, PRO_NAME_SIZE, pn, _TRUNCATE);
            if (!_wcsicmp(ty, L"asm"))      wcscpy_s(nd.type, PRO_MDLEXTENSION_SIZE, L"asm");
            else if (!_wcsicmp(ty, L"prt")) wcscpy_s(nd.type, PRO_MDLEXTENSION_SIZE, L"prt");
            else if (mdl_ver_exists(pn, L"asm") && !mdl_ver_exists(pn, L"prt"))
                                            wcscpy_s(nd.type, PRO_MDLEXTENSION_SIZE, L"asm");
            else                            wcscpy_s(nd.type, PRO_MDLEXTENSION_SIZE, L"prt");
            {
                ProError ec = ProDrawingFromTemplateCreate(nn, tn, &nd, PRODWGCREATE_DISPLAY_DRAWING, &drw, NULL);
                logf_("[native2] part='%ls' type='%ls'; create('%ls','%ls') -> %d (%s) drw=%p",
                      nd.name, nd.type, nn, tn, (int)ec, errname(ec), (void *)drw);
                if (ec == PRO_TK_NO_ERROR && drw) {
                    int wid = -1;
                    ProMdlDisplay((ProMdl)drw);
                    ProWindowCurrentGet(&wid);
                    if (wid > 0) ProWindowActivate(wid);
                    wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nn, _TRUNCATE);
                    g_loaded_drw = (ProMdl)drw;
                }
            }
        } else logf_("[native2] bad args");
    } else if (!wcsncmp(w, L"NATIVEDRW|", 10)) {
        /* r112: NATIVEDRW|<newname>|<template>|<part> �?native drawing creation (no SDCurrentDrawing) */
        wchar_t nn[64], tn[64], pn[64];
        if (swscanf_s(w + 10, L"%63[^|]|%63[^|]|%63s", nn, 64, tn, 64, pn, 64) >= 2) {
            ProMdl part = NULL;
            ProModel mdl;
            ProDrawing drw = NULL;
            ProError ep = ProMdlnameRetrieve(pn, PRO_MDLFILE_PART, &part);
            wcsncpy_s(mdl.name, 80, pn, _TRUNCATE);
            wcscpy_s(mdl.type, 10, L"part");
            {
                ProError ec = ProDrawingFromTmpltCreate(nn, tn, &mdl, PRODWGCREATE_DISPLAY_DRAWING, &drw, NULL);
                logf_("[native] part='%ls' retrieve=%d; create('%ls','%ls') -> %d (%s) drw=%p",
                      pn, (int)ep, nn, tn, (int)ec, errname(ec), (void *)drw);
                if (ec == PRO_TK_NO_ERROR && drw) {
                    int wid = -1;
                    ProMdlDisplay((ProMdl)drw);
                    ProWindowCurrentGet(&wid);
                    if (wid > 0) ProWindowActivate(wid);
                    wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nn, _TRUNCATE);
                    g_loaded_drw = (ProMdl)drw;
                }
            }
        } else {
            logf_("[native] bad args");
        }
    } else if (!wcsncmp(w, L"LOADFILE|", 9)) {
        /* r95: 按完整路径加载已保存图纸（不重建、不保存�?*/
        ProPath path;
        ProMdl drw = NULL;
        wcsncpy_s(path, PRO_PATH_SIZE, w + 9, _TRUNCATE);
        {
            ProError er = ProMdlFiletypeLoad(path, PRO_MDLFILE_DRAWING, PRO_B_FALSE, &drw);
            logf_("[loadfile] '%ls' -> %d (%s) handle=%p", path, (int)er, errname(er), (void *)drw);
            if (er == PRO_TK_NO_ERROR && drw) {
                ProName nm;
                int wid = -1;
                g_loaded_drw = drw;
                ProError ed, ew;
                nm[0] = 0;
                ProMdlMdlnameGet(drw, nm);
                ed = ProMdlDisplay(drw);
                ew = ProWindowCurrentGet(&wid);
                if (wid > 0) ProWindowActivate(wid);
                wcsncpy_s(g_def_model, PRO_NAME_SIZE + 8, nm, _TRUNCATE);
                logf_("[loadfile] name='%ls' display=%d win=%d cur=%d", nm, (int)ed, wid, (int)ew);
            }
        }
    } else if (!wcscmp(w, L"ERASEDRW")) {
        /* r95: 关闭/擦除会话里的空白图，便于从磁盘按原名加载成品 drw */
        ProMdl drw = g_loaded_drw;   /* 2026-11-13: LOADFILE 设的句柄优先（装配图靠它） */
          ProError er = PRO_TK_NO_ERROR;
          if (!drw) {
              er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
              logf_("[savethread] fallback retrieve('%ls') -> %d (%s) handle=%p", g_def_model, (int)er, errname(er), (void *)drw);
          } else {
              ProName dn; dn[0] = 0; ProMdlNameGet(drw, dn);
              logf_("[savethread] use g_loaded_drw '%ls' handle=%p", dn, (void *)drw);
          }
        logf_("[erase] retrieve('%ls') -> %d (%s)", g_def_model, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            int wid = -1;
            ProError ew = ProWindowCurrentGet(&wid);
            ProError ee = ProMdlErase(drw);
            logf_("[erase] window=%d current->%d ProMdlErase -> %d (%s)", wid, (int)ew, (int)ee, errname(ee));
        }
    } else if (!wcscmp(w, L"SAVETHREAD") || !wcsncmp(w, L"SAVEBACKTHREAD|", 15)) {
        /* r105: call the save API on a worker thread; the main thread keeps pumping messages */
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[savethread] retrieve('%ls') -> %d (%s) handle=%p", g_def_model, (int)er, errname(er), (void *)drw);
        if (er == PRO_TK_NO_ERROR && drw) {
            if (!g_save_evt) g_save_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
            ResetEvent(g_save_evt);
            if (!wcscmp(w, L"SAVETHREAD")) {
                HANDLE th = CreateThread(NULL, 0, save_thread_proc, (LPVOID)drw, 0, NULL);
                logf_("[savethread] ProMdlSave thread=%p started", (void *)th);
                if (th) CloseHandle(th);
            } else {
                wcsncpy_s(g_save_dir, PRO_PATH_SIZE, w + 15, _TRUNCATE);
                logf_("[savethread] backup dir='%ls'", g_save_dir);
                {
                    HANDLE th = CreateThread(NULL, 0, saveback_thread_proc, (LPVOID)drw, 0, NULL);
                    logf_("[savethread] ProMdlnameBackup thread=%p started", (void *)th);
                    if (th) CloseHandle(th);
                }
            }
        }
    } else if (!wcsncmp(w, L"SAVECUR", 7)) {
        /* r133: 只保存 LOADFILE 精确打开的那份图（g_loaded_drw），
           绕开 ProMdlnameRetrieve 走 search_path 解析（同名图会命中别的目录）。
           2026-11-04: R7 FIX - the old code unconditionally did
             ProDirectoryChange(L"D:\\J26060131\\X6 2P 0810")
           which wrote the drawing into the USER's directory. Save into the current
           working directory instead; SAVECUR|<dir> may name a target explicitly. */
        ProMdl drw = g_loaded_drw;
        if (!drw) {
            ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
            logf_("[savecur] g_loaded_drw NULL; retrieve('%ls') -> %d (%s)", g_def_model, (int)er, errname(er));
        }
        if (drw) {
            ProName nm;
            ProPath olddir, newdir;
            ProError es, ed1, ed2;

            olddir[0] = 0; newdir[0] = 0;
            ed1 = ProDirectoryCurrentGet(olddir);
            ed2 = PRO_TK_GENERAL_ERROR;
            if (w[7] == L'|') {
                wcsncpy_s(newdir, PRO_PATH_SIZE, w + 8, _TRUNCATE);
                ed2 = ProDirectoryChange(newdir);
            }
            logf_("[savecur] cwd='%ls' (get=%d) target='%ls' chdir=%d", olddir, (int)ed1, newdir, (int)ed2);
            nm[0] = 0;
            ProMdlMdlnameGet(drw, nm);
            prep_save_regen();
            logf_("[savecur] '%ls' sheetRegen -> %d", nm, (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
            es = ProMdlSave(drw);
            logf_("[savecur] ProMdlSave('%ls') -> %d (%s)", nm, (int)es, errname(es));
            pdf_gate_log("SAVECUR");
        } else logf_("[savecur] no drawing handle");
    } else if (!wcscmp(w, L"SAVEDRW")) {
        /* r92: 直接保存图纸（prep regen �?ProMdlSave），不做依赖备份
           r133: 优先保存 LOADFILE 精确加载的图，避�?search_path 同名图误�?*/
        ProMdl drw = g_loaded_drw;
        ProError er = PRO_TK_NO_ERROR;
        if (!drw) er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] SAVEDRW retrieve('%ls') -> %d (%s) src=%s", g_def_model, (int)er, errname(er),
              g_loaded_drw ? "loaded" : "byname");
        if (er == PRO_TK_NO_ERROR && drw) {
            ProError es;
            prep_save_regen();
            logf_("[cmd] SAVEDRW sheetRegen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
            es = ProMdlSave(drw);
            logf_("[cmd] SAVEDRW ProMdlSave -> %d (%s)", (int)es, errname(es));
            pdf_gate_log("SAVEDRW");
        }
    } else if (!wcscmp(w, L"SAVE")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] SAVE retrieve('%ls') -> %d (%s)", g_def_model, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            ProError eb;
            ProPath workdir;
            /* r89: 先清 part �?pending regen，避�?ProMdlSave 死等（改尺寸属性后实测�?*/
            prep_save_regen();
            logf_("[cmd] SAVE sheetRegen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
            /* r91: 先走 Backup（不触发 ProMdlSave 死锁），成功即视为已落盘 */
            wcsncpy_s(workdir, PRO_PATH_SIZE, L"D:\\creo-agent\\sync-toolkit\\work", _TRUNCATE);
            eb = ProMdlnameBackup(drw, workdir);
            logf_("[cmd] SAVE ProMdlnameBackup(work) -> %d (%s)", (int)eb, errname(eb));
            if (eb != PRO_TK_NO_ERROR) {
                ProError es = ProMdlSave(drw);
                logf_("[cmd] SAVE ProMdlSave -> %d (%s)", (int)es, errname(es));
            } else {
                logf_("[cmd] SAVE backup ok; ProMdlSave skipped (known deadlock after dim attrs)");
            }
            /* 轮次54：SAVE 里附带的 PDF 导出自校已停用（原来固定 export_pdf_model(drw, g_def_model)�?*/
            pdf_gate_log("SAVE");
        }
    } else if (!wcscmp(w, L"DELIVER")) {
        g_deliver_ok = 1;
        logf_("[pdfgate] DELIVER: PDF export ENABLED (DSH said 'deliver')");
    } else if (!wcscmp(w, L"PDFDUMP")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] PDFDUMP retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            if (!g_deliver_ok) {
                pdf_gate_log("PDFDUMP");      /* 轮次54：自校不�?PDF（用 grab_creo_window.ps1 截图�?*/
            } else {
                /* export to a private path (the project-dir PDF may be locked by a
                   viewer the user has open, which silently aborts ProPDFExport).
                   unique name per call: the previous live pdf may also be open. */
                ProPDFOptions opts = NULL;
                ProPath       out;
                ProError      ep;
                SYSTEMTIME    st2;
                GetLocalTime(&st2);
                swprintf_s(out, PRO_PATH_SIZE,
                           L"D:\\creo-agent\\sync-toolkit\\work\\pdf_live_%02u%02u%02u.pdf",
                           (unsigned)st2.wHour, (unsigned)st2.wMinute, (unsigned)st2.wSecond);
                ProPDFoptionsAlloc(&opts);
                ep = ProPDFExport(drw, out, opts);
                logf_("[cmd] PDFDUMP export(live) -> %d (%s)", (int)ep, errname(ep));
                if (opts) ProPDFoptionsFree(opts);
                dump_file_b64(out);
            }
        }
    } else if (!wcsncmp(w, L"NOTEAT|", 7)) {
        /* NOTEAT|x|y|text：在 (x,y) 建自由单行纯文本注释 */
        double nx = 0.0, ny = 0.0;
        const wchar_t *p = w + 7, *t = NULL;
        int bars = 0;

        if (swscanf_s(w + 7, L"%lf|%lf", &nx, &ny) == 2) {
            for (; *p; p++) {
                if (*p == L'|') { bars++; if (bars == 2) { t = p + 1; break; } }
            }
            {
                ProMdl d = inc_drw();

                if (t && *t && d) noteat_run(d, nx, ny, t);
                else logf_("[noteat] bad args / no drawing");
            }
        } else logf_("[noteat] bad args");
    } else if (!wcsncmp(w, L"MOVENOTE|", 9)) {
        /* MOVENOTE|<id>|<x>|<y>：把已有注记的 FREE 附着挪到纸面 (x,y)。
           2026-11-05: 图框里 '***' 内建注记删不掉(ProDtlnoteDelete -> -2)，
           注释又落不到正确位置，改用 隐藏原注记 + 原位建新注释 的办法。 */
        int    mid = -1;
        double mx = 0.0, my = 0.0;
        if (swscanf_s(w + 9, L"%d|%lf|%lf", &mid, &mx, &my) == 3) {
            ProMdl d = inc_drw();
            if (d) note_move_run(d, mid, mx, my);
            else logf_("[movenote] no drawing");
        } else logf_("[movenote] bad args");
    } else if (!wcscmp(w, L"DIAG")) {
        diag_notes();
    } else if (!wcscmp(w, L"DIMFIX")) {
        ProMdl drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            dimfix_run(drw);
        else
            logf_("[dimfix] drawing retrieve failed");
    } else if (!wcscmp(w, L"CROSSDELSOLID")) {
        /* r127: delete only SOLIDFONT draft lines of cross length (~34), never frame/title block */
        ProMdl drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            ProDtlentity *ents = NULL;
            int cnt = 0, i, deleted = 0;
            if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) == PRO_TK_NO_ERROR && ents) {
                ProArraySizeGet((ProArray)ents, &cnt);
                for (i = 0; i < cnt; i++) {
                    ProDtlentitydata d = NULL;
                    ProName fnt;
                    ProCurvedata cd;
                    int id = -1;
                    double w = -1.0;
                    if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
                    ProDtlentitydataIdGet(d, &id);
                    fnt[0] = 0;
                    ProDtlentitydataFontGet(d, fnt);
                    ProDtlentitydataWidthGet(d, &w);
                    memset(&cd, 0, sizeof(cd));
                    if (ProDtlentitydataCurveGet(d, &cd) == PRO_TK_NO_ERROR && cd.line.type == PRO_ENT_LINE) {
                        double dx = cd.line.end2[0] - cd.line.end1[0];
                        double dy = cd.line.end2[1] - cd.line.end1[1];
                        double len = sqrt(dx * dx + dy * dy);
                        if (_wcsicmp(fnt, L"SOLIDFONT") == 0 && len > 33.0 && len < 35.0) {
                            logf_("[crossdel] delete id=%d font='%ls' width=%.3f line (%.1f,%.1f)-(%.1f,%.1f) len=%.1f",
                                  id, fnt, w, cd.line.end1[0], cd.line.end1[1], cd.line.end2[0], cd.line.end2[1], len);
                            if (ProDtlentityDelete(&ents[i], NULL) == PRO_TK_NO_ERROR) deleted++;
                        }
                    }
                    ProDtlentitydataFree(d);
                }
                ProArrayFree((ProArray *)&ents);
                logf_("[crossdel] deleted=%d/%d", deleted, cnt);
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
            } else {
                logf_("[crossdel] collect failed");
            }
        }
    } else if (!wcscmp(w, L"CROSSINFO")) {
        /* 轮次55：列出图�?draft 实体（十字中心线）：id/视图/端点 */
        ProMdl d2 = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &d2) == PRO_TK_NO_ERROR && d2) crossinfo_run(d2);
        else logf_("[cross] drw retrieve failed");
    } else if (!wcsncmp(w, L"DATTACH|", 8)) {
        /* 轮次55：DATTACH|<�? —�?读回该尺寸的附件（类�?id/选取点） */
        double v2 = 0.0;
        ProMdl d2 = inc_drw();
        if (swscanf_s(w + 8, L"%lf", &v2) == 1 && d2)
            dattach_run(d2, v2);
        else logf_("[dattach] bad args");
    } else if (!wcscmp(w, L"DIMS")) {
        dims_report();
    } else if (!wcsncmp(w, L"DTOL|", 5)) {
        /* DTOL|id|ty|up|lo：给图纸尺寸设公差（图纸尺寸专用�?*/
        int tid = 0, tty = 4, tdec = 1;
        double tup = 0.0, tlo = 0.0;
        ProMdl td = inc_drw();
        if (swscanf_s(w + 5, L"%d|%d|%lf|%lf", &tid, &tty, &tup, &tlo) == 4 && td) {
            {   /* optional 5th field = tolerance decimal places */
                const wchar_t *p = w + 5;
                int bar = 0;
                for (; *p; p++) if (*p == L'|') { bar++; if (bar == 4) { tdec = _wtoi(p + 1); break; } }
            }
            dtol_run(td, tid, tty, tup, tlo, tdec);
        } else
            logf_("[dtol] bad args");
    } else if (!wcsncmp(w, L"LEADINFO|", 9)) {
        int lid = 0;
        ProMdl ld = inc_drw();
        if (swscanf_s(w + 9, L"%d", &lid) == 1 && ld) leadinfo_run(ld, lid);
        else logf_("[leadinfo] bad args");
    } else if (!wcsncmp(w, L"MOVED|", 6)) {
        /* MOVED|id|x|y：按图纸尺寸 id 精确移动（ProDrawingDimensionMove，绝对位置） */
        int mid = 0;
        double mx = 0.0, my = 0.0;
        ProMdl md = inc_drw();
        if (swscanf_s(w + 6, L"%d|%lf|%lf", &mid, &mx, &my) == 3 && md)
            moved_run(md, mid, mx, my);
        else
            logf_("[moved] bad args");
    } else if (!wcsncmp(w, L"DMOVE|", 6)) {
        /* DMOVE|value|x|y：移动指定值尺寸的文字，避免与其它尺寸文字/界线碰撞 */
        double dv = 0.0, dx = 0.0, dy = 0.0;
        ProMdl dm = inc_drw();
        if (swscanf_s(w + 6, L"%lf|%lf|%lf", &dv, &dx, &dy) == 3 && dm)
            dmove_run(dm, dv, dx, dy);
        else
            logf_("[dmove] bad args");
    } else if (!wcscmp(w, L"DDIMS")) {
        /* DDIMS：只读转�?存在图纸里的尺寸"（用户手�?+ 图纸侧创建） */
        ProMdl drw = inc_drw();
        if (drw)
            ddims_run(drw);
        else
            logf_("[ddims] drawing retrieve failed");
    } else if (!wcscmp(w, L"STATE")) {
        /* STATE：增量模式全量状态（视图/尺寸/注释/符号），只读 */
        inc_state_run();
    } else if (!wcsncmp(w, L"DELSYM|", 7)) {
        inc_delsym_run(w + 7);
    } else if (!wcsncmp(w, L"DELNOTE|", 8)) {
        inc_delnote_run(w + 8);
    } else if (!wcsncmp(w, L"HIDENOTE|", 9)) {
        inc_hidenote_run(w + 9);
    } else if (!wcsncmp(w, L"DELVIEW|", 8)) {
        inc_delview_run(w + 8);
    } else if (!wcsncmp(w, L"DELDIM|", 7)) {
        if (!_wcsicmp(w + 7, L"ALL")) inc_deldim_all_run();
        else inc_deldim_run(_wtoi(w + 7));
    } else if (!wcscmp(w, L"AFIND")) {
        /* AFIND：只读转储模型注解元素（3D 注释，含可能的尺寸注释） */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            afind_run(part);
        else
            logf_("[afind] part retrieve failed");
    } else if (!wcsncmp(w, L"ARROWS|", 7)) {
        /* ARROWS|<�?|<�?：只在【本张图纸】上设置箭头尺寸（ProDrawingSetupOption*）—�?
           �?严禁动全局 metric.dtl。ProLine 是宽字符串（wchar_t[81]）。读回留证�?*/
        double alen = 0.0, awid = 0.0;
        if (swscanf_s(w + 7, L"%lf|%lf", &alen, &awid) == 2) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                ProName opt;
                ProLine val;
                char    v8[160];

                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_length");
                {
                    ProError eg;
                    val[0] = 0;
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, val);
                    v8[0] = 0; wide_to_u8(val, v8, sizeof(v8));
                    logf_("[arrow] BEFORE draw_arrow_length = [%s] (get->%d)", v8, (int)eg);
                }
                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_width");
                {
                    ProError eg;
                    val[0] = 0;
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, val);
                    v8[0] = 0; wide_to_u8(val, v8, sizeof(v8));
                    logf_("[arrow] BEFORE draw_arrow_width = [%s] (get->%d)", v8, (int)eg);
                }
                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_length");
                swprintf_s(val, PRO_LINE_SIZE, L"%.2f", alen);
                {
                    ProError es = ProDrawingSetupOptionSet((ProDrawing)drw, opt, val);
                    logf_("[arrow] SET draw_arrow_length=%.2f -> %d (%s)", alen, (int)es, errname(es));
                }
                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_width");
                swprintf_s(val, PRO_LINE_SIZE, L"%.2f", awid);
                {
                    ProError es = ProDrawingSetupOptionSet((ProDrawing)drw, opt, val);
                    logf_("[arrow] SET draw_arrow_width=%.2f -> %d (%s)", awid, (int)es, errname(es));
                }
                {
                    ProError er2 = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[arrow] regen -> %d (%s)", (int)er2, errname(er2));
                }
                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_length");
                {
                    ProError eg;
                    val[0] = 0;
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, val);
                    v8[0] = 0; wide_to_u8(val, v8, sizeof(v8));
                    logf_("[arrow] AFTER  draw_arrow_length = [%s] (get->%d)", v8, (int)eg);
                }
                wcscpy_s(opt, PRO_NAME_SIZE, L"draw_arrow_width");
                {
                    ProError eg;
                    val[0] = 0;
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, val);
                    v8[0] = 0; wide_to_u8(val, v8, sizeof(v8));
                    logf_("[arrow] AFTER  draw_arrow_width = [%s] (get->%d)", v8, (int)eg);
                }
            } else {
                logf_("[arrow] drawing retrieve failed");
            }
        } else {
            logf_("[arrow] bad args");
        }
    } else if (!wcsncmp(w, L"DTLSET|", 7)) {
        /* DTLSET|<dtl选项>|<�?：单图纸设置 + 前后读回（⛔ 严禁改全局 metric.dtl）�?
           例：DTLSET|clip_dim_arrow_style|DOUBLE_ARROW */
        const wchar_t *p = w + 7;
        const wchar_t *bar = wcsrchr(p, L'|');
        if (bar && bar > p) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                ProName opt;
                ProLine val, oldv;
                char    o8[160], v8[160];
                int     n = (int)(bar - p);

                if (n > PRO_NAME_SIZE - 1) n = PRO_NAME_SIZE - 1;
                wcsncpy_s(opt, PRO_NAME_SIZE, p, n);
                opt[n] = 0;
                o8[0] = 0; wide_to_u8(opt, o8, sizeof(o8));
                {
                    ProError eg;
                    wcscpy_s(oldv, PRO_LINE_SIZE, bar + 1);
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, oldv);
                    v8[0] = 0; wide_to_u8(oldv, v8, sizeof(v8));
                    logf_("[dtl] %s BEFORE = [%s] (get->%d)", o8, v8, (int)eg);
                }
                wcscpy_s(val, PRO_LINE_SIZE, bar + 1);
                {
                    ProError es = ProDrawingSetupOptionSet((ProDrawing)drw, opt, val);
                    logf_("[dtl] %s SET -> %d (%s)", o8, (int)es, errname(es));
                }
                {
                    ProError er2 = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[dtl] regen -> %d (%s)", (int)er2, errname(er2));
                }
                {
                    ProError eg;
                    oldv[0] = 0;
                    eg = ProDrawingSetupOptionGet((ProDrawing)drw, opt, oldv);
                    v8[0] = 0; wide_to_u8(oldv, v8, sizeof(v8));
                    logf_("[dtl] %s AFTER  = [%s] (get->%d)", o8, v8, (int)eg);
                }
            } else {
                logf_("[dtl] drawing retrieve failed");
            }
        } else {
            logf_("[dtl] bad args");
        }
    } else if (!wcsncmp(w, L"TANEDGE", 7)) {
        /* TANEDGE[|solid]: set tangent-edge display of EXISTING views to solid.
           2026-10-29: dtl option tan_edge_display_for_new_views only affects views
           created AFTER the option is set; the quick-draw three views are already
           built, so it cannot touch them. Deleting and rebuilding the views loses
           the phase2 output (shown dims / tech notes / centerlines). Use
           ProDrawingViewDisplayGet/Set on each view instead.
           TANEDGE        -> report each view's current values
           TANEDGE|solid  -> set PRO_TANEDGE_SOLID on all views + regen + repaint */
        int want_solid = (w[7] == L'|' && (w[8] == L's' || w[8] == L'S'));
        ProMdl drw = inc_drw();
        if (!drw) {
            logf_("[tanedge] no loaded drawing");
        } else {
            ProView *views = NULL;
            if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
                logf_("[tanedge] viewsCollect failed");
            } else {
                int i, vc = 0, changed = 0;
                ProArraySizeGet((ProArray)views, &vc);
                logf_("[tanedge] views=%d mode=%s", vc, want_solid ? "SET-SOLID" : "REPORT");
                for (i = 0; i < vc; i++) {
                    ProName nm; char n8[128];
                    ProDrawingViewDisplay dp;
                    ProError eg;
                    memset(&dp, 0, sizeof(dp));
                    nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                    wide_to_u8(nm, n8, sizeof(n8));
                    eg = ProDrawingViewDisplayGet((ProDrawing)drw, views[i], &dp);
                    if (eg == PRO_TK_NO_ERROR) {
                        logf_("[tanedge] #%d '%s' tan=%d style=%d quilt=%d", i, n8,
                              (int)dp.tangent_edge_display, (int)dp.style, (int)dp.quilt_hlr);
                        if (want_solid) {
                            ProError es;
                            dp.tangent_edge_display = PRO_TANEDGE_SOLID;
                            es = ProDrawingViewDisplaySet((ProDrawing)drw, views[i], &dp);
                            logf_("[tanedge]   set -> %d (%s)", (int)es, errname(es));
                            if (es == PRO_TK_NO_ERROR) changed++;
                        }
                    } else {
                        logf_("[tanedge] #%d '%s' Get -> %d (%s)", i, n8, (int)eg, errname(eg));
                    }
                }
                if (want_solid && changed) {
                    ProError er = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[tanedge] regen -> %d (%s)", (int)er, errname(er));
                }
                logf_("[tanedge] done changed=%d/%d", changed, vc);
                ProArrayFree((ProArray *)&views);
            }
        }
    } else if (!wcsncmp(w, L"FDIM|", 5)) {
        /* FDIM|idA|idB|x|y|O|V ：直选两个面建尺寸（R3）。O: H/V；V: F=主视�?/ S=侧视�?*/
        int    fida = 0, fidb = 0;
        double fx = 0.0, fy = 0.0;
        wchar_t oc = L'H', vc = L'F';

        if (swscanf_s(w + 5, L"%d|%d|%lf|%lf", &fida, &fidb, &fx, &fy) == 4) {
            const wchar_t *p = w + 5;
            int            bar = 0;
            const wchar_t *f5 = NULL, *f6 = NULL;

            for (; *p; p++) {
                if (*p == L'|') {
                    bar++;
                    if (bar == 4) f5 = p + 1;
                    if (bar == 5) f6 = p + 1;
                }
            }
            if (f5) oc = f5[0];
            if (f6) vc = f6[0];
            {
                ProMdl part = NULL, drw = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                    ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                    fdim_run(part, drw, fida, fidb, fx, fy, oc, vc);
                else
                    logf_("[fdim] retrieve failed");
            }
        } else {
            logf_("[fdim] bad args");
        }
    } else if (!wcsncmp(w, L"XDIM|", 5)) {
        /* XDIM|idA|idB|x|y|O|V：用两条 draft 实体（十字中心线）建尺寸，显式落�?*/
        int    xida = 0, xidb = 0;
        double xx = 0.0, xy = 0.0;
        wchar_t xoc = L'H', xvc = L'F';

        if (swscanf_s(w + 5, L"%d|%d|%lf|%lf", &xida, &xidb, &xx, &xy) == 4) {
            const wchar_t *p = w + 5;
            int            bar = 0;
            const wchar_t *f5 = NULL, *f6 = NULL;

            for (; *p; p++) {
                if (*p == L'|') {
                    bar++;
                    if (bar == 4) f5 = p + 1;
                    if (bar == 5) f6 = p + 1;
                }
            }
            if (f5) xoc = f5[0];
            if (f6) xvc = f6[0];
            {
                ProMdl druk = inc_drw();
                if (druk)
                    xdim_run(druk, xida, xidb, xx, xy, xoc, xvc);
                else
                    logf_("[xdim2] drawing retrieve failed");
            }
        } else {
            logf_("[xdim2] bad args");
        }
    } else if (!wcsncmp(w, L"CDIMX|", 6)) {
        /* CDIMX|idA|idB|O|V : try every point-sense combination, log values, erase */
        int    eida = 0, eidb = 0;
        wchar_t eoc = L'V', evc = L'F';
        wchar_t t2[8], t3[8];
        t2[0] = 0; t3[0] = 0;
        if (swscanf_s(w + 6, L"%d|%d|%7[^|]|%7s", &eida, &eidb, t2, (unsigned)sizeof(t2) / sizeof(wchar_t), t3, (unsigned)sizeof(t3) / sizeof(wchar_t)) >= 2) {
            ProMdl part = NULL, drw = NULL;
            if (t2[0]) eoc = t2[0];
            if (t3[0]) evc = t3[0];
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                drw = inc_drw();
                if (drw) cdimx_run(part, drw, eida, eidb, eoc, evc);
                else logf_("[cdimx] drawing retrieve failed");
            } else logf_("[cdimx] part retrieve failed");
        } else logf_("[cdimx] bad args");
    } else if (!wcsncmp(w, L"CDIM|", 5)) {
        /* CDIM|idA|idB|x|y|O|V[|pt] : same as EDIM but attaches a chosen POINT of
           each edge (pt 1=end1 2=end2 3=center 4=none; default 3) so circular
           features can be dimensioned centre-to-centre. */
        int    eida = 0, eidb = 0, ptv = 3;
        double ex = 0.0, ey = 0.0;
        wchar_t eoc = L'H', evc = L'F';

        if (swscanf_s(w + 5, L"%d|%d|%lf|%lf", &eida, &eidb, &ex, &ey) == 4) {
            const wchar_t *p = w + 5;
            int            bar = 0;
            const wchar_t *f5 = NULL, *f6 = NULL, *f7 = NULL;

            for (; *p; p++) {
                if (*p == L'|') {
                    bar++;
                    if (bar == 4) f5 = p + 1;
                    if (bar == 5) f6 = p + 1;
                    if (bar == 6) f7 = p + 1;
                }
            }
            if (f5) eoc = f5[0];
            if (f6) evc = f6[0];
            if (f7) ptv = _wtoi(f7);
            {
                ProMdl part = NULL, drw = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                    drw = inc_drw();
                    if (drw)
                        cdim_run(part, drw, eida, eidb, ex, ey, eoc, evc, ptv);
                    else
                        logf_("[cdim] drawing retrieve failed");
                } else
                    logf_("[cdim] part retrieve failed");
            }
        } else {
            logf_("[cdim] bad args");
        }
    } else if (!wcsncmp(w, L"EDIM|", 5)) {
        /* EDIM|idA|idB|x|y|O|V ：直选两条边建尺寸（参数样式�?FDIM）。O: H/V；V: F=主视�?/ S=侧视�?*/
        int    eida = 0, eidb = 0;
        double ex = 0.0, ey = 0.0;
        wchar_t eoc = L'H', evc = L'F';

        if (swscanf_s(w + 5, L"%d|%d|%lf|%lf", &eida, &eidb, &ex, &ey) == 4) {
            const wchar_t *p = w + 5;
            int            bar = 0;
            const wchar_t *f5 = NULL, *f6 = NULL;

            for (; *p; p++) {
                if (*p == L'|') {
                    bar++;
                    if (bar == 4) f5 = p + 1;
                    if (bar == 5) f6 = p + 1;
                }
            }
            if (f5) eoc = f5[0];
            if (f6) evc = f6[0];
            {
                ProMdl part = NULL, drw = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                    drw = inc_drw();
                    if (drw)
                        edim_run(part, drw, eida, eidb, ex, ey, eoc, evc);
                    else
                        logf_("[edim] drawing retrieve failed");
                } else
                    logf_("[edim] part retrieve failed");
            }
        } else {
            logf_("[edim] bad args");
        }
    } else if (!wcsncmp(w, L"AUTOBUILD", 9)) {
        /* AUTOBUILD[|ALL|636|411]：改造A/B/C 全流程（几何合法性→位置搜索→创建→界线验证）�?
           无参数或 |ALL = 全部 7 根；|636 / |411 = 单根端到端验证�?*/
        const wchar_t *m = w + 9;
        ProMdl part = NULL, drw = NULL;

        if (m[0] == L'|') m++;
        else m = L"ALL";
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            autobuild_run(part, drw, m);
        else
            logf_("[auto] retrieve failed");
    } else if (!wcsncmp(w, L"SYMINFO", 7)) {
        /* SYMINFO[|subdir|name]：读符号库定�?+ 变量文本（默�?yat|jizhun�?*/
        const wchar_t *rest = w + 7;
        wchar_t        sub[64], nm[64];
        ProMdl         drw = NULL;

        wcscpy_s(sub, 64, L"yat");
        wcscpy_s(nm, 64, L"jizhun");
        if (rest[0] == L'|') {
            const wchar_t *b1 = wcschr(rest + 1, L'|');

            if (b1) {
                size_t len = (size_t)(b1 - (rest + 1));

                if (len < 63) { wcsncpy_s(sub, 64, rest + 1, len); sub[len] = 0; }
                wcsncpy_s(nm, 64, b1 + 1, 63);
            } else {
                wcsncpy_s(nm, 64, rest + 1, 63);
            }
        }
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            syminfo_run(drw, sub, nm);
        else
            logf_("[sym] drw retrieve failed");
    } else if (!wcsncmp(w, L"SYMPROBE", 8)) {
        /* SYMPROBE：符号库定位探针（检索矩�?+ 调色板回退 + 变量文本 + 试验实例�?*/
        ProMdl drw = NULL;

        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            symprobe_run(drw);
        else
            logf_("[sym] drw retrieve failed");
    } else if (!wcsncmp(w, L"SYMMARK|", 8)) {
        /* SYMMARK|x|y|value[|dimv]：放置符号库标记（yat/jizhun v1；value=显示编号�?*/
        double         sx = 0.0, sy = 0.0, sdv = 0.0;
        int            n1 = swscanf_s(w + 8, L"%lf|%lf", &sx, &sy);
        wchar_t        val[64];
        ProMdl         drw = NULL;
        const wchar_t *rest2 = w + 8;

        val[0] = 0;
        if (n1 == 2) {
            const wchar_t *bar1 = wcschr(rest2, L'|');
            const wchar_t *bar2 = bar1 ? wcschr(bar1 + 1, L'|') : NULL;
            const wchar_t *bar3 = bar2 ? wcschr(bar2 + 1, L'|') : NULL;

            if (bar2) {
                size_t len = (size_t)((bar3 ? bar3 : (bar2 + wcslen(bar2))) - (bar2 + 1));

                if (len > 63) len = 63;
                wcsncpy_s(val, 64, bar2 + 1, len);
                val[len] = 0;
            }
            if (bar3) swscanf_s(bar3 + 1, L"%lf", &sdv);
        }
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            symmark_run(drw, sx, sy, val, sdv);
        else
            logf_("[sym] drw retrieve failed");
    } else if (!wcsncmp(w, L"SYMMOVE|", 8)) {
        /* SYMMOVE|<index>|<x>|<y>: move an existing symbol instance to a FREE point */
        int si = -1;
        double sx = 0.0, sy = 0.0;
        ProMdl sd = inc_drw();

        if (swscanf_s(w + 8, L"%d|%lf|%lf", &si, &sx, &sy) == 3 && sd) symmove_run(sd, si, sx, sy);
        else logf_("[symmove] bad args");
    } else if (!wcsncmp(w, L"SYMHEIGHT|", 11)) {
        /* SYMHEIGHT|<index>|<height>: set one symbol instance's scaled height */
        int si = -1;
        double sh = 0.0;
        ProMdl sd = inc_drw();
        if (swscanf_s(w + 11, L"%d|%lf", &si, &sh) == 2 && sd) symheight_run(sd, si, sh);
        else logf_("[symheight] bad args");
    } else if (!wcsncmp(w, L"SYMVAR|", 7)) {
        /* SYMVAR|<index>: read-only dump of a drawing symbol instance */
        int si = -1;
        ProMdl sd = inc_drw();
        if (swscanf_s(w + 7, L"%d", &si) == 1 && sd) symvar_run(sd, si);
        else logf_("[symvar] bad args");
    } else if (!wcsncmp(w, L"SYMDBG|", 7)) {
        /* SYMDBG|x|y：符号可见性诊断（定义信息/可见性标�?Draw/Regen�?*/
        double sx = 0.0, sy = 0.0;
        ProMdl drw = NULL;

        if (swscanf_s(w + 7, L"%lf|%lf", &sx, &sy) == 2 &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            symdbg_run(drw, sx, sy);
        else
            logf_("[sym] dbg bad args/retrieve");
    } else if (!wcsncmp(w, L"SYMARKS", 7)) {
        /* SYMARKS：重要控制项符号标记（轮�?7：按 ±0.05 属�?方向定位—�?
           2±0.05(�? �?编号1；凸点竖向间�?�? �?编号2；凸点高�?�?顶部视图) �?编号3；放�?regen+repaint�?*/
        ProMdl part = NULL, drw = inc_drw();

        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part && drw) {
            int nv = ab_collect_views(drw);

            logf_("[sym] ab_collect_views -> %d", nv);
            symmarks_run(part, drw);
        } else
            logf_("[sym] retrieve failed");
    } else if (!wcsncmp(w, L"VIEWMOVE|", 9)) {
        /* VIEWMOVE|<idx>|<cx>|<cy>：ProDrawingViewMove 平移视图到目�?outline 中心（逻辑屏幕坐标�?*/
        int    vi = -1;
        double cx = 0.0, cy = 0.0;
        ProMdl drw = NULL;

        if (swscanf_s(w + 9, L"%d|%lf|%lf", &vi, &cx, &cy) == 3 &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            viewmove_run(drw, vi, cx, cy);
        else
            logf_("[vmove] bad args/retrieve");
    } else if (!wcsncmp(w, L"NOTEFIX|", 8)) {
        /* NOTEFIX|<from 行文�?|<to 行文�?：按整行匹配改注释（图例文字改正确表述） */
        const wchar_t *rest = w + 8;
        const wchar_t *bar  = wcschr(rest, L'|');
        ProMdl         drw = NULL;

        if (bar && *(bar + 1)) {
            wchar_t from[256], to[256];
            size_t  lenf = (size_t)(bar - rest), lent = wcslen(bar + 1);

            if (lenf > 255) lenf = 255;
            if (lent > 255) lent = 255;
            wcsncpy_s(from, 256, rest, lenf); from[lenf] = 0;
            wcsncpy_s(to, 256, bar + 1, lent); to[lent] = 0;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                int hits = edit_note_text_match(drw, from, to);

                logf_("[notefix] hits=%d", hits);
            } else {
                logf_("[notefix] drw retrieve failed");
            }
        } else {
            logf_("[notefix] bad args");
        }
    } else if (!wcsncmp(w, L"NOTEDUMP", 8)) {
        /* NOTEDUMP：多行注释全量转储（note[i] line[j] text=...�?*/
        ProMdl drw = NULL;

        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            note_dump_all(drw);
        else
            logf_("[notedump] drw retrieve failed");
    } else if (!wcsncmp(w, L"NOTEFIX2|", 9)) {
        /* NOTEFIX2|<contains>|<to 行文�?：多行注释按子串匹配整行替换 */
        const wchar_t *rest = w + 9;
        const wchar_t *bar  = wcschr(rest, L'|');
        ProMdl         drw = NULL;

        if (bar && *(bar + 1)) {
            wchar_t con[128], to2[256];
            size_t  lenc = (size_t)(bar - rest), lent = wcslen(bar + 1);

            if (lenc > 127) lenc = 127;
            if (lent > 255) lent = 255;
            wcsncpy_s(con, 128, rest, lenc); con[lenc] = 0;
            wcsncpy_s(to2, 256, bar + 1, lent); to2[lent] = 0;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                int hits = note_fix_sub(drw, con, to2);

                logf_("[notefix] hits=%d", hits);
            } else {
                logf_("[notefix] drw retrieve failed");
            }
        } else {
            logf_("[notefix] bad args");
        }
    } else if (!wcsncmp(w, L"NOTE2BUILD|", 11)) {
        /* NOTE2BUILD|<contains>|<to>：图例注释重建（复制全部行、命中行换文本、原附件、删旧） */
        const wchar_t *rest = w + 11;
        const wchar_t *bar  = wcschr(rest, L'|');
        ProMdl         drw = NULL;

        if (bar && *(bar + 1)) {
            wchar_t con[128], to2[256];
            size_t  lenc = (size_t)(bar - rest), lent = wcslen(bar + 1);

            if (lenc > 127) lenc = 127;
            if (lent > 255) lent = 255;
            wcsncpy_s(con, 128, rest, lenc); con[lenc] = 0;
            wcsncpy_s(to2, 256, bar + 1, lent); to2[lent] = 0;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                int hits = note_rebuild_legend(drw, con, to2);

                logf_("[note2] hits=%d", hits);
            } else {
                logf_("[note2] drw retrieve failed");
            }
        } else {
            logf_("[note2] bad args");
        }
    } else if (!wcsncmp(w, L"DARROW|", 7)) {
        /* DARROW|<�?|<类型>：逐箭头设 ProLeaderType（实�?10=DOUBLEARROW / 1=ARROWHEAD�?*/
        double dv = 0.0;
        int    ty = 0;
        if (swscanf_s(w + 7, L"%lf|%d", &dv, &ty) == 2) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                darrow_ctx_t dc;

                dc.target = dv; dc.ty = ty; dc.drw = (ProDrawing)drw; dc.n = 0;
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, darrow_cb, NULL, (ProAppData)&dc);
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_REF_DIMENSION, darrow_cb, NULL, (ProAppData)&dc);
                logf_("[darrow] dims touched = %d", dc.n);
                {
                    ProError er2 = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[darrow] regen -> %d (%s)", (int)er2, errname(er2));
                }
            } else {
                logf_("[darrow] drawing retrieve failed");
            }
        } else {
            logf_("[darrow] bad args");
        }
    } else if (!wcsncmp(w, L"AGEO|", 5)) {
        /* AGEO|<�?：只读——箭头头位置 + 界线端点（模型坐标）+ 关系�?*/
        double dv = 0.0;
        if (swscanf_s(w + 5, L"%lf", &dv) == 1) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                ageo_ctx_t ac;

                ac.target = dv; ac.drw = (ProDrawing)drw; ac.n = 0;
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, ageo_cb, NULL, (ProAppData)&ac);
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_REF_DIMENSION, ageo_cb, NULL, (ProAppData)&ac);
                logf_("[ageo] dims matched = %d", ac.n);
            } else {
                logf_("[ageo] drawing retrieve failed");
            }
        } else {
            logf_("[ageo] bad args");
        }
    } else if (!wcsncmp(w, L"MOVE|", 5)) {
        /* MOVE|<�?|<x>|<y>|<z>：把指定值的尺寸移到 3D 点（布局精摆/实验用） */
        double dv = 0, x = 0, y = 0, z = 0;
        if (swscanf_s(w + 5, L"%lf|%lf|%lf|%lf", &dv, &x, &y, &z) == 4) {
            ProMdl part = NULL, drw = NULL;
            logf_("[move] target=%.4f -> (%.2f, %.2f, %.2f)", dv, x, y, z);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                move_ctx_t mc;
                mc.target = dv;
                mc.pt[0] = x; mc.pt[1] = y; mc.pt[2] = z;
                mc.drw = (ProDrawing)drw;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_move_cb, NULL, &mc);
                /* 注意：此处不�?ProDwgSheetRegenerate —�?实测它会把尺寸文本“拉回”，
                   �?ProDrawingDimensionMove 设置的位置失效�?*/
            }
        } else {
            logf_("[move] bad args");
        }
    } else if (!wcsncmp(w, L"LEADER|", 7)) {
        /* LEADER|<�?[|<mode>]：只�?config（无 mode）或设置 config�?=LINEAR 2=CENTER_LEADER�?*/
        double dv = 0.0;
        int mode = -1;
        if (swscanf_s(w + 7, L"%lf|%d", &dv, &mode) >= 1 && (mode == -1 || (mode >= 0 && mode <= 3))) {
            ProMdl part = NULL, drw = NULL;
            logf_("[leader] target=%.4f mode=%d", dv, mode);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                leader_ctx_t lc;
                lc.target = dv;
                lc.drw = (ProDrawing)drw;
                lc.mode = mode;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_leader_cb, NULL, &lc);
            }
        } else {
            logf_("[leader] bad args");
        }
    } else if (!wcsncmp(w, L"SELINJ|", 7)) {
        /* SELINJ|<mode>：轮�?2 选择注入实验（先�?MACRO 启动 hd�?*/
        int mode = 0;
        if (swscanf_s(w + 7, L"%d", &mode) == 1) {
            ProMdl part = NULL, drw = NULL;
            logf_("[selinj] >> mode=%d", mode);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                selinj(part, drw, mode);
            else
                logf_("[selinj] retrieve failed");
        } else {
            logf_("[selinj] bad args");
        }
    } else if (!wcsncmp(w, L"SECDIM", 6)) {
        /* SECDIM[|1|2]：轮�?3/24 —�?给基础草绘加总长尺寸
           �? = 创建+Strengthen�? = 副本提交计数定案测试�?*/
        int strg = 0;
        if (w[6] == L'|') {
            if (w[7] == L'1') strg = 1;
            else if (w[7] == L'2') strg = 2;
        }
        {
            ProMdl part = NULL;
            logf_("[secdim] >> mode=%d", strg);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                if (strg == 2)
                    secdim_count_test(part);
                else
                    secdim_try(part, strg);
            } else {
                logf_("[secdim] part retrieve failed");
            }
        }
    } else if (!wcsncmp(w, L"VREFRESH", 8)) {
        /* VREFRESH: force each existing drawing view to re-scale in place.
           2026-10-29: quick-draw's FRONT-based view reports a 97x11 outline
           although the model is 63.6 x 41.1 - i.e. the view box never picked up
           its real extent, so the drawing shows only a sliver and the layout
           pass puts things in the wrong place. Re-applying the view's own scale
           via ProDrawingViewScaleSet regenerates the outline without deleting
           the view, so the shown dims / tech notes / centerlines survive.
           VREFRESH -> report only; VREFRESH|go -> re-apply scale on all views. */
        int go = (w[8] == L'|' && (w[9] == L'g' || w[9] == L'G'));
        ProMdl drw = inc_drw();
        if (!drw) {
            logf_("[vref] no loaded drawing");
        } else {
            ProView *views = NULL;
            if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
                logf_("[vref] viewsCollect failed");
            } else {
                int i, vc = 0, fixed = 0;
                ProArraySizeGet((ProArray)views, &vc);
                logf_("[vref] views=%d mode=%s", vc, go ? "GO" : "REPORT");
                for (i = 0; i < vc; i++) {
                    ProName nm; char n8[128];
                    double sc = -1.0;
                    ProPoint3d o[2];
                    ProError eg, es = PRO_TK_GENERAL_ERROR;
                    nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                    wide_to_u8(nm, n8, sizeof(n8));
                    eg = ProDrawingViewScaleGet((ProDrawing)drw, views[i], &sc);
                    o[0][0] = o[0][1] = o[0][2] = -9999.0;
                    o[1][0] = o[1][1] = o[1][2] = -9999.0;
                    ProDrawingViewOutlineGet((ProDrawing)drw, views[i], o);
                    logf_("[vref] #%d '%s' scaleGet=%d sc=%.4f outline x[%.1f..%.1f] y[%.1f..%.1f]",
                          i, n8, (int)eg, sc, o[0][0], o[1][0], o[0][1], o[1][1]);
                    if (go) {
                        double use = (sc > 0.0) ? sc : 1.0;
                        es = ProDrawingViewScaleSet((ProDrawing)drw, views[i], use);
                        logf_("[vref]   scaleSet(%.4f) -> %d (%s)", use, (int)es, errname(es));
                        if (es == PRO_TK_NO_ERROR) fixed++;
                        o[0][0] = o[0][1] = o[0][2] = -9999.0;
                        o[1][0] = o[1][1] = o[1][2] = -9999.0;
                        ProDrawingViewOutlineGet((ProDrawing)drw, views[i], o);
                        logf_("[vref]   after outline x[%.1f..%.1f] y[%.1f..%.1f]",
                              o[0][0], o[1][0], o[0][1], o[1][1]);
                    }
                }
                if (go) {
                    ProError er = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[vref] regen -> %d (%s)", (int)er, errname(er));
                    for (i = 0; i < vc; i++) {
                        ProName nm; char n8[128];
                        ProPoint3d o[2];
                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                        wide_to_u8(nm, n8, sizeof(n8));
                        o[0][0] = o[0][1] = o[0][2] = -9999.0;
                        o[1][0] = o[1][1] = o[1][2] = -9999.0;
                        ProDrawingViewOutlineGet((ProDrawing)drw, views[i], o);
                        logf_("[vref] final #%d '%s' x[%.1f..%.1f] y[%.1f..%.1f]",
                              i, n8, o[0][0], o[1][0], o[0][1], o[1][1]);
                    }
                }
                logf_("[vref] done fixed=%d/%d", fixed, vc);
                ProArrayFree((ProArray *)&views);
            }
        }
    } else if (!wcscmp(w, L"SHOWK")) {
        /* SHOWK：显�?KEEP 关键尺寸（含新增的总长 63.60；复�?phase2 的显示步骤） */
        ProMdl part = NULL, drw = NULL;
        logf_("[showk] >> show_key_dims re-run");
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            show_key_dims(part, drw);
        else
            logf_("[showk] retrieve failed");
    } else if (!wcscmp(w, L"REDEF")) {
        /* REDEF：轮�?4 路线A 阶段1 —�?元素树重定义提交总长（模型侧�?*/
        ProMdl part = NULL;
        logf_("[redef] >> start");
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            redef_try(part);
        else
            logf_("[redef] part retrieve failed");
    } else if (!wcsncmp(w, L"GEOQ|", 5)) {
        /* GEOQ|P|<�? / GEOQ|L|<�?|<�? / GEOQ|F|<�?|<�?：只读几何转储（轮次30 调查用） */
        wchar_t mode = w[5];

        if (mode == L'P' || mode == L'p') {
            double val = 0.0;
            if (w[6] == L'|' && swscanf_s(w + 7, L"%lf", &val) == 1) {
                ProMdl part = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
                    geoq_pairs(part, val);
                else
                    logf_("[geoq] part retrieve failed");
            } else {
                logf_("[geoq] bad args P");
            }
        } else if ((mode == L'Z' || mode == L'z') && w[6] == L'|') {
            int    vk2 = 0;
            double val2 = 0.0;

            if (swscanf_s(w + 7, L"%d|%lf", &vk2, &val2) == 2) {
                ProMdl part = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
                    geoq_zpairs(part, vk2, val2);
                else
                    logf_("[geoqz] part retrieve failed");
            } else {
                logf_("[geoqz] bad args");
            }
        } else if ((mode == L'L' || mode == L'l' || mode == L'F' || mode == L'f') && w[6] == L'|') {
            int    axis = -1;
            double val = 0.0;

            if (swscanf_s(w + 7, L"%d|%lf", &axis, &val) == 2 && axis >= 0 && axis <= 2) {
                ProMdl part = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                    if (mode == L'L' || mode == L'l') geoq_lines_axis(part, axis, val);
                    else                                geoq_faces_axis(part, axis, val);
                } else {
                    logf_("[geoq] part retrieve failed");
                }
            } else {
                logf_("[geoq] bad args L/F");
            }
        } else {
            logf_("[geoq] bad mode");
        }
    } else if (!wcsncmp(w, L"FACHK|", 6)) {
        /* FACHK|<�?：只读面诊断（找间距≈值的平行面片对，不改任何东西�?*/
        double dv = 0.0;
        if (swscanf_s(w + 6, L"%lf", &dv) == 1) {
            ProMdl part = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
                facechk_run(part, dv);
            else
                logf_("[fachk] part retrieve failed");
        } else {
            logf_("[fachk] bad args");
        }
    } else if (!wcsncmp(w, L"CONVDIM|", 8)) {
        /* CONVDIM|<�?[|<x>|<y>[|<视图>|<路线>]]：把该值的模型尺寸转成图纸侧创建（任务2）；
           可带显式目标位置（不依赖模型文本位，因未显示的尺寸不会被 PLACEDIMS 归位）�?
           视图：F=主视图（默认�? S=左侧视图；路线：E=边对（默认）/ P=面附�?/ C=边对+同平面过�?*/
        double dv = 0.0, tx = 0.0, ty = 0.0;
        int narg = swscanf_s(w + 8, L"%lf|%lf|%lf", &dv, &tx, &ty);

        if (narg >= 1) {
            int vkind = 0, route = 0;
            {
                const wchar_t *p = w + 8;
                const wchar_t *f4 = NULL, *f5 = NULL;
                int bar = 0;

                for (; *p; p++) {
                    if (*p == L'|') {
                        bar++;
                        if (bar == 3) f4 = p + 1;
                        if (bar == 4) f5 = p + 1;
                    }
                }
                if (f4 && (f4[0] == L'S' || f4[0] == L's')) vkind = 1;
                else if (f4 && (f4[0] == L'T' || f4[0] == L't')) vkind = 2;
                if (f5 && (f5[0] == L'P' || f5[0] == L'p')) route = 1;
                else if (f5 && (f5[0] == L'C' || f5[0] == L'c')) route = 2;
                else if (f5 && (f5[0] == L'O' || f5[0] == L'o')) route = 3;
                else if (f5 && (f5[0] == L'V' || f5[0] == L'v')) route = 4;
                else if (f5 && (f5[0] == L'Z' || f5[0] == L'z')) route = 5;
                else if (f5 && (f5[0] == L'B' || f5[0] == L'b')) route = 6;
                else if (f5 && (f5[0] == L'Y' || f5[0] == L'y')) route = 7;
                else if (f5 && (f5[0] == L'N' || f5[0] == L'n')) route = 8;
            }
            {
                ProMdl part = NULL, drw = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                    ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                    convdim_run(part, drw, dv, (narg >= 3) ? 1 : 0, tx, ty, vkind, route);
                else
                    logf_("[conv] retrieve failed");
            }
        } else {
            logf_("[conv] bad args");
        }
    } else if (!wcscmp(w, L"COORDS")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] COORDS retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            add_probe_note(drw,  50.0,  50.0, 0);
            add_probe_note(drw, 100.0, 100.0, 1);
            add_probe_note(drw, 200.0, 150.0, 2);
            add_probe_note(drw, 300.0, 200.0, 3);
            add_probe_note(drw, 350.0,  80.0, 4);
        }
    } else if (!wcsncmp(w, L"VXF", 3)) {
        /* VXF          : report each drawing view's view->drawing transform (read-only)
           VXF|SWAP|a|b : give view #a the orientation of view #b WITHOUT deleting or
                          recreating anything. Credo (2026-10-30): quick-draw names the
                          largest face "顶部_1" and the sliver "front", so the drawing
                          reads upside down for a human. Rewriting the matrix in place
                          keeps every shown dimension / note / centreline alive, which
                          delete+rebuild cannot.
           VXF|SET|idx|m00|m01|m02|m10|m11|m12|m20|m21|m22 : set explicit matrix. */
        int w2 = (w[3] == L'|' && (w[4] == L'S' || w[4] == L's'));
        ProMdl drw = inc_drw();
        if (!drw) {
            logf_("[vxf] no loaded drawing");
        } else {
            ProView *views = NULL;
            if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) {
                logf_("[vxf] viewsCollect failed");
            } else {
                int i, vc = 0;
                ProArraySizeGet((ProArray)views, &vc);
                if (w2) {
                    /* VXF|SWAP|a|b */
                    int ia = -1, ib = -1;
                    if (wcsncmp(w + 4, L"SWAP", 4) == 0) {
                        if (swscanf_s(w + 8, L"|%d|%d", &ia, &ib) == 2 &&
                            ia >= 0 && ia < vc && ib >= 0 && ib < vc) {
                            ProName nma; char n8a[128], n8b[128];
                            ProName nmb;
                            ProMatrix ta, tb, applied;
                            nma[0] = 0; nmb[0] = 0;
                            ProDrawingViewNameGet((ProDrawing)drw, views[ia], nma);
                            ProDrawingViewNameGet((ProDrawing)drw, views[ib], nmb);
                            wide_to_u8(nma, n8a, sizeof(n8a));
                            wide_to_u8(nmb, n8b, sizeof(n8b));
                            memset(&ta, 0, sizeof(ta));
                            memset(&tb, 0, sizeof(tb));
                            ProDrawingViewTransformGet((ProDrawing)drw, views[ia], PRO_B_TRUE, ta);
                            ProDrawingViewTransformGet((ProDrawing)drw, views[ib], PRO_B_TRUE, tb);
                            logf_("[vxf] SWAP: #%d '%s' <- #%d '%s'", ia, n8a, ib, n8b);
                            /* The drawing view keeps its matrix internally; there is no
                               public setter, so try regenerating after pointing the model
                               view at the target orientation. */
                            {
                                ProMdl part = NULL;
                                ProError er = retrieve_model_any(g_def_model, &part);
                                logf_("[vxf] part retrieve -> %d (%s)", (int)er, errname(er));
                            }
                            memset(&applied, 0, sizeof(applied));
                            logf_("[vxf] no public view-orientation setter; nothing written");
                        } else {
                            logf_("[vxf] SWAP bad args (vc=%d)", vc);
                        }
                    } else {
                        logf_("[vxf] unknown write verb");
                    }
                } else {
                    for (i = 0; i < vc; i++) {
                        ProName nm; char n8[128];
                        ProMatrix t;
                        ProError eg;
                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                        wide_to_u8(nm, n8, sizeof(n8));
                        memset(t, 0, sizeof(t));
                        eg = ProDrawingViewTransformGet((ProDrawing)drw, views[i], PRO_B_TRUE, t);
                        if (eg == PRO_TK_NO_ERROR) {
                            logf_("[vxf] #%d '%s' -> [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f]",
                                  i, n8, t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2], t[2][0], t[2][1], t[2][2]);
                        } else {
                            logf_("[vxf] #%d '%s' -> %d (%s)", i, n8, (int)eg, errname(eg));
                        }
                    }
                }
                ProArrayFree((ProArray *)&views);
            }
        }
    } else if (!wcsncmp(w, L"MKFRONT|", 8)) {
        int axis = -1, flip = 0;
        swscanf_s(w + 8, L"%d|%d", &axis, &flip);
        logf_("[cmd] MKFRONT axis=%d flip=%d", axis, flip);
        {
            ProMdl cur = NULL;
            if (ProMdlCurrentGet(&cur) == PRO_TK_NO_ERROR && cur && axis >= 0 && axis <= 2) {
                int alive = 3 - axis;             /* 面内轴：其余两个 */
                int a2 = (alive == 1) ? 0 : ((alive == 0) ? 1 : 0);
                int b2 = 3 - axis - a2;
                int fl2;
                ProMatrix m;
                ProView v = NULL;
                ProView old = NULL;
                if (ProViewNameToView(cur, (wchar_t *)L"front", &old) == PRO_TK_NO_ERROR && old)
                    ProViewDelete(cur, old);
                fl2 = !((a2 == 0 && b2 == 1 && axis == 2) ||
                        (a2 == 1 && b2 == 2 && axis == 0) ||
                        (a2 == 2 && b2 == 0 && axis == 1));
                if (flip) fl2 = !fl2;
                memset(m, 0, sizeof(m));
                m[0][a2] = 1.0;
                m[1][b2] = fl2 ? -1.0 : 1.0;
                m[2][axis] = 1.0;
                m[3][3] = 1.0;
                logf_("[cmd] MKFRONT set a=%d b=%d n=%d flip=%d", a2, b2, axis, fl2);
                logf_("[cmd] MKFRONT matrixSet -> %d", (int)ProViewMatrixSet(cur, NULL, m));
                logf_("[cmd] MKFRONT store -> %d", (int)ProViewStore(cur, (wchar_t *)L"front", &v));
            }
        }
    } else if (!wcsncmp(w, L"B64|", 4)) {
        /* B64|<全路�?：把任意文件�?Base64 明文流出（进程内读密文，供外部解码） */
        dump_file_b64(w + 4);
    } else if (!wcsncmp(w, L"PDFDUMP|", 8)) {
        /* PDFDUMP|<图名>：导出指定图纸（标题栏活体对照实验用）�?
           轮次54：自校不�?PDF（用 grab_creo_window.ps1 截图）；�?DELIVER 打开后可用�?*/
        ProMdl  drw = NULL;
        ProError er = ProMdlnameRetrieve(w + 8, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] PDFDUMP(%ls) retrieve -> %d (%s)", w + 8, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            ProPDFOptions opts = NULL;
            ProPath       out;
            ProError      ep, ed;
            SYSTEMTIME    st2;
            if (!g_deliver_ok) {
                pdf_gate_log("PDFDUMP|<name>");
            } else {
            ed = ProMdlDisplay(drw);
            logf_("[cmd] PDFDUMP(%ls) display -> %d (%s)", w + 8, (int)ed, errname(ed));
            GetLocalTime(&st2);
            swprintf_s(out, PRO_PATH_SIZE,
                       L"D:\\creo-agent\\sync-toolkit\\work\\pdf_named_%02u%02u%02u.pdf",
                       (unsigned)st2.wHour, (unsigned)st2.wMinute, (unsigned)st2.wSecond);
            ProPDFoptionsAlloc(&opts);
            ep = ProPDFExport(drw, out, opts);
            logf_("[cmd] PDFDUMP(%ls) export -> %d (%s)", w + 8, (int)ep, errname(ep));
            if (opts) ProPDFoptionsFree(opts);
            dump_file_b64(out);
            }
        }
    } else if (!wcsncmp(w, L"PARAMS|", 7)) {
        /* PARAMS|<模型�?|<prt|drw|asm>：导出参数表到日志（明文 CSV 回执�?*/
        wchar_t nm[256], ty[16];
        ty[0] = 0;
        if (swscanf_s(w + 7, L"%255[^|]|%15s", nm, 256, ty, 16) >= 1) {
            ProMdlfileType ft = PRO_MDLFILE_PART;
            ProMdl        mdl = NULL;
            ProError      er;
            if (!wcscmp(ty, L"drw"))      ft = PRO_MDLFILE_DRAWING;
            else if (!wcscmp(ty, L"asm")) ft = PRO_MDLFILE_ASSEMBLY;
            er = ProMdlnameRetrieve(nm, ft, &mdl);
            logf_("[cmd] PARAMS %ls|%ls retrieve -> %d (%s)", nm, ty, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR && mdl) dump_params_to_log(mdl, nm);
        } else {
            logf_("[cmd] PARAMS bad args");
        }
    } else if (!wcsncmp(w, L"WPARAM|", 7)) {
        /* WPARAM|<模型�?|<prt|drw>|<参数�?|<�?：写字符串参数（标题栏实验用�?*/
        wchar_t nm[256], ty[16], pn[64], pv[512];
        ty[0] = 0; pv[0] = 0;
        if (swscanf_s(w + 7, L"%255[^|]|%15[^|]|%63[^|]|%511[^|]", nm, 256, ty, 16, pn, 64, pv, 512) == 4) {
            ProMdlfileType ft = (!wcscmp(ty, L"drw")) ? PRO_MDLFILE_DRAWING : PRO_MDLFILE_PART;
            ProMdl        mdl = (!wcscmp(ty, L"drw")) ? inc_drw() : NULL;
            ProError      er = (mdl ? PRO_TK_NO_ERROR : ProMdlnameRetrieve(nm, ft, &mdl));
            char          v8[2048];
            char          tag[128];
            wide_to_u8(pv, v8, sizeof(v8));
            logf_("[cmd] WPARAM %ls|%ls|%ls=%s retrieve -> %d (%s)", nm, ty, pn, v8, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR && mdl) {
                sprintf_s(tag, sizeof(tag), "wparam-%ls", pn);
                set_string_param(mdl, pn, v8, tag);
            }
        } else {
            logf_("[cmd] WPARAM bad args");
        }
    } else if (!wcsncmp(w, L"PARAMNUM|", 9)) {
        /* PARAMNUM|<prt|drw>|<参数�?|<数�?：建/改数值参数（&MASS[.4] 解析用） */
        wchar_t ty[16], pn[64];
        double  val = 0.0;
        ty[0] = 0;
        if (swscanf_s(w + 9, L"%15[^|]|%63[^|]|%lf", ty, 16, pn, 64, &val) == 3) {
            ProMdl mdl = (!wcscmp(ty, L"drw")) ? inc_drw() : NULL;
            ProError er = (mdl ? PRO_TK_NO_ERROR : ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &mdl));
            char tag[128];
            logf_("[cmd] PARAMNUM %ls|%ls=%.6f retrieve -> %d (%s)", ty, pn, val, (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR && mdl) {
                sprintf_s(tag, sizeof(tag), "paramnum-%ls", pn);
                set_num_param(mdl, pn, val, tag);
            }
        } else {
            logf_("[cmd] PARAMNUM bad args");
        }
    } else if (!wcscmp(w, L"REGEN")) {
        ProMdl drw = inc_drw();
        if (drw)
            logf_("[cmd] REGEN sheetRegen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
        else
            logf_("[cmd] REGEN drawing not found");
    } else if (!wcscmp(w, L"WINACT")) {
        /* 2026-10-31: restore the drawing window from inside Creo so the GL canvas
           renders and PrintWindow can grab the page. A MINIMIZED window yields a
           blank PrintWindow buffer; calling ShowWindow from outside risks breaking
           the canvas, so let Creo activate its own window instead. */
        ProMdl drw = inc_drw();
        if (drw) {
            logf_("[winact] activate drawing window ...");
            activate_drawing_window(drw);
            {
                ProError ewr = ProWindowRepaint(-1);
                logf_("[winact] repaint -> %d (%s)", (int)ewr, errname(ewr));
            }
        } else {
            logf_("[winact] drawing not found");
        }
    } else if (!wcscmp(w, L"FACESCAN")) {
        /* FACESCAN : list planes and cylinders (id, origin, normal/axis, radius) */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[faces] part '%ls' retrieve failed", g_def_model);
        } else {
            int sid, npl = 0, ncy = 0;
            for (sid = 1; sid <= 6000 && (npl + ncy) < 800; sid++) {
                ProSurface s = NULL;
                ProGeomitem gi;
                ProGeomitemdata *gd = NULL;
                if (ProSurfaceInit((ProSolid)part, sid, &s) != PRO_TK_NO_ERROR || !s) continue;
                if (ProSurfaceToGeomitem((ProSolid)part, s, &gi) != PRO_TK_NO_ERROR) continue;
                if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_surface_data) {
                    if (gd) ProGeomitemdataFree(&gd);
                    continue;
                }
                {
                    ProSurfacedata *sd = gd->data.p_surface_data;
                    if (sd->type == PRO_SRF_PLANE) {
                        logf_("[faces] id=%d PLANE o=(%.2f,%.2f,%.2f) n=(%.2f,%.2f,%.2f)",
                              sid, sd->srf_shape.plane.origin[0], sd->srf_shape.plane.origin[1],
                              sd->srf_shape.plane.origin[2], sd->srf_shape.plane.e3[0],
                              sd->srf_shape.plane.e3[1], sd->srf_shape.plane.e3[2]);
                        npl++;
                    } else if (sd->type == PRO_SRF_CYL) {
                        logf_("[faces] id=%d CYL o=(%.2f,%.2f,%.2f) a=(%.2f,%.2f,%.2f) r=%.4f",
                              sid, sd->srf_shape.cylinder.origin[0], sd->srf_shape.cylinder.origin[1],
                              sd->srf_shape.cylinder.origin[2], sd->srf_shape.cylinder.e3[0],
                              sd->srf_shape.cylinder.e3[1], sd->srf_shape.cylinder.e3[2],
                              sd->srf_shape.cylinder.radius);
                        ncy++;
                    }
                }
                ProGeomitemdataFree(&gd);
            }
            logf_("[faces] planes=%d cylinders=%d", npl, ncy);
        }
    } else if (!wcsncmp(w, L"TOTEDGE|", 8)) {
        /* TOTEDGE|x|y : overall length + width from the extreme edges of the part */
        double x = 390.0, y = 530.0;
        ProMdl part = NULL;
        swscanf_s(w + 8, L"|%lf|%lf", &x, &y);
        if (!g_loaded_drw) {
            logf_("[tote] no loaded drawing");
        } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[tote] part '%ls' retrieve failed", g_def_model);
        } else {
            tote_dims(g_loaded_drw, part, x, y);
        }
    } else if (!wcsncmp(w, L"DIM1|", 5)) {
        /* DIM1|edgeid|x|y[|main|top|left] : single-edge dimension (circle -> dia) */
        int id = 0;
        double x = 0.0, y = 0.0;
        int kind = 0;
        wchar_t vw[16];
        ProMdl part = NULL;
        vw[0] = 0;
        if (swscanf_s(w + 5, L"%d|%lf|%lf|%15s", &id, &x, &y, vw, 16) >= 3) {
            if (vw[0] == L'T' || vw[0] == L't') kind = 1;
            else if (vw[0] == L'L' || vw[0] == L'l') kind = 2;
            if (!g_loaded_drw) {
                logf_("[d1] no loaded drawing");
            } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
                logf_("[d1] part '%ls' retrieve failed", g_def_model);
            } else {
                ProView v = find_view_by_key((ProDrawing)g_loaded_drw, kind);
                int wid = -1;
                ProMdlDisplay(g_loaded_drw);
                ProMdlWindowGet(g_loaded_drw, &wid);
                if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
                dim_one_edge(g_loaded_drw, part, v, id, x, y, "d1");
                ProDwgSheetRegenerate((ProDrawing)g_loaded_drw, 1);
            }
        } else logf_("[d1] bad args");
    } else if (!wcscmp(w, L"DUMPDIMS")) {
        /* DUMPDIMS : list every model dimension (id, symbol, value, type) */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[dims] part '%ls' retrieve failed", g_def_model);
        } else {
            dumpdims_ctx_t dc;
            dc.n = 0;
            ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dumpdims_cb, NULL, &dc);
            logf_("[dims] total=%d", dc.n);
        }
    } else if (!wcsncmp(w, L"TOLSET|", 7)) {
        /* TOLSET|<�?|<toltype 1..4>|<up>|<lo>：在线改公差（P0c 实验�?*/
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 7, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL;
            logf_("[tolset] target=%.4f type=%d up=%.4f lo=%.4f", dv, ty, up, lo);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                tolset_ctx_t tc;
                tc.target = dv; tc.ty = ty; tc.up = up; tc.lo = lo;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tol_cb, NULL, &tc);
            }
        } else {
            logf_("[tolset] bad args");
        }
    } else if (!wcsncmp(w, L"DIMTOL|", 7)) {
        /* DIMTOL|<id>|<ty>|<up>|<lo>：按图纸尺寸 id 直接设公�?*/
        int id = 0, ty = 4;
        double up = 0, lo = 0;
        if (swscanf_s(w + 7, L"%d|%d|%lf|%lf", &id, &ty, &up, &lo) == 4) {
            ProMdl drw = g_loaded_drw;
            if (!drw) {
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
            }
            if (drw) {
                dimtol_ctx_t tc;
                tc.target_id = id; tc.ty = ty; tc.up = up; tc.lo = lo; tc.hits = 0;
                logf_("[dimtol] target id=%d type=%d up=%.4f lo=%.4f", id, ty, up, lo);
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dimtol_cb, NULL, &tc);
                logf_("[dimtol] id=%d hits=%d", id, tc.hits);
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
                ProWindowRepaint(-1);
            }
        } else {
            logf_("[dimtol] bad args");
        }
    } else if (!wcsncmp(w, L"DIMDEC|", 7)) {
        /* DIMDEC|<id>|<n>: set decimal places of one drawing dimension */
        int id = 0, dec = 0;
        if (swscanf_s(w + 7, L"%d|%d", &id, &dec) == 2) {
            ProMdl drw = g_loaded_drw;
            if (!drw) ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
            if (drw) {
                dimdec_ctx_t tc;
                tc.target_id = id; tc.dec = dec; tc.hits = 0;
                logf_("[dimdec] target id=%d dec=%d", id, dec);
                ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dimdec_cb, NULL, &tc);
                logf_("[dimdec] id=%d hits=%d", id, tc.hits);
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
                ProWindowRepaint(-1);
            }
        } else {
            logf_("[dimdec] bad args");
        }
    } else if (!wcsncmp(w, L"DIMTEXT|", 8)) {
        /* DIMTEXT|<id>|<fmt>：格式串覆盖图纸尺寸文本（@D 仍为 Creo 计算值） */
        char tmp[512];
        char *bar;
        wide_to_u8(w + 8, tmp, sizeof(tmp));
        bar = strchr(tmp, '|');
        if (bar) {
            int id;
            *bar = 0;
            id = atoi(tmp);
            {
                ProMdl drw = g_loaded_drw;
                if (!drw) ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
                if (drw) {
                    dimtext_ctx_t tc;
                    tc.target_id = id;
                    tc.hits = 0;
                    if (!u8_to_wide(bar + 1, tc.fmt, 256)) tc.fmt[0] = 0;
                    ProDrawingDimensionVisit((ProDrawing)drw, PRO_DIMENSION, dimtext_cb, NULL, &tc);
                    logf_("[dimtext] id=%d hits=%d", id, tc.hits);
                    ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    ProWindowRepaint(-1);
                }
            }
        } else {
            logf_("[dimtext] bad args");
        }
    } else if (!wcsncmp(w, L"DXFOUT|", 7)) {
        /* DXFOUT|<path>：把当前内存图纸导出 DWG/DXF（不依赖保存�?*/
        ProMdl drw = g_loaded_drw;
        if (!drw) ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        if (drw) {
            ProPath out;
            Pro2dExportdata data = NULL;
            ProError ea, ex;
            wcsncpy_s(out, PRO_PATH_SIZE, w + 7, _TRUNCATE);
            ea = Pro2dExportdataAlloc(&data);
            if (ea == PRO_TK_NO_ERROR && data) Pro2dExportdataSheetoptionSet(data, PRO2DEXPORT_ALL);
            ex = Pro2dExport(PRO_DWG_FILE, out, drw, data);
            logf_("[dxfout] '%ls' -> %d (%s)", out, (int)ex, errname(ex));
            if (data) Pro2dExportdataFree(data);
        } else {
            logf_("[dxfout] no drawing");
        }
    } else if (!wcsncmp(w, L"COPYMODEL|", 10)) {
        /* COPYMODEL|<newname>：复制当前内存模型为新名字（复制操作自身写文件） */
        ProName nn;
        ProMdl newh = NULL;
        wcsncpy_s(nn, PRO_NAME_SIZE, w + 10, _TRUNCATE);
        {
            ProError e = ProMdlnameCopy(g_loaded_drw, nn, &newh);
            logf_("[copymodel] '%ls' -> %d (%s) new=%p", nn, (int)e, errname(e), (void *)newh);
            if (e == PRO_TK_NO_ERROR && newh) {
                ProError es = ProMdlSave(newh);
                logf_("[copymodel] ProMdlSave -> %d (%s)", (int)es, errname(es));
            }
        }
    } else if (!wcsncmp(w, L"TOLFREE|", 8)) {
        /* TOLFREE|<�?|<ty>|<up>|<lo>：摘公差表标�?+ 显式公差（P0c 实验�?*/
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 8, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                tolf_ctx_t tc;
                tc.target = dv; tc.mode = 1; tc.ty = ty; tc.up = up; tc.lo = lo;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tolfree_cb, NULL, &tc);
            }
        } else {
            logf_("[tolfree] bad args");
        }
    } else if (!wcscmp(w, L"TOLLABEL")) {
        /* TOLLABEL：dump 关键尺寸的公差标�?类型/值（只读诊断�?*/
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
            tolf_ctx_t tc;
            tc.target = -1; tc.mode = 0; tc.ty = 0; tc.up = 0; tc.lo = 0;
            ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tolfree_cb, NULL, &tc);
        }
    } else if (!wcsncmp(w, L"SETPARAM|", 9)) {
        /* SETPARAM|<name>|<value> : create/update a string parameter on the part
           (title block placeholders like CNAME/MSTD/DRWNUM resolve from these) */
        wchar_t pn[64], pv[256];
        pn[0] = 0; pv[0] = 0;
        if (swscanf_s(w + 9, L"%63[^|]|%255[^|]", pn, 64, pv, 256) == 2) {
            ProMdl spart = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &spart) == PRO_TK_NO_ERROR && spart) {
                char v8[1024];
                wide_to_u8(pv, v8, sizeof(v8));
                logf_("[setparam] '%ls' = '%s'", pn, v8);
                set_string_param(spart, pn, v8, "setparam");
                {
                    ProMdl sdrw = inc_drw();
                    if (sdrw) logf_("[setparam] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)sdrw, 1));
                }
            } else logf_("[setparam] part retrieve failed");
        } else logf_("[setparam] bad args");    } else if (!wcsncmp(w, L"MAT|", 4)) {
        /* MAT|<材料�?：材料库读取变体实验 + 质量属性打印（P0 用） */
        wchar_t nm[256];
        if (swscanf_s(w + 4, L"%255s", nm, 256) == 1) {
            ProMdl part = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                wchar_t v1[300], v2[300], v3[300];
                const wchar_t *vars[3];
                int vi, hit = -1;
                swprintf_s(v1, 300, L"%ls", nm);           /* 变体1：裸名（现状，预�?-4�?*/
                swprintf_s(v2, 300, L"金属\\%ls", nm);     /* 变体2：材料库子目录（金属\NB109�?*/
                swprintf_s(v3, 300, L"%ls.mtl", nm);       /* 变体3：带扩展�?*/
                vars[0] = v1; vars[1] = v2; vars[2] = v3;
                for (vi = 0; vi < 3; vi++) {
                    ProName  nmv;
                    ProError em;
                    wcsncpy_s(nmv, PRO_NAME_SIZE, vars[vi], _TRUNCATE);
                    em = ProMaterialfileRead((ProSolid)part, nmv);
                    logf_("[mat] ProMaterialfileRead('%ls') -> %d (%s)", vars[vi], (int)em, errname(em));
                    if (em == PRO_TK_NO_ERROR) { hit = vi; break; }
                }
                if (hit >= 0) {
                    ProMaterial mat;
                    ProError emc = ProMaterialCurrentGet((ProSolid)part, &mat);
                    logf_("[mat] CurrentGet -> %d (%s)", (int)emc, errname(emc));
                    if (emc == PRO_TK_NO_ERROR) {
                        ProError ems = ProMaterialCurrentSet(&mat);
                        logf_("[mat] CurrentSet -> %d (%s)", (int)ems, errname(ems));
                    }
                } else {
                    logf_("[mat] all variants failed");
                }
                {
                    ProMassProperty mp;
                    ProError       emp = ProSolidMassPropertyGet((ProSolid)part, NULL, &mp);
                    logf_("[mat] MassPropertyGet -> %d (%s)", (int)emp, errname(emp));
                    if (emp == PRO_TK_NO_ERROR)
                        logf_("[mat] mass=%.6f volume=%.6f density=%.6f", mp.mass, mp.volume, mp.density);
                }
            }
        } else {
            logf_("[mat] bad args");
        }
    } else if (!wcsncmp(w, L"DIAG|", 5)) {
        /* DIAG|<图名>：列出指定图纸的注记（标题栏格文�?参数引用判读用） */
        ProMdl  drw = NULL;
        ProError er = ProMdlnameRetrieve(w + 5, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] DIAG(%ls) retrieve -> %d (%s)", w + 5, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) diag_notes_of(drw, "diag");
    } else if (!wcscmp(w, L"SHEETINFO")) {
        ProMdl sd = inc_drw();
        if (sd) {
            int ns = -1, vc = -1;
            ProPlotPaperSize pt = A3_SIZE_PLOT;
            double sw = -1.0, sh = -1.0;
            ProUnititem u;
            ProView *vs = NULL;
            ProError e1 = ProDrawingSheetsCount((ProDrawing)sd, &ns);
            ProError e2 = ProDrawingSheetSizeGet((ProDrawing)sd, 1, &pt, &sw, &sh);
            ProError e4;
            memset(&u, 0, sizeof(u));
            e4 = ProDrawingSheetUnitsGet((ProDrawing)sd, 1, &u);
            logf_("[sheet] count=%d(get=%d) size=%.3f x %.3f (get=%d type=%d) units='%ls'(get=%d)",
                  ns, (int)e1, sw, sh, (int)e2, (int)pt, u.name, (int)e4);
            if (ProDrawingViewsCollect((ProDrawing)sd, &vs) == PRO_TK_NO_ERROR && vs) {
                ProArraySizeGet((ProArray)vs, &vc);
                ProArrayFree((ProArray *)&vs);
            }
            logf_("[sheet] views=%d", vc);
        } else logf_("[sheet] no drawing");
    } else if (!wcsncmp(w, L"FORMAT|", 7)) {
        wchar_t fp[260];
        int sheet = 1;
        fp[0] = 0;
        if (swscanf_s(w + 7, L"%259[^|]|%d", fp, 260, &sheet) >= 1) {
            ProMdl fd = inc_drw();
            ProSolid fsol = NULL;
            ProMdl fmt = NULL;
            if (fd) {
                ProError es = ProDrawingCurrentsolidGet((ProDrawing)fd, &fsol);
                ProError el = ProMdlFiletypeLoad(fp, PRO_MDLFILE_DWGFORM, PRO_B_FALSE, &fmt);
                logf_("[format] load('%ls') -> %d (%s) fmt=%p solidGet=%d solid=%p",
                      fp, (int)el, errname(el), (void *)fmt, (int)es, (void *)fsol);
                if (el == PRO_TK_NO_ERROR && fmt) {
                    ProError ea = ProDrawingFormatAdd((ProDrawing)fd, sheet, fsol, fmt, 1);
                    logf_("[format] FormatAdd(sheet=%d) -> %d (%s)", sheet, (int)ea, errname(ea));
                    if (ea == PRO_TK_NO_ERROR)
                        logf_("[format] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)fd, sheet));
                }
            }
        } else logf_("[format] bad args");
    } else if (!wcsncmp(w, L"PAPERSIZE|", 10)) {
        logf_("[papersize] ENTER arg='%ls'", w + 10);
        ProMdl pd = inc_drw();
        logf_("[papersize] inc_drw -> %p", (void *)pd);
        if (!pd) {
            logf_("[papersize] no drawing");
        } else {
            ProPlotPaperSize ps = (ProPlotPaperSize)-1;
            wchar_t kbuf[32];
            const wchar_t *k;
            int ki = 0;
            wcsncpy_s(kbuf, 32, w + 10, _TRUNCATE);
            /* 2026-11-05: cmd_now.txt 可能带 CRLF/尾部空白，先 trim 再比对，
               否则 PAPERSIZE|A4 会掉进 unknown size ''。 */
            while (ki < 31 && kbuf[ki]) ki++;
            while (ki > 0 && (kbuf[ki-1] == L'\r' || kbuf[ki-1] == L'\n' ||
                              kbuf[ki-1] == L' '  || kbuf[ki-1] == L'\t')) kbuf[--ki] = 0;
            k = kbuf;
            if (!_wcsicmp(k, L"A4"))      ps = A4_SIZE_PLOT;
            else if (!_wcsicmp(k, L"A3")) ps = A3_SIZE_PLOT;
            else if (!_wcsicmp(k, L"A2")) ps = A2_SIZE_PLOT;
            else if (!_wcsicmp(k, L"A1")) ps = A1_SIZE_PLOT;
            else if (!_wcsicmp(k, L"A0")) ps = A0_SIZE_PLOT;
            if ((int)ps < 0) {
                logf_("[papersize] unknown size '%ls'", k);
            } else {
                ProError ep = ProDrawingFormatSizeSet((ProDrawing)pd, 1, ps, 0.0, 0.0);
                logf_("[papersize] FormatSizeSet('%ls'=%d) -> %d (%s)", k, (int)ps, (int)ep, errname(ep));
                if (ep == PRO_TK_NO_ERROR) {
                    logf_("[papersize] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)pd, 1));
                }
            }
        }
    } else if (!wcsncmp(w, L"FMTHIDE|", 8)) {
        /* FMTHIDE|<sheet>：隐藏该页格式（等价 GUI「格式表-全部移除」的一半） */
        ProMdl pd = inc_drw();
        int sh = (int)wcstol(w + 8, NULL, 10);
        if (!pd) logf_("[fmthide] no drawing");
        else {
            if (sh < 1) sh = 1;
            ProError eh = ProDrawingSheetFormatHide((ProDrawing)pd, sh);
            logf_("[fmthide] sheet=%d FormatHide -> %d (%s)", sh, (int)eh, errname(eh));
        }
    } else if (!wcsncmp(w, L"FMTSHOW|", 8)) {
        ProMdl pd = inc_drw();
        int sh = (int)wcstol(w + 8, NULL, 10);
        if (!pd) logf_("[fmtshow] no drawing");
        else {
            if (sh < 1) sh = 1;
            ProError es = ProDrawingSheetFormatShow((ProDrawing)pd, sh);
            logf_("[fmtshow] sheet=%d FormatShow -> %d (%s)", sh, (int)es, errname(es));
        }
    } else if (!wcscmp(w, L"SHOWDRW")) {
        inc_show_drawing(inc_drw());    } else if (!wcsncmp(w, L"MODEL|", 6)) {
        ProMdl mdraw = inc_drw();
        if (!mdraw) {
            logf_("[model] no drawing");
        } else if (!wcsncmp(w + 6, L"list", 4)) {
            ProSolid *solids = NULL;
            int cnt = -1, i;
            ProError ec = ProDrawingSolidsCollect((ProDrawing)mdraw, &solids);
            if (ec == PRO_TK_NO_ERROR && solids) {
                ProArraySizeGet((ProArray)solids, &cnt);
                for (i = 0; i < cnt; i++) {
                    ProName nm;
                    nm[0] = 0;
                    ProMdlNameGet((ProMdl)solids[i], nm);
                    logf_("[model] list[%d] '%ls'", i, nm);
                }
                ProArrayFree((ProArray *)&solids);
            }
            logf_("[model] list collect -> %d (%s) count=%d", (int)ec, errname(ec), cnt);
            {
                ProSolid cur = NULL;
                ProError eg = ProDrawingCurrentsolidGet((ProDrawing)mdraw, &cur);
                logf_("[model] current solid -> %d (%s) %p", (int)eg, errname(eg), (void *)cur);
            }
        } else if (!wcsncmp(w + 6, L"add|", 4)) {
            wchar_t pn[64];
            ProMdl part = NULL;
            pn[0] = 0;
            if (swscanf_s(w + 10, L"%63s", pn, 64) == 1) {
                ProError er = ProMdlnameRetrieve(pn, PRO_MDLFILE_PART, &part);
                logf_("[model] retrieve '%ls' -> %d (%s) part=%p", pn, (int)er, errname(er), (void *)part);
                if (er == PRO_TK_NO_ERROR && part) {
                    ProError ea = ProDrawingSolidAdd((ProDrawing)mdraw, (ProSolid)part);
                    ProError es = ProDrawingCurrentsolidSet((ProDrawing)mdraw, (ProSolid)part);
                    logf_("[model] SolidAdd -> %d (%s); CurrentsolidSet -> %d (%s)",
                          (int)ea, errname(ea), (int)es, errname(es));
                    logf_("[model] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)mdraw, 1));
                }
            } else logf_("[model] bad args");
        } else {
            logf_("[model] usage: MODEL|list | MODEL|add|<part>");
        }
    } else if (!wcsncmp(w, L"VM|", 3)) {
        /* VM|<name>|x|y|scale|m00..m22 : create one general view with an explicit
           orientation matrix, then report its outline. Experimental helper. */
        wchar_t nm[32];
        double px = 0.0, py = 0.0, sc = 1.0;
        double mm[9];
        int got;
        nm[0] = 0;
        got = swscanf_s(w + 3,
                        L"%31[^|]|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf|%lf",
                        nm, 32, &px, &py, &sc,
                        &mm[0], &mm[1], &mm[2], &mm[3], &mm[4],
                        &mm[5], &mm[6], &mm[7], &mm[8]);
        logf_("[vm] parsed n=%d name='%ls' pos=(%.1f,%.1f) sc=%.2f m=%.3f,%.3f,%.3f / %.3f,%.3f,%.3f / %.3f,%.3f,%.3f",
              got, nm, px, py, sc, mm[0], mm[1], mm[2], mm[3], mm[4], mm[5], mm[6], mm[7], mm[8]);
        if (got >= 13) {
            ProMdl vdrw = inc_drw();
            ProSolid vpart = NULL;
            if (vdrw) {
                ProSolid *solids = NULL;
                if (ProDrawingSolidsCollect((ProDrawing)vdrw, &solids) == PRO_TK_NO_ERROR && solids) {
                    int scnt = 0;
                    ProArraySizeGet((ProArray)solids, &scnt);
                    if (scnt > 0) vpart = solids[0];
                    ProArrayFree((ProArray *)&solids);
                }
            }
            if (vdrw && vpart) {
                ProMatrix mat;
                ProPoint3d pos;
                ProView nv = NULL;
                ProError ec;
                inc_show_drawing(vdrw);
                memset(mat, 0, sizeof(mat));
                mat[0][0] = mm[0]; mat[0][1] = mm[1]; mat[0][2] = mm[2];
                mat[1][0] = mm[3]; mat[1][1] = mm[4]; mat[1][2] = mm[5];
                mat[2][0] = mm[6]; mat[2][1] = mm[7]; mat[2][2] = mm[8];
                mat[3][0] = 0.0; mat[3][1] = 0.0; mat[3][2] = 0.0; mat[3][3] = 1.0;
                pos[0] = px; pos[1] = py; pos[2] = 0.0;
                ec = ProDrawingGeneralviewCreate((ProDrawing)vdrw, vpart, 1, PRO_B_FALSE, pos, sc, mat, &nv);
                logf_("[vm] create '%ls' -> %d (%s) view=%p", nm, (int)ec, errname(ec), (void *)nv);
                if (ec == PRO_TK_NO_ERROR && nv) {
                    ProError en = ProDrawingViewNameSet((ProDrawing)vdrw, nv, nm);
                    ProPoint3d out[2];
                    double s1 = -1.0;
                    ProDwgSheetRegenerate((ProDrawing)vdrw, 1);
                    out[0][0] = out[0][1] = out[0][2] = -9999.0;
                    out[1][0] = out[1][1] = out[1][2] = -9999.0;
                    ProDrawingViewOutlineGet((ProDrawing)vdrw, nv, out);
                    ProDrawingViewScaleGet((ProDrawing)vdrw, nv, &s1);
                    logf_("[vm] nameSet -> %d; outline x[%.1f..%.1f] y[%.1f..%.1f] w=%.1f h=%.1f scale=%.2f",
                          (int)en, out[0][0], out[1][0], out[0][1], out[1][1],
                          out[1][0] - out[0][0], out[1][1] - out[0][1], s1);
                }
            } else logf_("[vm] no drawing/part");
        } else logf_("[vm] bad args");    } else if (!wcsncmp(w, L"STDV|", 5)) {
        double fx = 300.0, fy = 520.0, dx = 300.0, dy = 230.0, ix = 700.0, iy = 630.0, isc = 0.5;
        int n = swscanf_s(w + 5, L"%lf|%lf|%lf|%lf|%lf|%lf|%lf", &fx, &fy, &dx, &dy, &ix, &iy, &isc);
        ProMdl drw = inc_drw();
        ProSolid part = NULL;
        if (drw) {
            ProSolid *solids = NULL;
            if (ProDrawingSolidsCollect((ProDrawing)drw, &solids) == PRO_TK_NO_ERROR && solids) {
                int scnt = 0, si;
                ProArraySizeGet((ProArray)solids, &scnt);
                for (si = 0; si < scnt; si++) {
                    ProName snm;
                    snm[0] = 0;
                    ProMdlNameGet((ProMdl)solids[si], snm);
                    logf_("[stdv] solid[%d] '%ls'", si, snm);
                }
                if (scnt > 0) part = solids[0];
                ProArrayFree((ProArray *)&solids);
            }
            if (!part) {
                ProError epc = ProDrawingCurrentsolidGet((ProDrawing)drw, &part);
                logf_("[stdv] currentsolid fallback -> %d (%s) part=%p", (int)epc, errname(epc), (void *)part);
            }
        }
        if (drw && part) {
            /* part axes: X=41.10 (width), Y=4.55 (thickness), Z=63.58 (length)
               Matrices are read BY COLUMN: column k = image of model axis k.
               2026-10-30 verified against a live drawing: quick-draw's own
               "顶部_1" view reports transform
                   [2.381 0 0 | 0 0 2.381 | 0 -2.381 0]
               i.e. 1.0*(X->screen x), 1.0*(Z->screen y), -(Y->eye). That is the
               only orientation that shows the 41.1 x 63.58 face, so the former
               mf ({{0,0,1},{1,0,0},{0,1,0}}) was wrong: it put Z on the screen x
               axis and X on the screen y axis, which is what made the "front"
               view a 4.55-wide sliver. Use the verified one. */
            static const double mf[4][4] = {{1,0,0,0},{0,0,1,0},{0,-1,0,0},{0,0,0,1}};
            /* top (below front): must come out 63.58 wide x 41.1 tall on paper.
               Measured 2026-10-30 (screen = 2.3810 x model):
                 mf {{1,0,0},{0,0,1},{0,-1,0}} -> 97.8 x 151.4  (41.1 x 63.58)  OK
                 mt {{0,0,1},{1,0,0},{0,1,0}} -> 10.8 x 151.4  (4.55 x 63.58)  WRONG
               The second row is the screen-y row, so mt's row1 must put Z there,
               and row0 must put X on the screen x axis:
                 X->screen x (41.1? no)  -- we want 63.58 across, so Z->screen x,
                 X->screen y.  That is row0=(0,0,1), row1=(1,0,0) which is what we
               already had; the 10.8 means the view chose the thickness axis for
               row1 instead. Use the front matrix rotated 90 deg about the eye:
                 row0 = Z, row1 = X, row2 = Y. (identical intent, spelled out) */
            static const double mt[4][4] = {{0,0,1,0},{1,0,0,0},{0,1,0,0},{0,0,0,1}};
            /* left: 4.55 wide x 41.1 tall -> Y on screen x, X on screen y, Z to eye */
            static const double ms[4][4] = {{0,1,0,0},{1,0,0,0},{0,0,1,0},{0,0,0,1}};
            logf_("[stdv] n=%d front=(%.1f,%.1f) d=(%.1f,%.1f) iso=(%.1f,%.1f)x%.2f",
                  n, fx, fy, dx, dy, ix, iy, isc);
            inc_show_drawing(drw);
            add_native_ortho_view((ProDrawing)drw, (ProSolid)part, fx, fy, 1.0, mf, L"front");
            add_native_ortho_view((ProDrawing)drw, (ProSolid)part, fx + dx, fy, 1.0, ms, L"left");
            add_native_ortho_view((ProDrawing)drw, (ProSolid)part, fx, fy - dy, 1.0, mt, L"top");
            add_iso_view(drw, part, ix, iy, isc);
            logf_("[stdv] done");
        } else logf_("[stdv] retrieve failed part=%p drw=%p", (void *)part, (void *)drw);    } else if (!wcscmp(w, L"FILLTB")) {
        /* FILLTB：把标题�?5 个占位符注释文本替换成实际�?*/
        ProMdl part = NULL, drw = inc_drw();
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part && drw)
            fill_title_block(drw, part);
        else
            logf_("[filltb] retrieve failed");
    } else if (!wcsncmp(w, L"EDITNOTE|", 9)) {
        /* EDITNOTE|<旧文�?|<新文�?：按精确文本替换注记 */
        wchar_t frm[256], to[512];
        to[0] = 0;
        if (swscanf_s(w + 9, L"%255[^|]|%511[^|]", frm, 256, to, 512) == 2) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                int n = edit_note_text_match(drw, frm, to);
                logf_("[editnote] hits=%d", n);
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
            }
        } else {
            logf_("[editnote] bad args");
        }
    } else if (!wcscmp(w, L"TEXTPOS")) {
        /* TEXTPOS：打印关键尺寸的文本位置（排版收敛用�?*/
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_textpos_cb, NULL, (ProAppData)drw);
        else
            logf_("[textpos] retrieve failed");
    } else if (!wcscmp(w, L"BBOX")) {
        ProMdl  part = NULL;
        Pro3dPnt box[2];
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProSolidOutlineGet((ProSolid)part, box) == PRO_TK_NO_ERROR)
            logf_("[bbox] %.3f x %.3f x %.3f ; x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]",
                  box[1][0] - box[0][0], box[1][1] - box[0][1], box[1][2] - box[0][2],
                  box[0][0], box[1][0], box[0][1], box[1][1], box[0][2], box[1][2]);
        else
            logf_("[bbox] failed");
    } else if (!wcsncmp(w, L"ATTACH|", 7)) {
        double dv = 0;
        if (swscanf_s(w + 7, L"%lf", &dv) == 1) {
            ProMdl part = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                attach_ctx_t ac;
                ac.target = dv;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_attach_cb, NULL, &ac);
            }
        } else {
            logf_("[attach] bad args");
        }
    } else if (!wcsncmp(w, L"RESTOL|", 7)) {
        /* RESTOL|<�?|<ty>|<up>|<lo>：公差重显实验（erase + set + show�?*/
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 7, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL, drw = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                ProView *views = NULL;
                if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                    int vc = 0, i;
                    ProArraySizeGet((ProArray)views, &vc);
                    for (i = 0; i < vc; i++) {
                        restol_ctx_t rc;
                        rc.target = dv; rc.ty = ty; rc.up = up; rc.lo = lo;
                        rc.drw = (ProDrawing)drw; rc.view = views[i];
                        ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_restol_cb, NULL, &rc);
                    }
                    ProArrayFree((ProArray *)&views);
                }
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
            }
        } else {
            logf_("[restol] bad args");
        }
    } else if (!wcsncmp(w, L"CPFILE|", 7)) {
        /* CPFILE|<src>|<dst>：进程内复制（密文文件经授权进程读出后另存） */
        wchar_t *s = w + 7;
        wchar_t *bar = wcschr(s, L'|');
        if (bar) {
            FILE  *fi, *fo;
            *bar = 0;
            fi = _wfopen(s, L"rb");
            if (!fi) {
                logf_("[cpfile] open src failed");
            } else {
                fo = _wfopen(bar + 1, L"wb");
                if (!fo) {
                    fclose(fi);
                    logf_("[cpfile] open dst failed");
                } else {
                    char  buf[65536];
                    size_t n;
                    long  tot = 0;
                    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
                        fwrite(buf, 1, n, fo);
                        tot += (long)n;
                    }
                    fclose(fi);
                    fclose(fo);
                    logf_("[cpfile] copied %ld bytes", tot);
                }
            }
        } else {
            logf_("[cpfile] bad args");
        }
    } else if (!wcsncmp(w, L"HEAD|", 5)) {
        /* HEAD|<file>: read the first 64 bytes in-process and log ascii+hex */
        wchar_t *path = w + 5;
        FILE *f = _wfopen(path, L"rb");
        logf_("[head] '%ls' open=%p", path, (void *)f);
        if (f) {
            unsigned char b[64];
            size_t n = fread(b, 1, sizeof(b), f);
            char txt[65];
            char hex[160];
            size_t i;
            for (i = 0; i < n; i++) txt[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
            txt[n] = 0;
            hex[0] = 0;
            for (i = 0; i < n; i++) {
                char h[4];
                sprintf_s(h, sizeof(h), "%02X", (unsigned)b[i]);
                strcat_s(hex, sizeof(hex), h);
            }
            fclose(f);
            logf_("[head] bytes=%d ascii='%s'", (int)n, txt);
            logf_("[head] hex=%s", hex);
        }
    } else if (!wcsncmp(w, L"TAIL|", 5)) {
        /* TAIL|<file>: read the last 64 bytes in-process and log ascii+hex */
        wchar_t *path = w + 5;
        FILE *f = _wfopen(path, L"rb");
        logf_("[tail] '%ls' open=%p", path, (void *)f);
        if (f) {
            unsigned char b[64];
            size_t n = 0;
            if (fseek(f, -64, SEEK_END) == 0) n = fread(b, 1, sizeof(b), f);
            else { fseek(f, 0, SEEK_END); n = 0; }
            char txt[65];
            char hex[160];
            size_t i;
            for (i = 0; i < n; i++) txt[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
            txt[n] = 0;
            hex[0] = 0;
            for (i = 0; i < n; i++) {
                char h[4];
                sprintf_s(h, sizeof(h), "%02X", (unsigned)b[i]);
                strcat_s(hex, sizeof(hex), h);
            }
            fclose(f);
            logf_("[tail] bytes=%d ascii='%s'", (int)n, txt);
            logf_("[tail] hex=%s", hex);
        }
    } else if (!wcsncmp(w, L"ADDTOT", 6)) {
        /* ADDTOT[|x|y]：创建总长参考尺寸（默认位置看日志后可用 x,y 重试�?*/
        double lx = -1e9, ly = -1e9;
        wchar_t *rest = w + 6;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &lx, &ly);
        {
            ProMdl part = NULL, drw = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                if (lx < -1e8) {
                    /* 默认位置：主视图包围盒上�?*/
                    ProView *views = NULL;
                    if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                        int vc = 0, i;
                        ProArraySizeGet((ProArray)views, &vc);
                        for (i = 0; i < vc; i++) {
                            ProName nm;
                            nm[0] = 0;
                            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) {
                                ProPoint3d vb[2];
                                if (ProDrawingViewOutlineGet((ProDrawing)drw, views[i], vb) == PRO_TK_NO_ERROR) {
                                    lx = (vb[0][0] + vb[1][0]) / 2.0;
                                    ly = vb[1][1] + 15.0;
                                    logf_("[tot] view outline x[%.1f..%.1f] y[%.1f..%.1f]",
                                          vb[0][0], vb[1][0], vb[0][1], vb[1][1]);
                                }
                                break;
                            }
                        }
                        ProArrayFree((ProArray *)&views);
                    }
                    if (lx < -1e8) { lx = 250.0; ly = 640.0; }   /* 兜底 */
                }
                logf_("[tot] create at (%.1f, %.1f)", lx, ly);
                add_total_refdim(drw, part, lx, ly);
            }
        }
    } else if (!wcscmp(w, L"AXLINE")) {
        /* AXLINE：把模型轴转成图纸中心线（AxisToDtlaxis 安全版） */
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            axis_lines_to_views(drw, part);
    } else if (!wcsncmp(w, L"NATIVEFMT|", 10)) {
        ProMdl drw = NULL, fmt = NULL;
        ProName fn;
        wcsncpy_s(fn, PRO_NAME_SIZE, w + 10, _TRUNCATE);
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            ProError ef = ProMdlnameRetrieve(fn, PRO_MDLFILE_DWGFORM, &fmt);
            logf_("[nfmt] retrieve format '%ls' -> %d (%s) handle=%p", fn, (int)ef, errname(ef), (void *)fmt);
            if (ef == PRO_TK_NO_ERROR && fmt) {
                ProError ea = ProDrawingFormatAdd((ProDrawing)drw, 1, NULL, fmt, 1);
                logf_("[nfmt] FormatAdd sheet1 -> %d (%s)", (int)ea, errname(ea));
            }
        }
    } else if (!wcscmp(w, L"NATIVEDIAG")) {
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            int ns = -1, vc = -1, fmt = -1;
            double sc = -1.0;
            ProView *views = NULL;
            ProError e1 = ProDrawingSheetsCount((ProDrawing)drw, &ns);
            ProError e2 = ProDrawingScaleGet((ProDrawing)drw, NULL, 1, &sc);
            ProError e3 = ProDrawingViewsCollect((ProDrawing)drw, &views);
            ProError e4 = ProDrawingSheetFromFormatGet((ProDrawing)drw, 1, &fmt);
            if (e3 == PRO_TK_NO_ERROR && views) { ProArraySizeGet((ProArray)views, &vc); ProArrayFree((ProArray *)&views); }
            logf_("[ndiag] drw=%p part=%p sheets=%d(count->%d) scale=%.3f(get->%d) views=%d fmt_sheet=%d(get->%d)",
                  (void *)drw, (void *)part, ns, (int)e1, sc, (int)e2, vc, fmt, (int)e4);
            {
                ProError esc = ProDrawingScaleSet((ProDrawing)drw, NULL, 1, 1.0);
                logf_("[ndiag] scaleSet(1.0) -> %d (%s)", (int)esc, errname(esc));
            }
        }
    } else if (!wcsncmp(w, L"PROJVIEW|", 9)) {
        /* PROJVIEW|<parent name>|<x>|<y>[|<new name>] : create a PROJECTED view of
           an existing view (creo toolbar "projective view"). The position must be
           aligned with the parent (same y for a left/right projection, same x for
           up/down) or ProDrawingProjectedviewCreate returns BAD_PARENT. */
        char u8[512];
        char p8[64], n8[64];
        double ppx = 0.0, ppy = 0.0;
        wchar_t pwn[64], nwn[64];
        ProMdl drw = NULL;
        p8[0] = 0; n8[0] = 0; pwn[0] = 0; nwn[0] = 0;
        wide_to_u8(w + 9, u8, sizeof(u8));
        if (sscanf_s(u8, "%63[^|]|%lf|%lf|%63s", p8, (unsigned)sizeof(p8), &ppx, &ppy, n8, (unsigned)sizeof(n8)) >= 3) {
            u8_to_wide(p8, pwn, 64);
            if (n8[0]) u8_to_wide(n8, nwn, 64);
        }
        if (!pwn[0]) { logf_("[projview] bad args"); }
        else if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) != PRO_TK_NO_ERROR || !drw) {
            logf_("[projview] drawing retrieve failed");
        } else {
            ProView *vs = NULL;
            int vc = 0, i;
            ProView pv = NULL;
            if (ProDrawingViewsCollect((ProDrawing)drw, &vs) == PRO_TK_NO_ERROR && vs) {
                ProArraySizeGet((ProArray)vs, &vc);
                for (i = 0; i < vc; i++) {
                    ProName nm; nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, vs[i], nm);
                    if (!wcscmp(nm, pwn)) { pv = vs[i]; break; }
                }
                ProArrayFree((ProArray *)&vs);
            }
            if (!pv) { logf_("[projview] parent '%ls' not found", pwn); }
            else {
                ProPoint3d pos;
                ProView nv = NULL;
                ProError e;
                int wid = -1;
                ProMdlDisplay(drw);
                ProMdlWindowGet(drw, &wid);
                if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
                pos[0] = ppx; pos[1] = ppy; pos[2] = 0.0;
                e = ProDrawingProjectedviewCreate((ProDrawing)drw, pv, PRO_B_FALSE, pos, &nv);
                logf_("[projview] projected from '%ls' @(%.1f,%.1f) -> %d (%s) view=%p",
                      pwn, ppx, ppy, (int)e, errname(e), (void *)nv);
                if (e == PRO_TK_NO_ERROR && nv) {
                    if (nwn[0]) {
                        ProError en = ProDrawingViewNameSet((ProDrawing)drw, nv, nwn);
                        logf_("[projview] nameSet '%ls' -> %d (%s)", nwn, (int)en, errname(en));
                    }
                    {
                        ProError       eo;
                        ProPoint3d     o2[2];
                        double         sc2 = -1.0;
                        ProDrawingViewOutlineGet((ProDrawing)drw, nv, o2);
                        ProDrawingViewScaleGet((ProDrawing)drw, nv, &sc2);
                        logf_("[projview] outline=(%.1f,%.1f)-(%.1f,%.1f) scale=%.2f",
                              o2[0][0], o2[0][1], o2[1][0], o2[1][1], sc2);
                        (void)eo;
                    }
                    logf_("[projview] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
                }
            }
        }
    } else if (!wcsncmp(w, L"VIEWNAME|", 9)) {
        /* VIEWNAME|<old>|<new> : rename a drawing view (ProDrawingViewNameSet).
           Needed because 'S' in EDIM/CDIM matches the FIRST view whose name contains
           "left", so a mis-named strip steals the side-view target. */
        ProMdl drw = NULL;
        wchar_t oldn[128], newn[128];
        char u8[512];
        char o8[128], n8[128];
        oldn[0] = 0; newn[0] = 0; o8[0] = 0; n8[0] = 0;
        wide_to_u8(w + 9, u8, sizeof(u8));
        if (sscanf_s(u8, "%127[^|]|%127s", o8, (unsigned)sizeof(o8), n8, (unsigned)sizeof(n8)) == 2) {
            u8_to_wide(o8, oldn, 128);
            u8_to_wide(n8, newn, 128);
        }
        if (!oldn[0] || !newn[0]) { logf_("[viewname] bad args"); }
        else if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) != PRO_TK_NO_ERROR || !drw) {
            logf_("[viewname] drawing retrieve failed");
        } else {
            ProView *vs = NULL;
            int vc = 0, i, hit = -1;
            if (ProDrawingViewsCollect((ProDrawing)drw, &vs) == PRO_TK_NO_ERROR && vs) {
                ProArraySizeGet((ProArray)vs, &vc);
                for (i = 0; i < vc; i++) {
                    ProName nm; nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, vs[i], nm);
                    if (!wcscmp(nm, oldn)) { hit = i; break; }
                }
                if (hit >= 0) {
                    ProError en = ProDrawingViewNameSet((ProDrawing)drw, vs[hit], newn);
                    logf_("[viewname] '%ls' -> '%ls' = %d (%s)", oldn, newn, (int)en, errname(en));
                } else {
                    logf_("[viewname] '%ls' not found among %d views", oldn, vc);
                }
                ProArrayFree((ProArray *)&vs);
            }
        }
    } else if (!wcsncmp(w, L"VIEWADD|", 8)) {
        /* VIEWADD|<name>|<cx>|<cy>|<scale>|<F|E|S> : create ONE general ortho view
           with a chosen projection matrix and name it. Needed because NATIVEVIEWS
           only builds the fixed front/left/top triple and there is no standalone
           rename command.
             F: x<-Z  y<-X  dir<-Y   (front, 63.6 x 41.1)
             S: x<-X  y<-Y  dir<-Z   (true LEFT view, 41.1 x 4.55 -> 97.8 x 10.8)
             E: x<-Y  y<-Z  dir<-X   (edge/top strip, 4.55 x 63.6) */
        static const double mv_f[4][4] = {{0,0,1,0},{1,0,0,0},{0,-1,0,0},{0,0,0,1}};
        static const double mv_s[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,-1,0},{0,0,0,1}};
        static const double mv_e[4][4] = {{0,1,0,0},{0,0,1,0},{1,0,0,0},{0,0,0,1}};
        /* T: x<-Z, y<-Y, dir<-+X  - the MAIN view for parts whose thin axis is X
           (nb109_1: 7.55 thick in X, 19.44 wide in Y, 63.6 long in Z). */
        static const double mv_t[4][4] = {{0,0,1,0},{0,1,0,0},{-1,0,0,0},{0,0,0,1}};
        wchar_t vn[64];
        double vcx = 480.0, vcy = 520.0, vsc = 1.0;
        wchar_t vk2[8];
        ProMdl part = NULL, drw = NULL;
        vn[0] = 0; vk2[0] = 0;
        {
            char u8[512]; char nm8[64]; char k8[8];
            nm8[0] = 0; k8[0] = 0;
            wide_to_u8(w + 8, u8, sizeof(u8));
            if (sscanf_s(u8, "%63[^|]|%lf|%lf|%lf|%7s", nm8, (unsigned)sizeof(nm8), &vcx, &vcy, &vsc, k8, (unsigned)sizeof(k8)) >= 4) {
                u8_to_wide(nm8, vn, 64);
                u8_to_wide(k8, vk2, 8);
            }
        }
        if (!vn[0]) { logf_("[viewadd] bad args"); }
        else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[viewadd] part retrieve failed");
        } else if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) != PRO_TK_NO_ERROR || !drw) {
            logf_("[viewadd] drawing retrieve failed");
        } else {
            const double (*m)[4];
            m = (vk2[0] == L'S' || vk2[0] == L's') ? mv_s
              : (vk2[0] == L'E' || vk2[0] == L'e') ? mv_e
              : (vk2[0] == L'T' || vk2[0] == L't') ? mv_t : mv_f;
            logf_("[viewadd] '%ls' @(%.1f,%.1f) sc=%.2f kind=%ls", vn, vcx, vcy, vsc, vk2[0] ? vk2 : L"F");
            add_native_ortho_view((ProDrawing)drw, (ProSolid)part, vcx, vcy, vsc, m, vn);
            logf_("[viewadd] regen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
        }
    } else if (!wcscmp(w, L"NATIVEVIEW1")) {
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            static const double mf[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
            ProError esc = ProDrawingScaleSet((ProDrawing)drw, NULL, 1, 1.0);
            logf_("[nview1] scaleSet -> %d (%s)", (int)esc, errname(esc));
            add_native_ortho_view((ProDrawing)drw, (ProSolid)part, 380.0, 500.0, 1.0, mf, L"front");
        }
    } else if (!wcsncmp(w, L"NATIVEVIEWS", 11)) {
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            /* 2026-10-27: 支持 NATIVEVIEWS|<la> 显式指定最长轴 la=0(X)/1(Y)/2(Z)�?
               Creo 把视图矩阵按【列】解释（�?i �?= 屏幕 i 轴在模型坐标中的方向），
               所以下面的矩阵是按"�?写的。实测校准依据：�?m[0][2],m[1][0],m[2][1]
               得到的是 x←Y,y←Z,dir←X（即转置），故此处用转置形式�?*/
            int la = 0;
            double vsc = 1.0;
            if (w[11] == L'|') {
                int t = (int)wcstol(w + 12, NULL, 10);
                if (t >= 0 && t <= 2) la = t;
                {
                    const wchar_t *b2p = wcschr(w + 12, L'|');
                    if (b2p) vsc = wcstod(b2p + 1, NULL);
                }
            }
            logf_("[nativeviews] la=%d scale=%.3f", la, vsc);
            {   /* ProDrawingGeneralviewCreate needs the drawing DISPLAYED in a window,
                   otherwise it returns -2 BAD_INPUTS (same requirement VIEWS2 honours). */
                int wid0 = -1;
                ProError ed0 = ProMdlDisplay(drw);
                ProMdlWindowGet(drw, &wid0);
                if (wid0 > 0) { ProWindowActivate(wid0); ProWindowCurrentSet(wid0); }
                logf_("[nativeviews] display=%d win=%d (%s)", (int)ed0, wid0, errname(ed0));
            }
            {
                double mf[4][4], ms[4][4], mt[4][4];
                int i, j;
                for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) { mf[i][j] = 0.0; ms[i][j] = 0.0; mt[i][j] = 0.0; }
                mf[3][3] = ms[3][3] = mt[3][3] = 1.0;
                if (la == 2) {
                    /* 薄板：长 Z(63.58)、宽 X(41.10)、厚 Y(4.55)
                       2026-10-30：§419 的写法经实测是对的（mf 这样写得到 front 151.4x97.8）。
                       把 1 挪到列上反而变成薄片，所以保持原样，只保证 mt 不与 mf 撞车。 */
                    mf[0][1] = 1.0; mf[1][2] = 1.0; mf[2][0] = 1.0;      /* 主视 x←Z, y←X, dir←Y */
                    ms[2][0] = 1.0; ms[1][1] = 1.0; ms[0][2] = -1.0;     /* 侧视 x←Z, y←Y, dir←-X */
                    mt[0][2] = 1.0; mt[1][1] = 1.0; mt[2][0] = -1.0;     /* 俯视 x←Z, y←Y, dir←X */
                } else if (la == 1) {
                    mf[0][0] = 1.0; mf[1][2] = 1.0; mf[2][1] = -1.0;
                    ms[2][1] = 1.0; ms[1][2] = 1.0; ms[0][0] = 1.0;
                    mt[0][2] = 1.0; mt[1][0] = 1.0; mt[2][1] = 1.0;
                } else {
                    /* 长度�?X（原实现，nb109_2/4�?*/
                    mf[0][0] = 1.0; mf[1][1] = 1.0; mf[2][2] = 1.0;
                    ms[0][2] = 1.0; ms[1][1] = 1.0; ms[2][0] = -1.0;
                    mt[0][0] = 1.0; mt[1][2] = -1.0; mt[2][1] = 1.0;
                }
                add_native_ortho_view((ProDrawing)drw, (ProSolid)part, 300.0, 520.0, vsc, mf, L"front");
                add_native_ortho_view((ProDrawing)drw, (ProSolid)part, 595.0, 520.0, vsc, ms, L"left_1");
                add_native_ortho_view((ProDrawing)drw, (ProSolid)part, 300.0, 288.0, vsc, mt, L"top_1");
            }
        }
    } else if (!wcsncmp(w, L"FMTLOADED|", 10)) {
        /* FMTLOADED|<format name> : add company format to g_loaded_drw sheet 1 */
        ProName fn;
        ProMdl fmt = NULL;
        wcsncpy_s(fn, PRO_NAME_SIZE, w + 10, _TRUNCATE);
        if (!g_loaded_drw) {
            logf_("[fmtl] no loaded drawing");
        } else {
            ProError ef = ProMdlnameRetrieve(fn, PRO_MDLFILE_DWGFORM, &fmt);
            logf_("[fmtl] format '%ls' -> %d (%s) handle=%p", fn, (int)ef, errname(ef), (void *)fmt);
            if (ef == PRO_TK_NO_ERROR && fmt) {
                ProError ea = ProDrawingFormatAdd((ProDrawing)g_loaded_drw, 1, NULL, fmt, 1);
                logf_("[fmtl] FormatAdd sheet1 -> %d (%s)", (int)ea, errname(ea));
                ProDwgSheetRegenerate((ProDrawing)g_loaded_drw, 1);
            }
        }
    } else if (!wcsncmp(w, L"VIEWS2", 6)) {
        /* VIEWS2[|scale|cx|cy] : 3 ortho views centered at paper (cx,cy), mm units.
           Model origin is far from the geometry, so shift each view position by the
           bbox center projected onto that view's screen axes. */
        double sc = 1.0, cx = 150.0, cy = 210.0;
        ProMdl part = NULL;
        Pro3dPnt box[2];
        double xc, yc, zc;
        static const double m_main[4][4] = {{0,0,-1,0},{0,1,0,0},{1,0,0,0},{0,0,0,1}};
        static const double m_edge[4][4] = {{0,1,0,0},{0,0,-1,0},{1,0,0,0},{0,0,0,1}};
        static const double m_side[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,-1,0},{0,0,0,1}};
        if (w[6] == L'|') swscanf_s(w + 6, L"|%lf|%lf|%lf", &sc, &cx, &cy);
        if (!g_loaded_drw) {
            logf_("[views2] no loaded drawing");
        } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[views2] part '%ls' retrieve failed", g_def_model);
        } else if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) {
            logf_("[views2] outline failed");
        } else {
            ProDrawing drw = (ProDrawing)g_loaded_drw;
            int wid = -1;
            ProError ed = ProMdlDisplay(g_loaded_drw);
            ProMdlWindowGet(g_loaded_drw, &wid);
            if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
            logf_("[views2] display=%d win=%d", (int)ed, wid);
            xc = (box[0][0] + box[1][0]) / 2.0;
            yc = (box[0][1] + box[1][1]) / 2.0;
            zc = (box[0][2] + box[1][2]) / 2.0;
            logf_("[views2] bbox center=(%.3f,%.3f,%.3f) target=(%.1f,%.1f) sc=%.2f", xc, yc, zc, cx, cy, sc);
            ProDrawingScaleSet(drw, NULL, 1, sc);
            /* ProDrawingGeneralviewCreate position = outline center of the view
               (measured: STATE outline center == position). No origin offset. */
            logf_("[views2] centers: main=(%.1f,%.1f) top=(%.1f,%.1f) side=(%.1f,%.1f)",
                  cx, cy, cx, cy - 70.0, cx + 130.0, cy);
            add_native_ortho_view(drw, (ProSolid)part, cx, cy, sc, m_main, L"\u4E3B\u89C6\u56FE");
            add_native_ortho_view(drw, (ProSolid)part, cx, cy - 70.0, sc, m_edge, L"\u4FEF\u89C6\u56FE");
            add_native_ortho_view(drw, (ProSolid)part, cx + 130.0, cy, sc, m_side, L"\u5DE6\u89C6\u56FE");
            ProDwgSheetRegenerate(drw, 1);
        }
    } else if (!wcsncmp(w, L"VIEWSLOADED", 11)) {
        /* VIEWSLOADED[|scale|mx|my] : main/edge/side views on g_loaded_drw */
        double sc = 1.0, mx = 330.0, my = 560.0;
        ProMdl part = NULL;
        static const double m_main[4][4] = {{0,0,1,0},{0,1,0,0},{-1,0,0,0},{0,0,0,1}};
        static const double m_edge[4][4] = {{0,0,1,0},{1,0,0,0},{0,-1,0,0},{0,0,0,1}};
        static const double m_side[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
        if (w[11] == L'|') swscanf_s(w + 11, L"|%lf|%lf|%lf", &sc, &mx, &my);
        if (!g_loaded_drw) {
            logf_("[viewsl] no loaded drawing");
        } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[viewsl] part '%ls' retrieve failed", g_def_model);
        } else {
            ProDrawing drw = (ProDrawing)g_loaded_drw;
            ProError edp = ProMdlDisplay(g_loaded_drw);
            int wid = -1;
            ProError esc = ProDrawingScaleSet(drw, NULL, 1, sc);
            ProMdlWindowGet(g_loaded_drw, &wid);
            if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
            logf_("[viewsl] display=%d win=%d part='%ls' scaleSet(%.3f) -> %d (%s)",
                  (int)edp, wid, g_def_model, sc, (int)esc, errname(esc));
            ProDwgSheetRegenerate(drw, 1);
            add_native_ortho_view(drw, (ProSolid)part, mx, my, sc, m_main, L"\u4E3B\u89C6\u56FE");
            add_native_ortho_view(drw, (ProSolid)part, mx, my - 230.0, sc, m_edge, L"\u4FEF\u89C6\u56FE");
            add_native_ortho_view(drw, (ProSolid)part, mx + 230.0, my, sc, m_side, L"\u5DE6\u89C6\u56FE");
            ProDwgSheetRegenerate(drw, 1);
        }
    } else if (!wcsncmp(w, L"ISOLOADED", 9)) {
        /* ISOLOADED[|x|y|scale] : iso view on g_loaded_drw */
        double px = 900.0, py = 650.0, sc = 0.4;
        ProMdl part = NULL;
        if (w[9] == L'|') swscanf_s(w + 9, L"|%lf|%lf|%lf", &px, &py, &sc);
        if (!g_loaded_drw) {
            logf_("[isol] no loaded drawing");
        } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[isol] part '%ls' retrieve failed", g_def_model);
        } else {
            ProError edp = ProMdlDisplay(g_loaded_drw);
            int wid = -1;
            ProMdlWindowGet(g_loaded_drw, &wid);
            if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
            logf_("[isol] display=%d win=%d", (int)edp, wid);
            add_iso_view(g_loaded_drw, part, px, py, sc);
            ProDwgSheetRegenerate((ProDrawing)g_loaded_drw, 1);
        }
    } else if (!wcscmp(w, L"DISPLAYLOADED")) {
        /* display the loaded drawing so PDFCUR exports the drawing, not the part */
        if (g_loaded_drw) {
            int wid = -1;
            ProError ed = ProMdlDisplay(g_loaded_drw);
            ProError ew = ProMdlWindowGet(g_loaded_drw, &wid);
            if (wid > 0) { ProWindowActivate(wid); ProWindowCurrentSet(wid); }
            ProError eg = ProDwgSheetRegenerate((ProDrawing)g_loaded_drw, 1);
            logf_("[displ] display=%d winGet=%d win=%d regen=%d", (int)ed, (int)ew, wid, (int)eg);
        } else logf_("[displ] no loaded drawing");
    } else if (!wcsncmp(w, L"VMLOADED|", 9)) {
        /* VMLOADED|<idx>|<cx>|<cy> : move view on g_loaded_drw */
        int vi = -1;
        double cx = 0.0, cy = 0.0;
        if (g_loaded_drw && swscanf_s(w + 9, L"%d|%lf|%lf", &vi, &cx, &cy) == 3)
            viewmove_run(g_loaded_drw, vi, cx, cy);
        else
            logf_("[vml] bad args/no drawing");
    } else if (!wcsncmp(w, L"TITL2|", 6)) {
        /* TITL2|<drwnum> : fill title block params for the loaded drawing */
        char dn8[128];
        ProMdl part = NULL;
        wchar_t dn[PRO_NAME_SIZE];
        wcsncpy_s(dn, PRO_NAME_SIZE, w + 6, _TRUNCATE);
        wide_to_u8(dn, dn8, sizeof(dn8));
        if (!g_loaded_drw) {
            logf_("[titl2] no loaded drawing");
        } else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[titl2] part '%ls' retrieve failed", g_def_model);
        } else {
            set_string_param(part, L"PTC_COMMON_NAME", "\u8FDE\u63A5\u7247", "titl2-name");
            set_string_param(part, L"CNAME",  "\u8FDE\u63A5\u7247", "titl2-cname");
            set_string_param(part, L"MSTD",   "NB109", "titl2-mstd");
            set_string_param(part, L"DRWNUM", dn8, "titl2-drwnum");
            logf_("[titl2] drwnum='%s'", dn8);
            fill_title_block(g_loaded_drw, part);
        }
    } else if (!wcsncmp(w, L"TECH2|", 6)) {
        /* TECH2|x|y : technical requirements note on g_loaded_drw */
        double tx = 80.0, ty = 260.0;
        swscanf_s(w + 6, L"|%lf|%lf", &tx, &ty);
        if (g_loaded_drw) {
            add_technote_at(g_loaded_drw, tx, ty);
            logf_("[tech2] at (%.1f,%.1f)", tx, ty);
        } else logf_("[tech2] no loaded drawing");
    } else if (!wcsncmp(w, L"EBV|", 4)) {
        /* EBV|xmin|xmax|ymin|ymax|zmin|zmax : "Edges in BBox, Validated".
           LINESCAN iterates every edge of the part, which on multi-body models
           (copied geometry / patterns) also yields construction lines whose
           MIDPOINT happens to fall inside the solid - e.g. nb109_1 line 3377 with
           len=256 on a 7.55-thick part. Rule: keep an edge only when BOTH
           endpoints are inside the box and its length does not exceed the box
           diagonal. Those are the edges that can actually be seen in a view. */
        ProMdl part = NULL;
        double bx0 = -1e9, bx1 = 1e9, by0 = -1e9, by1 = 1e9, bz0 = -1e9, bz1 = 1e9, minlen = 0.0;
        int okargs = (swscanf_s(w + 4, L"%lf|%lf|%lf|%lf|%lf|%lf", &bx0, &bx1, &by0, &by1, &bz0, &bz1) == 6);
        {
            const wchar_t *p2 = w + 4;
            int bar = 0;
            for (; p2 && *p2; p2++) { if (*p2 == L'|') { bar++; if (bar == 6) { minlen = wcstod(p2 + 1, NULL); break; } } }
        }
        if (!okargs) { logf_("[ebv] bad args"); }
        else if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[ebv] part retrieve failed");
        } else {
            double dx = bx1 - bx0, dy = by1 - by0, dz = bz1 - bz0;
            double diag = sqrt(dx * dx + dy * dy + dz * dz) * 1.1;
            double tol = 0.6;
            int hid, n = 0, seen = 0;
            double longest = 0.0;
            logf_("[ebv] box x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f] diagLimit=%.2f minlen=%.2f",
                  bx0, bx1, by0, by1, bz0, bz1, diag, minlen);
            for (hid = 1; hid <= 6000 && n < 400; hid++) {
                ProEdge e = NULL;
                ProPoint3d a, b;
                ProVector d1, d2, dir;
                double len;
                int ina, inb;
                if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
                if (ProEdgeXyzdataEval(e, 0.0, a, d1, d2, dir) != PRO_TK_NO_ERROR) continue;
                if (ProEdgeXyzdataEval(e, 1.0, b, d1, d2, dir) != PRO_TK_NO_ERROR) continue;
                seen++;
                ina = (a[0] >= bx0 - tol && a[0] <= bx1 + tol && a[1] >= by0 - tol && a[1] <= by1 + tol && a[2] >= bz0 - tol && a[2] <= bz1 + tol);
                inb = (b[0] >= bx0 - tol && b[0] <= bx1 + tol && b[1] >= by0 - tol && b[1] <= by1 + tol && b[2] >= bz0 - tol && b[2] <= bz1 + tol);
                if (!ina || !inb) continue;
                len = sqrt((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) + (b[2]-a[2])*(b[2]-a[2]));
                if (len > diag) continue;
                if (len < minlen) continue;
                if (len > longest) longest = len;
                logf_("[ebv] id=%d a=(%.3f,%.3f,%.3f) b=(%.3f,%.3f,%.3f) len=%.3f", hid, a[0], a[1], a[2], b[0], b[1], b[2], len);
                n++;
            }
            logf_("[ebv] kept=%d scanned=%d longest=%.3f", n, seen, longest);
        }
    } else if (!wcsncmp(w, L"EALL", 4)) {
        /* EALL[|ymin|ymax] : dump EVERY edge (any curve type) whose endpoint reaches
           the given y band, with exact endpoints - used to locate smooth-surface
           extremes (bump apex / cup bottom) that LINESCAN and EDGESCAN cannot see. */
        ProMdl part = NULL;
        double y0 = -1e9, y1 = 1e9;
        if (w[4] == L'|') swscanf_s(w + 5, L"%lf|%lf", &y0, &y1);
        if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[eall] part retrieve failed");
        } else {
            int hid, n = 0;
            double ymax = -1e9, ymin = 1e9;
            for (hid = 1; hid <= 6000 && n < 500; hid++) {
                ProEdge e = NULL;
                ProPoint3d a, b;
                ProVector d1, d2, dir;
                int ok;

                if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
                ok = 0;
                if (ProEdgeXyzdataEval(e, 0.0, a, d1, d2, dir) == PRO_TK_NO_ERROR &&
                    ProEdgeXyzdataEval(e, 1.0, b, d1, d2, dir) == PRO_TK_NO_ERROR) ok = 1;
                if (!ok) continue;
                if (a[1] > ymax) ymax = a[1];
                if (b[1] > ymax) ymax = b[1];
                if (a[1] < ymin) ymin = a[1];
                if (b[1] < ymin) ymin = b[1];
                if (a[1] >= y0 && a[1] <= y1) {
                    logf_("[eall] id=%d a=(%.3f,%.3f,%.3f) b=(%.3f,%.3f,%.3f)", hid, a[0], a[1], a[2], b[0], b[1], b[2]);
                    n++;
                } else if (b[1] >= y0 && b[1] <= y1) {
                    logf_("[eall] id=%d a=(%.3f,%.3f,%.3f) b=(%.3f,%.3f,%.3f)", hid, a[0], a[1], a[2], b[0], b[1], b[2]);
                    n++;
                }
            }
            logf_("[eall] total=%d band y[%.2f..%.2f]  edge-vertex y range [%.3f..%.3f]", n, y0, y1, ymin, ymax);
        }
    } else if (!wcsncmp(w, L"LINESCAN", 8)) {
        /* LINESCAN[|minlen] : dump straight edges (id, midpoint, dir, length) */
        ProMdl part = NULL;
        double minlen = 2.0;
        if (w[8] == L'|') swscanf_s(w + 9, L"%lf", &minlen);
        if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
            logf_("[linescan] part '%ls' retrieve failed", g_def_model);
        } else {
            int hid, n = 0;
            for (hid = 1; hid <= 5000 && n < 400; hid++) {
                ProEdge e = NULL;
                ProGeomitem gi;
                ProGeomitemdata *gd = NULL;
                if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
                if (ProEdgeToGeomitem((ProSolid)part, e, &gi) != PRO_TK_NO_ERROR) continue;
                if (ProGeomitemdataGet(&gi, &gd) != PRO_TK_NO_ERROR || !gd || !gd->data.p_curve_data) {
                    if (gd) ProGeomitemdataFree(&gd);
                    continue;
                }
                if (gd->data.p_curve_data->line.type == PRO_ENT_LINE) {
                    ProPoint3d a, b;
                    double dx, dy, dz, len;
                    memcpy(a, gd->data.p_curve_data->line.end1, sizeof(ProPoint3d));
                    memcpy(b, gd->data.p_curve_data->line.end2, sizeof(ProPoint3d));
                    dx = b[0] - a[0]; dy = b[1] - a[1]; dz = b[2] - a[2];
                    len = sqrt(dx*dx + dy*dy + dz*dz);
                    if (len >= minlen) {
                        logf_("[linescan] id=%d mid=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f) len=%.3f",
                              hid, (a[0]+b[0])/2.0, (a[1]+b[1])/2.0, (a[2]+b[2])/2.0,
                              dx/len, dy/len, dz/len, len);
                        n++;
                    }
                }
                ProGeomitemdataFree(&gd);
            }
            logf_("[linescan] total=%d (minlen=%.2f)", n, minlen);
        }
    } else if (!wcsncmp(w, L"CROSSDIM", 8)) {
        /* CROSSDIM|1 = vertical centerline spacing (23), CROSSDIM|2 = horizontal (23.5) */
        int mode = 1;
        if (w[8] == L'|') swscanf_s(w + 9, L"%d", &mode);
        {
            ProMdl part = NULL, drw = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                logf_("[crossdim] mode=%d", mode);
                /* 2026-10-30: g_abv[0].v may still hold a view handle from an older
                   drawing session; ProAnnotationShow then returns -2 BAD_INPUTS and
                   the dimension is created but never shown. Refresh the cached
                   front-view handle from the live drawing before creating. */
                {
                    ProView *vs = NULL;
                    if (ProDrawingViewsCollect((ProDrawing)drw, &vs) == PRO_TK_NO_ERROR && vs) {
                        int vc2 = 0, q2;
                        ProArraySizeGet((ProArray)vs, &vc2);
                        for (q2 = 0; q2 < vc2; q2++) {
                            ProName nm2;
                            nm2[0] = 0;
                            ProDrawingViewNameGet((ProDrawing)drw, vs[q2], nm2);
                            if (wcsstr(nm2, L"front") || wcsstr(nm2, L"FRONT")) {
                                g_abv[0].v = vs[q2];
                                logf_("[crossdim] refreshed front view handle %p", (void *)vs[q2]);
                                break;
                            }
                        }
                        ProArrayFree((ProArray *)&vs);
                    }
                }
                boss_cross_dims(part, drw, 0, mode);
            }
        }
    } else if (!wcsncmp(w, L"ISO", 3)) {
        /* ISO[|x|y[|scale[|wx,wy,wz]]] : axis view (default (700,630) x0.5) */
        double px = 700.0, py = 630.0, sc = 0.5;
        wchar_t *rest = w + 3;
        if (rest[0] == L'|') {
            int got = swscanf_s(rest + 1, L"%lf|%lf|%lf", &px, &py, &sc);
            (void)got;
            {
                /* optional 4th field = eye direction in model coords */
                const wchar_t *p = rest + 1;
                int bar = 0;
                for (; p && *p; p++) {
                    if (*p == L'|') {
                        bar++;
                        if (bar == 3) {
                            double a = 0.0, b = 0.0, c = 0.0;
                            if (swscanf_s(p + 1, L"%lf,%lf,%lf", &a, &b, &c) == 3) {
                                g_iso_dir[0] = a; g_iso_dir[1] = b; g_iso_dir[2] = c;
                                logf_("[iso] dir override (%.3f,%.3f,%.3f)", a, b, c);
                            }
                            break;
                        }
                    }
                }
            }
        }
        {
            ProMdl part = NULL, drw = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                add_iso_view(drw, part, px, py, sc);
        }
    } else if (!wcsncmp(w, L"XEDGE|", 6)) {
        /* XEDGE|axis|minmax : read-only. Report the edge that actually reaches
           the extreme along model axis (0=X 1=Y 2=Z), minmax 0=min 1=max.
           nb109_1's outline is a spline: no vertex sits at z=50.532, so
           EALL/EBV vertex picking finds nothing there and TOTFACE picks the
           wrong pair (both ends on the same side, dim value 0.0096). */
        int ax = 2, wantmax = 1;
        ProMdl part = NULL;
        swscanf_s(w + 6, L"%d|%d", &ax, &wantmax);
        if (ax < 0 || ax > 2) ax = 2;
        {
            /* optional bbox filter: XEDGE|axis|minmax|x0|x1|y0|y1|z0|z1
               Without it the scan hits copied-geometry ghost edges that sit
               hundreds of mm away from the real part. */
            double bx0 = -1e9, bx1 = 1e9, by0 = -1e9, by1 = 1e9, bz0 = -1e9, bz1 = 1e9;
            const wchar_t *p = w + 6;
            int bar = 0;
            for (; p && *p; p++) if (*p == L'|') bar++;
            if (bar >= 7) {
                swscanf_s(w + 6, L"%d|%d|%lf|%lf|%lf|%lf|%lf|%lf",
                          &ax, &wantmax, &bx0, &bx1, &by0, &by1, &bz0, &bz1);
                logf_("[xedge] box x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]", bx0, bx1, by0, by1, bz0, bz1);
            }
            if (retrieve_model_any(g_def_model, &part) != PRO_TK_NO_ERROR || !part) {
                logf_("[xedge] part retrieve failed");
            } else {
                int hid, best = -1, second = -1, bestk = 0, nkeep = 0;
                double vbest = wantmax ? -1e30 : 1e30;
                double v2 = vbest;
                ProPoint3d pb, p2;
                pb[0] = pb[1] = pb[2] = 0.0;
                p2[0] = p2[1] = p2[2] = 0.0;
                for (hid = 1; hid <= 8000; hid++) {
                    ProEdge e = NULL;
                    int k, bk = -1, inside = 1;
                    double bv = wantmax ? -1e30 : 1e30;
                    ProPoint3d bp;
                    bp[0] = bp[1] = bp[2] = 0.0;
                    if (ProEdgeInit((ProSolid)part, hid, &e) != PRO_TK_NO_ERROR || !e) continue;
                    for (k = 0; k <= 16; k++) {
                        ProPoint3d p;
                        ProVector d1, d2, dir;
                        if (ProEdgeXyzdataEval(e, k / 16.0, p, d1, d2, dir) != PRO_TK_NO_ERROR) continue;
                        if (p[0] < bx0 - 0.6 || p[0] > bx1 + 0.6 || p[1] < by0 - 0.6 || p[1] > by1 + 0.6 ||
                            p[2] < bz0 - 0.6 || p[2] > bz1 + 0.6) { inside = 0; break; }
                        if ((wantmax && p[ax] > bv) || (!wantmax && p[ax] < bv)) {
                            bv = p[ax]; bk = k;
                            bp[0] = p[0]; bp[1] = p[1]; bp[2] = p[2];
                        }
                    }
                    if (!inside || bk < 0) continue;
                    nkeep++;
                    if ((wantmax && bv > vbest) || (!wantmax && bv < vbest)) {
                        v2 = vbest; second = best;
                        p2[0] = pb[0]; p2[1] = pb[1]; p2[2] = pb[2];
                        vbest = bv; best = hid; bestk = bk;
                        pb[0] = bp[0]; pb[1] = bp[1]; pb[2] = bp[2];
                    } else if ((wantmax && bv > v2) || (!wantmax && bv < v2)) {
                        v2 = bv; second = hid;
                        p2[0] = bp[0]; p2[1] = bp[1]; p2[2] = bp[2];
                    }
                }
                logf_("[xedge] axis=%d %s kept=%d best id=%d t=%.3f v=%.4f p=(%.3f,%.3f,%.3f)",
                      ax, wantmax ? "MAX" : "MIN", nkeep, best, bestk / 16.0, vbest, pb[0], pb[1], pb[2]);
                logf_("[xedge] axis=%d %s 2nd  id=%d v=%.4f p=(%.3f,%.3f,%.3f)",
                      ax, wantmax ? "MAX" : "MIN", second, v2, p2[0], p2[1], p2[2]);
            }
        }
    } else if (!wcsncmp(w, L"SAVELOCAL|", 10)) {
        /* SAVELOCAL|<dir> : copy the loaded drawing into <dir> via
           ProMdlnameBackup. ProMdlSave() hangs in this environment (the
           Windchill/uwgm hook blocks the call on the command thread), while
           backup is a plain file copy. The caller then renames the file to
           the next .drw revision. */
        wchar_t bdir[PRO_PATH_SIZE];
        ProMdl drw = g_loaded_drw;
        ProError eb;
        wcsncpy_s(bdir, PRO_PATH_SIZE, w + 10, _TRUNCATE);
        if (!drw) {
            ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
            logf_("[savelocal] retrieve('%ls') -> %d (%s)", g_def_model, (int)er, errname(er));
        }
        if (!drw) {
            logf_("[savelocal] no drawing");
        } else {
            CreateDirectoryW(bdir, NULL);
            eb = ProMdlnameBackup(drw, bdir);
            logf_("[savelocal] ProMdlnameBackup('%ls') -> %d (%s)", bdir, (int)eb, errname(eb));
        }
    } else if (!wcsncmp(w, L"TOTFACE", 7)) {
        /* TOTFACE[|x|y]：总长参考尺寸（面版，ref_dim=PRO_B_FALSE�?
           注意：不能叫 ADDTOTF —�?命令链里 ADDTOT(6字符) 会先吃掉这个前缀�?*/
        double lx = -1e9, ly = -1e9;
        wchar_t *rest = w + 7;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &lx, &ly);
        {
            ProMdl part = NULL, drw = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                if (lx < -1e8) {
                    ProView *views = NULL;
                    if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                        int vc = 0, i;
                        ProArraySizeGet((ProArray)views, &vc);
                        for (i = 0; i < vc; i++) {
                            ProName nm;
                            nm[0] = 0;
                            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) {
                                ProPoint3d vb[2];
                                if (ProDrawingViewOutlineGet((ProDrawing)drw, views[i], vb) == PRO_TK_NO_ERROR) {
                                    lx = (vb[0][0] + vb[1][0]) / 2.0;
                                    ly = vb[1][1] + 15.0;
                                }
                                break;
                            }
                        }
                        ProArrayFree((ProArray *)&views);
                    }
                    if (lx < -1e8) { lx = 385.0; ly = 561.0; }
                }
                logf_("[totf] create at (%.1f, %.1f)", lx, ly);
                /* 轮次25：图纸侧创建（官方附件结构）；旧�?add_total_dim_model（模型侧+错误结构）停�?*/
                tot_v2(drw, part, lx, ly);
            }
        }
    } else if (!wcscmp(w, L"PLACEDIMS")) {
        /* PLACEDIMS：命令通道里单独跑尺寸摆放（验证通过后才考虑接入管线�?*/
        ProMdl part = NULL, drw = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            place_dims(drw, part);
        else
            logf_("[place] retrieve failed");
    } else if (!wcscmp(w, L"CROSSINFO")) {
        ProMdl drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            ProDtlentity *ents = NULL;
            int cnt = 0, i;
            if (ProDrawingDtlentitiesCollect((ProDrawing)drw, NULL, 1, &ents) != PRO_TK_NO_ERROR || !ents) {
                logf_("[crossinfo] collect failed");
            } else {
                ProArraySizeGet((ProArray)ents, &cnt);
                logf_("[crossinfo] entities=%d", cnt);
                for (i = 0; i < cnt; i++) {
                    ProDtlentitydata d = NULL;
                    int id = -1;
                    ProName fnt;
                    double w = -1.0;
                    if (ProDtlentityDataGet(&ents[i], NULL, &d) != PRO_TK_NO_ERROR || !d) continue;
                    ProDtlentitydataIdGet(d, &id);
                    fnt[0] = 0;
                    ProDtlentitydataFontGet(d, fnt);
                    ProDtlentitydataWidthGet(d, &w);
                    {
                        ProCurvedata cd;
                        memset(&cd, 0, sizeof(cd));
                        if (ProDtlentitydataCurveGet(d, &cd) == PRO_TK_NO_ERROR && cd.line.type == PRO_ENT_LINE) {
                            logf_("[crossinfo] id=%d font='%ls' width=%.3f line=(%.1f,%.1f)-(%.1f,%.1f)",
                                  id, fnt, w, cd.line.end1[0], cd.line.end1[1], cd.line.end2[0], cd.line.end2[1]);
                        } else {
                            logf_("[crossinfo] id=%d font='%ls' width=%.3f type=nonline", id, fnt, w);
                        }
                    }
                    ProDtlentitydataFree(d);
                }
                ProArrayFree((ProArray *)&ents);
            }
        }
    } else if (!wcsncmp(w, L"CROSSES", 7)) {
        /* CROSSES[|ax|ay]: ax/ay = model axis shown as paper x/y (0=X 1=Y 2=Z) */
        int ax = 0, ay = 1;
        if (w[7] == L'|') {
            int got = swscanf_s(w + 8, L"%d|%d", &ax, &ay);
            if (got < 1) { ax = 0; ay = 1; }
            else if (got < 2) { ay = (ax == 1) ? 0 : 1; }
        }
        {
            ProMdl part = NULL, drw = NULL;
            logf_("[cross] CROSSES request ax=%d ay=%d", ax, ay);
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                boss_centerlines(drw, part, ax, ay);
            else
                logf_("[cross] retrieve failed");
        }
    } else if (!wcsncmp(w, L"CYLSCAN", 7)) {
        /* CYLSCAN[|rmin|rmax]：列举圆柱面；带范围时给范围内的圆柱面建基准�?*/
        double rmin = -1.0, rmax = -1.0;
        wchar_t *rest = w + 7;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &rmin, &rmax);
        {
            ProMdl part = NULL;
            if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
                cylscan((ProSolid)part, rmin, rmax);
            else
                logf_("[cyl] part retrieve failed");
        }
    } else if (!wcsncmp(w, L"FEATID", 6)) {
        /* FEATID：特征驱动探针（纯几何识别，不引用任何尺寸值） */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            featid_run(part);
        else
            logf_("[feat] part retrieve failed");
    } else if (!wcsncmp(w, L"EDGESCAN|", 9)) {
        double rmin = 3.0, rmax = 5.0;
        ProMdl part = NULL;
        swscanf_s(w + 9, L"%lf|%lf", &rmin, &rmax);
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            edgescan_run(part, rmin, rmax);
        else
            logf_("[edgescan] part retrieve failed");
    } else if (!wcsncmp(w, L"EINFO|", 6)) {
        /* EINFO|id1|id2|...：逐条打印边几何（线端/圆中心半径法向/弧） */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            einfo_run(part, w + 6);
        else
            logf_("[einfo] part retrieve failed");
    } else if (!wcsncmp(w, L"EDGEOWN|", 8)) {
        /* EDGEOWN|id1|id2|...：查每条边由哪个特征创建（排除复制几何/面组的鬼边） */
        ProMdl part = NULL;
        if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part)
            edgeown_run(part, w + 8);
        else
            logf_("[edgeown] part retrieve failed");
    } else if (!wcsncmp(w, L"CMDPROBE|", 9)) {
        /* CMDPROBE|<command name>: test-load a Creo UI command macro (no execute) */
        cmdprobe_run(w + 9);
    } else if (!wcsncmp(w, L"SNOTE|", 6)) {
        /* SNOTE|x|y|text */
        double nx = 0.0, ny = 0.0, nh = 3.5;
        char   tmp[512], tb8[256];
        ProMdl drw = NULL;

        tb8[0] = 0;
        wide_to_u8(w + 6, tmp, sizeof(tmp));
        nh = 3.5;
        if (sscanf_s(tmp, "%lf|%lf|%255[^|]|%lf", &nx, &ny, tb8, (unsigned)sizeof(tb8), &nh) >= 2 &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
            wchar_t wt[256];
            int okT = u8_to_wide(tb8, wt, 256);

            /* height 0 => use the drawing default (dtl text_height); no text style */
            snote_run(drw, nx, ny, okT ? wt : L"3.2", (nh >= 0.0 && nh < 20.0) ? nh : 3.5);
        } else {
            logf_("[snote] bad args");
        }
    } else if (!wcsncmp(w, L"ROUGHGRP", 8)) {
        /* ROUGHGRP[|cat|name]: dump the symbol group tree + vars after GROUP_ALL */
        ProMdl drw = NULL;
        wchar_t wc0[80], wn0[80];
        int okC = 0, okN = 0;

        wc0[0] = 0; wn0[0] = 0;
        {
            const wchar_t *p = w + 8;

            if (p && *p == L'|') {
                const wchar_t *bar = wcschr(p + 1, L'|');

                if (bar) {
                    size_t lc = (size_t)(bar - (p + 1)), ln = wcslen(bar + 1);

                    if (lc > 79) lc = 79;
                    if (ln > 79) ln = 79;
                    wcsncpy_s(wc0, 80, p + 1, lc); wc0[lc] = 0;
                    wcsncpy_s(wn0, 80, bar + 1, ln); wn0[ln] = 0;
                    okC = (lc > 0); okN = (ln > 0);
                }
            }
        }
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            roughgrp_run(drw, okC ? wc0 : L"", okN ? wn0 : L"");
        else
            logf_("[grp] drw retrieve failed");
    } else if (!wcsncmp(w, L"SYMDEFDEL|", 10)) {
        /* SYMDEFDEL|cat|name: delete a cached drawing symbol definition (read/cleanup) */
        wchar_t dc[256], dn[128];
        ProMdl dd = inc_drw();

        dc[0] = dn[0] = 0;
        if (swscanf_s(w + 10, L"%255[^|]|%127[^|]", dc, 256, dn, 128) == 2 && dd) {
            ProDtlsymdef def;
            ProError    er;

            memset(&def, 0, sizeof(def));
            er = ProDrawingDtlsymdefRetrieve((ProDrawing)dd, dc, dn, 0, PRO_B_TRUE, &def);
            logf_("[symdefdel] retrieve('%ls','%ls',v0) -> %d (%s)", dc, dn, (int)er, errname(er));
            if (er != PRO_TK_NO_ERROR)
                er = ProDrawingDtlsymdefRetrieve((ProDrawing)dd, dc, dn, 1, PRO_B_TRUE, &def);
            if (er == PRO_TK_NO_ERROR) {
                ProDtlsymdefdata sd = NULL;
                double h = -1.0;

                if (ProDtlsymdefDataGet(&def, &sd) == PRO_TK_NO_ERROR && sd)
                    ProDtlsymdefdataScaledheightGet(sd, &h);
                logf_("[symdefdel] defHeight before delete = %.3f", h);
                {
                    ProError ed = ProDtlsymdefDelete(&def);
                    logf_("[symdefdel] ProDtlsymdefDelete -> %d (%s)", (int)ed, errname(ed));
                }
            }
        } else logf_("[symdefdel] bad args");
    } else if (!wcsncmp(w, L"RSYM2|", 6)) {
        /* RSYM2|x|y|v1|v2|height|cat|name[|groups]：组默认 30,31 */
        double rx = 0.0, ry = 0.0, rh = 31.475;
        wchar_t rv1[128], rv2[128], rcat[256], rname[128], rg[64];
        ProMdl rd = inc_drw();
        int nf;

        rv1[0] = rv2[0] = rcat[0] = rname[0] = 0;
        wcscpy_s(rg, 64, L"30,31");
        nf = swscanf_s(w + 6, L"%lf|%lf|%127[^|]|%127[^|]|%lf|%255[^|]|%127[^|]",
                       &rx, &ry, rv1, 128, rv2, 128, &rh, rcat, 256, rname, 128);
        if (nf >= 5 && rd) {
            const wchar_t *p = w + 6;
            int bars = 0;
            const wchar_t *g8 = NULL;

            for (; *p; p++) {
                if (*p == L'|') { bars++; if (bars == 7) { g8 = p + 1; break; } }
            }
            if (g8 && *g8) wcsncpy_s(rg, 64, g8, _TRUNCATE);
            rsym2_run(rd, rx, ry, rv1, rv2, rh, rg, rcat, rname);
        } else
            logf_("[rsym2] bad args");
    } else if (!wcsncmp(w, L"ROUGHSYM|", 9)) {
        /* ROUGHSYM|x|y|value|method|groups|height[|cat|name] : surface-finish symbol */
        double rx = 0.0, ry = 0.0;
        char v8[64], m8[64], g8[80], c8[80], n8[80];
        double rh = 3.5;
        char tmp[512];
        ProMdl drw = NULL;

        v8[0] = 0; m8[0] = 0; g8[0] = 0; c8[0] = 0; n8[0] = 0;
        wide_to_u8(w + 9, tmp, sizeof(tmp));
        rh = 3.5;
        if (sscanf_s(tmp, "%lf|%lf|%63[^|]|%63[^|]|%63[^|]|%lf|%63[^|]|%63[^|]",
                     &rx, &ry, v8, (unsigned)sizeof(v8), m8, (unsigned)sizeof(m8),
                     g8, (unsigned)sizeof(g8), &rh, c8, (unsigned)sizeof(c8), n8, (unsigned)sizeof(n8)) >= 3) {
            wchar_t wv[64], wm[64], wg[80], wc8[80], wn8[80];
            int ok1 = u8_to_wide(v8, wv, 64), ok2 = u8_to_wide(m8, wm, 64), ok3 = g8[0] ? u8_to_wide(g8, wg, 80) : 0;
            int ok4 = c8[0] ? u8_to_wide(c8, wc8, 80) : 0, ok5 = n8[0] ? u8_to_wide(n8, wn8, 80) : 0;

            drw = inc_drw();
            if (!drw) { logf_("[rough] drw retrieve failed"); return; }
            roughsym_run(drw, rx, ry, ok1 ? wv : L"3.2", ok2 ? wm : L"NO_REMOVAL", ok3 ? wg : L"ALL",
                         (rh > 0.5 && rh < 20.0) ? rh : 3.5, ok4 ? wc8 : L"", ok5 ? wn8 : L"");
        } else {
            logf_("[rough] bad args");
        }
    } else if (!wcsncmp(w, L"ROUGHSET|", 9)) {
        /* ROUGHSET|x|y|value|height：面板同款组 + VartextsSet(3.2/冲裁�? */
        double rx = 0.0, ry = 0.0, rh = 12.0;
        char v8[64];
        char tmp[256];
        v8[0] = 0;
        wide_to_u8(w + 9, tmp, sizeof(tmp));
        if (sscanf_s(tmp, "%lf|%lf|%63[^|]|%lf", &rx, &ry, v8, (unsigned)sizeof(v8), &rh) >= 2) {
            wchar_t wv[64];
            if (!u8_to_wide(v8, wv, 64) || !wv[0]) wcscpy_s(wv, 64, L"3.2");
            g_rough_set_mode = 1;
            roughsym_run(inc_drw(), rx, ry, wv, L"NO_REMOVAL", L"CUSTOM:28,30,38,39,40",
                         (rh > 0.5 && rh < 20.0) ? rh : 12.0,
                         L"D:\\PiCreoClient\\PiDrawing\\symbols\\surface_finish", L"pi_roughness_old");
            g_rough_set_mode = 0;
        } else {
            logf_("[rough] ROUGHSET bad args");
        }
    } else if (!wcsncmp(w, L"CROSSSTYLE|", 11)) {
        /* CROSSSTYLE|<font>: set the line-style font on all draft entities (12 cross lines) */
        ProMdl drw = NULL;

        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            crossstyle_run(drw, w + 11);
        else
            logf_("[xstyle] drw retrieve failed");
    } else if (!wcsncmp(w, L"TECHNOTE|", 9)) {
        /* TECHNOTE|x|y：把"技术要�?注释块移�?(x,y)（屏幕像素，origin 左下；幂等） */
        double tx = 0.0, ty = 0.0;
        ProMdl drw = NULL;

        if (swscanf_s(w + 9, L"%lf|%lf", &tx, &ty) == 2 &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            technote_run(drw, tx, ty);
        else
            logf_("[tech] TECHNOTE bad args / drw retrieve failed");
    } else if (!wcsncmp(w, L"R57TRY|", 7)) {
        /* R57TRY|<what>|<variant>：试验尺寸变体（what=1 dDIMV �?hint 组合；what=2 总厚附件/视图组合�?*/
        int what = 0, v = 0;
        ProMdl part = NULL, drw = NULL;

        if (swscanf_s(w + 7, L"%d|%d", &what, &v) == 2 &&
            retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            r57_try(part, drw, what, v);
        else
            logf_("[try] args/retrieve failed");
    } else if (!wcsncmp(w, L"DBOUND|", 7)) {
        int di = 0; double dr = 0;
        ProMdl dd = inc_drw();
        if (swscanf_s(w + 7, L"%d|%lf", &di, &dr) == 2 && dd) dbound_run(dd, di, dr);
        else logf_("[dbound] bad args");
    } else if (!wcsncmp(w, L"DETAIL2|", 8)) {
        int di = 0;
        double dcx = 0, dcy = 0, dr = 0, dpx = 0, dpy = 0, dsc = 1.0;
        ProMdl dd = inc_drw();
        ProMdl dp = NULL;

        if (swscanf_s(w + 8, L"%d|%lf|%lf|%lf|%lf|%lf|%lf", &di, &dcx, &dcy, &dr, &dpx, &dpy, &dsc) == 7 &&
            dd && ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &dp) == PRO_TK_NO_ERROR && dp)
            detail2_run(dd, dp, di, dcx, dcy, dr, dpx, dpy, dsc);
        else
            logf_("[detail2] bad args");
    } else if (!wcsncmp(w, L"DETAIL|", 7)) {
        /* DETAIL|parentIdx|cx|cy|r|px|py|scale */
        int di = 0;
        double dcx = 0, dcy = 0, dr = 0, dpx = 0, dpy = 0, dsc = 1.0;
        ProMdl dd = inc_drw();
        ProMdl dp = NULL;

        if (swscanf_s(w + 7, L"%d|%lf|%lf|%lf|%lf|%lf|%lf", &di, &dcx, &dcy, &dr, &dpx, &dpy, &dsc) == 7 &&
            dd && ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &dp) == PRO_TK_NO_ERROR && dp)
            detail_run(dd, dp, di, dcx, dcy, dr, dpx, dpy, dsc);
        else
            logf_("[detail] bad args");
    } else if (!wcsncmp(w, L"LAYOUT", 6)) {
        /* LAYOUT=报告；LAYOUT|go=按包围盒移动视图 */
        int go = (w[6] == L'|' && (w[7] == L'g' || w[7] == L'G'));
        ProMdl drw = inc_drw();
        if (drw)
            layout_report(drw, go);
        else
            logf_("[layout] drawing not found");
    } else if (!wcsncmp(w, L"VIEWPOS|", 8)) {
        /* VIEWPOS|<索引>|<目标outline中心x>|<目标outline中心y>�?
           把视�?[索引] 平移到目�?outline 中心（屏幕单位）�?
           换算：Δorigin = Δscreen / (2.3805 × scale)（scale 用视图自己的比例，轴�?0.5）�?
           �?regen —�?regen 会把尺寸文本拉回默认位；尺寸归位由随后一�?PLACEDIMS 完成�?*/
        int vi = -1;
        double cx = 0.0, cy = 0.0;
        if (swscanf_s(w + 8, L"%d|%lf|%lf", &vi, &cx, &cy) == 3) {
            ProMdl drw = inc_drw();
            if (drw) {
                ProView *views = NULL;
                if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                    int vc = 0;
                    ProArraySizeGet((ProArray)views, &vc);
                    if (vi >= 0 && vi < vc) {
                        ProName    nm;
                        ProPoint3d org, out[2], norg;
                        ProSelection sr = NULL;
                        double     sc = 1.0;

                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                        org[0] = org[1] = org[2] = -9999.0;
                        ProDrawingViewOriginGet((ProDrawing)drw, views[vi], org, &sr);
                        out[0][0] = out[0][1] = out[0][2] = -9999.0;
                        out[1][0] = out[1][1] = out[1][2] = -9999.0;
                        ProDrawingViewOutlineGet((ProDrawing)drw, views[vi], out);
                        if (ProDrawingViewScaleGet((ProDrawing)drw, views[vi], &sc) != PRO_TK_NO_ERROR || sc <= 0.0)
                            sc = 1.0;
                        {
                            /* �?实测�?026-10-12 轮次14）：Δoutline = Δorigin × 2.3805 对【所有�?
                               视图成立，与视图自身 scale 无关（轴测图 scale=0.5 实测比�?2.3811�?
                               曾按 k×0.5 处理 �?位移翻�?412.1/173.1）。scale 只影响视图内�?
                               模型→屏幕的映射，不影响定位换算�?*/
                            double kv = 2.3805;
                            double ocx = (out[0][0] + out[1][0]) / 2.0;
                            double ocy = (out[0][1] + out[1][1]) / 2.0;
                            double dx = cx - ocx, dy = cy - ocy;
                            norg[0] = org[0] + dx / kv;
                            norg[1] = org[1] + dy / kv;
                            norg[2] = org[2];
                            logf_("[vpos] view[%d] '%ls' scale=%.2f kv=%.4f", vi, nm, sc, kv);
                            logf_("[vpos]   outline c=(%.1f,%.1f) -> (%.1f,%.1f)  d=(%.1f,%.1f)  org(%.2f,%.2f) -> (%.2f,%.2f)",
                                  ocx, ocy, cx, cy, dx, dy, org[0], org[1], norg[0], norg[1]);
                            {
                                ProError em = ProDrawingViewOriginSet((ProDrawing)drw, views[vi], norg, NULL);
                                logf_("[vpos]   OriginSet -> %d (%s)", (int)em, errname(em));
                            }
                            {
                                /* 实测：OriginSet �?outline 不会立即刷新，必�?regen 才提�?
                                   （与 layout_report 结尾同款；regen 会把尺寸文本拉回默认位，
                                     尺寸归位随后�?PLACEDIMS 完成，那是已验证的顺序）�?*/
                                ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                                logf_("[vpos]   regen -> %d (%s)", (int)eg, errname(eg));
                            }
                        }
                    } else {
                        logf_("[vpos] bad index %d (vc=%d)", vi, vc);
                    }
                    ProArrayFree((ProArray *)&views);
                } else {
                    logf_("[vpos] viewsCollect failed");
                }
            } else {
                logf_("[vpos] drw retrieve failed");
            }
        } else {
            logf_("[vpos] bad args");
        }
    } else if (!wcsncmp(w, L"VSCALE|", 7)) {
        /* VSCALE|<索引>|<scale>：设置视图比例（轮次18：三投影视图 2.5、轴�?1.0），
           设完 regen 提交并回读复核�?*/
        int vi = -1;
        double sc2 = 0.0;
        if (swscanf_s(w + 7, L"%d|%lf", &vi, &sc2) == 2 && sc2 >= 0.0) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                ProView *views = NULL;
                if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
                    int vc = 0;
                    ProArraySizeGet((ProArray)views, &vc);
                    if (vi >= 0 && vi < vc) {
                        ProName    nm;
                        double     s0 = -1.0, s1 = -1.0;
                        ProPoint3d out[2];

                        nm[0] = 0;
                        ProDrawingViewNameGet((ProDrawing)drw, views[vi], nm);
                        ProDrawingViewScaleGet((ProDrawing)drw, views[vi], &s0);
                        {
                            ProError es = ProDrawingViewScaleSet((ProDrawing)drw, views[vi], sc2);
                            logf_("[vscale] view[%d] '%ls' scale %.3f -> %.3f  set->%d (%s)",
                                  vi, nm, s0, sc2, (int)es, errname(es));
                        }
                        {
                            ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                            logf_("[vscale]   regen -> %d (%s)", (int)eg, errname(eg));
                        }
                        out[0][0] = out[0][1] = out[0][2] = -9999.0;
                        out[1][0] = out[1][1] = out[1][2] = -9999.0;
                        ProDrawingViewOutlineGet((ProDrawing)drw, views[vi], out);
                        ProDrawingViewScaleGet((ProDrawing)drw, views[vi], &s1);
                        logf_("[vscale]   after: scale=%.3f outline x[%.1f..%.1f] y[%.1f..%.1f]",
                              s1, out[0][0], out[1][0], out[0][1], out[1][1]);
                    } else {
                        logf_("[vscale] bad index %d (vc=%d)", vi, vc);
                    }
                    ProArrayFree((ProArray *)&views);
                } else {
                    logf_("[vscale] viewsCollect failed");
                }
            } else {
                logf_("[vscale] drw retrieve failed");
            }
        } else {
            logf_("[vscale] bad args");
        }
    } else if (!wcsncmp(w, L"SHEETSCALE|", 11)) {
        /* SHEETSCALE|<scale>：设置图纸比例（同步标题�?比例"格；轮次18 DSH 批准�?*/
        double sc2 = 0.0;
        if (swscanf_s(w + 11, L"%lf", &sc2) == 1 && sc2 > 0.0) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                double s0 = -1.0, s1 = -1.0;
                ProDrawingScaleGet((ProDrawing)drw, NULL, 1, &s0);
                {
                    ProError es = ProDrawingScaleSet((ProDrawing)drw, NULL, 1, sc2);
                    logf_("[sscale] sheet scale %.3f -> %.3f  set->%d (%s)", s0, sc2, (int)es, errname(es));
                }
                {
                    ProError eg = ProDwgSheetRegenerate((ProDrawing)drw, 1);
                    logf_("[sscale]   regen -> %d (%s)", (int)eg, errname(eg));
                }
                ProDrawingScaleGet((ProDrawing)drw, NULL, 1, &s1);
                logf_("[sscale]   after: sheet scale=%.3f", s1);
            } else {
                logf_("[sscale] drw retrieve failed");
            }
        } else {
            logf_("[sscale] bad args");
        }
    } else if (!wcsncmp(w, L"VROLL", 5)) {
        /* r153: VROLL|<deg> - roll the displayed part about its longest axis
           (after VIEWISOT this reveals thickness while keeping the axis level) */
        ProMdl part = NULL;
        double d = 22.0;
        ProError ep;
        if (w[5] == L'|') swscanf_s(w + 6, L"%lf", &d);
        ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) {
            Pro3dPnt box[2];
            ProRotate ax = PRO_Z_ROTATION;
            int i, la = 0, wid = -1;
            ProError er, ef, ew;
            if (ProSolidOutlineGet((ProSolid)part, box) == PRO_TK_NO_ERROR) {
                for (i = 1; i < 3; i++)
                    if ((box[1][i] - box[0][i]) > (box[1][la] - box[0][la])) la = i;
            }
            ax = (la == 0) ? PRO_X_ROTATION : ((la == 1) ? PRO_Y_ROTATION : PRO_Z_ROTATION);
            er = ProViewRotate(part, NULL, ax, d);
            ef = ProViewRefit(part, NULL);
            if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR && wid > 0) {
                ProWindowActivate(wid);
                ProWindowCurrentSet(wid);
                ef = ProWindowRefit(wid);
                logf_("[vroll] windowRefit(%d) -> %d (%s)", wid, (int)ef, errname(ef));
            }
            ew = ProWindowRepaint(-1);
            logf_("[vroll] '%ls' deg=%.1f axis=%d rot=%d refit=%d repaint=%d win=%d",
                  g_def_model, d, (int)ax, (int)er, (int)ef, (int)ew, wid);
        } else {
            logf_("[vroll] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
        }
    } else if (!wcsncmp(w, L"VLEVEL", 6)) {
        /* r153: VLEVEL|<tilt deg> - standard view, long axis level, tilt for depth */
        ProMdl part = NULL;
        double td = 18.0;
        ProError ep;
        if (w[6] == L'|') swscanf_s(w + 7, L"%lf", &td);
        ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) vlevel_run(part, g_def_model, td);
        else logf_("[vlevel] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"VSHOTN", 6)) {
        /* r153: level+tilt variant with the opposite rotation sign */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) vshot_run(part, g_def_model, -1);
        else logf_("[vshot] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"VSHOT", 5)) {
        /* r153: standard view, longest axis level, slight tilt (part screenshots) */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) vshot_run(part, g_def_model, 1);
        else logf_("[vshot] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"VIEWISOT", 8)) {
        /* r153: same 3/4 view, matrix transposed (test which convention Creo wants) */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) viewiso_run2(part, g_def_model, 1);
        else logf_("[viewiso] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"VIEWISO", 7)) {
        /* r153: 3/4 view, longest axis horizontal (for part screenshots) */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) viewiso_run2(part, g_def_model, 0);
        else logf_("[viewiso] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"VIEWSTD", 7)) {
        /* r153: standard orientation + fit + repaint on the displayed part */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) viewstd_run(part, g_def_model);
        else logf_("[viewstd] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcsncmp(w, L"FEATN", 5)) {
        /* r153: feature count (read-only) */
        ProMdl part = NULL;
        ProError ep = retrieve_model_any(g_def_model, &part);
        if (ep == PRO_TK_NO_ERROR && part) featn_run(part, g_def_model);
        else logf_("[featn] part retrieve('%ls') -> %d (%s)", g_def_model, (int)ep, errname(ep));
    } else if (!wcscmp(w, L"EXIT")) {
        logf_("[cmd] EXIT: stopping command channel");
        def_write_marker("exit: cmd channel stopped");
        if (g_def_timer) KillTimer(NULL, g_def_timer);
        g_def_timer = 0;
    } else {
        logf_("[cmd] unknown command");
    }
}

static void CALLBACK def_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
    char m8[128];

    wide_to_u8(g_def_model, m8, sizeof(m8));

    if (g_def_phase == 0) {
        int ok;
        if (GetEnvironmentVariableW(L"NO_QUICKDRAW", NULL, 0) > 0 ||
            GetFileAttributesW(L"D:\\creo-agent\\sync-toolkit\\work\\NO_QUICKDRAW.txt") != INVALID_FILE_ATTRIBUTES) {
            logf_("[defer] NO_QUICKDRAW flag present: skip SDCurrentDrawing AND phase2 (no drawing created/retrieved)");
            g_def_phase = 4;
            KillTimer(NULL, id);
            SetTimer(NULL, 0, 3000, def_timer_proc);
            return;
        }
        g_def_tries++;
        {
            ProMdl  cur = NULL;
            ProError ec = ProMdlCurrentGet(&cur);
            logf_("[defer] attempt %d/10 for %s (current model get -> %d %s)",
                  g_def_tries, m8, (int)ec, errname(ec));
        }
        if (g_def_tries == 1) {
            logf_("[defer] probe: firing SDAbout (a dialog window should appear if macros reach PiDrawing)");
            run_macro_text(L"~ Command `SDAbout`;", "probe-about");
        }
        ok = run_macro_text(L"~ Command `SDCurrentDrawing`;", "defer");
        if (ok) {
            ProMdl  cur2 = NULL;
            ProError ec2 = ProMdlCurrentGet(&cur2);
            logf_("[defer] macro accepted; polling for output (current model now -> %d %s)",
                  (int)ec2, errname(ec2));
            g_def_phase = 1;
            KillTimer(NULL, id);
            if (!SetTimer(NULL, 0, 5000, def_timer_proc)) {
                logf_("[defer] SetTimer(poll) failed");
                def_write_marker("error: timer failed");
            }
        } else if (g_def_tries < 10) {
            KillTimer(NULL, id);
            if (!SetTimer(NULL, 0, 20000, def_timer_proc)) {
                logf_("[defer] SetTimer(retry) failed");
                def_write_marker("error: timer failed");
            }
        } else {
            logf_("[defer] giving up after %d attempts", g_def_tries);
            KillTimer(NULL, id);
            def_write_marker("failed: macro not accepted");
        }
    } else if (g_def_phase == 1) {
        wchar_t path[PRO_PATH_SIZE];
        char    p8[PRO_PATH_SIZE * 4];

        /* 1) PDF 兜底：只承认 arm 之后新写/更新�?PDF（旧文件不算，否则会跳过标注�?*/
        swprintf_s(path, PRO_PATH_SIZE, L"%ls.pdf", g_def_model);
        wide_to_u8(path, p8, sizeof(p8));
        {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            int is_new = 0;
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
                ULARGE_INTEGER ua, uf;
                ua.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                ua.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                uf.QuadPart = g_def_arm_ft;
                if (ua.QuadPart > uf.QuadPart) {
                    is_new = 1;
                } else {
                    logf_("[defer] pdf exists but is stale (pre-arm); ignoring");
                }
            }
            if (is_new) {
                logf_("[defer] OUTPUT found (fresh): %s", p8);
                log_file_head(p8);
                KillTimer(NULL, id);
                def_write_marker("ok: pdf");
                return;
            }
        }

        /* 2) 快速出图的产物是图�?.drw)：在会话里找同名图纸，找到即保存落盘 */
        if (g_def_elapsed < 10 || (g_def_elapsed % 15) == 0) {
            ProMdl  drw = NULL;
            ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
            logf_("[defer] drawing retrieve -> %d (%s)", (int)er, errname(er));
            if (er == PRO_TK_NO_ERROR && drw) {
                ProName dn;
                char    d8[160];
                ProError ed;

                dn[0] = 0;
                ProMdlNameGet(drw, dn);
                wide_to_u8(dn, d8, sizeof(d8));
                logf_("[defer] DRAWING FOUND in session: %s", d8);

                ed = ProMdlDisplay(drw);
                logf_("[defer] display drawing -> %d (%s)", (int)ed, errname(ed));

                /* 顺序：先保存无标注版（与 14:00 成功路径一致）�?再标�?�?二次保存�?
                   实测：SDAutoDimension 会让会话处于“命令未结束”状态，之后立刻保存会挂起�?*/
                g_def_phase = 2;
                KillTimer(NULL, id);
                if (!SetTimer(NULL, 0, 5000, def_timer_proc)) {
                    logf_("[defer] SetTimer(phase2) failed");
                    def_write_marker("error: timer failed");
                }
                return;
            }
        }

        g_def_elapsed += 5;
        if (g_def_elapsed >= g_def_wait) {
            logf_("[defer] output NOT found after %ds; entering cmd-channel mode", g_def_elapsed);
            def_write_marker("failed: no output");
            g_def_phase = 3;
            KillTimer(NULL, id);
            SetTimer(NULL, 0, 3000, def_timer_proc);
        }
    } else if (g_def_phase == 2) {
        /* 第一段：让路 �?保存“无标注版”图�?+ 导出 PDF（保底交付）�?再发自动标注 */
        ProMdl drw = NULL;
        ProError er, es;
        if (g_has_dir) clear_drw_targets(g_dir, g_def_model);
        else           clear_drw_targets(L".", g_def_model);
        er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[defer] phase2 retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            activate_drawing_window(drw);

            /* 图纸比例 1:1（轮�?9：用户裁定改�?1:1。轮�?8 曾短暂设�?2.5:1�?
               已按用户要求回退；不要再擅自改比例。标题栏"比例"字段由图纸比例驱动。） */
            {
                ProError esc = ProDrawingScaleSet((ProDrawing)drw, NULL, 1, 1.0);
                logf_("[scale] ProDrawingScaleSet(1:1) -> %d (%s)", (int)esc, errname(esc));
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
            }

            /* 教程�?7 章：图名/材料参数（CNAME/MSTD）已初始化进模型（prt.5/6）�?
               实测：在本环境“创�?修改参数后再保存”会触发 ProMdlSave 死等�?
               故此处不再重设——快速出图时图框会直接从模型参数取值�?*/
            /* 标题�?4 格：图框 format 里是参数引用�?CNAME / &MSTD / &DRWNUM / &MASS[.4]），
               必须把“同名参数”写进模型才会解析；只写 PTC_COMMON_NAME 不够
               （那是系统参数，图框并不引用它），这正是标题栏显�?&XXX 原文的原因�?
               保存仍禁用（改参数后 ProMdlSave 会死等），PDF 导出不依赖保存�?*/
            if (1) {
                ProMdl part = NULL;
                if (retrieve_model_any(g_def_model, &part) == PRO_TK_NO_ERROR && part) {
                    set_string_param(part, L"PTC_COMMON_NAME", "连接片", "titlebar-name");
                    /* 图框（format）标题栏直接引用的参数名：CNAME=图名、MSTD=材料、DRWNUM=图号 */
                    set_string_param(part, L"CNAME",  "连接片",  "titlebar-cname");
                    set_string_param(part, L"MSTD",   "NB109",   "titlebar-mstd");
                    set_string_param(part, L"DRWNUM", "NB109_3", "titlebar-drwnum");
                    /* 赋材料：材料格与重量(&MASS[.4])的真实来源（有材料才有密度→质量） */
                    assign_material_name(part, L"NB109", "titlebar-material");
                } else {
                    logf_("[titlebar] part retrieve for params failed");
                }
                /* 参数写完必须重生成图纸，标题栏才会取到新值（前面那次 regen 在写参数之前�?*/
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
                logf_("[titlebar] params written + sheet regenerated");
            }
            /* 教程正规流程：先“显示模型注释”（�?3D 尺寸/公差捞到图纸），
               再技术要求注释，最后保�?+ 导出 PDF�?
               （SDAutoDimension“智能标注”在本环境实测不可用，弃用。） */
            /* 像工程师一样只显示关键尺寸（不再用“全�?拭除”，改为逐个精确显示�?*/
            {
                ProMdl part2 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part2) == PRO_TK_NO_ERROR && part2) {
                    show_key_dims(part2, drw);
                } else {
                    logf_("[showk] part retrieve failed");
                }
            }
            /* 整理尺寸（教程第 6 节）：自动分层排布，避免压线/堆叠 */
            cleanup_dims(drw);
            add_technote_at(drw, 500.0, 300.0);   /* r58: 默认位仍为旧位，交付前用 TECHNOTE|x|y 移到左下�?*/

            /* P0b：标题栏占位符字面文本（&CNAME 等）�?实际值（对照真人图纸同款格式�?*/
            {
                ProMdl part4 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part4) == PRO_TK_NO_ERROR && part4)
                    fill_title_block(drw, part4);
                else
                    logf_("[filltb] part retrieve failed (phase2)");
            }
            /* PRO_AXIS 已关闭（轮次 8：模型轴=�?length 长线，污染图面；
               6 凸点中心线改由下面的 boss_centerlines 手画短十字） */
            /* P3�? 个凸点的点画线十字中心线（图纸实体，不写模型�?*/
            {
                ProMdl part7 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part7) == PRO_TK_NO_ERROR && part7) {
                    /* 2026-10-29: scan plane used to be hardcoded (0,1) = X-Y. For a
                       plate that is thin in Y and long in Z (nb109_5: 41.1 x 4.55 x
                       63.58) the bosses spread in X-Z, so an X-Y scan sees one
                       collinear row and the crosses land on the sliver view.
                       Pick the two largest bbox axes; keep ascending order. */
                    Pro3dPnt bb[2];
                    int ax = 0, ay = 1;
                    if (ProSolidOutlineGet((ProSolid)part7, bb) == PRO_TK_NO_ERROR) {
                        double ext[3];
                        int k, i1 = 0, i2 = 1;
                        for (k = 0; k < 3; k++) ext[k] = bb[1][k] - bb[0][k];
                        if (ext[1] > ext[i1]) i1 = 1;
                        if (ext[2] > ext[i1]) i1 = 2;
                        for (k = 0; k < 3; k++) {
                            if (k == i1) continue;
                            if (i2 == i1 || ext[k] > ext[i2]) i2 = k;
                        }
                        if (i2 < i1) { int s = i1; i1 = i2; i2 = s; }
                        ax = i1; ay = i2;
                        logf_("[cross] scan plane auto: ax=%d ay=%d (ext %.2f/%.2f/%.2f)",
                              ax, ay, ext[0], ext[1], ext[2]);
                    }
                    boss_centerlines(drw, part7, ax, ay);
                }
            }
            /* P3：右上角 1:2 轴测�?*/
            {
                ProMdl part6 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part6) == PRO_TK_NO_ERROR && part6)
                    add_iso_view(drw, part6, 700.0, 630.0, 0.5);
            }
            /* P2：三视图紧凑化（间距 75 单位�?1.5mm；轴测图不参与） */
            layout_report(drw, 1);
            /* P1：总长 63.6 参考尺寸（模型侧建 + 显示）——暂缓：
               实测 ProDimensionCreate 返回 -2 后会污染标注上下文，
               紧接着�?ProDrawingDimensionMove 崩溃 �?管线重启死循环（本轮�?PDF）�?
               留在 TOTFACE 命令里单独调试，不接入相�?�?*/
            if (0) {
                ProMdl part8 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part8) == PRO_TK_NO_ERROR && part8)
                    add_total_dim_model(drw, part8);
            }
            /* �?place_dims 暂缓接入（轮�?：在 phase2 定时器回调里移动尺寸会触�?
               致命错误 �?phase2 死循环、无 PDF。纪律：先在命令通道单独验证，再接入。）
               函数保留，待�?PLACEDIMS 命令形式验证�?*/
            if (0) {
                ProMdl part9 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part9) == PRO_TK_NO_ERROR && part9)
                    place_dims(drw, part9);
            }

            /* 轮次53（DSH 要求）：自校一律用截图（grab_creo_window.ps1），
               **不再产生 pdf_live_*.pdf**；只�?DSH 明确�?交付"时才�?PDF�?
               �?保交�?导出已停用；需要导出时用命令通道�?PDFDUMP（人工确认后）�?*/
            logf_("[defer] phase2 pdf export SKIPPED (r53: self-check via screenshots only; use PDFDUMP on delivery request)");

            /* 落盘（备�?保存）暂时跳过：改过尺寸属性后它们会死等（已知问题），
               先把管线稳定送到命令通道，落盘方案另行处理�?*/
            if (0) {
                es = ProMdlnameBackup(drw, L"D:\\J26060131\\X6 2P 0810");
                logf_("[defer] phase2 backup -> %d (%s)", (int)es, errname(es));
                if (es != PRO_TK_NO_ERROR) {
                    prep_save_regen();
                    es = ProMdlSave(drw);
                    logf_("[defer] phase2 fallback save -> %d (%s)", (int)es, errname(es));
                }
            }
            es = PRO_TK_NO_ERROR;
            def_write_marker(es == PRO_TK_NO_ERROR ? "ok: tech+pdf" : "warn: pdf ok, save failed");
            logf_("[defer] phase2 done; entering cmd channel in 10s");
            g_def_phase = 3;
            KillTimer(NULL, id);
            SetTimer(NULL, 0, 10000, def_timer_proc);
        } else {
            logf_("[defer] phase2: drawing not found");
            def_write_marker("error: drawing lost in phase2");
            g_def_phase = 4;
            KillTimer(NULL, id);
            SetTimer(NULL, 0, 3000, def_timer_proc);
        }
    } else if (g_def_phase == 3) {
        /* 单遍流程：phase2 已出 PDF/备份，直接进入命令通道 */
        logf_("[defer] phase3 skipped (single-pass); entering cmd channel");
        g_def_phase = 4;
        KillTimer(NULL, id);
        SetTimer(NULL, 0, 3000, def_timer_proc);
        logf_("[defer] cmd-channel mode (phase4) - waiting for cmd_now.txt");
    } else {
        /* phase4：常驻命令通道 */
        run_cmd_channel();
    }
}

/* QUICKDRAW：打开模型 -> 确保它是“当前模型�?-> 执行快速出图宏 -> 等产�?*/
static int run_quickdraw_job(int k)
{
    ProMdl mdl = NULL;
    char   m8[128];

    wide_to_u8(g_qd_model[k], m8, sizeof(m8));
    logf_("---- QUICKDRAW %s (type=%s)", m8, g_qd_asm[k] ? "asm" : "prt");

    if (!retrieve_model(g_qd_model[k], g_qd_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  QUICKDRAW FAILED (retrieve)");
        return 0;
    }

    /* 出图前保证模型里�?front 定向视图（快速出图主视图优先�?front > XY|Z�?*/
    ensure_front_view(mdl);

    {
        ProMdl cur = NULL;
        ProName nm;
        char    c8[160];

        {
            ProError ec = ProMdlCurrentGet(&cur);
            nm[0] = 0;
            if (ec == PRO_TK_NO_ERROR && cur) ProMdlNameGet(cur, nm);
            wide_to_u8(nm, c8, sizeof(c8));
            logf_("  current model before macro = '%s' (get -> %d %s)", c8, (int)ec, errname(ec));
        }

        if (cur != mdl) {
            ProError ed = PRO_TK_GENERAL_ERROR;
            int     wid = -1;

            /* 2026-11-03 多窗口：ProMdlDisplay REUSES the current window, so N models
               ended up sharing one window. ProObjectwindowMdlnameCreate attaches the
               object to its own window (reusing an empty one if present) - that is the
               API the docs say to use when several objects must be visible at once. */
            ed = ProObjectwindowMdlnameCreate(g_qd_model[k],
                                              g_qd_asm[k] ? PRO_MDL_ASSEMBLY : PRO_MDL_PART, &wid);
            logf_("  ProObjectwindowMdlnameCreate -> %d (%s) wid=%d", (int)ed, errname(ed), wid);
            if (ed != PRO_TK_NO_ERROR || wid <= 0) {
                ed = ProMdlDisplay(mdl);
                logf_("  fallback ProMdlDisplay -> %d (%s)", (int)ed, errname(ed));
                wid = -1;
            } else {
                ProWindowCurrentSet(wid);
                ProWindowActivate(wid);
            }

            cur = NULL; nm[0] = 0;
            if (ProMdlCurrentGet(&cur) == PRO_TK_NO_ERROR && cur) ProMdlNameGet(cur, nm);
            wide_to_u8(nm, c8, sizeof(c8));
            logf_("  current model after display = '%s'", c8);

            if (ProWindowCurrentGet(&wid) == PRO_TK_NO_ERROR) {
                ProMdl  wm = NULL;
                ProName wnm;
                char    w8[160];
                ProError ea, es;

                logf_("  current window id = %d", wid);
                wnm[0] = 0;
                if (ProWindowMdlGet(wid, &wm) == PRO_TK_NO_ERROR && wm) ProMdlNameGet(wm, wnm);
                wide_to_u8(wnm, w8, sizeof(w8));
                logf_("  model in that window = '%s'", w8);

                ea = ProWindowActivate(wid);
                es = ProWindowCurrentSet(wid);
                logf_("  ProWindowActivate -> %d (%s); ProWindowCurrentSet -> %d (%s)",
                      (int)ea, errname(ea), (int)es, errname(es));

                cur = NULL; nm[0] = 0;
                if (ProMdlCurrentGet(&cur) == PRO_TK_NO_ERROR && cur) ProMdlNameGet(cur, nm);
                wide_to_u8(nm, c8, sizeof(c8));
                logf_("  current model after window ops = '%s'", c8);
            } else {
                logf_("  ProWindowCurrentGet failed (no window in this session?)");
            }
        }
    }

    if (g_qd_defer[k]) {
        g_def_mdl = mdl;
        swprintf_s(g_def_model, PRO_NAME_SIZE + 8, L"%ls", g_qd_model[k]);
        g_def_wait = g_qd_wait[k];
        g_def_phase = 0;
        g_def_tries = 0;
        g_def_elapsed = 0;
        g_def_armed = 1;
        {
            FILETIME ft0;
            GetSystemTimeAsFileTime(&ft0);
            g_def_arm_ft = ((unsigned long long)ft0.dwHighDateTime << 32) | (unsigned long long)ft0.dwLowDateTime;
        }
        if (g_def_timer) KillTimer(NULL, g_def_timer);
        g_def_timer = SetTimer(NULL, 0, 60000, def_timer_proc);
        logf_("  [defer] timer armed: first try in 60s, output wait=%ds", g_def_wait);
        if (!g_def_timer) { logf_("  [defer] SetTimer failed"); return 0; }
        return 1;
    }

    if (!run_macro_text(L"~ Command `SDCurrentDrawing`;", "quickdraw")) return 0;
    return wait_and_check_pdf(g_qd_model[k], g_qd_wait[k]);
}

/* DUMPNEW：目录里找“最新修改”的匹配文件，进程内读出（用于看 PiCreo/轨迹日志�?*/
static int run_dumpnew_job(int k)
{
    wchar_t pattern[PRO_PATH_SIZE];
    wchar_t best[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULARGE_INTEGER best_t;
    char p8[PRO_PATH_SIZE * 4];

    swprintf_s(pattern, PRO_PATH_SIZE, L"%ls\\%ls*", g_dn_dir[k], g_dn_pfx[k]);
    wide_to_u8(pattern, p8, sizeof(p8));
    logf_("---- DUMPNEW %s", p8);

    best[0] = 0;
    best_t.QuadPart = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("  (no file matches)"); return 0; }
    do {
        ULARGE_INTEGER t;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (t.QuadPart > best_t.QuadPart) {
            best_t = t;
            swprintf_s(best, PRO_PATH_SIZE, L"%ls\\%ls", g_dn_dir[k], fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (!best[0]) { logf_("  (no file matches)"); return 0; }
    {
        char b8[PRO_PATH_SIZE * 4];
        wide_to_u8(best, b8, sizeof(b8));
        dump_text_file(b8, "dumpnew");
    }
    return 1;
}

/* 简�?memmem（MSVC 无此函数�?*/
static void *memmem_s(const void *hay, int hlen, const void *nee, int nlen)
{
    int i;
    if (nlen <= 0 || hlen < nlen) return NULL;
    for (i = 0; i <= hlen - nlen; i++) {
        if (memcmp((const char *)hay + i, nee, (size_t)nlen) == 0)
            return (void *)((const char *)hay + i);
    }
    return NULL;
}

/* 文件头（16KB 内）是否�?"Process ID: <pid>"（UTF-8 �?UTF-16LE�?*/
static int head_has_pid(const char *buf, int len, int pid)
{
    char p8[64];
    char u16[128];
    int  n8, i, m = 0;

    n8 = sprintf_s(p8, sizeof(p8), "Process ID: %d", pid);
    if (n8 <= 0) return 0;
    if (memmem_s(buf, len, p8, n8)) return 1;
    for (i = 0; i < n8; i++) {
        u16[m++] = p8[i];
        u16[m++] = 0;
    }
    return memmem_s(buf, len, u16, m) ? 1 : 0;
}

/* DUMPPID：按“文件头�?Process ID: <pid>”定位文件并整读（找本会�?trail 用） */
static int run_dumppid_job(int k)
{
    wchar_t pattern[PRO_PATH_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char pp8[PRO_PATH_SIZE * 4];

    swprintf_s(pattern, PRO_PATH_SIZE, L"%ls\\%ls*", g_dp_dir[k], g_dp_pfx[k]);
    wide_to_u8(pattern, pp8, sizeof(pp8));
    if (g_dp_pid[k] == 0) g_dp_pid[k] = (int)GetCurrentProcessId();
    logf_("---- DUMPPID pid=%d in %s", g_dp_pid[k], pp8);

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("  (no file matches)"); return 0; }
    do {
        wchar_t full[PRO_PATH_SIZE];
        FILE   *f;
        char    head[16384];
        size_t  got;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        swprintf_s(full, PRO_PATH_SIZE, L"%ls\\%ls", g_dp_dir[k], fd.cFileName);
        f = _wfopen(full, L"rb");
        if (!f) continue;
        got = fread(head, 1, sizeof(head), f);
        fclose(f);
        if (head_has_pid(head, (int)got, g_dp_pid[k])) {
            char b8[PRO_PATH_SIZE * 4];
            wide_to_u8(full, b8, sizeof(b8));
            logf_("  matched: %s", b8);
            dump_text_file(b8, "dumppid");
            FindClose(h);
            return 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    logf_("  (no file with pid %d)", g_dp_pid[k]);
    return 0;
}

/* ---------------- 干涉检查（ProFit�?---------------- */

/* �?selection 引用的模型名（组件优先用装配路径解析），成功返回 1 */
static int sel_model_name(ProSelection sel, char *out, int cap)
{
    ProAsmcomppath path;
    ProMdl         cmdl = NULL;
    ProModelitem   item;
    ProName        nm;

    out[0] = 0;
    if (!sel) return 0;
    if (ProSelectionAsmcomppathGet(sel, &path) == PRO_TK_NO_ERROR &&
        ProAsmcomppathMdlGet(&path, &cmdl) == PRO_TK_NO_ERROR && cmdl) {
        nm[0] = 0;
        ProMdlNameGet(cmdl, nm);
        wide_to_u8(nm, out, cap);
        if (out[0]) return 1;
    }
    if (ProSelectionModelitemGet(sel, &item) == PRO_TK_NO_ERROR) {
        nm[0] = 0;
        ProModelitemNameGet(&item, nm);
        wide_to_u8(nm, out, cap);
        if (out[0]) return 1;
    }
    strncpy_s(out, cap, "?", _TRUNCATE);
    return 0;
}

/* 汇总：按模型对聚合 */
#define MAXGROUPS 4096
typedef struct {
    char   a[96];
    char   b[96];
    int    count;
    double sum;
    double vmax;
} igroup_t;

static igroup_t g_groups[MAXGROUPS];
static int      g_ngroups = 0;

static int group_idx(const char *a, const char *b)
{
    int i;
    for (i = 0; i < g_ngroups; i++) {
        if (strcmp(g_groups[i].a, a) == 0 && strcmp(g_groups[i].b, b) == 0) return i;
    }
    if (g_ngroups >= MAXGROUPS) return -1;
    strncpy_s(g_groups[g_ngroups].a, sizeof(g_groups[0].a), a, _TRUNCATE);
    strncpy_s(g_groups[g_ngroups].b, sizeof(g_groups[0].b), b, _TRUNCATE);
    g_groups[g_ngroups].count = 0;
    g_groups[g_ngroups].sum = 0.0;
    g_groups[g_ngroups].vmax = 0.0;
    return g_ngroups++;
}

static int cmp_group(const void *x, const void *y)
{
    const igroup_t *p = (const igroup_t *)x;
    const igroup_t *q = (const igroup_t *)y;
    if (p->vmax < q->vmax) return 1;
    if (p->vmax > q->vmax) return -1;
    return q->count - p->count;
}

/* 把一�?selection 格式化成单行�?模型�?(id=NNN)" */
static void log_sel_info(ProSelection sel, const char *tag)
{
    ProModelitem item;
    ProName       nm;
    char          u8[256];
    char          m8[256];
    int           have_model = 0;

    if (!sel) { logf_("      %s: <null>", tag); return; }

    m8[0] = 0;
    {
        ProAsmcomppath path;
        ProMdl         cmdl = NULL;
        if (ProSelectionAsmcomppathGet(sel, &path) == PRO_TK_NO_ERROR &&
            ProAsmcomppathMdlGet(&path, &cmdl) == PRO_TK_NO_ERROR && cmdl) {
            ProName cnm;
            cnm[0] = 0;
            ProMdlNameGet(cmdl, cnm);
            wide_to_u8(cnm, m8, sizeof(m8));
            have_model = (m8[0] != 0);
        }
    }

    if (ProSelectionModelitemGet(sel, &item) != PRO_TK_NO_ERROR) {
        logf_("      %s: %s   (id unknown)", tag, have_model ? m8 : "<unreadable>");
        return;
    }
    nm[0] = 0;
    ProModelitemNameGet(&item, nm);
    wide_to_u8(nm, u8, sizeof(u8));
    logf_("      %s: %s%s%s   (type=%d id=%d)", tag,
          have_model ? m8 : (u8[0] ? u8 : "<noname>"),
          (!have_model && u8[0]) ? "" : "",
          "", (int)item.type, item.id);
}

static int run_interf_job(interf_t *j)
{
    ProMdl  mdl = NULL;
    ProError e;
    int     i, n = 0;
    int     pairs = 0;
    ProInterferenceInfo *info = NULL;
    char    mu8[128];

    wide_to_u8(j->model, mu8, sizeof(mu8));
    logf_("---- INTERF %s (type=%s setup=%d facets=%d quilts=%d fast=%d volume=%d)",
          mu8, j->is_asm ? "asm" : "prt", j->setup, j->facets, j->quilts, j->fast, j->volume);

    if (!retrieve_model(j->model, j->is_asm ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  INTERF FAILED (retrieve)");
        return 0;
    }

    e = ProFitGlobalinterferenceCompute((ProAssembly)mdl,
                                        (ProFitComputeSetup)j->setup,
                                        j->facets ? PRO_B_TRUE : PRO_B_FALSE,
                                        j->quilts ? PRO_B_TRUE : PRO_B_FALSE,
                                        j->fast   ? PRO_B_TRUE : PRO_B_FALSE,
                                        &info);
    logf_("  ProFitGlobalinterferenceCompute -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;
    if (!info) { logf_("  interfering pairs: 0"); logf_("INTERF_DONE %s pairs=0", mu8); return 1; }

    ProArraySizeGet(info, &n);
    logf_("  interfering pairs: %d", n);

    g_ngroups = 0;
    for (i = 0; i < n; i++) {
        char na[96], nb[96], tmp[96];
        double vol = 0.0;
        int    gi;

        if (i < 200) {
            logf_("    [pair %d/%d]", i + 1, n);
            log_sel_info(info[i].part_1, "A");
            log_sel_info(info[i].part_2, "B");
        } else if (i == 200) {
            logf_("    ... (%d more pairs omitted from detail log)", n - 200);
        }

        sel_model_name(info[i].part_1, na, sizeof(na));
        sel_model_name(info[i].part_2, nb, sizeof(nb));
        if (strcmp(na, nb) > 0) { strcpy_s(tmp, sizeof(tmp), na); strcpy_s(na, sizeof(na), nb); strcpy_s(nb, sizeof(nb), tmp); }

        if (j->volume) {
            ProInterferenceData d = info[i].interf_data;
            int owned = 0;
            if (!d && i < 2000) {
                ProError ec = ProFitInterferenceCompute(info[i].part_1, info[i].part_2,
                                        j->facets ? PRO_B_TRUE : PRO_B_FALSE,
                                        j->quilts ? PRO_B_TRUE : PRO_B_FALSE,
                                        &d);
                if (i < 200) logf_("      ProFitInterferenceCompute -> %d (%s)", (int)ec, errname(ec));
                if (ec == PRO_TK_NO_ERROR && d) owned = 1;
            }
            if (d) {
                ProError ev = ProFitInterferencevolumeCompute(d, &vol);
                if (i < 200) logf_("      interference volume = %.4f (err=%d %s)", vol, (int)ev, errname(ev));
                if (ev != PRO_TK_NO_ERROR) vol = 0.0;
                if (owned) ProInterferenceDataFree(d);
            }
        }

        gi = group_idx(na, nb);
        if (gi >= 0) {
            g_groups[gi].count++;
            g_groups[gi].sum += vol;
            if (vol > g_groups[gi].vmax) g_groups[gi].vmax = vol;
        }
    }
    pairs = n;

    if (g_ngroups > 0) {
        int show;
        qsort(g_groups, (size_t)g_ngroups, sizeof(g_groups[0]), cmp_group);
        logf_("  distinct model pairs: %d", g_ngroups);
        logf_("  --- groups sorted by max interference volume (model units^3) ---");
        show = (g_ngroups < 300) ? g_ngroups : 300;
        for (i = 0; i < show; i++) {
            logf_("    %-40s <-> %-40s count=%-5d sum=%-14.4f max=%.4f",
                  g_groups[i].a, g_groups[i].b, g_groups[i].count, g_groups[i].sum, g_groups[i].vmax);
        }
        if (g_ngroups > show) logf_("    ... (%d more groups omitted)", g_ngroups - show);
    }

    ProInterferenceInfoProarrayFree(info);
    logf_("INTERF_DONE %s pairs=%d groups=%d", mu8, pairs, g_ngroups);
    return 1;
}

/*
 * 读文件头：本进程（xtop）读 —�?公司透明加密只对授权进程解密，所以这里看到的是明文；
 * 把明文大�?+ STEP 头打�?stderr（明文重定向）便于外部核对�?
 */
static void log_file_head(const char *path)
{
    FILE  *f;
    long   sz;
    size_t got;
    char   buf[16385];

    f = wfopen_utf8(path, L"rb");
    if (!f) { logf_("  [verify] cannot open %s", path); return; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    got = fread(buf, 1, 16384, f);
    buf[got] = 0;
    fclose(f);

    logf_("  [verify] %s  plaintext size = %ld bytes", path, sz);

    {
        char  *s = strstr(buf, "ISO-10303-21");
        char  *e = strstr(buf, "ENDSEC;");
        size_t i, n;
        if (s && e && e > s) {
            char block[2049];
            n = (size_t)(e - s) + 7;
            if (n > 2048) n = 2048;
            memcpy(block, s, n);
            block[n] = 0;
            for (i = 0; i < n; i++) {
                if (block[i] == '\r' || block[i] == '\n' || block[i] == '\t') block[i] = ' ';
            }
            logf_("  [verify] HEADER: %s", block);
        } else if (got > 0) {
            /* �?STEP 文件（例如别�?DLL 写的日志）：把前 1200 字节的可打印内容打出�?*/
            char head[1201];
            size_t m = (got < 1200) ? got : 1200;
            for (i = 0; i < m; i++) {
                unsigned char c = (unsigned char)buf[i];
                head[i] = (c >= 32 && c < 127) ? (char)c
                        : (c == '\n' || c == '\r' || c == '\t') ? ' ' : '.';
            }
            head[m] = 0;
            logf_("  [verify] (non-STEP) first %d bytes: %s", (int)m, head);
        }
    }
}

/* 导出后自校验：输出基�?+ Creo 自动补的 .stp */
static void log_export_head(const wchar_t *out_noext)
{
    char path[PRO_PATH_SIZE * 4];

    wide_to_u8(out_noext, path, sizeof(path));
    if (strlen(path) + 5 < sizeof(path)) strcat_s(path, sizeof(path), ".stp");
    log_file_head(path);
}


static void run_jobs(void)
{
    int i, ok = 0, fail = 0;

    log_open();
    logf_("==== sync_export (C sync toolkit) ====");

    if (!read_job()) {
        logf_("SYNC_EXPORT_DONE ok=0 fail=0 reason=nojob");
        {
            FILE *mf = fopen("sync_export_done.txt", "w");
            if (mf) { fprintf(mf, "nojob\n"); fclose(mf); }
        }
        return;
    }

    if (g_has_dir) {
        char u8[PRO_PATH_SIZE * 4];
        ProError e;
        wide_to_u8(g_dir, u8, sizeof(u8));
        e = ProDirectoryChange(g_dir);
        logf_("ProDirectoryChange(%s) -> %d (%s)", u8, (int)e, errname(e));
    }

    run_opt_ops();

    for (i = 0; i < g_njobs; i++) {
        job_t  *j = &g_jobs[i];
        ProMdl  mdl = NULL;
        char    mu8[128], ou8[PRO_PATH_SIZE * 4], pu8[PRO_PATH_SIZE * 4];

        wide_to_u8(j->model, mu8, sizeof(mu8));
        wide_to_u8(j->out, ou8, sizeof(ou8));
        wide_to_u8(j->profile, pu8, sizeof(pu8));

        logf_("---- job %d/%d: model=%s type=%s", i + 1, g_njobs, mu8, j->is_asm ? "asm" : "prt");
        logf_("     out=%s", ou8);
        logf_("     profile=%s", j->has_profile ? pu8 : "(none)");

        if (!retrieve_with_retry(j, &mdl)) {
            logf_("  job %d FAILED (retrieve)", i + 1);
            fail++;
            continue;
        }

        if (j->has_profile) {
            ProError ep = ProIntfExportProfileLoad(j->profile);
            logf_("  ProIntfExportProfileLoad -> %d (%s)", (int)ep, errname(ep));
        }

        {
            ProError ew = ProIntf3DFileWriteWithDefaultProfile((ProSolid)mdl, PRO_INTF_EXPORT_STEP, j->out);
            logf_("  ProIntf3DFileWriteWithDefaultProfile(STEP) -> %d (%s)", (int)ew, errname(ew));
            if (ew == PRO_TK_NO_ERROR) { ok++; log_export_head(j->out); } else fail++;
        }
    }

    for (i = 0; i < g_ncnt; i++) {
        char f8[PRO_PATH_SIZE * 4];
        FILE *f;
        long  sz;
        char *buf;
        int   cnt = 0;
        wide_to_u8(g_cnt_file[i], f8, sizeof(f8));
        f = _wfopen(g_cnt_file[i], L"rb");
        if (!f) { logf_("  [count] cannot open %s", f8); continue; }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 800L * 1024 * 1024) {
            buf = (char *)malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                char *p = buf;
                buf[sz] = 0;
                while ((p = strstr(p, g_cnt_pat[i])) != NULL) { cnt++; p++; }
            }
            if (buf) free(buf);
        }
        fclose(f);
        logf_("  [count] %-16s in %s = %d", g_cnt_pat[i], f8, cnt);
    }

    for (i = 0; i < g_npm; i++) {
        if (run_param_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_npd; i++) {
        if (run_paramd_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_nqd; i++) {
        if (run_quickdraw_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_ndn; i++) {
        if (run_dumpnew_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_ndp; i++) {
        if (run_dumppid_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_nmacro; i++) {
        char tag[32];
        sprintf_s(tag, sizeof(tag), "macro job %d", i + 1);
        if (run_macro_text(g_macro[i], tag)) ok++; else fail++;
    }

    for (i = 0; i < g_nflat; i++) {
        if (run_flatten_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_nv1; i++) {
        ProMdl  mdl = NULL;
        char    mu8[128], ou8[PRO_PATH_SIZE * 4];
        ProError ex;

        wide_to_u8(g_v1_model[i], mu8, sizeof(mu8));
        wide_to_u8(g_v1_out[i], ou8, sizeof(ou8));
        logf_("---- legacy(ProIntf3DFileWrite) job %d/%d: model=%s", i + 1, g_nv1, mu8);
        logf_("     out=%s", ou8);

        if (!retrieve_model(g_v1_model[i], g_v1_asm[i] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
            logf_("  legacy job %d FAILED (retrieve)", i + 1);
            fail++;
            continue;
        }
        ex = ProIntf3DFileWrite((ProSolid)mdl, PRO_INTF_EXPORT_STEP, g_v1_out[i],
                                PRO_OUTPUT_ASSEMBLY_SINGLE_FILE, NULL, NULL, NULL, NULL);
        logf_("  ProIntf3DFileWrite(STEP, legacy) -> %d (%s)", (int)ex, errname(ex));
        if (ex == PRO_TK_NO_ERROR) { ok++; log_file_head(ou8); } else fail++;
    }

    for (i = 0; i < g_n2d; i++) {
        ProMdl mdl = NULL;
        char   mu8[128], ou8[PRO_PATH_SIZE * 4];
        Pro2dExportdata data = NULL;
        ProError ea, ex;

        wide_to_u8(g_2d_model[i], mu8, sizeof(mu8));
        wide_to_u8(g_2d_out[i], ou8, sizeof(ou8));
        logf_("---- 2d job %d/%d: drawing=%s", i + 1, g_n2d, mu8);
        logf_("     out=%s", ou8);

        if (!retrieve_model(g_2d_model[i], PRO_MDLFILE_DRAWING, &mdl)) {
            logf_("  2d job %d FAILED (retrieve)", i + 1);
            fail++;
            continue;
        }
        ea = Pro2dExportdataAlloc(&data);
        if (ea == PRO_TK_NO_ERROR && data) Pro2dExportdataSheetoptionSet(data, PRO2DEXPORT_ALL);
        ex = Pro2dExport(PRO_DWG_FILE, g_2d_out[i], (ProMdl)mdl, data);
        logf_("  Pro2dExport(DWG) -> %d (%s)", (int)ex, errname(ex));
        if (ex == PRO_TK_NO_ERROR) { ok++; log_file_head(ou8); } else fail++;
        if (data) Pro2dExportdataFree(data);
    }

    for (i = 0; i < g_ninterfs; i++) {
        if (run_interf_job(&g_interfs[i])) ok++; else fail++;
    }

    for (i = 0; i < g_nverifies; i++) {
        char vu8[PRO_PATH_SIZE * 4];
        wide_to_u8(g_verifies[i], vu8, sizeof(vu8));
        logf_("---- verify %d/%d", i + 1, g_nverifies);
        log_file_head(vu8);
    }

    /* CMDONLY: arm the runtime command channel now, without any drawing work */
    if (g_cmdonly) {
        g_def_model[0] = 0;
        g_def_wait = 0;
        g_def_phase = 4;          /* jump straight to the polled channel */
        g_def_tries = 0;
        g_def_elapsed = 0;
        g_def_armed = 1;
        logf_("[cmdonly] arming command channel (phase4) - drawing untouched");
        SetTimer(NULL, 0, 3000, def_timer_proc);
    }

    logf_("SYNC_EXPORT_DONE ok=%d fail=%d", ok, fail);

    /* 完成标记文件：parametric.exe 带图形启动时 stderr 抓不到，外部脚本靠它判断结束�?
     * 有延迟任务时先不写，由定时器在最终结果时写入�?*/
    if (!g_def_armed) {
        FILE *mf = fopen("sync_export_done.txt", "w");
        if (mf) { fprintf(mf, "ok=%d fail=%d\n", ok, fail); fclose(mf); }
    } else {
        logf_("[i] deferred job armed; done-marker will be written by timer");
    }
}

#if defined(__cplusplus)
extern "C" {
#endif

int user_initialize(int argc, char *argv[], char *version, char *build, wchar_t errbuf[PRO_LINE_SIZE])
{
    log_open();
    logf_("user_initialize: Pro/TOOLKIT %s (%s) argc=%d", version ? version : "?", build ? build : "?", argc);
    run_jobs();
    return 0;
}

void user_terminate(void)
{
    logf_("user_terminate");
    if (g_log) { fclose(g_log); g_log = NULL; }
}

#if defined(__cplusplus)
}
#endif

