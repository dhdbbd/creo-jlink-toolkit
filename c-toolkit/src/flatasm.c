/*
 * sync_export.c -- Creo 9 同步 Pro/TOOLKIT 导出 DLL（job 文件驱动，无头运行）
 *
 * 由 Creo 启动时经 protk.dat 加载进进程，user_initialize 里读取工作目录下的
 * export_job.txt，按行执行导出（支持加载 .dep_step 导出配置文件）。
 *
 * export_job.txt 格式（UTF-8，空行/# 注释行忽略）：
 *   DIR|<模型目录>                     可选，检索模型前切到此目录
 *   EXPORT|<模型名>|<prt|asm>|<输出路径(不含扩展名)>|<配置文件路径，可空>
 *   EXPORT|...
 *
 * 运行结果写 sync_export_run.log（同时输出到 stderr，便于重定向抓取）。
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

/* 以 UTF-8 路径打开文件：fopen 走 ANSI 代码页，中文路径会打不开，故转宽字符用 _wfopen */
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

/* 旧版 3D 导出作业（Creo 5 起已弃用的 ProIntf3DFileWrite，PiCreo 等老 DLL 走这条）：
 * EXPORTV1|<模型>|<prt|asm>|<输出路径(不含扩展名)> */
static wchar_t g_v1_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_v1_out[MAXJOBS][PRO_PATH_SIZE];
static int     g_nv1 = 0;
static int     g_v1_asm[MAXJOBS];

/* 2D 导出作业（图纸 -> DWG/DXF）：EXPORT2D|<图纸>|<输出文件(含扩展名)> */
static wchar_t g_2d_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_2d_out[MAXJOBS][PRO_PATH_SIZE];
static int     g_n2d = 0;

/* 打平装配作业：
 *   FLATTEN|<输入 stp>|<输出无扩展名>|<profile 或 ->|<导入模型名 或 ->
 * 导入 STEP 装配 -> 把下层装配体里的零件按总装坐标直接装到总装下 -> 删掉下层装配体 -> 按 profile 导出 */
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

/* 打平指定子装配（一层）作业：
 *   FLATASM|<总装名>|<子装配名>|<另存为名 或 ->|<dry 或 run>
 * 把指定子装配（须为总装顶层元件）的直接下级元件（零件/子装配都算）按它们在总装坐标系里的
 * 绝对位置矩阵直接装到总装下，然后删掉该子装配元件。 */
static wchar_t g_fa_asm[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_fa_sub[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_fa_out[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_fa_dry[MAXJOBS];
static int     g_nfa = 0;

/* 设置公用名称作业：SETCOMMON|<模型名>|<prt|asm>|<公用名称> */
static wchar_t g_sc_model[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_sc_asm[MAXJOBS];
static wchar_t g_sc_cname[MAXJOBS][PRO_LINE_SIZE];
static int     g_nsc = 0;

/* 读取公用名称作业：READCOMMON|<模型名>|<prt|asm> */
static wchar_t g_rc_model[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_rc_asm[MAXJOBS];
static int     g_nrc = 0;

/* 重命名作业：RENAME|<旧模型名>|<prt|asm>|<新模型名>（等价另存为新名，磁盘版本链会跟着改名） */
static wchar_t g_rn_model[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_rn_asm[MAXJOBS];
static wchar_t g_rn_new[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_nrn = 0;

#define MAXCHILD 512
typedef struct { ProMdl mdl; ProMatrix mat; char name[160]; int is_asm; double vol; } childrec_t;
static childrec_t g_children[MAXCHILD];
static int        g_nchildren = 0;

#define MAXSNAP 4000
typedef struct { char name[160]; double m[4][4]; int used; } snapitem_t;
static snapitem_t g_snapA[MAXSNAP];
static int        g_nsnapA = 0;
static snapitem_t g_snapB[MAXSNAP];
static int        g_nsnapB = 0;

typedef struct { const char *want; int found_id; ProMdl found_mdl; } findsub_t;
typedef struct { ProAssembly top; int parent_id; } subwalk_t;
typedef struct { ProAssembly top; int depth; ProIdTable ids; } dumpwalk_t;
typedef struct { ProAssembly top; int depth; ProIdTable ids; snapitem_t *arr; int *n; } snapwalk_t;

static void log_file_head(const char *path);
static void log_export_head(const wchar_t *out_noext);

/* 统计作业（在 Creo 进程内读，绕过加密）：COUNT|<文件>|<子串> */
static wchar_t g_cnt_file[MAXJOBS][PRO_PATH_SIZE];
static char    g_cnt_pat[MAXJOBS][128];
static int     g_ncnt = 0;

/* 参数作业（进程内精确类型控制；异步侧只能建/改，删不了，改不了类型）：
 *   PARAMDEL|<模型>|<prt|asm>|<参数名>
 *   PARAMSET|<模型>|<prt|asm>|<参数名>|<字符串值>
 * 用途：已有参数是数值型、需要改成字符型时 —— 先 PARAMDEL 再 PARAMSET。 */
static wchar_t g_pm_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_pm_name[MAXJOBS][PRO_NAME_SIZE];
static wchar_t g_pm_val[MAXJOBS][PRO_LINE_SIZE];
static char    g_pm_val8[MAXJOBS][PRO_LINE_SIZE * 2];
static int     g_pm_asm[MAXJOBS];
static int     g_pm_is_set[MAXJOBS];
static int     g_pm_del_first[MAXJOBS];
static int     g_npm = 0;

/* 参数表导出作业（导出“工具>参数”那张表 + 来源/指定/访问列，并在进程内读回打印）：
 *   PARAMD|<模型>|<prt|asm>|<输出基名(不含扩展名，可空)> */
static wchar_t g_pd_model[MAXJOBS][PRO_NAME_SIZE + 8];
static wchar_t g_pd_base[MAXJOBS][PRO_PATH_SIZE];
static int     g_pd_asm[MAXJOBS];
static int     g_npd = 0;

/* 宏/命令作业（触发已注册命令，如 PiCreo 的 SD 系列）：
 *   MACRO|<宏文本>                     直接加载并执行一条命令宏（如 ~ Command `SDCurrentDrawing`;）
 *   QUICKDRAW|<模型>|<prt|asm>         打开模型 -> 执行 PiCreo「快速出图」(SDCurrentDrawing) -> 等产物 PDF
 */
static wchar_t g_macro[MAXJOBS][1024];
static int     g_nmacro = 0;

static wchar_t g_qd_model[MAXJOBS][PRO_NAME_SIZE + 8];
static int     g_qd_asm[MAXJOBS];
static int     g_qd_wait[MAXJOBS];
static int     g_qd_defer[MAXJOBS];
static int     g_nqd = 0;

/* 延迟触发（等待 PiCreo 等模块加载完成后再执行命令宏）：
 * QUICKDRAW 第 4 段带 d（如 QUICKDRAW|nb109_3|prt|300|d）时启用；
 * 本 DLL 的 user_initialize 在启动早期执行，此时 PiCreo 的 SD 命令尚未注册
 *（ProMacroExecute 返回 PRO_TK_NOT_VALID=-26），故用 SetTimer 推迟到主线程消息泵里执行。 */
static ProMdl   g_def_mdl = NULL;
static wchar_t  g_def_model[PRO_NAME_SIZE + 8];
static int      g_def_wait = 300;
static int      g_def_armed = 0;
static UINT_PTR g_def_timer = 0;
static int      g_def_phase = 0;
static int      g_def_tries = 0;
static int      g_def_elapsed = 0;
static unsigned long long g_def_arm_ft = 0;   /* arm time (FILETIME) for output freshness check */

/* 目录内取“最新修改”的匹配文件并读出（进程内读，绕加密）：DUMPNEW|<目录>|<文件名前缀> */
static wchar_t g_dn_dir[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_dn_pfx[MAXJOBS][128];
static int     g_ndn = 0;

/* 在目录里按“文件头含 Process ID: <pid>”找文件并读出（定位本会话 trail）：
 *   DUMPPID|<目录>|<文件名前缀>|<pid> */
static wchar_t g_dp_dir[MAXJOBS][PRO_PATH_SIZE];
static wchar_t g_dp_pfx[MAXJOBS][128];
static int     g_dp_pid[MAXJOBS];
static int     g_ndp = 0;

/* 配置选项作业：GETOPT|<选项> ；SETOPT|<选项>|<值> */
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
        } else if (strncmp(l, "FLATASM|", 8) == 0) {
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
            if (nf < 2) { logf_("[!] bad FLATASM line: %s", l); continue; }
            if (g_nfa >= MAXJOBS) { logf_("[!] too many FLATASM jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_fa_asm[g_nfa], PRO_NAME_SIZE + 8);
            u8_to_wide(trim(fld[1]), g_fa_sub[g_nfa], PRO_NAME_SIZE + 8);
            g_fa_out[g_nfa][0] = 0;
            g_fa_dry[g_nfa] = 0;
            if (nf >= 3 && fld[2][0] && fld[2][0] != '-')
                u8_to_wide(trim(fld[2]), g_fa_out[g_nfa], PRO_NAME_SIZE + 8);
            if (nf >= 4 && (fld[3][0] == 'd' || fld[3][0] == 'D')) g_fa_dry[g_nfa] = 1;
            logf_("[i] FLATASM %s : %s  (save-as=%s, mode=%s)", fld[0], fld[1],
                  g_fa_out[g_nfa][0] ? fld[2] : "(in place)", g_fa_dry[g_nfa] ? "dry" : "run");
            g_nfa++;
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
        } else if (strncmp(l, "SETCOMMON|", 10) == 0) {
            char *rest = l + 10;
            char *fld[4];
            int   nf = 0;
            fld[nf++] = rest;
            while (nf < 4) {
                char *q = strchr(fld[nf - 1], '|');
                if (!q) break;
                *q = 0;
                fld[nf++] = q + 1;
            }
            if (nf < 3) { logf_("[!] bad SETCOMMON line: %s", l); continue; }
            if (g_nsc >= MAXJOBS) { logf_("[!] too many SETCOMMON jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_sc_model[g_nsc], PRO_NAME_SIZE + 8);
            g_sc_asm[g_nsc] = (fld[1][0] == 'a' || fld[1][0] == 'A') ? 1 : 0;
            u8_to_wide(trim(fld[2]), g_sc_cname[g_nsc], PRO_LINE_SIZE);
            logf_("[i] SETCOMMON %s = %s", fld[0], fld[2]);
            g_nsc++;
        } else if (strncmp(l, "READCOMMON|", 11) == 0) {
            char *rest = l + 11;
            char *q = strchr(rest, '|');
            if (!q) { logf_("[!] bad READCOMMON line: %s", l); continue; }
            if (g_nrc >= MAXJOBS) { logf_("[!] too many READCOMMON jobs"); continue; }
            *q = 0;
            u8_to_wide(trim(rest), g_rc_model[g_nrc], PRO_NAME_SIZE + 8);
            g_rc_asm[g_nrc] = (q[1] == 'a' || q[1] == 'A') ? 1 : 0;
            logf_("[i] READCOMMON %s type=%s", rest, g_rc_asm[g_nrc] ? "asm" : "prt");
            g_nrc++;
        } else if (strncmp(l, "RENAME|", 7) == 0) {
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
            if (nf < 3) { logf_("[!] bad RENAME line: %s", l); continue; }
            if (g_nrn >= MAXJOBS) { logf_("[!] too many RENAME jobs"); continue; }
            u8_to_wide(trim(fld[0]), g_rn_model[g_nrn], PRO_NAME_SIZE + 8);
            g_rn_asm[g_nrn] = (fld[1][0] == 'a' || fld[1][0] == 'A') ? 1 : 0;
            u8_to_wide(trim(fld[2]), g_rn_new[g_nrn], PRO_NAME_SIZE + 8);
            logf_("[i] RENAME %s -> %s", fld[0], fld[2]);
            g_nrn++;
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
            g_n2d > 0 || g_nv1 > 0 || g_nflat > 0 || g_nfa > 0 || g_ncnt > 0 || g_npm > 0 || g_npd > 0 ||
            g_nmacro > 0 || g_nqd > 0 || g_ndn > 0 || g_ndp > 0 || g_nsc > 0 || g_nrc > 0 || g_nrn > 0);
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

/* ---------------- 打平装配（FLATTEN） ---------------- */

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
        /* 下层装配体里的零件：记下它在总装坐标系里的位置，稍后直接装到总装下 */
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

    /* 1) 导入 STEP（profile=NULL 用默认；rep=MASTER；先让 Creo 自己判断模型类型）*/
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

    /* 3) 按 profile 导出 */
    if (g_flat_hasprof[k]) {
        ProError ep = ProIntfExportProfileLoad(g_flat_prof[k]);
        logf_("  ProIntfExportProfileLoad -> %d (%s)", (int)ep, errname(ep));
    }
    e = ProIntf3DFileWriteWithDefaultProfile((ProSolid)imp, PRO_INTF_EXPORT_STEP, g_flat_out[k]);
    logf_("  export STEP -> %d (%s)", (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) { ok = 1; log_export_head(g_flat_out[k]); }
    return ok;
}

/* ---------------- 打平指定子装配（一层，FLATASM） ----------------
 * 位置校验：改动前/改动后各采一次“所有叶子零件在总装坐标系下的绝对矩阵”快照做比对；
 * 另存后再 ProMdlErase + 从磁盘复读一次比第三遍（校验落盘文件本身）。 */

static ProError findsub_visit(ProFeature *feat, ProError status, ProAppData app)
{
    findsub_t *f = (findsub_t *)app;
    ProMdl     m = NULL;
    ProName    nm;
    char       u8[160];

    if (!f) return PRO_TK_NO_ERROR;
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &m) != PRO_TK_NO_ERROR || !m) return PRO_TK_NO_ERROR;
    nm[0] = 0;
    ProMdlNameGet(m, nm);
    wide_to_u8(nm, u8, sizeof(u8));
    if (_stricmp(u8, f->want) == 0) {
        if (f->found_id < 0) { f->found_id = feat->id; f->found_mdl = m; }
        else logf_("  [!] more than one top-level component named %s (using id=%d)", f->want, f->found_id);
    }
    return PRO_TK_NO_ERROR;
}

static ProError subchild_visit(ProFeature *feat, ProError status, ProAppData app)
{
    subwalk_t     *w = (subwalk_t *)app;
    ProMdl         m = NULL;
    ProMdlType     mt = (ProMdlType)0;
    ProName        nm;
    ProIdTable     ids;
    ProAsmcomppath path;
    ProMatrix      mat;
    char           u8[160];

    if (!w || g_nchildren >= MAXCHILD) return PRO_TK_NO_ERROR;
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &m) != PRO_TK_NO_ERROR || !m) return PRO_TK_NO_ERROR;
    if (ProMdlTypeGet(m, &mt) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;

    ids[0] = w->parent_id;
    ids[1] = feat->id;
    memset(&path, 0, sizeof(path));
    if (ProAsmcomppathInit((ProSolid)w->top, ids, 2, &path) != PRO_TK_NO_ERROR ||
        ProAsmcomppathTrfGet(&path, PRO_B_TRUE, mat) != PRO_TK_NO_ERROR) {
        logf_("  [!] child id=%d: cannot read position - skipped", feat->id);
        return PRO_TK_NO_ERROR;
    }
    nm[0] = 0;
    ProMdlNameGet(m, nm);
    wide_to_u8(nm, u8, sizeof(u8));
    g_children[g_nchildren].mdl = m;
    g_children[g_nchildren].is_asm = (mt == PRO_MDL_ASSEMBLY) ? 1 : 0;
    memcpy(g_children[g_nchildren].mat, mat, sizeof(ProMatrix));
    strcpy_s(g_children[g_nchildren].name, sizeof(g_children[0].name), u8);
    logf_("  child[%02d] %-3s %-42s id=%-5d t=(%.3f %.3f %.3f)", g_nchildren,
          (mt == PRO_MDL_ASSEMBLY) ? "ASM" : "PRT", u8, feat->id,
          mat[3][0], mat[3][1], mat[3][2]);
    g_nchildren++;
    return PRO_TK_NO_ERROR;
}

/* 只列非元件特征（基准/注释/导入特征等），元件已有 [tree] 行 */
static ProError dump_feat_visit(ProFeature *feat, ProError status, ProAppData app)
{
    ProFeattype t = (ProFeattype)0;

    if (ProFeatureTypeGet(feat, &t) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (t == PRO_FEAT_COMPONENT) return PRO_TK_NO_ERROR;
    logf_("       feat: id=%-5d type=%d", feat->id, (int)t);
    return PRO_TK_NO_ERROR;
}

static ProError dump_visit(ProFeature *feat, ProError status, ProAppData app)
{
    dumpwalk_t *w = (dumpwalk_t *)app;
    ProMdl      m = NULL;
    ProMdlType  mt = (ProMdlType)0;
    ProName     nm;
    char        u8[160];
    const char *ts = "OTH";

    if (!w || w->depth >= PRO_MAX_ASSEM_LEVEL) return PRO_TK_NO_ERROR;
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &m) != PRO_TK_NO_ERROR || !m) return PRO_TK_NO_ERROR;
    if (ProMdlTypeGet(m, &mt) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (mt == PRO_MDL_ASSEMBLY) ts = "ASM";
    else if (mt == PRO_MDL_PART) ts = "PRT";

    nm[0] = 0;
    ProMdlNameGet(m, nm);
    wide_to_u8(nm, u8, sizeof(u8));
    w->ids[w->depth] = feat->id;
    {
        ProAsmcomppath path;
        ProMatrix     mat;
        char          pos[160];
        memset(&path, 0, sizeof(path));
        if (ProAsmcomppathInit((ProSolid)w->top, w->ids, w->depth + 1, &path) == PRO_TK_NO_ERROR &&
            ProAsmcomppathTrfGet(&path, PRO_B_TRUE, mat) == PRO_TK_NO_ERROR)
            sprintf_s(pos, sizeof(pos), " t=(%.3f %.3f %.3f)", mat[3][0], mat[3][1], mat[3][2]);
        else
            strcpy_s(pos, sizeof(pos), " t=?");
        logf_("[tree] %*s%-3s %-42s id=%-5d d=%d%s", (w->depth * 2) + 2, "", ts, u8,
              feat->id, w->depth, pos);
    }
    if (mt == PRO_MDL_ASSEMBLY && w->depth + 1 < PRO_MAX_ASSEM_LEVEL) {
        logf_("[tree] %*s  . own features:", (w->depth * 2) + 4, "");
        ProSolidFeatVisit((ProSolid)m, dump_feat_visit, NULL, w);
        w->depth++;
        ProSolidFeatVisit((ProSolid)m, dump_visit, flat_filter, w);
        w->depth--;
    }
    return PRO_TK_NO_ERROR;
}

static void log_tree_full(ProAssembly asm, const char *tag)
{
    dumpwalk_t w;
    memset(&w, 0, sizeof(w));
    w.top = asm;
    w.depth = 0;
    logf_("  -- tree %s --", tag);
    logf_("[tree] (top) own features:");
    ProSolidFeatVisit((ProSolid)asm, dump_feat_visit, NULL, &w);
    ProSolidFeatVisit((ProSolid)asm, dump_visit, flat_filter, &w);
}

static ProError snap_visit(ProFeature *feat, ProError status, ProAppData app)
{
    snapwalk_t *w = (snapwalk_t *)app;
    ProMdl      m = NULL;
    ProMdlType  mt = (ProMdlType)0;

    if (!w || !w->arr || !w->n || w->depth >= PRO_MAX_ASSEM_LEVEL) return PRO_TK_NO_ERROR;
    if (*w->n >= MAXSNAP) return PRO_TK_NO_ERROR;
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &m) != PRO_TK_NO_ERROR || !m) return PRO_TK_NO_ERROR;
    if (ProMdlTypeGet(m, &mt) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;

    w->ids[w->depth] = feat->id;
    if (mt == PRO_MDL_ASSEMBLY) {
        if (w->depth + 1 < PRO_MAX_ASSEM_LEVEL) {
            w->depth++;
            ProSolidFeatVisit((ProSolid)m, snap_visit, flat_filter, w);
            w->depth--;
        }
    } else {
        ProAsmcomppath path;
        ProMatrix     mat;
        ProName       nm;
        snapitem_t   *it;
        memset(&path, 0, sizeof(path));
        if (ProAsmcomppathInit((ProSolid)w->top, w->ids, w->depth + 1, &path) != PRO_TK_NO_ERROR ||
            ProAsmcomppathTrfGet(&path, PRO_B_TRUE, mat) != PRO_TK_NO_ERROR) {
            logf_("  [!] snapshot: cannot read leaf position (id=%d)", feat->id);
            return PRO_TK_NO_ERROR;
        }
        it = &w->arr[*w->n];
        nm[0] = 0;
        ProMdlNameGet(m, nm);
        wide_to_u8(nm, it->name, sizeof(it->name));
        memcpy(it->m, mat, sizeof(ProMatrix));
        it->used = 0;
        (*w->n)++;
    }
    return PRO_TK_NO_ERROR;
}

static void snapshot_leaves(ProAssembly asm, snapitem_t *arr, int *n)
{
    snapwalk_t w;
    *n = 0;
    memset(&w, 0, sizeof(w));
    w.top = asm;
    w.arr = arr;
    w.n = n;
    ProSolidFeatVisit((ProSolid)asm, snap_visit, flat_filter, &w);
}

/* 按“同名 + 16 元素全量差 <= 1e-3”做多重集匹配；返回 0=PASS */
static int snapshot_compare(snapitem_t *a, int na, snapitem_t *b, int nb, const char *tag)
{
    int    i, j, matched = 0, missed = 0;
    double maxdev = 0.0;

    for (i = 0; i < na; i++) a[i].used = 0;
    for (i = 0; i < nb; i++) {
        int    found = -1;
        double bestdev = 1e30;
        for (j = 0; j < na; j++) {
            int    r, c;
            double dev = 0.0;
            if (a[j].used) continue;
            if (strcmp(a[j].name, b[i].name) != 0) continue;
            for (r = 0; r < 4; r++) {
                for (c = 0; c < 4; c++) {
                    double d = fabs(a[j].m[r][c] - b[i].m[r][c]);
                    if (d > dev) dev = d;
                }
            }
            if (dev < bestdev) { bestdev = dev; found = j; }
        }
        if (found >= 0 && bestdev <= 1e-3) {
            a[found].used = 1;
            matched++;
            if (bestdev > maxdev) maxdev = bestdev;
        } else {
            missed++;
            if (missed <= 15) {
                if (found >= 0)
                    logf_("  [x] %-42s same name but dev=%.6f  t=(%.3f %.3f %.3f)",
                          b[i].name, bestdev, b[i].m[3][0], b[i].m[3][1], b[i].m[3][2]);
                else
                    logf_("  [x] %-42s missing in baseline  t=(%.3f %.3f %.3f)",
                          b[i].name, b[i].m[3][0], b[i].m[3][1], b[i].m[3][2]);
            }
        }
    }
    logf_("  [CHECK %s] baseline=%d current=%d matched=%d missed=%d maxdev=%.6f -> %s",
          tag, na, nb, matched, missed, maxdev, (missed == 0 && na == nb) ? "PASS" : "FAIL");
    return (missed == 0 && na == nb) ? 0 : 1;
}

static void log_outline(ProSolid s, const char *tag)
{
    double   o[2][3];
    ProError e = ProSolidOutlineGet(s, o);
    if (e == PRO_TK_NO_ERROR)
        logf_("  [bbox %s] x=[%.3f,%.3f] y=[%.3f,%.3f] z=[%.3f,%.3f]", tag,
              o[0][0], o[1][0], o[0][1], o[1][1], o[0][2], o[1][2]);
    else
        logf_("  [bbox %s] n/a (%d %s)", tag, (int)e, errname(e));
}

/* 几何体积探针（只读）：判断"随子装配删除而消失的自带特征"是否携带实体几何 */
static double volume_of(ProSolid s, const char *tag)
{
    ProMassProperty mp;
    ProError e = ProSolidMassPropertyGet(s, NULL, &mp);
    if (e != PRO_TK_NO_ERROR)
        e = ProSolidMassPropertyWithDensityGet(s, NULL, PRO_MP_DENS_USE_IF_MISSING, 1.0, &mp);
    if (e != PRO_TK_NO_ERROR) {
        logf_("  [vol] %-42s n/a (%d %s)", tag, (int)e, errname(e));
        return -1.0;
    }
    logf_("  [vol] %-42s volume=%.4f area=%.4f mass=%.4f", tag, mp.volume, mp.surface_area, mp.mass);
    return mp.volume;
}

static int run_flatasm_job(int k)
{
    ProMdl     top = NULL;
    ProError   e;
    ProMdlType mt = (ProMdlType)0;
    char       a8[256], s8[256], o8[256];
    int        i, ok = 0, nassem_fail = 0;
    int        target_id = -1;
    ProMdl     target_mdl = NULL;
    double     v_before = -1.0;

    wide_to_u8(g_fa_asm[k], a8, sizeof(a8));
    wide_to_u8(g_fa_sub[k], s8, sizeof(s8));
    o8[0] = 0;
    if (g_fa_out[k][0]) wide_to_u8(g_fa_out[k], o8, sizeof(o8));

    logf_("---- FLATASM: %s  sub=%s  mode=%s  save-as=%s", a8, s8,
          g_fa_dry[k] ? "DRY" : "RUN", o8[0] ? o8 : "(in place)");

    if (!retrieve_model(g_fa_asm[k], PRO_MDLFILE_ASSEMBLY, &top)) {
        logf_("  FLATASM FAILED (retrieve %s)", a8);
        return 0;
    }
    if (ProMdlTypeGet(top, &mt) != PRO_TK_NO_ERROR || mt != PRO_MDL_ASSEMBLY) {
        logf_("  FLATASM FAILED (not an assembly)");
        return 0;
    }

    log_tree_full((ProAssembly)top, "BEFORE");
    log_outline((ProSolid)top, "BEFORE");
    snapshot_leaves((ProAssembly)top, g_snapA, &g_nsnapA);
    logf_("  baseline leaves = %d", g_nsnapA);

    {
        findsub_t f;
        memset(&f, 0, sizeof(f));
        f.want = s8;
        f.found_id = -1;
        ProSolidFeatVisit((ProSolid)top, findsub_visit, flat_filter, &f);
        target_id = f.found_id;
        target_mdl = f.found_mdl;
    }
    if (target_id < 0) {
        logf_("  FLATASM FAILED: top-level component '%s' not found", s8);
        return 0;
    }
    if (ProMdlTypeGet(target_mdl, &mt) != PRO_TK_NO_ERROR || mt != PRO_MDL_ASSEMBLY) {
        logf_("  FLATASM FAILED: '%s' is not an assembly", s8);
        return 0;
    }
    logf_("  target sub-assembly id=%d", target_id);

    g_nchildren = 0;
    {
        subwalk_t sw;
        memset(&sw, 0, sizeof(sw));
        sw.top = (ProAssembly)top;
        sw.parent_id = target_id;
        ProSolidFeatVisit((ProSolid)target_mdl, subchild_visit, flat_filter, &sw);
    }
    logf_("  direct children collected = %d", g_nchildren);

    /* 体积探针：子装配"自带特征"（坐标系/导入特征等）贡献的实体体积
     * = 子装配体积 - 各直接下级体积之和。明显 > 0 说明删掉该子装配会连带丢几何。 */
    {
        double vsub, vsum = 0.0;
        int    j, vfail = 0;
        v_before = volume_of((ProSolid)top, "TOP (before)");
        vsub = volume_of((ProSolid)target_mdl, "TARGET SUB-ASSY");
        for (j = 0; j < g_nchildren; j++) {
            int q;
            g_children[j].vol = -1.0;
            for (q = 0; q < j; q++) {
                if (g_children[q].mdl == g_children[j].mdl) { g_children[j].vol = g_children[q].vol; break; }
            }
            if (g_children[j].vol < 0.0)
                g_children[j].vol = volume_of((ProSolid)g_children[j].mdl, g_children[j].name);
            if (g_children[j].vol >= 0.0) vsum += g_children[j].vol;
            else vfail++;
        }
        if (vsub >= 0.0 && vfail == 0)
            logf_("  [vol] SUB OWN-FEATURE volume = %.4f   (sub %.4f - children %.4f)",
                  vsub - vsum, vsub, vsum);
        else
            logf_("  [vol] own-feature volume NOT computed (vsub_ok=%d vfail=%d)",
                  (vsub >= 0.0) ? 1 : 0, vfail);
    }

    if (g_fa_dry[k]) { logf_("  (DRY RUN - nothing modified)"); return 1; }
    if (g_nchildren == 0) { logf_("  FLATASM ABORT: no children collected"); return 0; }

    /* 1) 按原绝对位置把下级元件挂到总装下 */
    for (i = 0; i < g_nchildren; i++) {
        ProAsmcomp nc;
        ProError   ea;
        memset(&nc, 0, sizeof(nc));
        ea = ProAsmcompAssemble((ProAssembly)top, (ProSolid)g_children[i].mdl, g_children[i].mat, &nc);
        logf_("  assemble %2d/%d  %-42s -> %d (%s)", i + 1, g_nchildren,
              g_children[i].name, (int)ea, errname(ea));
        if (ea != PRO_TK_NO_ERROR) nassem_fail++;
    }
    if (nassem_fail > 0) {
        logf_("  [!] %d assemble failure(s) - ABORT (nothing deleted, nothing saved)", nassem_fail);
        return 0;
    }

    /* 2) 删掉目标子装配 */
    e = ProFeatureDelete((ProSolid)top, &target_id, 1, NULL, 0);
    logf_("  delete sub-assembly component (id=%d) -> %d (%s)", target_id, (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) {
        int *arr = NULL;
        int  one = target_id;
        if (ProArrayAlloc(0, sizeof(int), 1, (ProArray *)&arr) == PRO_TK_NO_ERROR) {
            ProArrayObjectAdd((ProArray *)&arr, PRO_VALUE_UNUSED, 1, &one);
            e = ProFeatureWithoptionsDelete((ProSolid)top, arr, NULL, PRO_REGEN_NO_FLAGS);
            logf_("  retry ProFeatureWithoptionsDelete -> %d (%s)", (int)e, errname(e));
            ProArrayFree((ProArray *)&arr);
        }
    }
    if (e != PRO_TK_NO_ERROR) { logf_("  [!] delete failed - ABORT (not saving)"); return 0; }

    {
        ProError er = ProSolidRegenerate((ProSolid)top, PRO_REGEN_NO_FLAGS);
        logf_("  regenerate -> %d (%s)", (int)er, errname(er));
    }

    /* 3) 内存态校验 */
    log_tree_full((ProAssembly)top, "AFTER");
    log_outline((ProSolid)top, "AFTER");
    {
        double vafter = volume_of((ProSolid)top, "TOP (after)");
        if (vafter >= 0.0 && v_before >= 0.0)
            logf_("  [vol] TOP volume delta = %.4f  (after %.4f - before %.4f)",
                  vafter - v_before, vafter, v_before);
    }
    snapshot_leaves((ProAssembly)top, g_snapB, &g_nsnapB);
    if (snapshot_compare(g_snapA, g_nsnapA, g_snapB, g_nsnapB, "mem") != 0) {
        logf_("  [!] position check FAILED (in memory) - ABORT (not saving)");
        return 0;
    }

    /* 4) 另存（给名字=另存为新名；给 - = 原文件新版本） */
    if (o8[0]) {
        ProMdlName nn;
        u8_to_wide(o8, nn, (int)(sizeof(nn) / sizeof(wchar_t)));
        e = ProMdlnameRename(top, nn);
        logf_("  ProMdlnameRename(%s) -> %d (%s)", o8, (int)e, errname(e));
        if (e != PRO_TK_NO_ERROR) { logf_("  [!] rename failed - ABORT (not saving)"); return 0; }
    }
    e = ProMdlSave(top);
    logf_("  ProMdlSave -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) { logf_("  [!] save failed"); return 0; }
    ok = 1;

    /* 5) 落盘复读校验 */
    {
        ProError ee = ProMdlErase(top);
        ProMdl   re = NULL;
        logf_("  reload check: ProMdlErase -> %d (%s)", (int)ee, errname(ee));
        if (ee == PRO_TK_NO_ERROR) {
            const wchar_t *rname = o8[0] ? g_fa_out[k] : g_fa_asm[k];
            if (retrieve_model(rname, PRO_MDLFILE_ASSEMBLY, &re)) {
                char r8[256];
                wide_to_u8(rname, r8, sizeof(r8));
                logf_("  reloaded '%s' from disk", r8);
                log_tree_full((ProAssembly)re, "DISK");
                log_outline((ProSolid)re, "DISK");
                snapshot_leaves((ProAssembly)re, g_snapB, &g_nsnapB);
                if (snapshot_compare(g_snapA, g_nsnapA, g_snapB, g_nsnapB, "disk") != 0)
                    logf_("  [!] disk check FAILED - file is saved, but inspect manually");
            } else {
                logf_("  [!] reload check skipped (retrieve failed)");
            }
        }
    }
    return ok;
}

/* ---------------- 参数作业（PARAMDEL / PARAMSET） ---------------- */

/* 删除模型上的某个参数（用于把数值型参数改成字符型：先删后建） */
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

/* 建/改一个字符型参数；type 固定 string（ProParameterWithUnitsCreate + PRO_PARAM_STRING） */
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

/* 赋材料：按名从材料库（search_path 里的 .mtl）读入并设为当前材料。
   材料一旦赋上，图框的 &MSTD（材料）与 &MASS[.4]（重量）才有真值来源。 */
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

/* ---------------- 参数表导出作业（PARAMD） ---------------- */

/* 把文件内容按行打到日志（在本进程内读；兼容 UTF-16LE / UTF-8 / GBK） */
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

/* PARAMD：导出“工具 > 参数”那张表（CSV 指定列 + TXT 会话列），并读回打印 */
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

/* ---------------- 宏/命令作业（MACRO / QUICKDRAW） ---------------- */

/* 加载并执行一条命令宏（与按下 mapkey 等价），例如 L"~ Command `SDCurrentDrawing`;" */
/* 设置模型公用名称（SETCOMMON）：等价 GUI 重命名对话框里的“公用名称” */
static int run_setcommon_job(int k)
{
    ProMdl   mdl = NULL;
    ProError e;
    char     m8[128], c8[512];

    wide_to_u8(g_sc_model[k], m8, sizeof(m8));
    wide_to_u8(g_sc_cname[k], c8, sizeof(c8));
    logf_("---- SETCOMMON %s = %s", m8, c8);

    if (!retrieve_model(g_sc_model[k], g_sc_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  SETCOMMON FAILED (retrieve)");
        return 0;
    }
    e = ProMdlCommonnameSet(mdl, g_sc_cname[k]);
    logf_("  ProMdlCommonnameSet -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;

    e = ProMdlSave(mdl);
    logf_("  ProMdlSave -> %d (%s)", (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

/* 读取模型公用名称（READCOMMON）：只读，不保存 */
static int run_readcommon_job(int k)
{
    ProMdl     mdl = NULL;
    ProError   e;
    wchar_t   *cn = NULL;
    ProBoolean mod = PRO_B_FALSE;
    char       m8[128], c8[512];

    wide_to_u8(g_rc_model[k], m8, sizeof(m8));
    logf_("---- READCOMMON %s", m8);

    if (!retrieve_model(g_rc_model[k], g_rc_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  READCOMMON FAILED (retrieve)");
        return 0;
    }
    e = ProMdlCommonnameGet(mdl, &cn, &mod);
    if (e != PRO_TK_NO_ERROR) {
        logf_("  ProMdlCommonnameGet -> %d (%s)", (int)e, errname(e));
        return 0;
    }
    c8[0] = 0;
    if (cn) wide_to_u8(cn, c8, sizeof(c8));
    logf_("  [commonname] %s = %s  (modifiable=%d)", m8, c8, (int)mod);
    if (cn) ProWstringFree(cn);
    return 1;
}

/* 重命名模型（RENAME）：ProMdlnameRename + 保存；磁盘 .1~N 版本链会改名为新名 */
static int run_rename_job(int k)
{
    ProMdl    mdl = NULL;
    ProError  e;
    char      old8[128], new8[128];

    wide_to_u8(g_rn_model[k], old8, sizeof(old8));
    wide_to_u8(g_rn_new[k], new8, sizeof(new8));
    logf_("---- RENAME %s -> %s", old8, new8);

    if (!retrieve_model(g_rn_model[k], g_rn_asm[k] ? PRO_MDLFILE_ASSEMBLY : PRO_MDLFILE_PART, &mdl)) {
        logf_("  RENAME FAILED (retrieve)");
        return 0;
    }
    e = ProMdlnameRename(mdl, g_rn_new[k]);
    logf_("  ProMdlnameRename -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return 0;

    e = ProMdlSave(mdl);
    logf_("  ProMdlSave -> %d (%s)", (int)e, errname(e));
    return (e == PRO_TK_NO_ERROR) ? 1 : 0;
}

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

/* 轮询等待 <cwd>/<模型>.pdf 出现（PiExport 默认出 PDF），出现后打印明文大小/文件头 */
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

/* 若模型没有名为 front 的定向视图，则按“最短轴=板法向”自动创建一个（仅内存，不保存模型）。
   快速出图的主视图由 stk_drawing_config.ini 的 STK_MAIN_VIEW=front|XY|Z 决定：
   有 front 视图就用它，否则退回 XY|Z —— 这正是此前“3D 摆放不正”的原因。 */
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
        /* 注意：此处不调用 ProWstringArrayFree —— 实测（强转类型桥接）会触发
           Creo fatal error（Auto-traceback 弹窗、进程挂起）。泄漏几十字节可忽略。 */
    }
    if (found) {
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

    /* 最短尺寸轴 = 板件法向(朝观察者)；面内较长轴 = 屏幕水平方向 */
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
           拿不到则退回“当前视图(NULL)” */
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
    /* 不调用 ProMdlSave：视图仅存内存，磁盘上的模型文件不变 */
}

/* 导出 PDF（模型=图纸或零件；输出到当前目录 <base>.pdf） */
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

/* 保存图纸前“让路”：把目标目录里已存在的 <model>.drw.* 移到 work\old\ 归档。
   背景：磁盘上残留的 drw 版本文件（如异常退出/恢复会话留下的）会让
   ProMdlSave 在“同名已存在”场景下无限等待（实测挂起）。移开后即同 14:00 成功场景。 */
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

/* 技术要求注释：按样本风格用 ProDtlnote API 直接写入图纸（右下区域）。
   中文文本在中文语言的会话里渲染/导出正常（与 PDF 字体问题同源）。 */
static void add_technote(ProMdl drw)
{
    static const wchar_t *LINES[] = {
        L"技术要求",
        L"1. 表面需光洁、无划痕、油污，无明显变形，毛刺高度<0,05，冲裁方向与凸点方向相反",
        L"2. 材料硬度为: 120-170HV",
        L"3. 所有增加自动点焊凸点处， 除凸点外镜片应平整，不可起翘变形从而影响自动点焊效果",
        L"4. 能通过8小时5%浓度盐雾测试",
        L"5. 未注圆角 R0.5"
    };
    ProDtlnotedata data = NULL;
    ProDtlattach   attach = NULL;
    ProDtlnote     note;   /* struct type (ProModelitem family), not a pointer */
    ProVector      loc;
    ProError       e;
    int i;

    /* location is in SCREEN client-area pixels (origin lower-left), converted
       by Creo to sheet space with the live view scale. Calibrated with MARK
       probes: sheet_mm ~= px * 0.4. Target like the sample drawings: text block
       lower-left at ~ (208mm, 104mm) -> px ~ (500, 250). y=300: 上移避免
       长条文行压到标题栏/图框。 */
    loc[0] = 500.0;
    loc[1] = 300.0;
    loc[2] = 0.0;

    e = ProDtlnotedataAlloc(drw, &data);
    logf_("  [tech] dataAlloc -> %d (%s)", (int)e, errname(e));
    if (e != PRO_TK_NO_ERROR) return;

    for (i = 0; i < 6; i++) {
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

/* 显示模型注释：把 3D 里的尺寸/公差等“捞”到图纸（教程第 3 章的“主力做法”） */
static void show_model_annotations(ProMdl drw)
{
    /* PRO_AXIS 已移除：模型轴在图纸上是"全长"线，污染图面；6 个凸点中心线
       改由 boss_centerlines() 用 ProDtlentity 手画短十字（已验证）。 */
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
   41.30=总宽、23.00=端子盘横向中心距、23.50=端子盘纵向中心距、
   3.30=端子孔径、2.00=凸点高度、0.25=料厚、4.80=台阶、9.65=深度；
   盘径 Ø20 与 R1/R0.5 不单标（圆角并入技术要求“未注圆角 R0.5”），
   其余草图构造值（163/22.35/24.7/35.55...）与中间值（5/10/7/3...）全部不标。 */
static int value_in_keep_list(double v)
{
    /* 6×Ø3.3 已按用户裁定删除（五张人样一个 n×Ø 都没有 → 归"非功能圆径"不标） */
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

/* 正攻法：不“全显后拭除”，而是只把关键尺寸逐个“显示”到合适的视图。
   对不匹配的视图，ProAnnotationShow 会返回 PRO_TK_INVALID_MATRIX（方向冲突），
   相当于让 Creo 自己判断每个尺寸应该出现在哪个视图。 */
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
    /* 轮次26：已改由图纸侧创建的尺寸不再从模型显示（避免新旧重复）。
       Undisplay 对 Show 过的注释无效、且本 SDK 无 ProAnnotationErase ——
       最干净的做法是让它们从一开始就【不显示】。 */
    /* 轮次32（用户裁定 R1：所有尺寸必须由工具包创建、禁止用显示）。
       至此跳过清单覆盖全部"改为创建"的值：2 / 23 / 23.5 / 9.65 / 41.3 / 0.25 / 4.8
       —— 41.3（不可检验+冗余）、4.8（几何不可见）就此彻底不出现在图上；
       0.25 由 CONVDIM 创建。清单漏值 = 尺寸悄悄走显示混进交付件（轮26-31 的教训）。 */
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
    /* 先把公差/前后缀设好，再显示：显示瞬间的注释文本会带上当前属性 */
    {
        /* 关键尺寸加公差：装配定位 ±0.1、功能尺寸 ±0.05（国标规则：配合/定位必标公差） */
        double tol = -1.0;
        if ((v > 22.99 && v < 23.01) || (v > 23.49 && v < 23.51) || (v > 19.99 && v < 20.01)) tol = 0.10;
        else if ((v > 1.99 && v < 2.01) || (v > 0.24 && v < 0.26) || (v > 4.79 && v < 4.81)) tol = 0.05;
        if (tol > 0) {
            ProDimToleranceType pt0 = (ProDimToleranceType)0;
            double pu0 = -999, pl0 = -999;
            ProName tab0;
            /* 写之前先读模型自带公差：区分"显示的是模型自带还是设置副作用"（P0c） */
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
                ProError e2 = ProDimensionToleranceSet(dim, tol, -tol);   /* 上 +tol / 下 -tol */
                double up = -999, lo = -999;
                ProError e3 = ProDimensionToleranceGet(dim, &up, &lo);
                logf_("[showk]   tol sym %.2f type->%d set->%d ; readback up=%.3f lo=%.3f (%d)",
                      tol, (int)e1, (int)e2, up, lo, (int)e3);
            }
        }
        /* 端子孔数量前缀：6×Ø3.3（Ø = U+00D8，Creo 的直径符号） */
        if (v > 3.29 && v < 3.31) {
            ProError e3 = ProDimensionPrefixSet(dim, (wchar_t *)L"6\u00D7\u00D8");   /* 6×Ø */
            logf_("[showk]   prefix 6x-dia -> %d (%s)", (int)e3, errname(e3));
        }
        /* 功能类尺寸加 △ 标记（五张人样硬规则：2±0.05△ / 0.25±0.05△ / 4.8±0.05△）；
           0.25 同时以此替代模型自带的英文后缀 "THICK" */
        if ((v > 1.99 && v < 2.01) || (v > 0.24 && v < 0.26) || (v > 4.79 && v < 4.81)) {
            ProError e4 = ProDimensionSuffixSet(dim, (wchar_t *)L"\u25B3");   /* △ U+25B3 */
            logf_("[showk]   suffix tri -> %d (%s)", (int)e4, errname(e4));
        }
    }
    e = ProAnnotationShow((ProAnnotation *)dim, NULL, c->view);
    logf_("[showk] %.4f -> view %d : %d (%s)", v, (int)c->view, (int)e, errname(e));
    if (e == PRO_TK_NO_ERROR) {
        /* 轮次32 自检：交付前日志里不允许出现任何 SHOWN —— 出现即说明有尺寸走了"显示" */
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

/* 整理尺寸（教程 clm）：对全图自动分层排布，options 走当前 detail options */
static void cleanup_dims(ProMdl drw)
{
    ProError e0, e;

    /* 调大整理间距：更接近样本的规整分层（偏移/增量按 A3 图幅试值） */
    e0 = ProMdlDetailOptionSet(drw, (wchar_t *)L"default_clean_dim_offset", (wchar_t *)L"0.5");
    logf_("[clm] set offset -> %d (%s)", (int)e0, errname(e0));
    e0 = ProMdlDetailOptionSet(drw, (wchar_t *)L"default_clean_dim_increment", (wchar_t *)L"0.6");
    logf_("[clm] set increment -> %d (%s)", (int)e0, errname(e0));

    e = ProDrawingDimensionsCleanup((ProDrawing)drw, NULL);
    logf_("[clm] dimensionsCleanup -> %d (%s)", (int)e, errname(e));
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 保存前清除模型“待重算”状态：改过尺寸属性（公差/前缀）后直接 ProMdlSave 会死等（实测） */
static void prep_save_regen(void)
{
    ProMdl part = NULL;
    if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
        ProError erg = ProSolidRegenerate((ProSolid)part, 0);
        logf_("[save-fix] ProSolidRegenerate -> %d (%s)", (int)erg, errname(erg));
    } else {
        logf_("[save-fix] part retrieve failed");
    }
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
        /* 移动前文本位置（图纸坐标读回） */
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
        /* 移动后文本位置 */
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

/* LEADER（轮次19，含撤销支持）：`LEADER|<值>` = 只读（打印 config 实测值）；
   `LEADER|<值>|<mode>` = 设置 config（1=LINEAR, 2=CENTER_LEADER；轮次19 撤销时用 1）。
   只改显示配置，不动测量附件。 */
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

/* ===== 轮次22 实验：把 ProSelection 注入运行中的 hd 命令（ProSelect 机制）=====
   SELINJ|<mode>：先扫出零件两条端部竖边，构造带主视图上下文的 ProSelection，
   然后按 mode 尝试注入：
     mode 1 = ProSelectExternalselectionRecord（文档："Places external selection into ProSelect() buffer"）
     mode 2 = ProSelbufferSelectionAdd（UI 选择缓冲区）
     mode 4 = mode 1 但去掉视图上下文
   运行前应先用 MACRO|~ Command `SDHorizonDimension` ; 启动 hd。 */
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

/* ===== 轮次23：给基础草绘加总长尺寸（ProSecdimCreate —— 唯一未试过的路线）=====
   遍历 PRO_FEAT_PROTRUSION → SectionCopy → 枚举 2D 线实体 →
   找一对"平行且间距最接近 63.6"的端线（同时把实测距离打进日志）→
   ProSecdimCreate(PRO_TK_DIM_LINE_LINE) → 回读值 → dims 遍历验证 → 再生 + bbox 对比。
   SECDIM|<mode>：mode 0（默认）= 仅创建；mode 1 = 创建后 Strengthen（强尺寸）。
   注意：不 Save（本环境保存会死等）；改的是会话内模型。 */
typedef struct {
    int        n;
    int        fid[8];
    int        nsec[8];
    ProFeature feat[8];
} secfinder_t;

static void dims_report(void);   /* 前向声明（定义在后面） */

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
    /* —— 验证链：打印全部模型尺寸（看 63.60 是否出现）→ 再生 → bbox 对比 —— */
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

/* ===== 轮次24：总长路线A 阶段1 —— 元素树重定义提交（ProFeatureWithoptionsRedefine）=====
   REDEF：备份记录（bbox + 特征树快照）→ ElemtreeExtract(基础拉伸) → 定位 PRO_E_SKETCHER
   → ValueGet/DataGet 取 section 指针 → 重扫端线对(≈63.6) → ProSecdimCreate →
   ProFeatureWithoptionsRedefine 提交 → 再生 → bbox 守门（63.580×41.100×4.000）→ dims 遍历验证。
   任何一步失败即停并完整记录。 */
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
                /* —— 提交重定义 —— */
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
                /* —— 再生 + bbox 守门 —— */
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
                /* —— 决定性验证：能否遍历到 63.6000 —— */
                dims_report();
            } else {
                logf_("[redef] sketcher special value unavailable");
            }
        }
        ProFeatureElemtreeFree(&f.feat[k], &tree);
        logf_("[redef] done");
    }
}

/* ===== 轮次25：图纸侧标总长 63.6（照抄官方 UgDrawingDimensions.c 的结构）=====
   关键三点（此前全错）：
   ① ProDimAttachment 数组 2 个条目，每项 [0]=选择副本、[1]=NULL；
   ② ProSelectionViewSet(view, &sel) 传【地址】；
   ③ ProDrawingDimensionCreate 的 orient 参数传 PRO_DIM_ORNT_NONE（朝向放 sense）。
   顺序：找主视图 → 扫两条端部竖边 → 选择+视图上下文 → 附件/朝向数组 →
   创建 → ProDimensionValueGet 回读 → ProAnnotationShow 显示。 */
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
        if (ProSolidOutlineGet((ProSolid)part, box) != PRO_TK_NO_ERROR) { logf_("[tot2] bbox failed"); return; }
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
                if (cx < box[0][0] - 0.5 || cx > box[1][0] + 0.5) continue;
                if (cx < blx) { blx = cx; best_l = hid; }
                if (cx > brx) { brx = cx; best_r = hid; }
            }
        }
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
        ProSelectionViewSet(front, &att[0]);          /* ★ 官方：传地址 */
        if (ProSelectionAlloc(NULL, (ProModelitem *)&gi2, &att[1]) != PRO_TK_NO_ERROR) { logf_("[tot2] sel R failed"); return; }
        ProSelectionViewSet(front, &att[1]);
        logf_("[tot2] selections built (view ctx by-pointer)");

        ProArrayAlloc(2, sizeof(ProDimAttachment), 1, (ProArray *)&arr);   /* ★ 2 条目 */
        ProArrayAlloc(2, sizeof(ProDimSense), 1, (ProArray *)&sen);
        ProSelectionCopy(att[0], &arr[0][0]);   /* ★ [0]=选择副本 */
        arr[0][1] = NULL;                       /* ★ [1]=NULL */
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

/* ===== 轮次24 尝试3：section 副本"写入是否提交"的计数定案 =====
   copyA 取尺寸数 → 在 copyA 上创建 63.6 → copyA 再数 → 全新 copyB 数：
   若 copyB 数量 == copyA(创建后) ⇒ 提交成功（弱尺寸，模型遍历不可见）；
   若 copyB == 原始数 ⇒ 副本写入不提交。 */
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

/* ===== 轮次26（任务2）：把模型自带（显示出来）的尺寸转成图纸侧创建 =====
   CONVDIM|<值>：对每个"已显示且值匹配"的模型尺寸：
   ① ProDimensionAttachmentsGet 读原附件/朝向（同一测量对象！）；
   ② 选择加视图上下文后原样重建 → ProDrawingDimensionCreate（验证过的结构）；
   ③ 复刻公差/△（与 showk 同规则）→ ProAnnotationShow 新尺寸；
   ④ ProAnnotationUndisplay 旧尺寸（同值只转一根；其余同值旧实例一并隐藏）；
   ⑤ ProDrawingDimensionMove 到旧文本位置（排版不变）。
   —— 不 regen（Undisplay 的"直到下次再生"约束正好满足）。 */
typedef struct {
    double     target;
    ProDrawing drw;
    ProView    view;
    ProMdl     part;
    int        converted;
    int        hidden;
    int        has_pos;      /* 1 = 使用显式目标位置（tx,ty） */
    double     tx, ty;
    int        view_kind;    /* 0=front（主视图）, 1=左侧（侧视图，4.8 用） */
    int        route;        /* 0=边对（默认）, 1=面附件（41.3/4.8）, 2=边对+同平面过滤（0.25）, 3=视图平面共面实验 */
    int        only_shown;   /* 1 = 只处理图纸上真正显示的实例（同值多实例防拿错几何） */
    int        attempted;    /* 轮次32：该值已尝试过一轮——同值多实例不再重复试建（预算=2） */
    int        no_move;      /* 轮次35 实验B：创建即最终位置，跳过 ProDrawingDimensionMove */
    ProVector  vp_n;         /* route 3：从视图推导的注释平面法向（模型空间，已归一） */
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
    if ((v > 1.99 && v < 2.01) || (v > 0.24 && v < 0.26) || (v > 4.79 && v < 4.81)) {
        ProError e4 = ProDimensionSuffixSet(dim, (wchar_t *)L"\u25B3");
        logf_("[conv]   suffix tri -> %d (%s)", (int)e4, errname(e4));
    }
}

/* ===== 轮次27：面附件路线（41.3 / 4.8）=====
   41.3/4.8 是厚度/深度类尺寸，附件是【面】而不是边对；r26 的边对扫描对它们无候选。
   面来源：ProSurfaceInit + ProSurfaceDataGet（旧 faceold 版验证过枚举可行），
   选择用 ProSurfaceToGeomitem + ProSelectionAlloc，附件结构仍用已验证的
   2 条目、每项 [1]=NULL（与 tot_v2 相同）。 */
/* ===== 轮次45 改造A/B（公共工具）：包围盒 / 边合法性 / 中心合法性 / 矩形与线段几何 =====
   合法性两道检查（简报要求）：① 两端点全在 ProSolidOutlineGet 包围盒内（X/Y/Z 三轴都查）；
   ② 长度 ≤ 该边主轴方向的 bbox 跨度（如 283.6 > 41.1 的辅助构造线 ⇒ 直接排除）。 */
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
typedef struct { int ia, ib; double dist, err, prox; } conv_fpair_t;   /* 匿名结构体不能跨声明点赋值（C2440） */
typedef struct { int ia, ib; double dist, err, mz; } conv_chkpair_t;

static int conv_faces_collect(ProMdl part, conv_fplane_t *FP, int cap)
{
    int id, n = 0;

    for (id = 100; id <= 4000 && n < cap; id++) {   /* id>=100 跳过基准面（同 faceold） */
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
   成功则复刻公差/△、显示新的、隐藏旧的、移到 p0。返回 1=成功。 */
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
    if (sensekind == 1) {          /* 圆-圆：两点圆心 */
        sen2[0].type = PRO_DIM_SNS_TYP_PNT; sen2[0].sense = PRO_POINT_TYP_CENTER; sen2[0].orient_hint = orienth;
        sen2[1].type = PRO_DIM_SNS_TYP_PNT; sen2[1].sense = PRO_POINT_TYP_CENTER; sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
    } else if (sensekind == 2) {   /* 线-圆：线 + 圆心 */
        sen2[0].type = PRO_DIM_SNS_TYP_NONE; sen2[0].orient_hint = orienth;
        sen2[1].type = PRO_DIM_SNS_TYP_PNT; sen2[1].sense = PRO_POINT_TYP_CENTER; sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
    } else {                       /* 线-线 / 面-面 */
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
    }
    ProArrayFree((ProArray *)&arr2);
    ProArrayFree((ProArray *)&sen2);
    return ok;
}

/* 面路线本体：平行平面对（间距≈目标）→ 取 top-2 试建（上限 2，遵照本轮预算）。 */
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
            /* 轮次32：排除"法向平行于视向"的面对（在该视图里投影退化，不可能在此视图标注）。
               前视图视向=z（掉 |nz|>0.9）；侧视图视向=x（掉 |nx|>0.9）。 */
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
    /* 按 err*1000+prox 升序 */
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
            /* orient_hint 由法向主分量定：X 向→水平（主视图）/ Y 向→垂直 / Z 向→水平（侧视图） */
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

/* ===== 轮次33：Z 向间距路线（route Z）=====
   用户口径的 0.25：0.25 厚法兰台阶的"顶面/底面"两条长边——间距在 **Z** 方向，
   在俯视图里渲染为"竖直"尺寸（上下箭头相对）。2D(x,y) 距离扫描对这种纯 Z 间距
   投影后 dist=0，永远进不了候选（20+ 次试建全盲的根因之一）。
   本路线：只收"方向⊥视向、同深度、x/y 投影重合、3D 垂距≈目标"的平行线对。 */
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
            /* 方向必须⊥视向（否则在该视图投影退化，无法标注） */
            if (c->view_kind == 2)      { if (fabs(ua[1]) > 0.3) continue; }   /* 俯视图：视向=y */
            else if (c->view_kind == 1) { if (fabs(ua[0]) > 0.3) continue; }   /* 侧视图：视向=x */
            else                        { if (fabs(ua[2]) > 0.3) continue; }   /* 主视图：视向=z */
            /* 同深度：y（俯视图）基本相同 */
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
            if (dz2 < 0.1) continue;      /* 只收真 z 间距的对 */
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
    for (a = 1; a < nD; a++) {   /* 按 score 升序 */
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
            /* 轮次33：0.25 = 竖直测量（俯视图里 z 间距呈竖直；用户图2 上下箭头相对）→ 显式 VERT */
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
    /* 轮次26：不再要求"已显示"——showk 对已转出的尺寸已不显示，
       本命令负责一次性创建（同值多实例时只建一根）。 */
    if (ProAnnotationIsShown((ProAnnotation *)dim, (ProDrawing)c->drw, &shown) != PRO_TK_NO_ERROR)
        shown = PRO_B_FALSE;
    logf_("[conv] %.4f id=%d shown=%d", v, (int)dim->id, (int)shown);
    /* 轮次27：同值多实例时（0.25 有三胞胎），只处理图纸上真正显示的那一根——
       否则会拿隐藏实例的 3D 位置去反查几何，锚错特征（r26 的 8 连败嫌疑点） */
    if (c->only_shown && !shown) {
        logf_("[conv]   not the displayed instance; skip (shown-preference)");
        return PRO_TK_NO_ERROR;
    }

    if (c->converted > 0) {
        /* 同值的其它已显示实例（罕见重复）：只隐藏，不再建 */
        ProError eh = ProAnnotationUndisplay((ProAnnotation *)dim, NULL, (ProDrawing)c->drw);
        logf_("[conv]   extra instance hidden -> %d (%s)", (int)eh, errname(eh));
        if (eh == PRO_TK_NO_ERROR) c->hidden++;
        return PRO_TK_NO_ERROR;
    }
    /* 轮次32：同值多实例（0.25 三胞胎）——一轮尝试后其余实例直接跳过，
       防止"每个实例各试 2 次"把预算翻倍（r32 实测踩过：3 实例 = 6 次试建） */
    if (c->attempted) {
        logf_("[conv]   same value already attempted; skip (budget cap 2)");
        return PRO_TK_NO_ERROR;
    }
    c->attempted = 1;
    {
        /* 旧文本位置（图纸坐标；有显式目标则优先用）+ 旧尺寸 3D 位置（就近判据） */
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
        if (c->route == 1) {           /* 面附件路线（41.3 / 4.8） */
            conv_face_route(c, dim, v, p0, dl);
            return PRO_TK_NO_ERROR;
        }
        if (c->route == 5) {           /* 轮次33：Z 向间距路线（0.25 法兰台阶） */
            conv_zgap_route(c, dim, v, p0, dl);
            return PRO_TK_NO_ERROR;
        }
        {
            /* —— 按值反查几何：扫 LINE 与 CIRCLE/ARC 边（不依赖 AttachmentsGet——实测 -27）—— */
            struct { int id; double mx, my, mz, ux, uy; double ax, ay, az, bx, by, bz, len; } LL[400];
            struct { int id; double cx, cy, cz; } CC[400];
            struct cand_t { int kind; int ia; int ib; double dist, err, dz, prox, score; };
            struct cand_t CD[600];
            int nL = 0, nC = 0, nD = 0, hid, a, b;
            int nDropC = 0, nDropLog = 0;   /* route C（0.25）：同平面过滤的丢弃计数 */

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
                t = gd->data.p_curve_data->line.type;   /* 所有联合体成员首字段都是 type */
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

            /* 候选一：线-线（平行、垂距≈目标） */
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
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤：两条线必须同平面且在标注平面内 */
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
                    else if (c->route == 3 && c->vp_ok) {   /* route O：视图平面共面判定 */
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
            /* 候选二：圆-圆（圆心距≈目标） */
            for (a = 0; a < nC; a++) {
                for (b = a + 1; b < nC; b++) {
                    double dx = CC[b].cx - CC[a].cx;
                    double dy = CC[b].cy - CC[a].cy;
                    double dist = sqrt(dx * dx + dy * dy);
                    double err = fabs(dist - c->target);
                    double dz, prox;

                    if (err > 0.06 || nD >= 600) continue;
                    dz = fabs(CC[a].cz - CC[b].cz);
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤（圆-圆） */
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
            /* 候选三：线-圆（圆心到直线垂距≈目标；4.8 这类高度尺寸） */
            for (a = 0; a < nL; a++) {
                for (b = 0; b < nC; b++) {
                    double dxc = CC[b].cx - LL[a].mx;
                    double dyc = CC[b].cy - LL[a].my;
                    double dist = fabs(dxc * (-LL[a].uy) + dyc * LL[a].ux);
                    double err = fabs(dist - c->target);
                    double dz, prox;

                    if (err > 0.06 || nD >= 600) continue;
                    dz = fabs(CC[b].cz - LL[a].mz);
                    if (c->route == 2 && dl[2] > -9998.0) {   /* 同平面过滤（线-圆） */
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
            /* 按 score 升序（插入排序） */
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
            if (c->route == 3) {   /* 轮次28：候选明细（端点/长度/该对所在平面 法向+偏移），供共面判定留证 */
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
                    if (c->route == 7) orienth = PRO_DIM_ORNT_VERT;    /* 轮次38 实验：强制 VERT */
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
                        sen2[0].type = PRO_DIM_SNS_TYP_NONE;         /* 线 */
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_PNT;          /* 圆 → 圆心 */
                        sen2[1].sense = PRO_POINT_TYP_CENTER;
                        sen2[1].orient_hint = PRO_DIM_ORNT_NONE;
                    }
                    if (c->route == 8) {
                        /* 轮次39 实验 N：不使用 PNT/CENTER——把两个圆"当边"（NONE sense），
                           等价于人在 UI 里 Ctrl 选两条可见的圆边去标 */
                        sen2[0].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[0].orient_hint = orienth;
                        sen2[1].type = PRO_DIM_SNS_TYP_NONE;
                        sen2[1].orient_hint = orienth;
                        logf_("[conv]   route N senses: NONE/NONE (circles as edges)");
                    }
                    if (c->route == 4 && kind == 0) {
                        /* 轮次32 route V：附件改用"边 + 端点"（PNT sense）——量最近的角点对
                           （0.25 缺口的角点到角点；与 NONE/面两类附件都不同的一种附件类） */
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
                        /* 轮次28：route 3 显式传 HORIZ（对照实验）；轮次38：route 7 显式传 VERT；其余路线保持 NONE */
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
                    }
                    ProArrayFree((ProArray *)&arr2);
                    ProArrayFree((ProArray *)&sen2);
                    ProSelectionFree(&att2[0]);
                    ProSelectionFree(&att2[1]);
                    ProArrayFree((ProArray *)&att2);
                }
                if (!done) {
                    /* 轮次32 R1：全部候选失败不再回显旧尺寸（禁止"显示"出图——缺比显好） */
                    logf_("[conv] *** all %d attempts failed; old dim NOT re-shown (R1: display forbidden) ***",
                          nD < cap ? nD : cap);
                }
            }
        }
    }
    return PRO_TK_NO_ERROR;
}

/* 轮次27：预扫描——该值有多少"已显示"的实例（决定是否启用 shown 优先） */
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

/* 遍历模型尺寸：打印 id/类型/值（用于挑选“像人一样”的关键尺寸白名单） */
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
    c.no_move = (route == 6 || route == 7 || route == 8) ? 1 : 0;   /* 轮次35/38/39 实验：创建即终位、不做 Move */
    if (route == 3) {
        /* 轮次28：视图注释平面（从模型 FRONT 视图推导）——0.25 的 dl 读不出（-9999），改用它做共面判定 */
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
                /* 左侧视图（名含 左侧；4.8 量的是零件厚度方向，标在侧视图上） */
                if (wcsstr(nm, L"\u5DE6\u4FA7")) { c.view = views[vi]; break; }
            } else if (view_kind == 2) {
                /* 俯视图（名含 顶部；0.25 的 z 间距在这里呈竖直） */
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

/* ===== 轮次27：FACHK|<值> —— 只读面诊断 =====
   枚举全部平面面，列出平行面片对里间距≈目标（±1.0，比试建窗口宽）的组合，
   用于开建之前确认"41.3/4.8 到底对应哪两个面"，避免盲试。 */
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
    /* 按 err 升序 */
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

/* ===== 轮次30：GEOQ 只读几何转储（调查专用；不创建/不修改任何东西） =====
   GEOQ|P|<值>        按值反查几何（只读版）：线-线/圆-圆/线-圆 候选对，±0.06，日志前 10
   GEOQ|L|<轴>|<值>   中点坐标 ≈ 值的直线边清单（0=x,1=y,2=z,容差 0.8）
   GEOQ|F|<轴>|<值>   轴向平面面清单（|n_轴|>0.99 且平面常数 ≈ 值） */
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
    /* 只读版"按值反查"（不创建任何东西）：与 CONVDIM 同款扫描，只打日志 */
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
    for (a = 1; a < nD; a++) {   /* 按 score 升序 */
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

/* 轮次43：GEOQ|Z|<vk>|<val> —— 只读 z 向间距扫描（route Z 的只读版，绝不创建）。
   规则与 route Z 一致：平行 + 方向⊥视向(vk) + 同深度(y) + x/y 投影重合 + 3D 垂距≈目标 + 真 z 差。 */
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
    ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part);
    logf_("[dims] retrieve -> %d (%s)", (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR || !part) return;
    {
        ProError ev = ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_visit_cb, NULL, NULL);
        logf_("[dims] solidDimensionVisit -> %d (%s)", (int)ev, errname(ev));
    }
}

/* ===== 轮次31：DDIMS 只读转储"存在图纸里的尺寸"（dd/rdd/add 名 —— 图纸侧创建/用户手标；
   不含"模型自带并显示的"。用于查用户新标的尺寸 + 我们转换出来的尺寸。绝不改动任何东西。 ===== */
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

        if (ProDimensionLocationGet(dim, NULL, drw, &loc) == PRO_TK_NO_ERROR && loc)
            ProDimlocationTextGet(loc, &he, tp, &el);
        logf_("[ddims] id=%d type=%d value=%.4f shown=%d text=(%.1f,%.1f)",
              (int)dim->id, (int)dim->type, v, (int)sh, tp[0], tp[1]);
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

/* ===== 轮次31：AFIND 只读转储"模型注解元素"（3D 注释——用户可能在零件里标的尺寸）。 ===== */
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

/* ===== 轮次37：DARROW|<值>|<类型> —— 对图纸上该值的尺寸逐箭头设 ProLeaderType
   （实验：10=PROLEADERTYPE_DOUBLEARROW, 1=ARROWHEAD；只读之外唯一改动=箭头类型） */
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

/* ===== 轮次43：FDIM|idA|idB|x|y|O|V —— 直选两个面建尺寸（R3："看着那两个面去选"）。
   O: H/V（orient_hint）；V: F=主视图 / S=侧视图。创建即终位（不做 Move）、无兜底、如实打印 Creo 算值。 */
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
                if (wcsstr(nm, L"\u5DE6\u4FA7")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { view = views[i]; break; }
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

/* ===== 轮次44：EDIM|idA|idB|x|y|O|V —— 直选两条边建尺寸（63.6 界线锚点调查后的专用工具）。
   O: H/V orient；V: F=主视图 / S=侧视图（参数样式与 FDIM 相同）。 */
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
                if (wcsstr(nm, L"\u5DE6\u4FA7")) { view = views[i]; break; }
            } else {
                if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { view = views[i]; break; }
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

/* ===== 轮次45 改造B/C 运行时：视图适配 / 占用图 / 位置可用空间搜索 / 界线验证 / AUTOBUILD =====
   位置决定全部来自"可用空间搜索"（无命令传入坐标、无视图中心+固定值、无硬编码兜底）：
   占用图 = 各视图框 + 图幅页边 + 标题栏/技术要求（模板固定件）+ 已放尺寸（线框+界线带）；
   候选 = 方向 × 层距，评分 = min(间隙V,间隙F)×0.2 − 层序号；界线预检（不得穿其它视图）不通过即弃。
   放完读回界线（ProDimlocationWitnesslinesGet）：端点须在自身视图框 120px 内、界线不穿其它视图——日志留证。 */

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

static void ab_occ_reset(void) { g_abnv = 0; g_abnfurn = 0; g_abndr = 0; }

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
            } else if (wcsstr(nm, L"\u9876\u90E8")) {
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

/* 位置搜索：orient 0=竖排(方向=左右) 1=横排(方向=上下)；gx/gy=几何投影范围（px） */
typedef struct { int orient, dir1, dir2, ownvi; double gx0, gy0, gx1, gy1; } ab_req_t;

static void ab_search(const ab_req_t *q, double *olx, double *oly, int *ok)
{
    static const double LAY[11] = { 12.0, 18.0, 26.0, 36.0, 48.0, 62.0, 80.0, 96.0, 110.0, 130.0, 155.0 };
    const ab_view_t  *own = &g_abv[q->ownvi];
    double best = -1e9;
    int    bd = 0, bl = 0, d, L;

    *ok = 0; *olx = 0; *oly = 0;
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
            if (lr.x0 < 20.0 || lr.x1 > 980.0 || lr.y0 < 88.0 || lr.y1 > 755.0) {
                valid = 0; sprintf_s(why, sizeof(why), "off-sheet");
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
                double need = (q->orient == 1) ? 10.0 : 16.0;   /* 竖排矩形gap16 ⇒ 线间距≈30px；横排堆叠10 */

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
            score = valid ? ((gv < gf ? gv : gf) * 0.2 - LAY[L]) : -1e9;   /* 罚项=实际层距像素（防远推） */
            logf_("[pos] cand dir=%s layer=%.0f loc=(%.1f,%.1f) gapV=%.1f gapF=%.1f gapD=%.1f valid=%d %s score=%.2f",
                  (q->orient == 0) ? (dir > 0 ? "RIGHT" : "LEFT") : (dir > 0 ? "UP" : "DOWN"),
                  LAY[L], lx, ly, gv, gf, gd, valid, why[0] ? why : "-", score);
            if (valid && score > best) { best = score; bd = dir; bl = L; *olx = lx; *oly = ly; *ok = 1; }
        }
    }
    if (*ok)
        logf_("[pos] CHOSEN dir=%s layer=%.0f loc=(%.1f,%.1f) score=%.2f",
              (q->orient == 0) ? (bd > 0 ? "RIGHT" : "LEFT") : (bd > 0 ? "UP" : "DOWN"), LAY[bl], *olx, *oly, best);
    else
        logf_("[pos] *** NO valid candidate ***");
}

/* 改造C：界线读回验证 */
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

/* 直建（与 convdim 同款已验证结构；无 tol/olddim） */
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
        }
    }
    ProArrayFree((ProArray *)&arr);
    ProArrayFree((ProArray *)&sen);
}

/* —— 63.6：意图 = 零件左右端"最长的合法竖边"（每候选打印合法性判定，含辅助构造线的排除） —— */
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
        q.orient = 1; q.dir1 = +1; q.dir2 = 0; q.ownvi = ownvi;   /* 界线锚点在零件顶缘，只允许上方 */
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
        ab_auto_create((ProDrawing)drw, s1, s2, 0, PRO_DIM_ORNT_HORIZ, lx, ly, fv->v, "636");
        ab_verify_dim(drw, 63.6, ownvi);
        ab_dim_add(q.gx0 - 8.0, ly - 7.0, q.gx1 + 8.0, ly + 7.0, "dim636");
        ab_dim_add(pxL - 3.0, pyT - 6.0, pxR + 3.0, ly + 8.0, "dim636W");
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
    }
}

/* —— 41.1：意图 = bbox 上下极值的合法 ±y 平面（面附着点实测约在 56.85 列，预检按保守取右缘） —— */
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
        ay = (FP[i].ny > 0.0) ? off : -off;   /* 该面所在 y 值 */
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
        ab_auto_create((ProDrawing)drw, s1, s2, 0, PRO_DIM_ORNT_VERT, lx, ly, fv->v, "411");
        ab_verify_dim(drw, 41.1, ownvi);
        ab_dim_add(lx - 7.0, q.gy0 - 8.0, lx + 7.0, q.gy1 + 8.0, "dim411");
        ProSelectionFree(&s1);
        ProSelectionFree(&s2);
    }
}

/* —— 转换类尺寸（23/9.65/23.5/2/0.25）：几何判定在 convdim 候选清单（含 [legal] 行）；
      位置由搜索给出后调用 convdim_run（转换/隐藏逻辑不变） —— */
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
    convdim_run(part, drw, value, 1, lx, ly, vkind, route);
    ab_verify_dim(drw, value, ownvi);
    {
        ab_rect_t r;

        if (orient == 0) { r.x0 = lx - 7.0; r.x1 = lx + 7.0; r.y0 = q.gy0 - 8.0; r.y1 = q.gy1 + 8.0; }
        else             { r.x0 = q.gx0 - 8.0; r.x1 = q.gx1 + 8.0; r.y0 = ly - 7.0; r.y1 = ly + 7.0; }
        ab_dim_add(r.x0, r.y0, r.x1, r.y1, name);
    }
}

static void autobuild_run(ProMdl part, ProMdl drw, const wchar_t *mode)
{
    int do_all = 1, do636 = 1, do411 = 1;
    int fvi = -1, svi = -1, tvi = -1, i;

    if (mode[0] == L'6') { do_all = 0; do411 = 0; }
    else if (mode[0] == L'4') { do_all = 0; do636 = 0; }

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
    ab_furn_add(555.0, 76.0, 996.0, 208.0, "titleblock");
    ab_furn_add(492.0, 227.0, 698.0, 299.0, "technote");
    logf_("[auto] furniture: titleblock x[555..996] y[76..208]; technote x[492..698] y[227..299]; sheet margin 20px");
    if (do_all) {
        ab_dim_conv(part, drw, 23.0, 0, 0, 1, +1, 0, 30.0, fvi, "d23");
        if (do636) ab_dim_636(part, drw, fvi);
        ab_dim_conv(part, drw, 9.65, 0, 0, 0, +1, 0, 12.0, fvi, "d965");
        ab_dim_conv(part, drw, 23.5, 0, 0, 0, +1, 0, 28.0, fvi, "d235");
        if (do411) ab_dim_411(part, drw, fvi);
        ab_dim_conv(part, drw, 2.0, 0, 6, 1, -1, 0, 34.0, fvi, "d2");
        if (tvi >= 0) ab_dim_conv(part, drw, 0.25, 2, 5, 0, -1, +1, 6.0, tvi, "d025");
    } else if (!do636) {
        ab_dim_411(part, drw, fvi);
    } else {
        ab_dim_636(part, drw, fvi);
    }
    logf_("[auto] AUTOBUILD done");
}

/* ===== 轮次46：符号库标记（yat/jizhun）—— 只读定义读取 + 变量文本 + 试验实例 ===== */
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

/* ===== 轮次46：SYMPROBE —— 符号库定位探针（检索矩阵 + 调色板回退 + 变量文本 + 试验实例） ===== */
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

/* ===== 轮次46：SYMMARK|x|y|value|dimv —— 放置符号库标记（yat/jizhun v1；变量 A = 显示编号） ===== */
static void symmark_run(ProMdl drw, double x, double y, const wchar_t *value, double dimv)
{
    ProDtlsymdef def;
    ProError     er;

    (void)dimv;
    memset(&def, 0, sizeof(def));
    er = ProDrawingDtlsymdefRetrieve(drw, (wchar_t *)L"yat", (wchar_t *)L"jizhun", 1, PRO_B_TRUE, &def);
    logf_("[sym] retrieve yat/jizhun v1 -> %d (%s)", (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR) return;
    {
        ProDtlsyminstdata id = NULL;

        if (ProDtlsyminstdataAlloc(drw, &id) != PRO_TK_NO_ERROR || !id) { logf_("[sym] data alloc failed"); return; }
        ProDtlsyminstdataDefSet(id, &def);
        if (value && value[0]) {
            ProDtlvartext vt = NULL;

            if (ProDtlvartextAlloc((wchar_t *)L"A", (wchar_t *)value, &vt) == PRO_TK_NO_ERROR && vt) {
                ProError ea = ProDtlsyminstdataVartextAdd(id, vt);

                logf_("[sym] vartextAdd A='%ls' -> %d (%s)", value, (int)ea, errname(ea));
                /* 不 Free：Add 之后由实例持有（实测 Free 后再 Create 异常退出） */
            } else {
                logf_("[sym] vartext alloc failed");
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
            if (att) ProDtlattachFree(&att);
        }
        /* 不 ProDtlsyminstdataFree(id)：创建后实例持有数据（与 D2 工作流一致，避免悬空） */
    }
}

/* ===== 轮次46：SYMDBG|x|y —— 符号可见性诊断（定义信息/可见性标志/Draw/Regen 后查看） ===== */
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

/* ===== 轮次38：AGEO|<值> —— 只读打印该值尺寸的箭头头位置 + 界线端点（模型坐标） ===== */
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

/* 诊断：列出图纸里的注释数量 + 每个注释的显示状态和首行文本（排查“标了但看不见”）。
   diag_notes_of 可对任意图纸句柄调用；DIAG|<图名> 用它做活体对照。 */
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
    ProMdl drw = NULL;
    ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
    logf_("[diag] retrieve -> %d (%s)", (int)er, errname(er));
    if (er != PRO_TK_NO_ERROR || !drw) return;
    diag_notes_of(drw, "diag");
}

/* 坐标探针：放一条单行注释 "MARKk (x,y)"，用于反推图纸注释的坐标约定 */
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

/* 把文件内容以 Base64 输出到 stderr（明文通道：授权进程内读密文文件 -> 文本流出） */
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

/* 参数表导出到日志（明文通道：导出 CSV 后进程内读回打印）——PARAMS 命令用 */
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

/* 公差在线实验：给指定值的模型尺寸设公差并回读（P0c：排查"显示=设定值2倍"） */
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

/* 关键尺寸文本位置 dump（排版收敛用） */
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

/* 公差标签诊断/摘除（P0c 深挖：模型尺寸可能挂在 ISO/DIN 通用公差表上，
   regen 会把表值重新写回 → 图面始终显示 ±0.2/±0.1。摘除标签后显式值应生效。） */
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
   （0.0327 / 麦拉片 / 绝缘PC / C021006-016-00 / INSULATION_PP_2P_2），不是参数引用；
   快速出图模板给的是占位符字面文本 "&CNAME" 等 —— 所以唯一正确修法=直接改注释文本。 ---- */
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
                if (lc == 1) {   /* 目标占位符都是单行注释 */
                    ProDtlnotetext *txts = NULL;
                    if (ProDtlnotelineTextsCollect(lines[0], &txts) == PRO_TK_NO_ERROR && txts) {
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
                                   文本也有效；StringSet 对 &MASS[.4] 这类参数型文本实测无效） */
                                {
                                    ProDtlnotetext  tnew = NULL;
                                    ProDtlnotetext *arr = NULL;
                                    ProDtlnotetextAlloc(&tnew);
                                    ProDtlnotetextStringSet(tnew, nt);
                                    ProArrayAlloc(1, sizeof(ProDtlnotetext), 1, (ProArray *)&arr);
                                    arr[0] = tnew;
                                    es = ProDtlnotelineTextsSet(lines[0], arr);
                                    logf_("[editnote] note[%d] '%ls'->'%ls' textsSet=%d (%s)", i, from, to, (int)es, errname(es));
                                }
                                /* 提交（收集到的 notedata 是工作副本） */
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
                }
                ProArrayFree((ProArray *)&lines);
            }
        }
    }
    ProArrayFree((ProArray *)&notes);
    return hits;
}

static void get_part_string_param(ProMdl part, const wchar_t *pname, wchar_t *out, int outsz)
{
    ProModelitem owner;
    ProParameter p;
    ProParamvalue pv;

    out[0] = 0;
    if (ProMdlToModelitem(part, &owner) != PRO_TK_NO_ERROR) return;
    if (ProParameterInit(&owner, (wchar_t *)pname, &p) != PRO_TK_NO_ERROR) return;
    memset(&pv, 0, sizeof(pv));
    /* 管线写入用的是 ProParameterWithUnitsCreate —— 读取必须用带单位接口，
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
        /* NB109 密度 8.2e3 kg/m^3（材料库 金属\NB109.mtl）；模型单位 mm/kg，
           API 直接乘体积(mm^3)，故密度换算成 kg/mm^3 = 8.2e-6 */
        m = mp.mass;
        logf_("[filltb] mass(WithDensity 8.2e-6) = %.6g vol=%.3f", m, mp.volume);
    } else {
        logf_("[filltb] mass: both APIs failed");
    }
    return m;
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
    if (drwnum[0]) edit_note_text_match(drw, L"&DRWNUM", drwnum);   /* 两处：名称栏 + 左上倒置栏 */
    if (mass_s[0]) edit_note_text_match(drw, L"&MASS[.4]", mass_s);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

/* 公差重显实验：设公差 -> 擦除该尺寸 -> 重新显示（测试图面公差文本是否跟随） */
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
    for (id = 100; id <= 4000; id++) {   /* id>=100 跳过基准面 */
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
        ProAnnotationPlane plane;      /* 值结构体（同 ProDtlnote） */
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
        loc[0] = (bb[0][0] + bb[1][0]) / 2.0;    /* 初始位置：零件上方（模型坐标） */
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

/* 总长 63.6（轮次11 阶梯A）：模型侧参考尺寸（PRO_REF_DIMENSION，绝不建驱动尺寸）。
   A1 平面：ActiveGet 优先 → FromView(FRONT) → FlatToScreen；
   A2 附件/朝向：ProArrayAlloc 真 ProArray；
   A3 附件：两条竖直长边（内侧一对 = x≈2.05 / 65.65，实测扫描）；
   A4 location：两端点中点上方 +17（落在注释平面上，不用 (0,0,0)）。
   A 失败 → 阶梯 B：图纸侧边对（add_total_refdim，ref_dim=PRO_B_FALSE，loc=(385.4,597.2)）。 */
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
        /* ★ 轮次13 强制检查：ProCurvedata 是联合体，非直线按 .line 读 = 垃圾
           （上一版 z=+70.66 越界疑似此因）。只有 PRO_ENT_LINE 才可作候选。 */
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
        int          li = uc / 2 - 1, ri = uc / 2;   /* 最内一对（实测 = 2.05/65.65） */
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
        /* A4'（轮次12，单点改动）：loc = 两端边【曲线数据中点】的中点，
           全程用 ProGeomitemdataGet（模型空间，axis_dtl_cb 已验证）；
           绝不用 ProEdgeTessellationGet —— 其坐标不在模型空间（上一版 loc 越界即此因）。 */
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

/* ============ 终版：主动尺寸摆放 place_dims ============
   规则（轮次 8 规范）：
   1) 收集所有已显示关键尺寸的文本位置 P
   2) 归属视图 = 中心离 P 最近的视图；侧 = P 相对视图矩形在哪个方向（上/下/左/右）
   3) 同 (视图,侧) 一组内按值升序分层（小在内、大在外），层距 17 单位（7mm）
   4) 水平尺寸改 y 保 x；垂直尺寸改 x 保 y
   5) ★ 之后绝不再 regen（regen 会把文本拉回） */
typedef struct {
    ProDimension *dim;          /* 仅收集趟记录（不跨回调使用） */
    double v, tx, ty;           /* 值 / 当前文本位置 */
    double nx, ny;              /* 目标位置 */
    int    vidx, side, layer;   /* side: 0=上 1=下 2=左 3=右 */
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

    /* ① 必须真正显示在图纸上（返回 NOT_DISPLAYED 的直接跳过；之前的 bug：
       收集到了未显示的 0.25 实例并对它做 Move → 崩） */
    {
        ProView dv = NULL;
        int     found = -1, q;

        if (ProDrawingDimensionViewGet(c->drw, dim, &dv) != PRO_TK_NO_ERROR || !dv)
            return PRO_TK_NO_ERROR;
        for (q = 0; q < c->vc; q++) {
            if (c->views[q] == dv) { found = q; break; }
        }
        if (found < 0) return PRO_TK_NO_ERROR;   /* 视图不在表内：不用 */
        if (ProDimensionValueGet(dim, &v) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
        if (!value_in_keep_list(v)) return PRO_TK_NO_ERROR;
        {
            int r;
            for (r = 0; r < c->n; r++) {
                double d = c->items[r].v - v;
                if (d < 0) d = -d;
                if (d < 0.02) return PRO_TK_NO_ERROR;   /* 同值去重 */
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

/* 第二趟：在 visit 回调内用“新鲜”的尺寸指针执行移动（与命令通道 MOVE| 同款模式，
   已验证不会崩；跨回调保存的 ProDimension* 会失效，是上一版只摆 1 根的根因）。 */
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

    /* 与收集趟相同的“已显示”过滤 */
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
    /* —— 第一趟：收集 + 算归属/侧/层/目标（不改动任何东西） —— */
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
    /* —— 第二趟：统一移动（回调内用新鲜指针） —— */
    mv.drw = (ProDrawing)drw;
    mv.c = &c;
    ProSolidDimensionVisit(c.solid, PRO_B_FALSE, place_move_cb, NULL, &mv);
    logf_("[place] done");
}

/* P1：总长参考尺寸。枚举竖直长边（span>20、x 恒定）→ 按 x 排序 →
   从最内侧一对开始试建（视图只显示零件本体；外侧的料带边不在视图内会 BAD_INPUTS）。 */
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
        /* ★ 轮次13 强制检查：只有 PRO_ENT_LINE 才可作候选（联合体误读防护） */
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

        /* 找主视图（选择器需要带视图上下文：ProSelectionViewSet） */
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
            /* 文档要求 ProArray 形式的附件/朝向数组 */
            ProArrayAlloc(1, sizeof(ProDimAttachment), 1, (ProArray *)&att_arr);
            ProArrayAlloc(1, sizeof(ProDimSense), 1, (ProArray *)&sense_arr);
            att_arr[0][0] = s1;
            att_arr[0][1] = s2;
            memset(&sense_arr[0], 0, sizeof(ProDimSense));
            sense_arr[0].type = PRO_DIM_SNS_TYP_NONE;
            sense_arr[0].orient_hint = PRO_DIM_ORNT_HORIZ;
            loc[0] = loc_x; loc[1] = loc_y; loc[2] = 0.0;
            memset(&nd, 0, sizeof(nd));
            /* 文档：ref_dim=PRO_B_FALSE 创建 PRO_REF_DIMENSION（参考尺寸），TRUE 是普通尺寸 */
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

/* P1 v3：总长参考尺寸（"面版"）。扫 X 法向平面 → 从最外一对开始试建参考尺寸
   （ref_dim=PRO_B_FALSE → PRO_REF_DIMENSION 参考尺寸，不驱动模型）。 */
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

/* &MASS[.4] 是"参数型"文本，StringSet/Modify 均无效（实测）；改为删除旧注记、原挂接位置重建纯文本 */
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

/* P3：中心线。把零件坐标系的轴在各视图里“显示”出来（图纸里轴=点画线中心线） */
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

/* P3 安全版：轴 -> geomitem -> 3D选择(带视图) -> ProDrawingAxisToDtlaxis 生成图纸轴(点画线)。
   （旧实现把 ProAxis 强转 ProAnnotation 调 Show，触发致命错误 + 管线自重启，已废弃。）
   定位：视图轮廓(outline) 与 模型包围盒(bbox) 做线性映射（实测 transform API 与 outline
   不同系，映射更可靠）：sx = out0x + (mx-bbox0x)*(out1x-out0x)/(bbox1x-bbox0x)。 */
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
            /* 轴=直线：取两端中点（union 直读 .line；type 值仅记录） */
            loc[0] = (cd->line.end1[0] + cd->line.end2[0]) / 2.0;
            loc[1] = (cd->line.end1[1] + cd->line.end2[1]) / 2.0;
            loc[2] = (cd->line.end1[2] + cd->line.end2[2]) / 2.0;
            logf_("[axisdtl] axis type=%d mid(model)=(%.1f,%.1f,%.1f)", cd->line.type, loc[0], loc[1], loc[2]);
            ProGeomitemdataFree(&gd);
        } else {
            logf_("[axisdtl] no curve data; loc=(0,0,0)");
        }
        /* 模型坐标 -> 视图轮廓坐标（线性映射；outline 与尺寸 loc 同系） */
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

/* ---- 会话内补建基准轴（不存盘）----
   模型里凸点没有轴 → 扫圆柱面 → 建“穿过圆柱面”的基准轴（PRO_DTMAXIS_CONSTR_TYPE_THRU）。
   轴随后由 PRO_AXIS 显示机制画成中心线。仅内存生效，模型文件不被改动。 */
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

/* ---- P3：给 6 个凸点画点画线十字中心线（图纸级 draft entity，screen 坐标）
   凸点圆心（模型坐标，来自 CYLSCAN 实测）：x∈{10.85,33.85,56.85}，y∈{156.06,179.56}，
   节距 23.0×23.5 与图上尺寸吻合。屏幕坐标 = 主视图轮廓 + 模型包围盒线性映射。 ---- */
static int mk_dtl_line(ProMdl drw, ProView view, double x1, double y1, double x2, double y2)
{
    ProDtlentitydata data = NULL;
    ProDtlentity     ent;
    ProCurvedata     cd;
    ProName          fnt;
    ProError         e;
    int              ec;

    memset(&cd, 0, sizeof(cd));
    cd.line.type = PRO_ENT_LINE;   /* 实测：轴曲线的 line.type=2 = PRO_ENT_LINE */
    cd.line.end1[0] = x1; cd.line.end1[1] = y1; cd.line.end1[2] = 0.0;
    cd.line.end2[0] = x2; cd.line.end2[1] = y2; cd.line.end2[2] = 0.0;
    if (ProDtlentitydataAlloc(drw, &data) != PRO_TK_NO_ERROR) return 0;
    ProDtlentitydataCurveSet(data, &cd);
    ProDtlentitydataViewSet(data, view);
    wcsncpy_s(fnt, PRO_NAME_SIZE, L"centerline", _TRUNCATE);
    e = ProDtlentitydataFontSet(data, fnt);
    ProDtlentitydataWidthSet(data, 0.15);
    /* 注意：不要 ProDtlentitydataConstructionSet(TRUE) —— 实测它把线段变成
       无限长构造线（满图纸的长线）。普通线段+centerline 字体即可（Create 自带显示，
       不必 Draw；r10 已验证可用版本）。 */
    ec = ProDtlentityCreate(drw, NULL, data, &ent);
    if (ec != PRO_TK_NO_ERROR) {
        /* 字体名可能不被接受：清空字体（默认）再试 */
        ProName f2;
        f2[0] = 0;
        ProDtlentitydataFontSet(data, f2);
        ec = ProDtlentityCreate(drw, NULL, data, &ent);
    }
    logf_("[cross] line (%.1f,%.1f)-(%.1f,%.1f) font->%d create->%d (%s)",
          x1, y1, x2, y2, (int)e, (int)ec, errname((ProError)ec));
    ProDtlentitydataFree(data);
    return (ec == PRO_TK_NO_ERROR) ? 1 : 0;
}

static void boss_centerlines(ProMdl drw, ProMdl part)
{
    static const double BX[3] = { 10.85, 33.85, 56.85 };
    static const double BY[2] = { 156.06, 179.56 };
    ProView    *views = NULL;
    ProView     front = NULL;
    ProPoint3d  out[2];   /* 必须是真数组：ProPoint3d 是 double[3]，局部变量不保证相邻 */
    ProPoint3d  box[2];
    int         ix, iy, made = 0;

    if (ProDrawingViewsCollect((ProDrawing)drw, &views) != PRO_TK_NO_ERROR || !views) return;
    {
        int vc = 0, i;
        ProArraySizeGet((ProArray)views, &vc);
        for (i = 0; i < vc; i++) {
            ProName nm;
            nm[0] = 0;
            ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
            if (wcsstr(nm, L"front") || wcsstr(nm, L"FRONT")) { front = views[i]; break; }
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
    {
        double kx = (out[1][0] - out[0][0]) / (box[1][0] - box[0][0]);
        double ky = (out[1][1] - out[0][1]) / (box[1][1] - box[0][1]);
        double half = 17.0 * (kx / 2.3805);   /* 半长自适应：1:1 时 =17；放大后按视图比例同步（轮次18） */
        logf_("[cross] map kx=%.3f ky=%.3f outline x[%.1f..%.1f] y[%.1f..%.1f] bbox x[%.2f..%.2f] y[%.2f..%.2f]",
              kx, ky, out[0][0], out[1][0], out[0][1], out[1][1],
              box[0][0], box[1][0], box[0][1], box[1][1]);
        for (ix = 0; ix < 3; ix++) {
            for (iy = 0; iy < 2; iy++) {
                double sx = out[0][0] + (BX[ix] - box[0][0]) * kx;
                double sy = out[0][1] + (BY[iy] - box[0][1]) * ky;
                made += mk_dtl_line(drw, front, sx, sy - half, sx, sy + half);   /* 竖 */
                made += mk_dtl_line(drw, front, sx - half, sy, sx + half, sy);   /* 横 */
            }
        }
    }
    logf_("[cross] done, lines created=%d", made);
    ProDwgSheetRegenerate((ProDrawing)drw, 1);
}

static void axis_lines_to_views(ProMdl drw, ProMdl part)
{
    /* 中心线 = 显示模型的轴（PRO_AXIS 注解）。零坐标运算：投影由 Creo 自己做。
       （AxisToDtlaxis 路线 4 轮 ok=0，已放弃；保留上面的 axis_dtl_cb 以备后用。） */
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

/* P3：右上角 1:2 轴测图（先给零件存 '3d' 视图再用 ProDrawingGeneralviewCreate） */
static void add_iso_view(ProMdl drw, ProMdl part, double px, double py, double scale)
{
    static const double iso_m[4][4] = {
        { -0.7071068,  0.7071068, 0.0,       0.0 },
        { -0.4082483, -0.4082483, 0.8164966, 0.0 },
        {  0.5773503,  0.5773503, 0.5773503, 0.0 },
        {  0.0,        0.0,       0.0,       1.0 }
    };
    ProMatrix iso;
    ProView   v = NULL;
    ProError  e;

    memcpy(iso, iso_m, sizeof(iso));
    v = NULL;
    e = ProMdlDisplay(drw);
    logf_("[iso] A display -> %d (%s)", (int)e, errname(e));
    /* 注意：不要调 ProViewMatrixSet/ProViewStore —— p_view=NULL 表示“当前活动图纸视图”，
       实测会把 顶部_1 的朝向污染成轴测方向（变成大斜视图）。手推矩阵直接传给
       GeneralviewCreate 即可（create->0 已实测）。 */
    /* 先清掉旧的轴测图（识别规则：视图比例 <0.9；我们的轴测图=0.5，投影视图=1.0。
       注意：主视图/侧视图/俯视图也是 general 类型，绝不能按类型清 —— 按比例清。 */
    {
        ProView *views = NULL;
        if (ProDrawingViewsCollect((ProDrawing)drw, &views) == PRO_TK_NO_ERROR && views) {
            int vc = 0, i;
            ProArraySizeGet((ProArray)views, &vc);
            for (i = 0; i < vc; i++) {
                double vscale = -1.0;
                if (ProDrawingViewScaleGet((ProDrawing)drw, views[i], &vscale) == PRO_TK_NO_ERROR &&
                    vscale > 0.0 && vscale < 0.9) {
                    ProName nm;
                    ProError ee;
                    nm[0] = 0;
                    ProDrawingViewNameGet((ProDrawing)drw, views[i], nm);
                    ee = ProDrawingViewErase((ProDrawing)drw, views[i]);
                    logf_("[iso] erase old iso view '%ls' (scale=%.2f) -> %d (%s)", nm, vscale, (int)ee, errname(ee));
                }
            }
            ProArrayFree((ProArray *)&views);
        }
    }
    {
        /* 若模型已有 '3d' 视图，优先采用其矩阵；否则用手推标准等轴测矩阵 */
        ProView v3 = NULL;
        if (ProViewNameToView((ProSolid)part, L"3d", &v3) == PRO_TK_NO_ERROR && v3) {
            ProError eg = ProViewMatrixGet((ProMdl)part, v3, iso);
            logf_("[iso] B matrixGet from '3d' view -> %d (%s)", (int)eg, errname(eg));
        } else {
            logf_("[iso] B no '3d' view; use built-in iso matrix");
        }
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

/* P2：三视图紧凑布局。report=只打印；do_move=按包围盒计算目标并移动（ProDrawingViewOriginSet） */
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
        /* 目标：其余视图与主视图间距 ~75 单位（≈30mm），右视图水平对齐、俯视图垂直对齐。
           注意两套空间：outline（logical screen）与 OriginSet 位置差 2.381 倍（实测标定；
           见 评审记录 坐标系对照表）。轴测图（scale<0.9）一律不参与。 */
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
                    dx = (f_out1[0] + gap) - out[0][0];              /* 左缘对齐主视图右侧+间隙 */
                    dy = (f_out0[1] + f_out1[1]) / 2.0 - (out[0][1] + out[1][1]) / 2.0;  /* 垂直居中 */
                } else {
                    dy = (f_out0[1] - gap) - out[1][1];              /* 顶缘对齐主视图下方-间隙 */
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

/* 运行时命令通道：每 tick 检查 work\cmd_now.txt（首行=一条命令），结果打到 stderr。
   MACRO|<宏文本> | STAT | SAVE | MKFRONT|<轴0-2>|<flip 0/1> | EXIT */
#define CMD_FILE L"D:\\creo-agent\\sync-toolkit\\work\\cmd_now.txt"
static wchar_t g_cmd_last[2048] = L"";

static void run_cmd_channel(void)
{
    FILE  *f;
    char   buf[4096];
    size_t n;
    wchar_t w[2048];
    wchar_t *nl;

    f = _wfopen(CMD_FILE, L"rb");
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n == 0) return;
    if (!u8_to_wide(buf, w, 2048)) return;
    nl = wcschr(w, L'\n');
    if (nl) *nl = 0;
    if (w[0] == 0 || !wcscmp(w, g_cmd_last)) return;   /* 同一命令不重复执行 */
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
                    /* 不调用 ProWstringArrayFree（会触发 fatal error，见 ensure_front_view 注释） */
                }
                if (ProSolidOutlineGet((ProSolid)cur, box) == PRO_TK_NO_ERROR)
                    logf_("[cmd] STAT bbox %.2f x %.2f x %.2f",
                          box[1][0]-box[0][0], box[1][1]-box[0][1], box[1][2]-box[0][2]);
            }
        }
    } else if (!wcscmp(w, L"SAVE")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] SAVE retrieve('%ls') -> %d (%s)", g_def_model, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            ProError es = ProMdlSave(drw);
            logf_("[cmd] SAVE ProMdlSave -> %d (%s)", (int)es, errname(es));
            export_pdf_model(drw, g_def_model);
        }
    } else if (!wcscmp(w, L"PDFDUMP")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] PDFDUMP retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
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
    } else if (!wcscmp(w, L"DIAG")) {
        diag_notes();
    } else if (!wcscmp(w, L"DIMS")) {
        dims_report();
    } else if (!wcscmp(w, L"DDIMS")) {
        /* DDIMS：只读转储"存在图纸里的尺寸"（用户手标 + 图纸侧创建） */
        ProMdl drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            ddims_run(drw);
        else
            logf_("[ddims] drawing retrieve failed");
    } else if (!wcscmp(w, L"AFIND")) {
        /* AFIND：只读转储模型注解元素（3D 注释，含可能的尺寸注释） */
        ProMdl part = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
            afind_run(part);
        else
            logf_("[afind] part retrieve failed");
    } else if (!wcsncmp(w, L"ARROWS|", 7)) {
        /* ARROWS|<长>|<宽>：只在【本张图纸】上设置箭头尺寸（ProDrawingSetupOption*）——
           ⛔ 严禁动全局 metric.dtl。ProLine 是宽字符串（wchar_t[81]）。读回留证。 */
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
        /* DTLSET|<dtl选项>|<值>：单图纸设置 + 前后读回（⛔ 严禁改全局 metric.dtl）。
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
    } else if (!wcsncmp(w, L"FDIM|", 5)) {
        /* FDIM|idA|idB|x|y|O|V ：直选两个面建尺寸（R3）。O: H/V；V: F=主视图 / S=侧视图 */
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
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                    ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                    fdim_run(part, drw, fida, fidb, fx, fy, oc, vc);
                else
                    logf_("[fdim] retrieve failed");
            }
        } else {
            logf_("[fdim] bad args");
        }
    } else if (!wcsncmp(w, L"EDIM|", 5)) {
        /* EDIM|idA|idB|x|y|O|V ：直选两条边建尺寸（参数样式同 FDIM）。O: H/V；V: F=主视图 / S=侧视图 */
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
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                    ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                    edim_run(part, drw, eida, eidb, ex, ey, eoc, evc);
                else
                    logf_("[edim] retrieve failed");
            }
        } else {
            logf_("[edim] bad args");
        }
    } else if (!wcsncmp(w, L"AUTOBUILD", 9)) {
        /* AUTOBUILD[|ALL|636|411]：改造A/B/C 全流程（几何合法性→位置搜索→创建→界线验证）。
           无参数或 |ALL = 全部 7 根；|636 / |411 = 单根端到端验证。 */
        const wchar_t *m = w + 9;
        ProMdl part = NULL, drw = NULL;

        if (m[0] == L'|') m++;
        else m = L"ALL";
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            autobuild_run(part, drw, m);
        else
            logf_("[auto] retrieve failed");
    } else if (!wcsncmp(w, L"SYMINFO", 7)) {
        /* SYMINFO[|subdir|name]：读符号库定义 + 变量文本（默认 yat|jizhun） */
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
        /* SYMPROBE：符号库定位探针（检索矩阵 + 调色板回退 + 变量文本 + 试验实例） */
        ProMdl drw = NULL;

        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            symprobe_run(drw);
        else
            logf_("[sym] drw retrieve failed");
    } else if (!wcsncmp(w, L"SYMMARK|", 8)) {
        /* SYMMARK|x|y|value[|dimv]：放置符号库标记（yat/jizhun v1；value=显示编号） */
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
    } else if (!wcsncmp(w, L"SYMDBG|", 7)) {
        /* SYMDBG|x|y：符号可见性诊断（定义信息/可见性标志/Draw/Regen） */
        double sx = 0.0, sy = 0.0;
        ProMdl drw = NULL;

        if (swscanf_s(w + 7, L"%lf|%lf", &sx, &sy) == 2 &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            symdbg_run(drw, sx, sy);
        else
            logf_("[sym] dbg bad args/retrieve");
    } else if (!wcsncmp(w, L"DARROW|", 7)) {
        /* DARROW|<值>|<类型>：逐箭头设 ProLeaderType（实验 10=DOUBLEARROW / 1=ARROWHEAD） */
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
        /* AGEO|<值>：只读——箭头头位置 + 界线端点（模型坐标）+ 关系量 */
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
        /* MOVE|<值>|<x>|<y>|<z>：把指定值的尺寸移到 3D 点（布局精摆/实验用） */
        double dv = 0, x = 0, y = 0, z = 0;
        if (swscanf_s(w + 5, L"%lf|%lf|%lf|%lf", &dv, &x, &y, &z) == 4) {
            ProMdl part = NULL, drw = NULL;
            logf_("[move] target=%.4f -> (%.2f, %.2f, %.2f)", dv, x, y, z);
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                move_ctx_t mc;
                mc.target = dv;
                mc.pt[0] = x; mc.pt[1] = y; mc.pt[2] = z;
                mc.drw = (ProDrawing)drw;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_move_cb, NULL, &mc);
                /* 注意：此处不要 ProDwgSheetRegenerate —— 实测它会把尺寸文本“拉回”，
                   使 ProDrawingDimensionMove 设置的位置失效。 */
            }
        } else {
            logf_("[move] bad args");
        }
    } else if (!wcsncmp(w, L"LEADER|", 7)) {
        /* LEADER|<值>[|<mode>]：只读 config（无 mode）或设置 config（1=LINEAR 2=CENTER_LEADER） */
        double dv = 0.0;
        int mode = -1;
        if (swscanf_s(w + 7, L"%lf|%d", &dv, &mode) >= 1 && (mode == -1 || (mode >= 0 && mode <= 3))) {
            ProMdl part = NULL, drw = NULL;
            logf_("[leader] target=%.4f mode=%d", dv, mode);
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
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
        /* SELINJ|<mode>：轮次22 选择注入实验（先用 MACRO 启动 hd） */
        int mode = 0;
        if (swscanf_s(w + 7, L"%d", &mode) == 1) {
            ProMdl part = NULL, drw = NULL;
            logf_("[selinj] >> mode=%d", mode);
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                selinj(part, drw, mode);
            else
                logf_("[selinj] retrieve failed");
        } else {
            logf_("[selinj] bad args");
        }
    } else if (!wcsncmp(w, L"SECDIM", 6)) {
        /* SECDIM[|1|2]：轮次23/24 —— 给基础草绘加总长尺寸
           （1 = 创建+Strengthen；2 = 副本提交计数定案测试） */
        int strg = 0;
        if (w[6] == L'|') {
            if (w[7] == L'1') strg = 1;
            else if (w[7] == L'2') strg = 2;
        }
        {
            ProMdl part = NULL;
            logf_("[secdim] >> mode=%d", strg);
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
                if (strg == 2)
                    secdim_count_test(part);
                else
                    secdim_try(part, strg);
            } else {
                logf_("[secdim] part retrieve failed");
            }
        }
    } else if (!wcscmp(w, L"SHOWK")) {
        /* SHOWK：显示 KEEP 关键尺寸（含新增的总长 63.60；复跑 phase2 的显示步骤） */
        ProMdl part = NULL, drw = NULL;
        logf_("[showk] >> show_key_dims re-run");
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            show_key_dims(part, drw);
        else
            logf_("[showk] retrieve failed");
    } else if (!wcscmp(w, L"REDEF")) {
        /* REDEF：轮次24 路线A 阶段1 —— 元素树重定义提交总长（模型侧） */
        ProMdl part = NULL;
        logf_("[redef] >> start");
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
            redef_try(part);
        else
            logf_("[redef] part retrieve failed");
    } else if (!wcsncmp(w, L"GEOQ|", 5)) {
        /* GEOQ|P|<值> / GEOQ|L|<轴>|<值> / GEOQ|F|<轴>|<值>：只读几何转储（轮次30 调查用） */
        wchar_t mode = w[5];

        if (mode == L'P' || mode == L'p') {
            double val = 0.0;
            if (w[6] == L'|' && swscanf_s(w + 7, L"%lf", &val) == 1) {
                ProMdl part = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
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
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
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
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
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
        /* FACHK|<值>：只读面诊断（找间距≈值的平行面片对，不改任何东西） */
        double dv = 0.0;
        if (swscanf_s(w + 6, L"%lf", &dv) == 1) {
            ProMdl part = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
                facechk_run(part, dv);
            else
                logf_("[fachk] part retrieve failed");
        } else {
            logf_("[fachk] bad args");
        }
    } else if (!wcsncmp(w, L"CONVDIM|", 8)) {
        /* CONVDIM|<值>[|<x>|<y>[|<视图>|<路线>]]：把该值的模型尺寸转成图纸侧创建（任务2）；
           可带显式目标位置（不依赖模型文本位，因未显示的尺寸不会被 PLACEDIMS 归位）。
           视图：F=主视图（默认）/ S=左侧视图；路线：E=边对（默认）/ P=面附件 / C=边对+同平面过滤 */
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
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
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
        /* B64|<全路径>：把任意文件以 Base64 明文流出（进程内读密文，供外部解码） */
        dump_file_b64(w + 4);
    } else if (!wcsncmp(w, L"PDFDUMP|", 8)) {
        /* PDFDUMP|<图名>：导出指定图纸（标题栏活体对照实验用） */
        ProMdl  drw = NULL;
        ProError er = ProMdlnameRetrieve(w + 8, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] PDFDUMP(%ls) retrieve -> %d (%s)", w + 8, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            ProPDFOptions opts = NULL;
            ProPath       out;
            ProError      ep, ed;
            SYSTEMTIME    st2;
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
    } else if (!wcsncmp(w, L"PARAMS|", 7)) {
        /* PARAMS|<模型名>|<prt|drw|asm>：导出参数表到日志（明文 CSV 回执） */
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
        /* WPARAM|<模型名>|<prt|drw>|<参数名>|<值>：写字符串参数（标题栏实验用） */
        wchar_t nm[256], ty[16], pn[64], pv[512];
        ty[0] = 0; pv[0] = 0;
        if (swscanf_s(w + 7, L"%255[^|]|%15[^|]|%63[^|]|%511[^|]", nm, 256, ty, 16, pn, 64, pv, 512) == 4) {
            ProMdlfileType ft = (!wcscmp(ty, L"drw")) ? PRO_MDLFILE_DRAWING : PRO_MDLFILE_PART;
            ProMdl        mdl = NULL;
            ProError      er = ProMdlnameRetrieve(nm, ft, &mdl);
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
    } else if (!wcscmp(w, L"REGEN")) {
        ProMdl drw = NULL;
        ProError er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] REGEN retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw)
            logf_("[cmd] REGEN sheetRegen -> %d", (int)ProDwgSheetRegenerate((ProDrawing)drw, 1));
    } else if (!wcsncmp(w, L"TOLSET|", 7)) {
        /* TOLSET|<值>|<toltype 1..4>|<up>|<lo>：在线改公差（P0c 实验） */
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 7, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL;
            logf_("[tolset] target=%.4f type=%d up=%.4f lo=%.4f", dv, ty, up, lo);
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
                tolset_ctx_t tc;
                tc.target = dv; tc.ty = ty; tc.up = up; tc.lo = lo;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tol_cb, NULL, &tc);
            }
        } else {
            logf_("[tolset] bad args");
        }
    } else if (!wcsncmp(w, L"TOLFREE|", 8)) {
        /* TOLFREE|<值>|<ty>|<up>|<lo>：摘公差表标签 + 显式公差（P0c 实验） */
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 8, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
                tolf_ctx_t tc;
                tc.target = dv; tc.mode = 1; tc.ty = ty; tc.up = up; tc.lo = lo;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tolfree_cb, NULL, &tc);
            }
        } else {
            logf_("[tolfree] bad args");
        }
    } else if (!wcscmp(w, L"TOLLABEL")) {
        /* TOLLABEL：dump 关键尺寸的公差标签/类型/值（只读诊断） */
        ProMdl part = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
            tolf_ctx_t tc;
            tc.target = -1; tc.mode = 0; tc.ty = 0; tc.up = 0; tc.lo = 0;
            ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_tolfree_cb, NULL, &tc);
        }
    } else if (!wcsncmp(w, L"MAT|", 4)) {
        /* MAT|<材料名>：材料库读取变体实验 + 质量属性打印（P0 用） */
        wchar_t nm[256];
        if (swscanf_s(w + 4, L"%255s", nm, 256) == 1) {
            ProMdl part = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
                wchar_t v1[300], v2[300], v3[300];
                const wchar_t *vars[3];
                int vi, hit = -1;
                swprintf_s(v1, 300, L"%ls", nm);           /* 变体1：裸名（现状，预计 -4） */
                swprintf_s(v2, 300, L"金属\\%ls", nm);     /* 变体2：材料库子目录（金属\NB109） */
                swprintf_s(v3, 300, L"%ls.mtl", nm);       /* 变体3：带扩展名 */
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
        /* DIAG|<图名>：列出指定图纸的注记（标题栏格文本/参数引用判读用） */
        ProMdl  drw = NULL;
        ProError er = ProMdlnameRetrieve(w + 5, PRO_MDLFILE_DRAWING, &drw);
        logf_("[cmd] DIAG(%ls) retrieve -> %d (%s)", w + 5, (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) diag_notes_of(drw, "diag");
    } else if (!wcscmp(w, L"FILLTB")) {
        /* FILLTB：把标题栏 5 个占位符注释文本替换成实际值 */
        ProMdl part = NULL, drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            fill_title_block(drw, part);
        else
            logf_("[filltb] retrieve failed");
    } else if (!wcsncmp(w, L"EDITNOTE|", 9)) {
        /* EDITNOTE|<旧文本>|<新文本>：按精确文本替换注记 */
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
        /* TEXTPOS：打印关键尺寸的文本位置（排版收敛用） */
        ProMdl part = NULL, drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_textpos_cb, NULL, (ProAppData)drw);
        else
            logf_("[textpos] retrieve failed");
    } else if (!wcscmp(w, L"BBOX")) {
        ProMdl  part = NULL;
        Pro3dPnt box[2];
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
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
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
                attach_ctx_t ac;
                ac.target = dv;
                ProSolidDimensionVisit((ProSolid)part, PRO_B_FALSE, dim_attach_cb, NULL, &ac);
            }
        } else {
            logf_("[attach] bad args");
        }
    } else if (!wcsncmp(w, L"RESTOL|", 7)) {
        /* RESTOL|<值>|<ty>|<up>|<lo>：公差重显实验（erase + set + show） */
        double dv = 0, up = 0, lo = 0;
        int    ty = 4;
        if (swscanf_s(w + 7, L"%lf|%d|%lf|%lf", &dv, &ty, &up, &lo) == 4) {
            ProMdl part = NULL, drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
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
    } else if (!wcsncmp(w, L"ADDTOT", 6)) {
        /* ADDTOT[|x|y]：创建总长参考尺寸（默认位置看日志后可用 x,y 重试） */
        double lx = -1e9, ly = -1e9;
        wchar_t *rest = w + 6;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &lx, &ly);
        {
            ProMdl part = NULL, drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
                if (lx < -1e8) {
                    /* 默认位置：主视图包围盒上方 */
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
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            axis_lines_to_views(drw, part);
    } else if (!wcsncmp(w, L"ISO", 3)) {
        /* ISO[|x|y[|scale]]：1:2 轴测图（默认 (700,630) ×0.5） */
        double px = 700.0, py = 630.0, sc = 0.5;
        wchar_t *rest = w + 3;
        if (rest[0] == L'|') {
            int got = swscanf_s(rest + 1, L"%lf|%lf|%lf", &px, &py, &sc);
            (void)got;
        }
        {
            ProMdl part = NULL, drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
                ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
                add_iso_view(drw, part, px, py, sc);
        }
    } else if (!wcsncmp(w, L"TOTFACE", 7)) {
        /* TOTFACE[|x|y]：总长参考尺寸（面版，ref_dim=PRO_B_FALSE）
           注意：不能叫 ADDTOTF —— 命令链里 ADDTOT(6字符) 会先吃掉这个前缀。 */
        double lx = -1e9, ly = -1e9;
        wchar_t *rest = w + 7;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &lx, &ly);
        {
            ProMdl part = NULL, drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
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
                /* 轮次25：图纸侧创建（官方附件结构）；旧的 add_total_dim_model（模型侧+错误结构）停用 */
                tot_v2(drw, part, lx, ly);
            }
        }
    } else if (!wcscmp(w, L"PLACEDIMS")) {
        /* PLACEDIMS：命令通道里单独跑尺寸摆放（验证通过后才考虑接入管线） */
        ProMdl part = NULL, drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            place_dims(drw, part);
        else
            logf_("[place] retrieve failed");
    } else if (!wcscmp(w, L"CROSSES")) {
        /* CROSSES：给 6 个凸点画十字中心线（图纸实体） */
        ProMdl part = NULL, drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part &&
            ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            boss_centerlines(drw, part);
        else
            logf_("[cross] retrieve failed");
    } else if (!wcsncmp(w, L"CYLSCAN", 7)) {
        /* CYLSCAN[|rmin|rmax]：列举圆柱面；带范围时给范围内的圆柱面建基准轴 */
        double rmin = -1.0, rmax = -1.0;
        wchar_t *rest = w + 7;
        if (rest[0] == L'|') swscanf_s(rest + 1, L"%lf|%lf", &rmin, &rmax);
        {
            ProMdl part = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part)
                cylscan((ProSolid)part, rmin, rmax);
            else
                logf_("[cyl] part retrieve failed");
        }
    } else if (!wcsncmp(w, L"LAYOUT", 6)) {
        /* LAYOUT=报告；LAYOUT|go=按包围盒移动视图 */
        int go = (w[6] == L'|' && (w[7] == L'g' || w[7] == L'G'));
        ProMdl drw = NULL;
        if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw)
            layout_report(drw, go);
        else
            logf_("[layout] drw retrieve failed");
    } else if (!wcsncmp(w, L"VIEWPOS|", 8)) {
        /* VIEWPOS|<索引>|<目标outline中心x>|<目标outline中心y>：
           把视图 [索引] 平移到目标 outline 中心（屏幕单位）。
           换算：Δorigin = Δscreen / (2.3805 × scale)（scale 用视图自己的比例，轴测 0.5）。
           不 regen —— regen 会把尺寸文本拉回默认位；尺寸归位由随后一次 PLACEDIMS 完成。 */
        int vi = -1;
        double cx = 0.0, cy = 0.0;
        if (swscanf_s(w + 8, L"%d|%lf|%lf", &vi, &cx, &cy) == 3) {
            ProMdl drw = NULL;
            if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw) == PRO_TK_NO_ERROR && drw) {
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
                            /* ★ 实测（2026-10-12 轮次14）：Δoutline = Δorigin × 2.3805 对【所有】
                               视图成立，与视图自身 scale 无关（轴测图 scale=0.5 实测比值 2.3811，
                               曾按 k×0.5 处理 → 位移翻倍 412.1/173.1）。scale 只影响视图内部
                               模型→屏幕的映射，不影响定位换算。 */
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
                                /* 实测：OriginSet 后 outline 不会立即刷新，必须 regen 才提交
                                   （与 layout_report 结尾同款；regen 会把尺寸文本拉回默认位，
                                     尺寸归位随后由 PLACEDIMS 完成，那是已验证的顺序）。 */
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
        /* VSCALE|<索引>|<scale>：设置视图比例（轮次18：三投影视图 2.5、轴测 1.0），
           设完 regen 提交并回读复核。 */
        int vi = -1;
        double sc2 = 0.0;
        if (swscanf_s(w + 7, L"%d|%lf", &vi, &sc2) == 2 && sc2 > 0.0) {
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
        /* SHEETSCALE|<scale>：设置图纸比例（同步标题栏"比例"格；轮次18 DSH 批准） */
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

        /* 1) PDF 兜底：只承认 arm 之后新写/更新的 PDF（旧文件不算，否则会跳过标注） */
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

        /* 2) 快速出图的产物是图纸(.drw)：在会话里找同名图纸，找到即保存落盘 */
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

                /* 顺序：先保存无标注版（与 14:00 成功路径一致）→ 再标注 → 二次保存。
                   实测：SDAutoDimension 会让会话处于“命令未结束”状态，之后立刻保存会挂起。 */
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
        /* 第一段：让路 → 保存“无标注版”图纸 + 导出 PDF（保底交付）→ 再发自动标注 */
        ProMdl drw = NULL;
        ProError er, es;
        if (g_has_dir) clear_drw_targets(g_dir, g_def_model);
        else           clear_drw_targets(L".", g_def_model);
        er = ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_DRAWING, &drw);
        logf_("[defer] phase2 retrieve -> %d (%s)", (int)er, errname(er));
        if (er == PRO_TK_NO_ERROR && drw) {
            activate_drawing_window(drw);

            /* 图纸比例 1:1（轮次19：用户裁定改回 1:1。轮次18 曾短暂设为 2.5:1，
               已按用户要求回退；不要再擅自改比例。标题栏"比例"字段由图纸比例驱动。） */
            {
                ProError esc = ProDrawingScaleSet((ProDrawing)drw, NULL, 1, 1.0);
                logf_("[scale] ProDrawingScaleSet(1:1) -> %d (%s)", (int)esc, errname(esc));
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
            }

            /* 教程第 7 章：图名/材料参数（CNAME/MSTD）已初始化进模型（prt.5/6）。
               实测：在本环境“创建/修改参数后再保存”会触发 ProMdlSave 死等，
               故此处不再重设——快速出图时图框会直接从模型参数取值。 */
            /* 标题栏 4 格：图框 format 里是参数引用（&CNAME / &MSTD / &DRWNUM / &MASS[.4]），
               必须把“同名参数”写进模型才会解析；只写 PTC_COMMON_NAME 不够
               （那是系统参数，图框并不引用它），这正是标题栏显示 &XXX 原文的原因。
               保存仍禁用（改参数后 ProMdlSave 会死等），PDF 导出不依赖保存。 */
            if (1) {
                ProMdl part = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part) == PRO_TK_NO_ERROR && part) {
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
                /* 参数写完必须重生成图纸，标题栏才会取到新值（前面那次 regen 在写参数之前） */
                ProDwgSheetRegenerate((ProDrawing)drw, 1);
                logf_("[titlebar] params written + sheet regenerated");
            }
            /* 教程正规流程：先“显示模型注释”（把 3D 尺寸/公差捞到图纸），
               再技术要求注释，最后保存 + 导出 PDF。
               （SDAutoDimension“智能标注”在本环境实测不可用，弃用。） */
            /* 像工程师一样只显示关键尺寸（不再用“全显+拭除”，改为逐个精确显示） */
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
            add_technote(drw);

            /* P0b：标题栏占位符字面文本（&CNAME 等）→ 实际值（对照真人图纸同款格式） */
            {
                ProMdl part4 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part4) == PRO_TK_NO_ERROR && part4)
                    fill_title_block(drw, part4);
                else
                    logf_("[filltb] part retrieve failed (phase2)");
            }
            /* PRO_AXIS 已关闭（轮次 8：模型轴=全-length 长线，污染图面；
               6 凸点中心线改由下面的 boss_centerlines 手画短十字） */
            /* P3：6 个凸点的点画线十字中心线（图纸实体，不写模型） */
            {
                ProMdl part7 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part7) == PRO_TK_NO_ERROR && part7)
                    boss_centerlines(drw, part7);
            }
            /* P3：右上角 1:2 轴测图 */
            {
                ProMdl part6 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part6) == PRO_TK_NO_ERROR && part6)
                    add_iso_view(drw, part6, 700.0, 630.0, 0.5);
            }
            /* P2：三视图紧凑化（间距 75 单位≈31.5mm；轴测图不参与） */
            layout_report(drw, 1);
            /* P1：总长 63.6 参考尺寸（模型侧建 + 显示）——暂缓：
               实测 ProDimensionCreate 返回 -2 后会污染标注上下文，
               紧接着的 ProDrawingDimensionMove 崩溃 → 管线重启死循环（本轮无 PDF）。
               留在 TOTFACE 命令里单独调试，不接入相位2。 */
            if (0) {
                ProMdl part8 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part8) == PRO_TK_NO_ERROR && part8)
                    add_total_dim_model(drw, part8);
            }
            /* ★ place_dims 暂缓接入（轮次9：在 phase2 定时器回调里移动尺寸会触发
               致命错误 → phase2 死循环、无 PDF。纪律：先在命令通道单独验证，再接入。）
               函数保留，待以 PLACEDIMS 命令形式验证。 */
            if (0) {
                ProMdl part9 = NULL;
                if (ProMdlnameRetrieve(g_def_model, PRO_MDLFILE_PART, &part9) == PRO_TK_NO_ERROR && part9)
                    place_dims(drw, part9);
            }

            /* 先导出 PDF 并 Base64 流出（保交付：不依赖保存是否成功）。
               注意：place_dims 之后绝不 regen —— 直接导出。 */
            {
                ProPath       out;
                SYSTEMTIME    st2;
                ProPDFOptions opts = NULL;
                GetLocalTime(&st2);
                swprintf_s(out, PRO_PATH_SIZE,
                           L"D:\\creo-agent\\sync-toolkit\\work\\pdf_live_%02u%02u%02u.pdf",
                           (unsigned)st2.wHour, (unsigned)st2.wMinute, (unsigned)st2.wSecond);
                ProPDFoptionsAlloc(&opts);
                logf_("[defer] phase2 pdf export -> %d", (int)ProPDFExport(drw, out, opts));
                if (opts) ProPDFoptionsFree(opts);
                dump_file_b64(out);
            }

            /* 落盘（备份/保存）暂时跳过：改过尺寸属性后它们会死等（已知问题），
               先把管线稳定送到命令通道，落盘方案另行处理。 */
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

/* QUICKDRAW：打开模型 -> 确保它是“当前模型” -> 执行快速出图宏 -> 等产物 */
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

    /* 出图前保证模型里有 front 定向视图（快速出图主视图优先级 front > XY|Z） */
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
            ProError ed = ProMdlDisplay(mdl);
            int     wid = -1;

            logf_("  ProMdlDisplay -> %d (%s)", (int)ed, errname(ed));

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

/* DUMPNEW：目录里找“最新修改”的匹配文件，进程内读出（用于看 PiCreo/轨迹日志） */
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

/* 简易 memmem（MSVC 无此函数） */
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

/* 文件头（16KB 内）是否含 "Process ID: <pid>"（UTF-8 或 UTF-16LE） */
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

/* DUMPPID：按“文件头含 Process ID: <pid>”定位文件并整读（找本会话 trail 用） */
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

/* ---------------- 干涉检查（ProFit） ---------------- */

/* 取 selection 引用的模型名（组件优先用装配路径解析），成功返回 1 */
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

/* 把一条 selection 格式化成单行："模型名 (id=NNN)" */
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
 * 读文件头：本进程（xtop）读 —— 公司透明加密只对授权进程解密，所以这里看到的是明文；
 * 把明文大小 + STEP 头打到 stderr（明文重定向）便于外部核对。
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
            /* 非 STEP 文件（例如别的 DLL 写的日志）：把前 1200 字节的可打印内容打出来 */
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

/* 导出后自校验：输出基名 + Creo 自动补的 .stp */
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

    for (i = 0; i < g_nrc; i++) {
        if (run_readcommon_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_nrn; i++) {
        if (run_rename_job(i)) ok++; else fail++;
    }

    for (i = 0; i < g_nsc; i++) {
        if (run_setcommon_job(i)) ok++; else fail++;
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

    for (i = 0; i < g_nfa; i++) {
        if (run_flatasm_job(i)) ok++; else fail++;
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

    logf_("SYNC_EXPORT_DONE ok=%d fail=%d", ok, fail);

    /* 完成标记文件：parametric.exe 带图形启动时 stderr 抓不到，外部脚本靠它判断结束；
     * 有延迟任务时先不写，由定时器在最终结果时写入。 */
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
