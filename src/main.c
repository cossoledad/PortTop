#include "porttop.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f, "PortTop %s - find the process behind a port\n"
        "Usage: porttop [options]\n\n"
        "  -a, --all              Include connected TCP/UDP sockets\n"
        "  -p, --port PORT        Exact local port (1..65535)\n"
        "  -f, --filter TEXT      Match port, PID, user, address or command\n"
        "  -i, --interval MS      Refresh interval (100..60000, default 1000)\n"
        "  -s, --sort KEY         port (default), pid, cpu, memory\n"
        "  -r, --reverse          Reverse sort order\n"
        "  -1, --once             Print a snapshot and exit\n"
        "  -j, --json             Print JSON snapshot and exit\n"
        "      --no-color         Disable terminal colors\n"
        "  -h, --help             Show help\n"
        "  -v, --version          Show version\n\n"
        "Keys: arrows move, j down, / filter, s sort, r reverse, a all sockets,\n"
        "      Space pause, c CPU core page, g graph scale, k SIGTERM, K SIGKILL (popup supports parents/subtrees), ? help, q quit.\n"
        "Default: current user only; root sees all visible owners. TCP listeners + UDP.\n", PT_VERSION);
}
static unsigned number(const char *s, unsigned low, unsigned high) {
    char *end; errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < low || n > high) {
        fprintf(stderr, "porttop: expected a number between %u and %u: %s\n", low, high, s);
        exit(2);
    }
    return (unsigned)n;
}
int main(int argc, char **argv) {
    Options o = {.interval_ms=1000, .color=true};
    static const struct option opts[] = {
        {"all",0,0,'a'}, {"port",1,0,'p'}, {"filter",1,0,'f'},
        {"interval",1,0,'i'}, {"sort",1,0,'s'}, {"reverse",0,0,'r'},
        {"once",0,0,'1'}, {"json",0,0,'j'}, {"no-color",0,0,1000},
        {"help",0,0,'h'}, {"version",0,0,'v'}, {0,0,0,0}
    };
    int c;
    while ((c=getopt_long(argc,argv,"ap:f:i:s:r1jhv",opts,NULL)) != -1) {
        switch(c) {
        case 'a': o.all=true; break;
        case 'p': o.port=number(optarg,1,65535); break;
        case 'i': o.interval_ms=number(optarg,100,60000); break;
        case 'f':
            if (strlen(optarg)>=sizeof(o.filter)) { fputs("porttop: filter too long\n",stderr); return 2; }
            snprintf(o.filter,sizeof(o.filter),"%s",optarg); text_safe(o.filter); break;
        case 's':
            if (!strcmp(optarg,"port")) o.sort=SORT_PORT;
            else if (!strcmp(optarg,"pid")) o.sort=SORT_PID;
            else if (!strcmp(optarg,"cpu")) o.sort=SORT_CPU;
            else if (!strcmp(optarg,"memory")) o.sort=SORT_MEMORY;
            else { fputs("porttop: sort must be port, pid, cpu or memory\n",stderr); return 2; }
            break;
        case 'r': o.reverse=true; break;
        case '1': o.once=true; break;
        case 'j': o.json=true; break;
        case 1000: o.color=false; break;
        case 'h': usage(stdout); return 0;
        case 'v': puts("porttop " PT_VERSION); return 0;
        default: return 2;
        }
    }
    if (optind!=argc) { usage(stderr); return 2; }
    if (getenv("NO_COLOR")) o.color=false;
    if (o.once || o.json || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        Snapshot s={0};
        if (snapshot_read(&s,NULL,o.all)<0) { perror("porttop: snapshot"); return 1; }
        print_snapshot(&s,&o); snapshot_free(&s); return ferror(stdout) ? 1 : 0;
    }
    return ui_run(&o);
}
