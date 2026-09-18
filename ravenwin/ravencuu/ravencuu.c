/* ravencuu - Raven Ridge (1002:15dd) CU unlock for Windows.
 *
 * C port of the retired Go prototype (v0.6) - behavior-identical, ASCII output.
 * The only valid write window is inside amdkmdag init, between the harvest
 * restore and the CU bitmap read. pounce recreates it: disable the device
 * (driver unloads) -> write CC -> enable under a spin loop that re-strikes
 * on the harvest restore (~+284..422 ms), before the bitmap read.
 *
 * Register path (identical to uefi/RavenCuTest, Linux v6.8 gfx_v9):
 *   GRBM_GFX_INDEX               BAR5 + 0x30800   (selector, segment 1)
 *   CC_GC_SHADER_ARRAY_CONFIG    BAR5 + 0x89bc    (inactive CUs, bits 16-31)
 *   GC_USER_SHADER_ARRAY_CONFIG  BAR5 + 0x89c0    (never written here)
 *   SE0/SH0/all-instances selector write: 0x40000000
 *   CU masks: 9 -> 0x100, 10 -> 0x300, 11 -> 0x700 (bits 8-10 only;
 *   clearing bits 11-15 hung the GPU under Linux - never touch them)
 *
 * Commands: status | pounce --count <9|10|11> --confirm [--retries N]
 *           | install-autostart | uninstall-autostart | cleanup
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>

#pragma comment(lib, "shell32.lib")

/* not declared by every SDK under WIN32_LEAN_AND_MEAN */
extern BOOL WINAPI IsUserAnAdmin(void);

/* ---- fully CRT-free: these would otherwise pull vcruntime140.dll /
 * ucrtbase.dll, which we deliberately do not depend on ---- */
int _fltused;   /* referenced because of the double math in %.Nf */

/* define the replacements BEFORE the implementations below, so their own
 * bodies compile against them too */
#define memcpy  xmemcpy
#define memset  xmemset
#define memmove xmemmove
#define strlen  xstrlen
#define strcmp  xstrcmp
#define strncmp xstrncmp
#define strstr  xstrstr
#define strrchr xstrrchr
#define atoi    xatoi

/* volatile stores stop /O1 loop-idiom recognition from replacing these
 * loops with calls to the real CRT functions we avoid */
static void *xmemcpy(void *d, const void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static void *xmemset(void *d, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static void *xmemmove(void *d, const void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if (p < q) { while (n--) *p++ = *q++; }
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

static size_t xstrlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int xstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int xstrncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static char *xstrstr(const char *h, const char *n)
{
    size_t nl = xstrlen(n);
    for (; *h; h++)
        if (!xstrncmp(h, n, nl)) return (char *)h;
    return NULL;
}

static char *xstrrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    return (char *)last;
}

static int xatoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ---- constants (identical to the Go build) ---- */

#define IOCTL_TS_R   0x80006498u   /* \\.\ThrottleStop  physical read  */
#define IOCTL_TS_W   0x8000649Cu   /* \\.\ThrottleStop  physical write */
#define IOCTL_RPCI   ((40000u << 16) | (1u << 14) | (0x851u << 2)) /* \\.\WinRing0_1_2_0 */
#define IOCTL_WPCI   ((40000u << 16) | (2u << 14) | (0x852u << 2))

#define RAVEN_VEN  0x1002u
#define RAVEN_DID  0x15ddu
#define SUBSYS_VEN 0x1458u   /* Gigabyte board the EFI app is gated to */
#define SUBSYS_DEV 0xd000u

#define OFF_GRBM 0x30800ui64
#define OFF_CC   0x89bcui64
#define OFF_USER 0x89c0ui64
#define SEL_SE0SH0 0x40000000u

#define MAX_POUNCES 16

static const char *SVC_TS = "ThrottleStop";
static const char *SVC_WR0 = "WinRing0_1_2_0";
static const char *BOOT_TASK = "ravencuu";
static const char *DEV_TS = "\\\\.\\ThrottleStop";
static const char *DEV_WR0 = "\\\\.\\WinRing0_1_2_0";

/* ---- minimal formatter (the only specifiers this tool uses):
 * %s %d %u %x with '0'-padded widths, %%.Nf, %% ---- */

typedef struct {
    char *p;
    size_t left;
} Buf;

static void bufEmit(Buf *b, const char *s, size_t n)
{
    while (n-- && b->left > 1) {
        *b->p++ = *s++;
        b->left--;
    }
}

static void emitNum(Buf *b, unsigned long long v, unsigned base, int pad, int neg)
{
    char tmp[24];
    const char *dig = "0123456789abcdef";
    int n = 0, i;

    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    if (neg) tmp[n++] = '-';
    for (i = n; i < pad && i < 24; i++) tmp[i] = '0';
    while (i > 0) { bufEmit(b, &tmp[--i], 1); }
}

static void vformat(Buf *b, const char *fmt, va_list ap)
{
    while (*fmt) {
        const char *s = fmt;
        int pad = 0, prec = 6, islong = 0;

        if (*fmt != '%') {
            while (*fmt && *fmt != '%') fmt++;
            bufEmit(b, s, (size_t)(fmt - s));
            continue;
        }
        fmt++;
        if (*fmt == '%') { bufEmit(b, "%", 1); fmt++; continue; }
        if (*fmt == '0') { pad = -1; fmt++; }       /* remember leading-0 */
        while (*fmt >= '0' && *fmt <= '9') {
            if (pad < 0) pad = 0;
            pad = pad * 10 + (*fmt++ - '0');
        }
        if (pad < 0) pad = 0;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { islong++; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *v = va_arg(ap, const char *);
            bufEmit(b, v ? v : "(null)", xstrlen(v ? v : "(null)"));
            break;
        }
        case 'd': {
            long long v = islong ? va_arg(ap, long long) : va_arg(ap, int);
            emitNum(b, v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v,
                    10, pad, v < 0);
            break;
        }
        case 'u':
        case 'x': {
            unsigned long long v = islong ? va_arg(ap, unsigned long long)
                                          : va_arg(ap, unsigned int);
            emitNum(b, v, *fmt == 'x' ? 16 : 10, pad, 0);
            break;
        }
        case 'f': {
            double d = va_arg(ap, double);
            unsigned long long scale = 1;
            long long cents;
            int k, neg;

            for (k = 0; k < prec; k++) scale *= 10;
            cents = (long long)(d * (double)scale + (d < 0 ? -0.5 : 0.5));
            neg = cents < 0;
            if (neg) cents = -cents;
            emitNum(b, (unsigned long long)(cents / (long long)scale), 10, 0, neg);
            if (prec) {
                bufEmit(b, ".", 1);
                emitNum(b, (unsigned long long)(cents % (long long)scale), 10, prec, 0);
            }
            break;
        }
        }
        fmt++;
    }
}

static void xsprintf(char *out, size_t outsz, const char *fmt, ...)
{
    va_list ap;
    Buf b = { out, outsz };

    va_start(ap, fmt);
    vformat(&b, fmt, ap);
    va_end(ap);
    if (b.left) *b.p = 0; else if (outsz) out[outsz - 1] = 0;
}

/* ---- logging (mirror of the Go say(): console + ravencuu.log,
 * via kernel32 WriteFile - no stdio, no CRT initialization needed) ---- */

static HANDLE g_logH = INVALID_HANDLE_VALUE;
static char g_fmt[16384];

static size_t sayRaw(const char *s, size_t len)
{
    DWORD w = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)len, &w, NULL);
    if (g_logH != INVALID_HANDLE_VALUE)
        WriteFile(g_logH, s, (DWORD)len, &w, NULL);
    return len;
}

static void rawOut(const char *s)
{
    sayRaw(s, xstrlen(s));
}

static void say(const char *fmt, ...)
{
    va_list ap;
    Buf b = { g_fmt, sizeof(g_fmt) };

    va_start(ap, fmt);
    vformat(&b, fmt, ap);
    va_end(ap);
    if (b.left) *b.p = 0; else g_fmt[sizeof(g_fmt) - 1] = 0;
    sayRaw(g_fmt, sizeof(g_fmt) - b.left);
}

/* directory of the running exe (the log and drivers\ live next to it) */
static void exeDir(char *dir, size_t dirsz)
{
    char path[MAX_PATH];
    char *slash;

    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = xstrrchr(path, '\\');
    if (slash) *slash = 0;
    lstrcpynA(dir, path, (int)dirsz);
}

static const char *sysRoot(void)
{
    static char buf[MAX_PATH];
    if (!buf[0] && !GetEnvironmentVariableA("SystemRoot", buf, sizeof(buf)))
        lstrcpynA(buf, "C:\\Windows", sizeof(buf));
    return buf;
}

static void openLog(void)
{
    char path[MAX_PATH * 2], dir[MAX_PATH];
    SYSTEMTIME st;

    exeDir(dir, sizeof(dir));
    xsprintf(path, sizeof(path), "%s\\ravencuu.log", dir);
    g_logH = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logH != INVALID_HANDLE_VALUE)
        SetFilePointer(g_logH, 0, NULL, FILE_END);
    GetLocalTime(&st);
    say("==== %04u-%02u-%02u %02u:%02u:%02u ====\n",
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* ---- runOut: run a command line, capture combined output (hidden) ----
 * Returns the process exit code, or -1 when it could not be started. */

static void trim(char *s)
{
    char *start = s, *e;
    while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t') start++;
    e = start + strlen(start);
    while (e > start && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n' || e[-1] == '\t')) *--e = 0;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static int runOut(const char *cmdline, char *out, size_t outsz)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    static char buf[4096];
    DWORD n, code = (DWORD)-1;
    size_t used = 0;
    BOOL ok;

    if (out && outsz) out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    ok = CreateProcessA(NULL, (char *)cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n) {
        if (out && used + 1 < outsz) {
            size_t take = n;
            if (used + take >= outsz) take = outsz - 1 - used;
            memcpy(out + used, buf, take);
            used += take;
            out[used] = 0;
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void sayOut(const char *prefix, const char *text)
{
    static char t[16384];
    lstrcpynA(t, text, sizeof(t));
    trim(t);
    say("%s%s\n", prefix, t);
}

/* ---- BYOVD device access ---- */

static HANDLE openDevice(const char *name)
{
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int pciRd(HANDLE wh, DWORD bdf, DWORD reg, DWORD *val)
{
    unsigned char ib[8];
    DWORD ob = 0, br = 0;
    memcpy(ib, &bdf, 4);
    memcpy(ib + 4, &reg, 4);
    if (!DeviceIoControl(wh, IOCTL_RPCI, ib, sizeof(ib), &ob, 4, &br, NULL)) return -1;
    *val = ob;
    return 0;
}

static int pciWr(HANDLE wh, DWORD bdf, DWORD reg, DWORD val)
{
    unsigned char ib[12];
    DWORD br = 0;
    memcpy(ib, &bdf, 4);
    memcpy(ib + 4, &reg, 4);
    memcpy(ib + 8, &val, 4);
    if (!DeviceIoControl(wh, IOCTL_WPCI, ib, sizeof(ib), NULL, 0, &br, NULL)) return -1;
    return 0;
}

static int tsRead(HANDLE th, uint64_t addr, DWORD *val)
{
    unsigned char ib[8];
    DWORD ob = 0, br = 0;
    memcpy(ib, &addr, 8);
    if (!DeviceIoControl(th, IOCTL_TS_R, ib, sizeof(ib), &ob, 4, &br, NULL)) return -1;
    *val = ob;
    return 0;
}

static int tsWrite(HANDLE th, uint64_t addr, DWORD val)
{
    unsigned char ib[12];
    DWORD br = 0;
    memcpy(ib, &addr, 8);
    memcpy(ib + 8, &val, 4);
    if (!DeviceIoControl(th, IOCTL_TS_W, ib, sizeof(ib), NULL, 0, &br, NULL)) return -1;
    return 0;
}

/* ---- service management via sc.exe (demand kernel services) ---- */

static int svcQuery(const char *svc, const char *needle, char *buf, size_t bufsz)
{
    char cmd[256];
    xsprintf(cmd, sizeof(cmd), "sc.exe query %s", svc);
    if (runOut(cmd, buf, bufsz) != 0) return 0;
    return strstr(buf, needle) != NULL;
}

static int svcExists(const char *svc, char *buf, size_t bufsz)
{
    return svcQuery(svc, "STATE", buf, bufsz);
}

static int svcRunning(const char *svc, char *buf, size_t bufsz)
{
    return svcQuery(svc, "RUNNING", buf, bufsz);
}

static int ensureDriver(const char *svc, const char *file)
{
    static char buf[16384];
    char dir[MAX_PATH], src[MAX_PATH * 2], dst[MAX_PATH * 2], cmd[MAX_PATH * 2];

    exeDir(dir, sizeof(dir));
    xsprintf(src, sizeof(src), "%s\\drivers\\%s", dir, file);
    xsprintf(dst, sizeof(dst), "%s\\System32\\drivers\\%s", sysRoot(), file);

    if (GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES) {
        CopyFileA(src, dst, FALSE);            /* idempotent 14/50 KB copy */
    } else if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) {
        say("[!] driver file missing: %s (and no copy in System32\\drivers)\n", src);
        return -1;
    }

    if (!svcExists(svc, buf, sizeof(buf))) {
        xsprintf(cmd, sizeof(cmd),
                 "sc.exe create %s type= kernel start= demand binPath= \\SystemRoot\\System32\\drivers\\%s",
                 svc, file);
        runOut(cmd, buf, sizeof(buf));
    }
    if (!svcRunning(svc, buf, sizeof(buf))) {
        xsprintf(cmd, sizeof(cmd), "sc.exe start %s", svc);
        if (runOut(cmd, buf, sizeof(buf)) != 0) {
            /* stale / marked-for-deletion service: recreate once (50HX self-heal) */
            xsprintf(cmd, sizeof(cmd), "sc.exe delete %s", svc);
            runOut(cmd, buf, sizeof(buf));
            xsprintf(cmd, sizeof(cmd),
                     "sc.exe create %s type= kernel start= demand binPath= \\SystemRoot\\System32\\drivers\\%s",
                     svc, file);
            runOut(cmd, buf, sizeof(buf));
            xsprintf(cmd, sizeof(cmd), "sc.exe start %s", svc);
            if (runOut(cmd, buf, sizeof(buf)) != 0) {
                say("[!] start %s failed: %s\n", svc, buf);
                say("    (the Microsoft vulnerable-driver blocklist / Defender can block it - see README)\n");
                return -1;
            }
        }
    }
    return 0;
}

static void removeDriver(const char *svc, const char *file)
{
    static char buf[16384];
    char cmd[MAX_PATH * 2];

    xsprintf(cmd, sizeof(cmd), "sc.exe query %s", svc);
    if (runOut(cmd, buf, sizeof(buf)) == 0) {
        xsprintf(cmd, sizeof(cmd), "sc.exe stop %s", svc);
        runOut(cmd, buf, sizeof(buf));
        xsprintf(cmd, sizeof(cmd), "sc.exe delete %s", svc);
        runOut(cmd, buf, sizeof(buf));
    }
    xsprintf(cmd, sizeof(cmd), "%s\\System32\\drivers\\%s", sysRoot(), file);
    DeleteFileA(cmd);
}

/* ---- GPU discovery and register access ---- */

typedef struct {
    DWORD grbm, cc, user;
} Regs;

typedef struct {
    DWORD bdf, subsysVen, subsysDev;
    uint64_t bar5;
    BOOL mmioEnable;
} RavenDev;

/* scans PCI config space (buses 0-15) for the Raven iGPU; fills the first */
static int findRaven(HANDLE wh, RavenDev *d)
{
    DWORD bus, dev, fn, id, ss, bar5, cmd;
    for (bus = 0; bus < 16; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                DWORD bdf = (bus << 8) | (dev << 3) | fn;
                if (pciRd(wh, bdf, 0x00, &id) || id == 0xFFFFFFFFu) continue;
                if ((id & 0xFFFF) != RAVEN_VEN || (id >> 16) != RAVEN_DID) continue;
                pciRd(wh, bdf, 0x2C, &ss);
                pciRd(wh, bdf, 0x24, &bar5);
                pciRd(wh, bdf, 0x04, &cmd);
                d->bdf = bdf;
                d->subsysVen = ss & 0xFFFF;
                d->subsysDev = ss >> 16;
                d->bar5 = (uint64_t)(bar5 & ~(DWORD)0xF);
                d->mmioEnable = (cmd & 0x2) != 0;
                return 1;
            }
        }
    }
    return 0;
}

static int readRegs(HANDLE th, uint64_t bar5, Regs *r)
{
    if (tsRead(th, bar5 + OFF_GRBM, &r->grbm)) return -1;
    if (tsRead(th, bar5 + OFF_CC, &r->cc)) return -1;
    if (tsRead(th, bar5 + OFF_USER, &r->user)) return -1;
    return 0;
}

static int activeCUs(const Regs *r)
{
    DWORD bits = ~((r->cc | r->user) >> 16) & 0x7FF;
    int n = 0;
    while (bits) { n += bits & 1; bits >>= 1; }
    return n;
}

static DWORD cuMask(int count)
{
    switch (count) {
    case 9:  return 0x100;
    case 10: return 0x300;
    case 11: return 0x700;
    }
    return 0;
}

static void printRegs(const Regs *r)
{
    say("    GRBM_GFX_INDEX              = 0x%08x\n", (unsigned)r->grbm);
    say("    CC_GC_SHADER_ARRAY_CONFIG   = 0x%08x\n", (unsigned)r->cc);
    say("    GC_USER_SHADER_ARRAY_CONFIG = 0x%08x\n", (unsigned)r->user);
    say("    active CUs = %d (bitmap 0x%03x)\n", activeCUs(r),
         (unsigned)(~((r->cc | r->user) >> 16) & 0x7FF));
}

static void printDev(const RavenDev *d)
{
    say("[*] Raven iGPU at %02x:%02x.%x  subsys %04x:%04x  BAR5=0x%llx  MMIO=%d\n",
         (unsigned)(d->bdf >> 8), (unsigned)((d->bdf >> 3) & 0x1F), (unsigned)(d->bdf & 7),
         (unsigned)d->subsysVen, (unsigned)d->subsysDev,
         (unsigned long long)d->bar5, d->mmioEnable ? 1 : 0);
}

/* selects the Linux SE0/SH0 bank, writes CC, verifies the read-back and
 * the USER register, restores the prior selector. Never rolls the CU
 * change back on error (a failed test may need a cold boot). */
static int writeCC(HANDLE th, uint64_t bar5, DWORD newCC, Regs *afterOut)
{
    Regs before, after;
    DWORD savedSel;

    if (readRegs(th, bar5, &before)) return -1;
    savedSel = before.grbm;
    if (tsWrite(th, bar5 + OFF_GRBM, SEL_SE0SH0)) {
        say("[!] selector write failed\n");
        return -1;
    }
    if (tsWrite(th, bar5 + OFF_CC, newCC)) {
        tsWrite(th, bar5 + OFF_GRBM, savedSel);
        say("[!] CC write failed\n");
        return -1;
    }
    if (readRegs(th, bar5, &after)) {
        tsWrite(th, bar5 + OFF_GRBM, savedSel);
        return -1;
    }
    if (tsWrite(th, bar5 + OFF_GRBM, savedSel)) {
        say("  [!] selector restore failed (a cold boot clears it)\n");
    }
    if (afterOut) *afterOut = after;
    if (after.cc != newCC) {
        say("[!] CC read-back mismatch: wrote 0x%08x, read 0x%08x (write ignored?)\n",
             (unsigned)newCC, (unsigned)after.cc);
        return -1;
    }
    if (after.user != before.user) {
        say("[!] USER register changed: 0x%08x -> 0x%08x\n",
             (unsigned)before.user, (unsigned)after.user);
        return -1;
    }
    return 0;
}

/* deploys both drivers, opens both device handles; 0 on success */
static int deployAll(HANDLE *wh, HANDLE *th)
{
    say("[*] deploying BYOVD drivers (WinRing0x64 + ThrottleStop)...\n");
    if (ensureDriver(SVC_WR0, "WinRing0x64.sys")) return -1;
    if (ensureDriver(SVC_TS, "ThrottleStop.sys")) return -1;
    *wh = openDevice(DEV_WR0);
    if (*wh == INVALID_HANDLE_VALUE) {
        say("[!] open %s failed\n", DEV_WR0);
        return -1;
    }
    *th = openDevice(DEV_TS);
    if (*th == INVALID_HANDLE_VALUE) {
        say("[!] open %s failed\n", DEV_TS);
        CloseHandle(*wh);
        return -1;
    }
    return 0;
}

/* deploys drivers, finds the device, prints it, checks BAR5/MMIO */
static int openRaven(HANDLE *wh, HANDLE *th, RavenDev *d)
{
    if (deployAll(wh, th)) return -1;
    if (!findRaven(*wh, d)) {
        say("[!] no PCI 1002:15dd device found (buses 0-15 scanned)\n");
        CloseHandle(*wh);
        CloseHandle(*th);
        return -1;
    }
    printDev(d);
    if (!d->mmioEnable || d->bar5 == 0) {
        say("[!] BAR5 not assigned or MMIO decoding off (bar5=0x%llx mmio=%d)\n",
             (unsigned long long)d->bar5, d->mmioEnable ? 1 : 0);
        CloseHandle(*wh);
        CloseHandle(*th);
        return -1;
    }
    return 0;
}

/* hard write-mode gate: only the tested board */
static int gateSubsys(const RavenDev *d)
{
    if (d->subsysVen != SUBSYS_VEN || d->subsysDev != SUBSYS_DEV) {
        say("[!] subsystem %04x:%04x is not the tested board %04x:%04x - refusing\n",
             (unsigned)d->subsysVen, (unsigned)d->subsysDev,
             (unsigned)SUBSYS_VEN, (unsigned)SUBSYS_DEV);
        return -1;
    }
    return 0;
}

/* ---- pounce ---- */

typedef struct {
    int wipes;
    double firstMs;
    int nEvents;
    char events[8][96];
} PounceStats;

typedef struct {
    HANDLE wh, th;
    uint64_t *bar5p;
    DWORD bdf;
    DWORD stockCC, newCC;
    LARGE_INTEGER *enableQpc;   /* 0 until the enable command is issued */
    volatile LONG stop;
    PounceStats *stats;
} PounceCtx;

static DWORD WINAPI pounceLoop(LPVOID arg)
{
    PounceCtx *c = (PounceCtx *)arg;
    LARGE_INTEGER freq, now;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    QueryPerformanceFrequency(&freq);

    while (!c->stop) {
        DWORD cc;
        if (tsRead(c->th, *c->bar5p + OFF_CC, &cc)) {
            DWORD b5;                       /* BAR may have been reprogrammed */
            if (!pciRd(c->wh, c->bdf, 0x24, &b5)) *c->bar5p = (uint64_t)(b5 & ~(DWORD)0xF);
            continue;
        }
        if (cc != c->stockCC) continue;

        QueryPerformanceCounter(&now);
        if (c->enableQpc->QuadPart) {
            double rel = (double)(now.QuadPart - c->enableQpc->QuadPart) * 1000.0 / freq.QuadPart;
            if (!c->stats->wipes) c->stats->firstMs = rel;
            if (c->stats->nEvents < 8) {
                xsprintf(c->stats->events[c->stats->nEvents++], 96,
                         "wipe #%d at +%.3fms", c->stats->wipes + 1, rel);
            }
        } else if (c->stats->nEvents < 8) {
            xsprintf(c->stats->events[c->stats->nEvents++], 96,
                     "wipe #%d pre-enable", c->stats->wipes + 1);
        }
        c->stats->wipes++;
        if (c->stats->wipes <= MAX_POUNCES) {
            tsWrite(c->th, *c->bar5p + OFF_GRBM, SEL_SE0SH0);
            tsWrite(c->th, *c->bar5p + OFF_CC, c->newCC);
            tsWrite(c->th, *c->bar5p + OFF_GRBM, 0);
        }
    }
    return 0;
}

/* finds the iGPU PnP instance via PowerShell (property names are not
 * localized, unlike pnputil's labels) */
static int instanceID(char *id, size_t idsz)
{
    static char out[16384];
    int rc = runOut("powershell.exe -NoProfile -NonInteractive -Command \"Get-PnpDevice -PresentOnly "
                    "| Where-Object { $_.InstanceId -like 'PCI\\VEN_1002&DEV_15DD*' } "
                    "| Select-Object -ExpandProperty InstanceId\"",
                    out, sizeof(out));
    trim(out);
    if (rc != 0 || !out[0]) {
        say("[!] iGPU instance ID not found (Get-PnpDevice) - check Device Manager\n");
        return -1;
    }
    lstrcpynA(id, out, (int)idsz);
    return 0;
}

/* one full pounce attempt; 1 = won, 0 = lost the race, -1 = error */
static int runPonce(int count, int attempt)
{
    static char buf[16384];
    char cmd[MAX_PATH * 2], id[MAX_PATH];
    HANDLE wh = INVALID_HANDLE_VALUE, th = INVALID_HANDLE_VALUE, ht = NULL;
    RavenDev d;
    Regs before, final;
    DWORD stockCC, newCC, cmdreg, b5, mask = cuMask(count);
    int disabled = 0, won = -1, rc, ev;
    ULONGLONG deadline;
    uint64_t bar5v;
    LARGE_INTEGER enableQpc;
    PounceStats stats;
    PounceCtx ctx;

    if (attempt > 1) say("[*] pounce attempt %d ...\n", attempt);
    if (!mask) {
        say("[!] count must be 9, 10 or 11\n");
        return -1;
    }
    if (instanceID(id, sizeof(id))) return -1;
    if (openRaven(&wh, &th, &d)) return -1;
    if (gateSubsys(&d)) goto done;

    /* at boot the task can fire before amdkmdag brings the GC domain out
     * of reset - BAR5 then reads all-ones. Wait for sane registers. */
    deadline = GetTickCount64() + 90000;
    for (;;) {
        Regs r = {0};
        int bad = readRegs(th, d.bar5, &r) || r.cc == 0 || r.cc == 0xFFFFFFFFu;
        if (!bad) { before = r; break; }
        if (GetTickCount64() > deadline) {
            say("[!] registers not readable (CC=0x%08x after 90 s) - driver init stuck?\n",
                 (unsigned)r.cc);
            goto done;
        }
        say("[*] waiting for the GPU to come up (CC=0x%08x) ...\n", (unsigned)r.cc);
        Sleep(3000);
    }
    say("[*] before:\n");
    printRegs(&before);
    stockCC = before.cc;
    newCC = before.cc & ~(mask << 16);

    say("[*] disabling iGPU (display may freeze - expected) ...\n");
    xsprintf(cmd, sizeof(cmd), "pnputil.exe /disable-device %s", id);
    rc = runOut(cmd, buf, sizeof(buf));
    sayOut("    ", buf);
    if (rc) { say("[!] disable failed (exit %d)\n", rc); goto done; }
    disabled = 1;

    /* PnP disable may drop MMIO decode from the PCI command register */
    if (!pciRd(wh, d.bdf, 0x04, &cmdreg) && (cmdreg & 0x6) != 0x6) {
        cmdreg |= 0x6;
        pciWr(wh, d.bdf, 0x04, cmdreg);
        say("[*] forced PCI command MMIO decode back on\n");
    }

    if (writeCC(th, d.bar5, newCC, NULL)) {
        say("[!] offline write failed\n");
        goto done;
    }
    say("[+] offline write verified (CC=0x%08x) - arming pounce loop\n", (unsigned)newCC);

    memset(&stats, 0, sizeof(stats));
    bar5v = d.bar5;
    enableQpc.QuadPart = 0;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wh = wh;
    ctx.th = th;
    ctx.bar5p = &bar5v;
    ctx.bdf = d.bdf;
    ctx.stockCC = stockCC;
    ctx.newCC = newCC;
    ctx.enableQpc = &enableQpc;
    ctx.stats = &stats;
    ht = CreateThread(NULL, 0, pounceLoop, &ctx, 0, NULL);
    Sleep(200);                        /* let the spin loop get hot */

    QueryPerformanceCounter(&enableQpc);
    say("[*] enabling iGPU - pouncing on every harvest restore ...\n");
    xsprintf(cmd, sizeof(cmd), "pnputil.exe /enable-device %s", id);
    rc = runOut(cmd, buf, sizeof(buf));
    sayOut("    ", buf);
    if (rc) {
        say("[!] enable failed (exit %d) - check Device Manager\n", rc);
        goto stop_thread;
    }
    disabled = 0;

    Sleep(15000);
    ctx.stop = 1;
    WaitForSingleObject(ht, INFINITE);
    CloseHandle(ht);
    ht = NULL;
    Sleep(200);

    if (pciRd(wh, d.bdf, 0x24, &b5)) b5 = (DWORD)d.bar5;
    if (readRegs(th, (uint64_t)(b5 & ~(DWORD)0xF), &final)) {
        say("[!] final read failed - run: ravencuu status\n");
        goto done;
    }
    say("[*] final:\n");
    printRegs(&final);
    say("[*] harvest restores seen: %d", stats.wipes);
    if (stats.wipes) say(" (first at +%.2f ms)", stats.firstMs);
    say("\n");
    for (ev = 0; ev < stats.nEvents; ev++) say("    %s\n", stats.events[ev]);

    if (final.cc == newCC) {
        say("[+] CC=0x%08x held (%d CU).\n", (unsigned)final.cc, activeCUs(&final));
        won = 1;
    } else if (final.cc == stockCC) {
        say("[!] pounce lost the race (%d restores, first +%.2f ms).\n", stats.wipes, stats.firstMs);
        won = 0;
    } else {
        say("[!] CC=0x%08x unexpected - run: ravencuu status\n", (unsigned)final.cc);
    }
    goto done;

stop_thread:
    if (ht) {
        ctx.stop = 1;
        WaitForSingleObject(ht, INFINITE);
        CloseHandle(ht);
    }
done:
    if (disabled) {
        say("[*] re-enabling iGPU (error path) ...\n");
        xsprintf(cmd, sizeof(cmd), "pnputil.exe /enable-device %s", id);
        runOut(cmd, buf, sizeof(buf));
        sayOut("    ", buf);
    }
    if (wh != INVALID_HANDLE_VALUE) CloseHandle(wh);
    if (th != INVALID_HANDLE_VALUE) CloseHandle(th);
    return won;
}

static int cmdPounce(int argc, char **argv)
{
    int i, count = 0, confirm = 0, retries = 1, attempt, won;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--retries") && i + 1 < argc) retries = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--confirm")) confirm = 1;
    }
    if (!cuMask(count)) {
        say("[!] usage: ravencuu pounce --count <9|10|11> --confirm [--retries N]\n");
        return 1;
    }
    if (!confirm) {
        say("[!] refusing without --confirm (display goes down; the race can wedge the GPU mid-init - hard reset recovers)\n");
        return 1;
    }
    for (attempt = 1; attempt <= retries; attempt++) {
        won = runPonce(count, attempt);
        if (won < 0) return 1;
        if (won) {
            if (attempt > 1) say("[+] won on attempt %d.\n", attempt);
            return 0;
        }
        if (attempt < retries) {
            say("[*] retrying ...\n");
            Sleep(2000);
        }
    }
    say("[!] pounce lost the race after %d attempt(s) - 8 CU this boot; see ravencuu.log\n", retries);
    return 1;
}

/* ---- the rest of the commands ---- */

static int cmdStatus(void)
{
    HANDLE wh, th;
    RavenDev d;
    Regs r;

    if (openRaven(&wh, &th, &d)) return 1;
    if (readRegs(th, d.bar5, &r)) {
        CloseHandle(wh);
        CloseHandle(th);
        return 1;
    }
    printRegs(&r);
    if (r.cc == 0 || r.cc == 0xFFFFFFFFu) {
        say("  [!] implausible CC value - BAR5 mapping or read path suspect; do NOT write\n");
    }
    CloseHandle(wh);
    CloseHandle(th);
    return 0;
}

static int cmdInstallAutostart(void)
{
    static char buf[16384];
    char exe[MAX_PATH], cmd[MAX_PATH * 3];

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    /* /TR must arrive as ONE argv token: whole value quoted, exe path
     * backslash-quote-escaped (the form Go's exec produced). */
    xsprintf(cmd, sizeof(cmd),
             "schtasks.exe /Create /F /TN %s /TR \"\\\"%s\\\" pounce --count 11 --confirm --retries 3\" "
             "/SC ONSTART /RU SYSTEM /RL HIGHEST",
             BOOT_TASK, exe);
    if (runOut(cmd, buf, sizeof(buf)) != 0) {
        sayOut("[!] schtasks create failed: ", buf);
        return 1;
    }
    sayOut("", buf);
    say("[+] task %s registered: every boot, SYSTEM, highest priority.\n", BOOT_TASK);
    say("    Reboot to see it fire (brief display blip before login; check ravencuu.log).\n");
    return 0;
}

static int cmdUninstallAutostart(void)
{
    static char buf[16384];
    char cmd[256];
    xsprintf(cmd, sizeof(cmd), "schtasks.exe /Delete /TN %s /F", BOOT_TASK);
    if (runOut(cmd, buf, sizeof(buf)) != 0) {
        sayOut("[!] schtasks delete failed (was it installed?): ", buf);
        return 1;
    }
    sayOut("", buf);
    say("[+] task %s removed - next boot is stock 8 CU.\n", BOOT_TASK);
    return 0;
}

static int cmdCleanup(void)
{
    removeDriver(SVC_TS, "ThrottleStop.sys");
    removeDriver(SVC_WR0, "WinRing0x64.sys");
    say("[+] services deleted, System32 driver copies removed.\n");
    return 0;
}

static void usage(void)
{
    rawOut("usage:\n"
           "  ravencuu status                        read-only: find GPU, read CC/USER/GRBM, report CU count\n"
           "  ravencuu pounce --count 11 --confirm   disable -> write -> raced enable (the unlock)\n"
           "  ravencuu install-autostart             register the boot task (pounce at every startup, 3 retries)\n"
           "  ravencuu uninstall-autostart           remove the boot task\n"
           "  ravencuu cleanup                       remove the BYOVD services + driver files\n");
}

/* the raw console entry point receives NO argc/argv (that is CRT-startup
 * magic) - tokenize GetCommandLineA ourselves; quote groups supported,
 * which covers every argument this tool takes */
static int splitArgs(char *cl, char **argv, int max)
{
    int n = 0;

    while (*cl && n < max) {
        while (*cl == ' ' || *cl == '\t') cl++;
        if (!*cl) break;
        argv[n++] = cl;
        if (*cl == '"') {
            cl++;
            argv[n - 1] = cl;
            while (*cl && *cl != '"') cl++;
        } else {
            while (*cl && *cl != ' ' && *cl != '\t') cl++;
        }
        if (*cl) *cl++ = 0;
    }
    return n;
}

int main(void)
{
    char *argv[64];
    int argc, rc;

    argc = splitArgs(GetCommandLineA(), argv, 64);
    rawOut("ravencuu 1.0 - Raven Ridge (1002:15dd) CU unlock (C port)\n");
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!IsUserAnAdmin()) {
        rawOut("[!] administrator rights required (BYOVD services + MMIO).\n");
        return 1;
    }
    openLog();

    if (!strcmp(argv[1], "status")) rc = cmdStatus();
    else if (!strcmp(argv[1], "pounce")) rc = cmdPounce(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "install-autostart")) rc = cmdInstallAutostart();
    else if (!strcmp(argv[1], "uninstall-autostart")) rc = cmdUninstallAutostart();
    else if (!strcmp(argv[1], "cleanup")) rc = cmdCleanup();
    else if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        rc = 0;
    } else {
        static char t[256];
        xsprintf(t, sizeof(t), "unknown command \"%s\"\n", argv[1]);
        rawOut(t);
        usage();
        rc = 2;
    }
    return rc;
}
