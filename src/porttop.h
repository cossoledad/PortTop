#ifndef PORTTOP_H
#define PORTTOP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PT_VERSION "0.4.0"
#define PT_COMMAND 16384

typedef struct {
    unsigned port, remote_port, state;
    uint64_t inode;
    char protocol[5], address[64], remote[64];
} Socket;

typedef struct {
    pid_t pid, ppid, group, session;
    long tty;
    char state;
    uid_t uid;
    uint64_t start, ticks, rss;
    double cpu;
    bool truncated, cpu_valid;
    char name[256], user[64], command[PT_COMMAND];
    char cwd[4096], exe[4096];
} Process;

typedef struct { size_t socket, process; } Row; /* SIZE_MAX = unknown owner */
typedef struct {
    unsigned id;
    uint64_t ticks, idle;
    double usage;
    bool valid;
} Cpu;

typedef struct {
    Socket *sockets; size_t nsockets;
    Process *processes; size_t nprocesses;
    Row *rows; size_t nrows;
    size_t denied, unknown, unavailable_tables;
    double sampled_at, host_cpu;
    bool host_cpu_valid;
    Cpu *cores; size_t ncores;
    uint64_t host_ticks, host_idle, mem_total, mem_available;
} Snapshot;

typedef enum { SORT_PORT, SORT_PID, SORT_CPU, SORT_MEMORY } Sort;
typedef struct {
    bool all, json, once, reverse, color;
    unsigned interval_ms, port;
    Sort sort;
    char filter[256];
} Options;

int snapshot_read(Snapshot *out, const Snapshot *previous, bool all);
void snapshot_free(Snapshot *s);
int process_read(pid_t pid, Process *out);
int process_signal(pid_t pid, uint64_t start, int sig, char *message, size_t size);
void text_safe(char *text);
const char *socket_state(const Socket *s);
bool process_visible(const Process *p, uid_t viewer);
size_t *rows_select(const Snapshot *s, const Options *o, size_t *count);
void print_snapshot(const Snapshot *s, const Options *o);

/* A reviewed, finite set of process identities. No process-group signalling. */
typedef struct {
    pid_t pid, ppid;
    uint64_t start;
    char name[256], command[1024];
} ProcessTarget;
typedef struct {
    ProcessTarget *targets;
    size_t count, denied;
    bool tree;
} SignalPlan;

bool process_protected(pid_t pid);
int signal_plan_build(SignalPlan *plan, pid_t root, uint64_t start, bool tree,
                      char *message, size_t size);
int signal_plan_send(const SignalPlan *plan, int sig, char *message, size_t size);
void signal_plan_free(SignalPlan *plan);


#define PT_HISTORY 240
typedef struct {
    double cpu;
    uint64_t rss;
    bool present, cpu_valid;
} ProcessPoint;
typedef struct {
    pid_t pid;
    uint64_t start, last_seen;
    ProcessPoint points[PT_HISTORY];
} ProcessHistory;
typedef struct {
    double cpu, memory, sampled_at;
    size_t cores;
    uint64_t mem_total;
    bool cpu_valid, memory_valid;
} HostPoint;
typedef struct {
    HostPoint points[PT_HISTORY];
    ProcessHistory *processes;
    size_t nprocesses, next, count;
    uint64_t generation;
} History;
int history_push(History *h, const Snapshot *s);
const ProcessHistory *history_process(const History *h, pid_t pid, uint64_t start);
void history_free(History *h);
void history_scale(const History *h, const ProcessHistory *series, double *cpu, uint64_t *memory);

int ui_run(Options *o);
double monotonic_seconds(void);
#endif
