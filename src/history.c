#include "porttop.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const ProcessHistory *history_process(const History *h,pid_t pid,uint64_t start) {
    for (size_t i=0;i<h->nprocesses;i++) if (h->processes[i].pid==pid && h->processes[i].start==start)
        return &h->processes[i];
    return NULL;
}
int history_push(History *h,const Snapshot *s) {
    /* Grow before advancing the ring so allocation failure leaves history intact. */
    size_t needed=h->nprocesses;
    for (size_t i=0;i<s->nprocesses;i++) {
        const Process *p=&s->processes[i];
        if (process_visible(p,geteuid()) && !history_process(h,p->pid,p->start)) ++needed;
    }
    if (needed>h->nprocesses) {
        void *p=realloc(h->processes,needed*sizeof(*h->processes)); if (!p) return -1;
        h->processes=p;
    }
    ++h->generation;
    size_t slot=h->next;
    HostPoint point={.cpu=s->host_cpu,.sampled_at=s->sampled_at,.cores=s->ncores,
        .mem_total=s->mem_total,.cpu_valid=s->host_cpu_valid,
        .memory_valid=s->mem_total>0 && s->mem_available<=s->mem_total};
    if (point.memory_valid) point.memory=100.*(double)(s->mem_total-s->mem_available)/(double)s->mem_total;
    h->points[slot]=point;
    for (size_t i=0;i<h->nprocesses;i++) memset(&h->processes[i].points[slot],0,sizeof(ProcessPoint));
    for (size_t i=0;i<s->nprocesses;i++) {
        const Process *p=&s->processes[i]; if (!process_visible(p,geteuid())) continue;
        ProcessHistory *series=NULL;
        for (size_t j=0;j<h->nprocesses;j++) if (h->processes[j].pid==p->pid && h->processes[j].start==p->start) { series=&h->processes[j]; break; }
        if (!series) {
            series=&h->processes[h->nprocesses++]; memset(series,0,sizeof(*series)); series->pid=p->pid; series->start=p->start;
        }
        series->last_seen=h->generation;
        series->points[slot]=(ProcessPoint){.cpu=p->cpu,.rss=p->rss,.cpu_valid=p->cpu_valid,.present=true};
    }
    for (size_t i=0;i<h->nprocesses;) {
        if (h->generation-h->processes[i].last_seen>=PT_HISTORY) h->processes[i]=h->processes[--h->nprocesses];
        else ++i;
    }
    h->next=(slot+1)%PT_HISTORY; if (h->count<PT_HISTORY) ++h->count;
    return 0;
}
void history_free(History *h) { free(h->processes); memset(h,0,sizeof(*h)); }

void history_scale(const History *h,const ProcessHistory *series,double *cpu,uint64_t *memory) {
    *cpu=100.; *memory=32*1024*1024;
    if (!series) return;
    for (size_t i=0;i<h->count;i++) {
        size_t slot=(h->next+PT_HISTORY-h->count+i)%PT_HISTORY;
        const ProcessPoint *p=&series->points[slot]; if (!p->present) continue;
        if (p->cpu_valid) while (*cpu<p->cpu) *cpu*=2.;
        while (*memory<p->rss && *memory<=UINT64_MAX/2) *memory*=2;
        if (*memory<p->rss) *memory=UINT64_MAX;
    }
}
