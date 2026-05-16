/*
 * monitor_windows.c
 * Servidor HTTP de monitoramento de sistema para Windows
 * Porta: 8888
 *
 * Compilar (MinGW / GCC no Windows):
 *   gcc monitor_windows.c -o monitor.exe -lws2_32 -lpdh -liphlpapi -lpsapi -lntdll -O2
 *
 * Compilar (MSVC Developer Prompt):
 *   cl monitor_windows.c /Fe:monitor.exe ws2_32.lib pdh.lib iphlpapi.lib psapi.lib
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   /* Windows 7+ */
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")

/* ─────────────────────────────────────────────
   Structs (mesmas do projeto Linux)
───────────────────────────────────────────── */

typedef struct {
    double user;
    double system;
    double idle;
    double usage;          /* % total */
    int    cores;
} CpuInfo;

typedef struct {
    unsigned long long total_mb;
    unsigned long long free_mb;
    unsigned long long used_mb;
    double             usage;   /* % */
    unsigned long long swap_total_mb;
    unsigned long long swap_free_mb;
    unsigned long long swap_used_mb;
    double             swap_usage;
} MemInfo;

typedef struct {
    char   name[64];
    double read_mb;
    double write_mb;
    double usage_pct;          /* placeholder PDH */
} DiskInfo;

typedef struct {
    char               iface[64];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    unsigned long long rx_speed; /* bytes/s — delta */
    unsigned long long tx_speed;
} NetInfo;

typedef struct {
    DWORD  pid;
    char   name[256];
    double cpu_pct;
    double mem_mb;
} ProcInfo;

typedef struct {
    DWORD pid;
    char  name[256];
    unsigned long long cpu_time;
} ProcCpuState;

typedef struct {
    char   os_name[128];
    char   os_version[64];
    char   hostname[128];
    double uptime_sec;
    char   arch[32];
} SysInfo;

#define MAX_DISKS  8
#define MAX_NETS   8
#define MAX_PROCS  32
#define MAX_PROC_SCAN 1024

typedef struct {
    CpuInfo  cpu;
    MemInfo  mem;
    DiskInfo disks[MAX_DISKS];
    int      disk_count;
    NetInfo  nets[MAX_NETS];
    int      net_count;
    ProcInfo procs[MAX_PROCS];
    int      proc_count;
    SysInfo  sys;
} AllMetrics;

static int cmp_proc_mem_desc(const void *a, const void *b) {
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pb->mem_mb > pa->mem_mb) return  1;
    if (pb->mem_mb < pa->mem_mb) return -1;
    return 0;
}

static int cmp_proc_cpu_desc(const void *a, const void *b) {
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pb->cpu_pct > pa->cpu_pct) return  1;
    if (pb->cpu_pct < pa->cpu_pct) return -1;
    if (pb->mem_mb > pa->mem_mb) return  1;
    if (pb->mem_mb < pa->mem_mb) return -1;
    return 0;
}

/* ─────────────────────────────────────────────
   Helpers para CPU (GetSystemTimes delta)
───────────────────────────────────────────── */
static FILETIME prev_idle, prev_kernel, prev_user;
static int cpu_initialized = 0;

static double filetime_to_u64(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart;
}

static unsigned long long filetime_to_ull(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static void collect_cpu(CpuInfo *cpu) {
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpu->cores = (int)si.dwNumberOfProcessors;

    if (!cpu_initialized) {
        prev_idle   = idle;
        prev_kernel = kernel;
        prev_user   = user;
        cpu_initialized = 1;
        cpu->usage  = 0.0;
        cpu->user   = 0.0;
        cpu->system = 0.0;
        cpu->idle   = 100.0;
        return;
    }

    double d_idle   = filetime_to_u64(idle)   - filetime_to_u64(prev_idle);
    double d_kernel = filetime_to_u64(kernel) - filetime_to_u64(prev_kernel);
    double d_user   = filetime_to_u64(user)   - filetime_to_u64(prev_user);

    /* kernel time inclui idle no Windows */
    double total = d_kernel + d_user;
    if (total == 0.0) total = 1.0;

    double sys_only = d_kernel - d_idle;

    cpu->idle   = (d_idle   / total) * 100.0;
    cpu->system = (sys_only / total) * 100.0;
    cpu->user   = (d_user   / total) * 100.0;
    cpu->usage  = 100.0 - cpu->idle;

    prev_idle   = idle;
    prev_kernel = kernel;
    prev_user   = user;
}

/* ─────────────────────────────────────────────
   Memória — GlobalMemoryStatusEx
───────────────────────────────────────────── */
static void collect_mem(MemInfo *mem) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    mem->total_mb = ms.ullTotalPhys / (1024*1024);
    mem->free_mb  = ms.ullAvailPhys / (1024*1024);
    mem->used_mb  = mem->total_mb - mem->free_mb;
    mem->usage    = (mem->total_mb > 0)
                    ? (double)mem->used_mb / mem->total_mb * 100.0
                    : 0.0;

    /* Page file como "swap" */
    ULONGLONG page_total = ms.ullTotalPageFile;
    ULONGLONG page_avail = ms.ullAvailPageFile;
    mem->swap_total_mb = page_total / (1024*1024);
    mem->swap_free_mb  = page_avail / (1024*1024);
    mem->swap_used_mb  = mem->swap_total_mb - mem->swap_free_mb;
    mem->swap_usage    = (mem->swap_total_mb > 0)
                         ? (double)mem->swap_used_mb / mem->swap_total_mb * 100.0
                         : 0.0;
}

/* ─────────────────────────────────────────────
   Disco — GetLogicalDrives + GetDiskFreeSpaceEx
───────────────────────────────────────────── */
static void collect_disks(DiskInfo *disks, int *count) {
    *count = 0;
    DWORD drives = GetLogicalDrives();

    for (int i = 0; i < 26 && *count < MAX_DISKS; i++) {
        if (!(drives & (1 << i))) continue;

        char root[4] = { 'A' + i, ':', '\\', 0 };
        UINT type = GetDriveTypeA(root);
        /* Apenas discos fixos e redes */
        if (type != DRIVE_FIXED && type != DRIVE_REMOTE) continue;

        ULARGE_INTEGER free_bytes, total_bytes, total_free;
        if (!GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, &total_free))
            continue;

        DiskInfo *d = &disks[*count];
        snprintf(d->name, sizeof(d->name), "%c:", 'A' + i);

        double total_gb = (double)total_bytes.QuadPart / (1024.0*1024*1024);
        double free_gb  = (double)free_bytes.QuadPart  / (1024.0*1024*1024);
        double used_gb  = total_gb - free_gb;

        d->read_mb    = used_gb  * 1024.0;   /* "usado" como read_mb p/ compatibilidade JSON */
        d->write_mb   = total_gb * 1024.0;   /* "total"  como write_mb */
        d->usage_pct  = (total_gb > 0) ? (used_gb / total_gb * 100.0) : 0.0;

        (*count)++;
    }
}

/* ─────────────────────────────────────────────
   Rede — GetIfTable2 (iphlpapi)
───────────────────────────────────────────── */
static unsigned long long prev_rx[MAX_NETS];
static unsigned long long prev_tx[MAX_NETS];
static DWORD              prev_if_index[MAX_NETS];
static int                net_initialized = 0;
static DWORD              prev_net_tick   = 0;

static void collect_net(NetInfo *nets, int *count) {
    *count = 0;

    ULONG table_size = 0;
    if (GetIfTable(NULL, &table_size, FALSE) != ERROR_INSUFFICIENT_BUFFER) {
        return;
    }

    MIB_IFTABLE *table = (MIB_IFTABLE *)malloc(table_size);
    if (!table) return;

    if (GetIfTable(table, &table_size, FALSE) != NO_ERROR) {
        free(table);
        return;
    }

    DWORD now_tick = GetTickCount();
    double elapsed = net_initialized
                     ? (now_tick - prev_net_tick) / 1000.0
                     : 1.0;
    if (elapsed <= 0) elapsed = 1.0;

    for (DWORD i = 0; i < table->dwNumEntries && *count < MAX_NETS; i++) {
        MIB_IFROW *row = &table->table[i];

        /* Pular loopback e interfaces sem tráfego */
        if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row->dwInOctets == 0 && row->dwOutOctets == 0) continue;

        NetInfo *n = &nets[*count];

        /* Nome da interface (Unicode → ASCII) */
        DWORD name_len = row->dwDescrLen;
        if (name_len >= sizeof(n->iface)) name_len = sizeof(n->iface) - 1;
        memcpy(n->iface, row->bDescr, name_len);
        n->iface[name_len] = 0;
        /* Truncar nome longo */
        if (strlen(n->iface) > 30) {
            n->iface[28] = '.';
            n->iface[29] = '.';
            n->iface[30] = 0;
        }

        n->rx_bytes = row->dwInOctets;
        n->tx_bytes = row->dwOutOctets;

        /* Calcular velocidade via delta */
        n->rx_speed = 0;
        n->tx_speed = 0;
        if (net_initialized) {
            for (int j = 0; j < MAX_NETS; j++) {
                if (prev_if_index[j] == row->dwIndex) {
                    unsigned long long drx = 0;
                    unsigned long long dtx = 0;

                    if (n->rx_bytes >= prev_rx[j]) {
                        drx = n->rx_bytes - prev_rx[j];
                    }
                    if (n->tx_bytes >= prev_tx[j]) {
                        dtx = n->tx_bytes - prev_tx[j];
                    }

                    n->rx_speed = (unsigned long long)(drx / elapsed);
                    n->tx_speed = (unsigned long long)(dtx / elapsed);
                    break;
                }
            }
        }

        /* Salvar estado anterior */
        prev_if_index[*count] = row->dwIndex;
        prev_rx[*count] = n->rx_bytes;
        prev_tx[*count] = n->tx_bytes;

        (*count)++;
    }

    free(table);
    net_initialized = 1;
    prev_net_tick   = now_tick;
}

/* ─────────────────────────────────────────────
   Processos — CreateToolhelp32Snapshot
───────────────────────────────────────────── */

/* CPU% por processo requer dois snapshots; aqui usamos memória como métrica principal */
static ProcCpuState prev_proc_cpu[MAX_PROC_SCAN];
static int prev_proc_cpu_count = 0;
static ULONGLONG prev_proc_tick = 0;

static int find_prev_proc_cpu(DWORD pid, const char *name) {
    for (int i = 0; i < prev_proc_cpu_count; i++) {
        if (prev_proc_cpu[i].pid == pid &&
            strcmp(prev_proc_cpu[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void collect_procs(ProcInfo *procs, int *count) {
    ProcInfo scanned[MAX_PROC_SCAN];
    ProcCpuState current_cpu[MAX_PROC_SCAN];
    int scanned_count = 0;
    int current_cpu_count = 0;
    ULONGLONG now_tick = GetTickCount64();
    double elapsed = prev_proc_tick
                     ? (now_tick - prev_proc_tick) / 1000.0
                     : 0.0;
    if (elapsed < 0.001) elapsed = 0.0;

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    int cpu_cores = (int)sys_info.dwNumberOfProcessors;
    if (cpu_cores <= 0) cpu_cores = 1;

    *count = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        if (scanned_count >= MAX_PROC_SCAN) break;

        ProcInfo *p = &scanned[scanned_count];
        p->pid = pe.th32ProcessID;
        strncpy(p->name, pe.szExeFile, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = 0;
        p->cpu_pct = 0.0;
        p->mem_mb  = 0.0;

        /* Tentar ler uso de memória */
        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, p->pid);
        if (hProc) {
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                p->mem_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
            }

            FILETIME create_time, exit_time, kernel_time, user_time;
            if (GetProcessTimes(hProc, &create_time, &exit_time,
                                &kernel_time, &user_time)) {
                unsigned long long proc_time =
                    filetime_to_ull(kernel_time) + filetime_to_ull(user_time);

                if (current_cpu_count < MAX_PROC_SCAN) {
                    current_cpu[current_cpu_count].pid = p->pid;
                    strncpy(current_cpu[current_cpu_count].name,
                            p->name,
                            sizeof(current_cpu[current_cpu_count].name) - 1);
                    current_cpu[current_cpu_count]
                        .name[sizeof(current_cpu[current_cpu_count].name) - 1] = 0;
                    current_cpu[current_cpu_count].cpu_time = proc_time;
                    current_cpu_count++;
                }

                if (elapsed > 0.0) {
                    int prev_idx = find_prev_proc_cpu(p->pid, p->name);
                    if (prev_idx >= 0 &&
                        proc_time >= prev_proc_cpu[prev_idx].cpu_time) {
                        unsigned long long delta =
                            proc_time - prev_proc_cpu[prev_idx].cpu_time;
                        p->cpu_pct =
                            ((double)delta / (elapsed * 10000000.0 * cpu_cores)) * 100.0;
                        if (p->cpu_pct < 0.0) p->cpu_pct = 0.0;
                        if (p->cpu_pct > 100.0) p->cpu_pct = 100.0;
                    }
                }
            }
            CloseHandle(hProc);
        }

        scanned_count++;
    } while (Process32Next(snap, &pe));

    CloseHandle(snap);

    /* Ordenar por memória decrescente (qsort — mesmo do projeto Linux) */
    memcpy(prev_proc_cpu, current_cpu, sizeof(ProcCpuState) * current_cpu_count);
    prev_proc_cpu_count = current_cpu_count;
    prev_proc_tick = now_tick;

    qsort(scanned, scanned_count, sizeof(ProcInfo), cmp_proc_cpu_desc);

    *count = scanned_count < MAX_PROCS ? scanned_count : MAX_PROCS;
    for (int i = 0; i < *count; i++) {
        procs[i] = scanned[i];
    }
}

/* ─────────────────────────────────────────────
   Informações do sistema
───────────────────────────────────────────── */
static void collect_sysinfo(SysInfo *sys) {
    /* Hostname */
    DWORD len = sizeof(sys->hostname);
    GetComputerNameA(sys->hostname, &len);

    /* Uptime */
    sys->uptime_sec = GetTickCount64() / 1000.0;

    /* Arquitetura */
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: strcpy(sys->arch, "x86_64");  break;
        case PROCESSOR_ARCHITECTURE_ARM64: strcpy(sys->arch, "arm64");   break;
        case PROCESSOR_ARCHITECTURE_INTEL: strcpy(sys->arch, "x86");     break;
        default:                           strcpy(sys->arch, "unknown"); break;
    }

    /* Versão do Windows via RtlGetVersion */
    RTL_OSVERSIONINFOW osv = {0};
    osv.dwOSVersionInfoSize = sizeof(osv);

    typedef NTSTATUS (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");

    if (fn && fn(&osv) == 0) {
        /* Mapear build number → nome amigável */
        DWORD build = osv.dwBuildNumber;
        const char *name = "Windows";
        if      (build >= 22000) name = "Windows 11";
        else if (build >= 10240) name = "Windows 10";
        else if (build >= 9200)  name = "Windows 8/8.1";
        else if (build >= 7600)  name = "Windows 7";

        snprintf(sys->os_name, sizeof(sys->os_name), "%s", name);
        snprintf(sys->os_version, sizeof(sys->os_version),
                 "%lu.%lu (Build %lu)",
                 osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);
    } else {
        strcpy(sys->os_name,    "Windows");
        strcpy(sys->os_version, "Unknown");
    }
}

/* ─────────────────────────────────────────────
   Serialização JSON (compatível com o dashboard)
───────────────────────────────────────────── */
static int build_json(const AllMetrics *m, char *buf, int buf_size) {
    int n = 0;
    unsigned long long ram_used_bytes = m->mem.used_mb * 1024ULL * 1024ULL;
    unsigned long long ram_total_bytes = m->mem.total_mb * 1024ULL * 1024ULL;
    unsigned long long disk_total_mb = 0;
    unsigned long long disk_used_mb = 0;
    double disk_usage = 0.0;
    unsigned long long net_rx_bytes = 0;
    unsigned long long net_tx_bytes = 0;
    unsigned long long net_rx_speed = 0;
    unsigned long long net_tx_speed = 0;

    if (m->disk_count > 0) {
        disk_used_mb = (unsigned long long)m->disks[0].read_mb;
        disk_total_mb = (unsigned long long)m->disks[0].write_mb;
        disk_usage = m->disks[0].usage_pct;
    }

    for (int i = 0; i < m->net_count; i++) {
        net_rx_bytes += m->nets[i].rx_bytes;
        net_tx_bytes += m->nets[i].tx_bytes;
        net_rx_speed += m->nets[i].rx_speed;
        net_tx_speed += m->nets[i].tx_speed;
    }

    n += snprintf(buf + n, buf_size - n,
        "{"
        "\"cpu\":{"
            "\"usage_pct\":%.1f,"
            "\"cores\":%d"
        "},",
        m->cpu.usage, m->cpu.cores);

    n += snprintf(buf + n, buf_size - n,
        "\"ram\":{"
            "\"usage_pct\":%.1f,"
            "\"used_bytes\":%llu,"
            "\"total_bytes\":%llu"
        "},",
        m->mem.usage, ram_used_bytes, ram_total_bytes);

    n += snprintf(buf + n, buf_size - n,
        "\"disk\":{"
            "\"usage_pct\":%.1f,"
            "\"free_bytes\":%llu,"
            "\"total_bytes\":%llu"
        "},",
        disk_usage,
        (disk_total_mb > disk_used_mb ? disk_total_mb - disk_used_mb : 0) * 1024ULL * 1024ULL,
        disk_total_mb * 1024ULL * 1024ULL);

    n += snprintf(buf + n, buf_size - n,
        "\"network\":{"
            "\"send_bytes_sec\":%llu,"
            "\"recv_bytes_sec\":%llu,"
            "\"sent_bytes_total\":%llu,"
            "\"recv_bytes_total\":%llu,"
            "\"adapter_count\":%d"
        "},",
        net_tx_speed, net_rx_speed, net_tx_bytes, net_rx_bytes, m->net_count);

    /* Processos */
    n += snprintf(buf + n, buf_size - n, "\"processes\":[");
    for (int i = 0; i < m->proc_count; i++) {
        const ProcInfo *p = &m->procs[i];
        if (i > 0) buf[n++] = ',';

        /* Escapar aspas no nome do processo */
        char safe_name[260];
        int si = 0, di = 0;
        while (p->name[si] && di < 255) {
            if (p->name[si] == '"') safe_name[di++] = '\\';
            safe_name[di++] = p->name[si++];
        }
        safe_name[di] = 0;

        n += snprintf(buf + n, buf_size - n,
            "{\"pid\":%lu,"
             "\"name\":\"%s\","
             "\"cpu_pct\":%.1f,"
             "\"ram_bytes\":%llu}",
            (unsigned long)p->pid, safe_name, p->cpu_pct,
            (unsigned long long)(p->mem_mb * 1024.0 * 1024.0));
    }
    n += snprintf(buf + n, buf_size - n, "],");

    /* Sistema */
    n += snprintf(buf + n, buf_size - n,
        "\"system\":{"
            "\"os\":\"%s\","
            "\"version\":\"%s\","
            "\"hostname\":\"%s\","
            "\"uptime\":%.0f,"
            "\"arch\":\"%s\""
        "}"
        "}",
        m->sys.os_name, m->sys.os_version,
        m->sys.hostname, m->sys.uptime_sec, m->sys.arch);

    return n;
}

/* ─────────────────────────────────────────────
   HTTP handler (mesma lógica do projeto Linux)
───────────────────────────────────────────── */
#define JSON_BUF_SIZE  (64 * 1024)
#define HTTP_BUF_SIZE  (JSON_BUF_SIZE + 512)

static void handle_client(SOCKET client_sock) {
    /* Ler request (basta consumir) */
    char req[2048] = {0};
    recv(client_sock, req, sizeof(req) - 1, 0);

    /* Verificar se é GET /metrics ou GET / */
    int is_metrics = (strstr(req, "GET /metrics") != NULL);
    int is_root    = (strstr(req, "GET / ")       != NULL ||
                      strstr(req, "GET /\r")      != NULL);

    if (!is_metrics && !is_root) {
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n\r\nNot Found";
        send(client_sock, not_found, (int)strlen(not_found), 0);
        closesocket(client_sock);
        return;
    }

    /* Coletar métricas */
    AllMetrics m = {0};
    collect_cpu(&m.cpu);
    collect_mem(&m.mem);
    collect_disks(m.disks, &m.disk_count);
    collect_net(m.nets, &m.net_count);
    collect_procs(m.procs, &m.proc_count);
    collect_sysinfo(&m.sys);

    /* Serializar JSON */
    char *json_buf = (char *)malloc(JSON_BUF_SIZE);
    if (!json_buf) { closesocket(client_sock); return; }

    int json_len = build_json(&m, json_buf, JSON_BUF_SIZE);

    /* Montar resposta HTTP */
    char *http_buf = (char *)malloc(HTTP_BUF_SIZE);
    if (!http_buf) { free(json_buf); closesocket(client_sock); return; }

    int http_len = snprintf(http_buf, HTTP_BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Connection: close\r\n"
        "\r\n",
        json_len);

    send(client_sock, http_buf, http_len, 0);
    send(client_sock, json_buf, json_len, 0);

    free(json_buf);
    free(http_buf);
    closesocket(client_sock);
}

/* ─────────────────────────────────────────────
   main — Winsock + loop de accept
───────────────────────────────────────────── */
int main(void) {
    /* Inicializar Winsock (equivalente a nada no Linux) */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup falhou: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() falhou: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* SO_REUSEADDR */
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(8888);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() falhou: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    if (listen(server_sock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "listen() falhou: %d\n", WSAGetLastError());
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    printf("=================================================\n");
    printf("  Monitor Windows — porta 8888\n");
    printf("  API:       http://localhost:8888/metrics\n");
    printf("  Dashboard: abra o index.html no navegador\n");
    printf("=================================================\n");

    /* Loop principal — single-thread (mesma arquitetura do Linux) */
    while (1) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        SOCKET client = accept(server_sock,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client == INVALID_SOCKET) continue;

        handle_client(client);
    }

    closesocket(server_sock);
    WSACleanup();
    return 0;
}
