/*
 * APK 签名校验移除工具 - 原生启动器（Windows / Linux）
 *
 * 内置一个极简 HTTP 服务，提供与 Web 版一致的操作流程：
 *   上传 APK -> 反编译 -> 去除签名校验补丁 -> 重打包 -> 重签名 -> 下载
 *
 * 跨平台：Linux 用 BSD socket / pthread；Windows 用 Winsock2 / _beginthreadex。
 * 核心去签名能力依赖 Java（apktool + uber-apk-signer），需在 tools/ 放好 jar，
 * 并在 PATH 或 ./jre 中提供 Java 运行时。
 *
 * 仅供安全研究、学习，以及对自己拥有合法权益的应用进行测试。
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <io.h>
#include <direct.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "assets.h"

/* ---------------- 平台抽象 ---------------- */
#ifndef _WIN32
#define closesocket close
#endif

#ifdef _WIN32
typedef HANDLE thread_t;
static CRITICAL_SECTION g_cs;
#define MUTEX_INIT() InitializeCriticalSection(&g_cs)
#define MUTEX_LOCK() EnterCriticalSection(&g_cs)
#define MUTEX_UNLOCK() LeaveCriticalSection(&g_cs)
static unsigned __stdcall worker_shim(void* a);
static unsigned __stdcall conn_shim(void* a);
#define BEGIN_THREAD_FN(th, fn, arg) do { unsigned _tid; (th) = (HANDLE)_beginthreadex(NULL, 0, fn, (arg), 0, &_tid); } while (0)
#else
typedef pthread_t thread_t;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MUTEX_INIT() ((void)0)
#define MUTEX_LOCK() pthread_mutex_lock(&g_mutex)
#define MUTEX_UNLOCK() pthread_mutex_unlock(&g_mutex)
static void* worker_shim(void* a);
static void* conn_shim(void* a);
#define BEGIN_THREAD_FN(th, fn, arg) pthread_create(&(th), NULL, fn, (arg))
#endif

#define BEGIN_THREAD(th, arg) BEGIN_THREAD_FN(th, worker_shim, arg)

static void open_browser(const char* url);

/* 工具函数 */
static char* stristr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle)) {
            const char* h = hay; const char* n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
            if (!*n) return (char*)hay;
        }
    }
    return NULL;
}

static void make_dir(const char* d) {
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
}

static char* my_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* 前向声明 */
static void patch_file(const char* path);

/* 将字符串转义为 JSON 字符串内容（转义引号、反斜杠与所有控制字符，保留 UTF-8 原样） */
static void json_escape(const char* s, char* out, int outsz) {
    int oi = 0;
    for (int i = 0; s[i] && oi < outsz - 8; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { out[oi++] = '\\'; out[oi++] = '"'; }
        else if (c == '\\') { out[oi++] = '\\'; out[oi++] = '\\'; }
        else if (c < 0x20) {
            int n = snprintf(out + oi, outsz - oi, "\\u%04x", c);
            oi += n;
        } else {
            out[oi++] = (char)c;
        }
    }
    out[oi] = '\0';
}

/* ---------------- 全局任务状态 ---------------- */
enum { ST_QUEUED = 0, ST_PROCESSING = 1, ST_DONE = 2, ST_ERROR = 3 };

typedef struct {
    int status;
    int progress;
    char log_buf[16384];
    int log_len;
    char result_path[1024];
    char result_name[256];
    int patched;
    char error[512];
} Job;

static Job g_job;
static int g_port = 0;

/* ---------------- 工具函数 ---------------- */
static void append_log(const char* line) {
    MUTEX_LOCK();
    int remain = (int)(sizeof(g_job.log_buf) - 1 - g_job.log_len);
    if (remain > 0) {
        int n = (int)strlen(line);
        if (n > remain) n = remain;
        memcpy(g_job.log_buf + g_job.log_len, line, n);
        g_job.log_len += n;
        if (g_job.log_len < (int)sizeof(g_job.log_buf) - 1)
            g_job.log_buf[g_job.log_len++] = '\n';
        g_job.log_buf[g_job.log_len] = '\0';
    }
    MUTEX_UNLOCK();
}

static void set_progress(int p) {
    MUTEX_LOCK(); g_job.progress = p; MUTEX_UNLOCK();
}

/* 查找 Java 命令：优先 ./jre/bin/java，否则 PATH 中的 java */
static void find_java(char* out, int outsz) {
#ifdef _WIN32
    if (access("jre/bin/java.exe", 0) == 0) {
        snprintf(out, outsz, "jre/bin/java.exe");
        return;
    }
#else
    if (access("jre/bin/java", 0) == 0) {
        snprintf(out, outsz, "jre/bin/java");
        return;
    }
#endif
    snprintf(out, outsz, "java");
}

/* 解析 apktool 命令前缀：有 jar 用 java -jar，否则用系统 apktool 命令 */
static void apktool_prefix(char* out, int outsz, const char* java) {
    if (access("tools/apktool.jar", 0) == 0) {
        snprintf(out, outsz, "%s -jar \"tools/apktool.jar\"", java);
    } else {
        snprintf(out, outsz, "apktool");
    }
}

/* 执行命令并逐行写入日志（合并 stderr） */
static int run_capture(const char* cmd) {
    char full[4200];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    append_log(full);
    FILE* f = popen(full, "r");
    if (!f) { append_log("[错误] 无法启动进程"); return -1; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        append_log(line);
    }
    int rc = pclose(f);
    return rc;
}

/* ---------------- smali 补丁（跨平台目录遍历） ---------------- */
#ifdef _WIN32
#include <windows.h>
static void patch_dir(const char* dir) {
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            patch_dir(path);
        } else if (strlen(fd.cFileName) > 6 &&
                   strcmp(fd.cFileName + strlen(fd.cFileName) - 6, ".smali") == 0) {
            patch_file(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void patch_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            patch_dir(path);
        } else if (strlen(e->d_name) > 6 &&
                   strcmp(e->d_name + strlen(e->d_name) - 6, ".smali") == 0) {
            patch_file(path);
        }
    }
    closedir(d);
}
#endif

/* 对单个 smali 文件做补丁：把 Signature.equals 的返回强制为 true */
static void patch_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* 按行扫描 */
    char* out = (char*)malloc(sz * 2 + 1024);
    if (!out) { free(buf); return; }
    long out_len = 0;
    int patched_here = 0;

    char* line = buf;
    char* nl;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';
        char* cur = line;
        /* 计算缩进（指向首个非空白字符） */
        char* p = cur;
        while (*p == ' ' || *p == '\t') p++;

        if (strstr(cur, "invoke-virtual") &&
            strstr(cur, "Landroid/content/pm/Signature;->equals(Ljava/lang/Object;)Z")) {
            /* 下一行应为 move-result <reg> */
            char* nxt = nl + 1;
            char* nl2 = strchr(nxt, '\n');
            int has_next = (nl2 != NULL);
            if (has_next) *nl2 = '\0';
            if (has_next && strstr(nxt, "move-result")) {
                /* 提取目标寄存器 */
                char reg[32]; reg[0] = '\0';
                char* mr = strstr(nxt, "move-result");
                mr += strlen("move-result");
                while (*mr == ' ' || *mr == '\t') mr++;
                int ri = 0;
                while (*mr && *mr != ' ' && *mr != '\t' && *mr != '\r') {
                    if (ri < 31) reg[ri++] = *mr;
                    mr++;
                }
                reg[ri] = '\0';

                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                         "%s# [sig-remover] 强制 Signature.equals 返回 true\n"
                         "%sconst/4 %s, 0x1\n",
                         p, p, reg);
                memcpy(out + out_len, tmp, strlen(tmp));
                out_len += (long)strlen(tmp);
                patched_here++;
                line = nl2 + 1;
                continue;
            } else if (has_next) {
                *nl2 = '\n'; /* 还原 */
            }
        }
        /* 原样复制该行 + 换行 */
        memcpy(out + out_len, cur, strlen(cur));
        out_len += (long)strlen(cur);
        out[out_len++] = '\n';
        line = nl + 1;
    }
    /* 末尾残余（无换行） */
    if (*line) {
        memcpy(out + out_len, line, strlen(line));
        out_len += (long)strlen(line);
    }
    out[out_len] = '\0';

    if (patched_here > 0) {
        FILE* w = fopen(path, "wb");
        if (w) { fwrite(out, 1, out_len, w); fclose(w); }
        char msg[256];
        snprintf(msg, sizeof(msg), "  [补丁] %s：%d 处签名比对", path, patched_here);
        append_log(msg);
        MUTEX_LOCK(); g_job.patched += patched_here; MUTEX_UNLOCK();
    }
    free(buf); free(out);
}

/* ---------------- 流水线 ---------------- */
#ifdef _WIN32
static void find_first_apk(const char* dir, char* out, int outsz) {
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    out[0] = '\0';
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strstr(fd.cFileName, ".apk") != NULL) {
            snprintf(out, outsz, "%s/%s", dir, fd.cFileName);
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void find_first_apk(const char* dir, char* out, int outsz) {
    DIR* d = opendir(dir);
    out[0] = '\0';
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".apk") != NULL) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);
}
#endif

static void worker(void* arg) {
    (void)arg;
    const char* apk = (const char*)arg; /* 上传文件路径 */
    char java[256], apkcmd[256], cmd[4096];
    char decoded[1024], unsigned_apk[1024], signed_dir[1024], signed_apk[1024];

    MUTEX_LOCK(); g_job.status = ST_PROCESSING; g_job.progress = 10; MUTEX_UNLOCK();

    find_java(java, sizeof(java));
    apktool_prefix(apkcmd, sizeof(apkcmd), java);

    snprintf(decoded, sizeof(decoded), "work/decoded");
    snprintf(unsigned_apk, sizeof(unsigned_apk), "work/unsigned.apk");
    snprintf(signed_dir, sizeof(signed_dir), "work/signed");

    /* 仅清理上次的解码/打包/签名产物，保留上传文件 */
#ifdef _WIN32
    system("if exist work\\decoded rmdir /s /q work\\decoded >nul 2>&1");
    system("if exist work\\signed rmdir /s /q work\\signed >nul 2>&1");
    system("if exist work\\unsigned.apk del /q work\\unsigned.apk >nul 2>&1");
#else
    system("rm -rf work/decoded work/unsigned.apk work/signed");
#endif
    make_dir("work");

    /* 1) 反编译 */
    append_log("反编译 APK（apktool decode）…");
    snprintf(cmd, sizeof(cmd), "%s d -f -r -o \"%s\" \"%s\"", apkcmd, decoded, apk);
    if (run_capture(cmd) != 0) { goto fail; }

    /* 2) 补丁 */
    set_progress(45);
    MUTEX_LOCK(); g_job.patched = 0; MUTEX_UNLOCK();
    append_log("去除签名校验补丁…");
    patch_dir(decoded);
    { char m[128]; snprintf(m,sizeof(m),"共修补签名比对 %d 处。", g_job.patched); append_log(m); }

    /* 3) 重打包 */
    set_progress(70);
    append_log("重新打包 APK（apktool build）…");
    snprintf(cmd, sizeof(cmd), "%s b -f -o \"%s\" \"%s\"", apkcmd, unsigned_apk, decoded);
    if (run_capture(cmd) != 0) { goto fail; }

    /* 4) 重签名 */
    set_progress(85);
    append_log("重新签名 APK（uber-apk-signer）…");
    snprintf(cmd, sizeof(cmd), "%s -jar \"tools/uber-apk-signer.jar\" --apks \"%s\" --out \"%s\"",
             java, unsigned_apk, signed_dir);
    if (run_capture(cmd) != 0) { goto fail; }

    find_first_apk(signed_dir, signed_apk, sizeof(signed_apk));
    if (signed_apk[0] == '\0') { snprintf(g_job.error,sizeof(g_job.error),"未找到签名后的 APK"); goto fail; }

    /* 结果命名 */
    const char* base = strrchr(apk, '/');
#ifdef _WIN32
    const char* base2 = strrchr(apk, '\\');
    if (base2 > base) base = base2;
#endif
    base = base ? base + 1 : apk;
    char name[256];
    snprintf(name, sizeof(name), "%.*s_nosig.apk", (int)(strrchr(base,'.') ? (strrchr(base,'.')-base) : (int)strlen(base)), base);
    snprintf(g_job.result_path, sizeof(g_job.result_path), "work/%s", name);
    snprintf(g_job.result_name, sizeof(g_job.result_name), "%s", name);
#ifdef _WIN32
    rename(signed_apk, g_job.result_path);
#else
    rename(signed_apk, g_job.result_path);
#endif

    append_log("处理完成！已生成结果。");
    MUTEX_LOCK(); g_job.status = ST_DONE; g_job.progress = 100; MUTEX_UNLOCK();
    free((void*)apk);
    return;

fail:
    MUTEX_LOCK();
    if (g_job.error[0] == '\0') snprintf(g_job.error, sizeof(g_job.error), "处理失败，请查看日志");
    g_job.status = ST_ERROR;
    MUTEX_UNLOCK();
    free((void*)apk);
}

/* ---------------- HTTP 服务 ---------------- */
static int read_exact(int sock, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(sock, buf + got, n - got, 0);
        if (r <= 0) return got;
        got += r;
    }
    return got;
}

static void send_text(int sock, int code, const char* ctype, const char* body, int len) {
    char head[512];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 %d OK\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, ctype, len);
    send(sock, head, hl, 0);
    if (len > 0) send(sock, body, len, 0);
}

static void send_file(int sock, const char* path, const char* filename) {
    FILE* f = fopen(path, "rb");
    if (!f) { send_text(sock, 404, "text/plain", "not found", 9); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char head[512];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.android.package-archive\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
        filename, sz);
    send(sock, head, hl, 0);
    char buf[8192];
    long left = sz;
    while (left > 0) {
        int chunk = (int)(left > 8192 ? 8192 : left);
        int r = (int)fread(buf, 1, chunk, f);
        if (r <= 0) break;
        send(sock, buf, r, 0);
        left -= r;
    }
    fclose(f);
}

static void handle_connection(int client) {
    char hdr[65536];
    int total = 0;
    /* 读取头部直到 \r\n\r\n */
    while (total < (int)sizeof(hdr) - 1) {
        int r = recv(client, hdr + total, 1, 0);
        if (r <= 0) { closesocket(client); return; }
        total += r;
        if (total >= 4 && hdr[total-4]=='\r' && hdr[total-3]=='\n' && hdr[total-2]=='\r' && hdr[total-1]=='\n')
            break;
    }
    hdr[total] = '\0';

    char method[16] = {0}, path[2048] = {0};
    sscanf(hdr, "%15s %2047s", method, path);

    int content_length = 0;
    char* cl = stristr(hdr, "Content-Length:");
    if (cl) content_length = atoi(cl + 15);

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            send_text(client, 200, "text/html", INDEX_HTML, (int)strlen(INDEX_HTML));
        } else if (strcmp(path, "/static/app.js") == 0) {
            send_text(client, 200, "application/javascript", APP_JS, (int)strlen(APP_JS));
        } else if (strcmp(path, "/static/style.css") == 0) {
            send_text(client, 200, "text/css", STYLE_CSS, (int)strlen(STYLE_CSS));
        } else if (strncmp(path, "/api/status/", 12) == 0) {
            char json[120000];
            MUTEX_LOCK();
            char log_esc[100000];
            char err_esc[1024];
            json_escape(g_job.log_buf, log_esc, sizeof(log_esc));
            json_escape(g_job.error, err_esc, sizeof(err_esc));
            const char* rp = g_job.result_path;
            const char* rn = g_job.result_name;
            snprintf(json, sizeof(json),
                "{\"status\":%d,\"progress\":%d,\"patched\":%d,\"error\":\"%s\",\"log\":\"%s\",\"result_path\":\"%s\",\"result\":\"%s\"}",
                g_job.status, g_job.progress, g_job.patched, err_esc, log_esc, rp, rn);
            MUTEX_UNLOCK();
            send_text(client, 200, "application/json", json, (int)strlen(json));
        } else if (strncmp(path, "/api/download/", 14) == 0) {
            MUTEX_LOCK();
            char rp[1024]; snprintf(rp, sizeof(rp), "%s", g_job.result_path);
            char rn[256]; snprintf(rn, sizeof(rn), "%s", g_job.result_name);
            MUTEX_UNLOCK();
            if (rp[0] == '\0') send_text(client, 404, "text/plain", "no result", 9);
            else send_file(client, rp, rn);
        } else if (strcmp(path, "/api/health") == 0) {
            char h[256];
            int ok = (access("tools/uber-apk-signer.jar", 0) == 0) ? 1 : 0;
            snprintf(h, sizeof(h), "{\"ok\":%d}", ok);
            send_text(client, 200, "application/json", h, (int)strlen(h));
        } else {
            send_text(client, 404, "text/plain", "not found", 9);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/remove") == 0) {
        /* 以原始二进制方式接收上传文件 */
        if (access("work", 0) != 0)
            make_dir("work");
        const char* up = "work/upload.apk";
        FILE* wf = fopen(up, "wb");
        if (!wf) { send_text(client, 500, "application/json", "{\"error\":\"无法写入\"}", 22); closesocket(client); return; }
        long left = content_length;
        char buf[8192];
        while (left > 0) {
            int chunk = (int)(left > 8192 ? 8192 : left);
            int r = read_exact(client, buf, chunk);
            if (r <= 0) break;
            fwrite(buf, 1, r, wf);
            left -= r;
        }
        fclose(wf);

        /* 重置任务状态 */
        MUTEX_LOCK();
        memset(&g_job, 0, sizeof(g_job));
        g_job.status = ST_QUEUED;
        snprintf(g_job.log_buf, sizeof(g_job.log_buf), "已接收文件：work/upload.apk");
        g_job.log_len = (int)strlen(g_job.log_buf);
        MUTEX_UNLOCK();

        char* arg = my_strdup(up);
        thread_t th;
        BEGIN_THREAD(th, arg);
        if (!th) { send_text(client, 500, "application/json", "{\"error\":\"无法启动\"}", 22); closesocket(client); return; }
#ifdef _WIN32
        CloseHandle(th);
#else
        pthread_detach(th);
#endif
        send_text(client, 200, "application/json", "{\"job_id\":\"1\"}", 15);
    } else {
        send_text(client, 405, "text/plain", "method not allowed", 18);
    }
    closesocket(client);
}

/* ---------------- 主函数 ---------------- */
#ifdef _WIN32
unsigned __stdcall worker_shim(void* a) { worker(a); return 0; }
unsigned __stdcall conn_shim(void* a) { int* p = (int*)a; handle_connection(*p); free(p); return 0; }

static void open_browser(const char* url) {
    ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
}

static char* get_exe_dir(char* buf, int sz) {
    GetModuleFileNameA(0, buf, sz);
    char* s = strrchr(buf, '\\');
    if (s) *s = '\0';
    return buf;
}
#else
void* worker_shim(void* a) { worker(a); return NULL; }
void* conn_shim(void* a) { int* p = (int*)a; handle_connection(*p); free(p); return NULL; }

static void open_browser(const char* url) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' &", url);
    system(cmd);
}

static char* get_exe_dir(char* buf, int sz) {
    ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
    if (n < 0) { buf[0] = '\0'; return buf; }
    buf[n] = '\0';
    char* s = strrchr(buf, '/');
    if (s) *s = '\0';
    return buf;
}
#endif

int main(void) {
    char exedir[2048];
    get_exe_dir(exedir, sizeof(exedir));
    if (exedir[0]) {
#ifdef _WIN32
        _chdir(exedir);
#else
        chdir(exedir);
#endif
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "WSA 初始化失败\n"); return 1; }
#endif

    MUTEX_INIT();
    memset(&g_job, 0, sizeof(g_job));
    g_job.status = ST_QUEUED;

    /* 选择端口 */
    int listen_sock = -1;
    for (int port = 5000; port <= 5020; port++) {
        listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) continue;
        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((unsigned short)port);
        if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            if (listen(listen_sock, 16) == 0) { g_port = port; break; }
        }
        closesocket(listen_sock);
        listen_sock = -1;
    }
    if (listen_sock < 0) {
        fprintf(stderr, "无法绑定端口\n");
        return 1;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_port);
    printf("APK 签名校验移除工具已启动：%s\n", url);
    printf("（首次使用请确保已安装 Java，且 tools/ 下包含 apktool.jar 与 uber-apk-signer.jar）\n");
    fflush(stdout);

    open_browser(url);

    while (1) {
        struct sockaddr_in client_addr;
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        int client = accept(listen_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client < 0) continue;
        thread_t th;
        /* 每个连接开线程处理 */
        int* carg = (int*)malloc(sizeof(int));
        *carg = client;
        BEGIN_THREAD_FN(th, conn_shim, carg);
        if (!th) { closesocket(client); free(carg); }
#ifdef _WIN32
        CloseHandle(th);
#else
        /* 连接的线程自行关闭 socket；这里 detach 由 handle 内 closesocket，线程结束 */
        pthread_detach(th);
#endif
    }
    return 0;
}
