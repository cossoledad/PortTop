#include "porttop.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

bool process_protected(pid_t pid) {
    if (pid<=1) return true;
    pid_t current=getpid();
    for (int depth=0;current>1 && depth<256;depth++) {
        if (current==pid) return true;
        Process p;
        if (process_read(current,&p)<0 || p.ppid==current) break;
        current=p.ppid;
    }
    return false;
}
void signal_plan_free(SignalPlan *plan) {
    free(plan->targets); memset(plan,0,sizeof(*plan));
}
static ProcessTarget target_of(const Process *p) {
    ProcessTarget t={.pid=p->pid,.ppid=p->ppid,.start=p->start};
    snprintf(t.name,sizeof(t.name),"%s",p->name);
    snprintf(t.command,sizeof(t.command),"%.*s",(int)sizeof(t.command)-1,p->command);
    return t;
}
static int add_target(ProcessTarget **list,size_t *count,ProcessTarget t) {
    if (*count>=16384) { errno=E2BIG; return -1; }
    if (*count%64==0) {
        void *p=realloc(*list,(*count+64)*sizeof(**list)); if (!p) return -1; *list=p;
    }
    (*list)[(*count)++]=t; return 0;
}
static int pid_order(const void *a,const void *b) {
    const ProcessTarget *x=a,*y=b; return (x->pid>y->pid)-(x->pid<y->pid);
}
int signal_plan_build(SignalPlan *plan,pid_t root,uint64_t start,bool tree,char *message,size_t size) {
    memset(plan,0,sizeof(*plan)); plan->tree=tree;
    Process p;
    if (process_protected(root)) { snprintf(message,size,"Protected PID: own process, ancestor, or PID 1"); return -1; }
    if (process_read(root,&p)<0 || p.start!=start) { snprintf(message,size,"Root exited or identity changed; reopen dialog"); return -1; }
    if (add_target(&plan->targets,&plan->count,target_of(&p))<0) goto fail;
    if (!tree) return 0;
    ProcessTarget *all=NULL; size_t count=0;
    DIR *dir=opendir("/proc"); if (!dir) goto fail;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        char *end; long pid=strtol(entry->d_name,&end,10);
        if (*end || pid<=1 || pid>INT32_MAX) continue;
        if (process_read((pid_t)pid,&p)<0) { if (errno==EACCES || errno==EPERM) ++plan->denied; continue; }
        if (p.state=='Z' || p.state=='X') continue;
        if (add_target(&all,&count,target_of(&p))<0) { closedir(dir); free(all); goto fail; }
    }
    closedir(dir);
    if (count>1) qsort(all,count,sizeof(*all),pid_order);
    /* Root-first breadth-first order stops launchers before their reviewed children.
       Only the captured identities are sent signals, never newly discovered PIDs. */
    for (size_t i=0;i<plan->count;i++) {
        for (size_t j=0;j<count;j++) if (all[j].ppid==plan->targets[i].pid && all[j].pid!=root &&
                                      all[j].start>=plan->targets[i].start) {
            bool duplicate=false;
            for (size_t k=0;k<plan->count;k++) if (plan->targets[k].pid==all[j].pid) duplicate=true;
            if (duplicate) continue;
            if (process_protected(all[j].pid)) {
                snprintf(message,size,"Subtree contains a protected process; no signal sent");
                free(all); signal_plan_free(plan); return -1;
            }
            if (add_target(&plan->targets,&plan->count,all[j])<0) { free(all); goto fail; }
        }
    }
    free(all);
    if (process_read(root,&p)<0 || p.start!=start) {
        snprintf(message,size,"Root changed while building preview; reopen dialog"); signal_plan_free(plan); return -1;
    }
    return 0;
fail:
    snprintf(message,size,"Cannot build signal preview: %s",strerror(errno)); signal_plan_free(plan); return -1;
}
int signal_plan_send(const SignalPlan *plan,int sig,char *message,size_t size) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    if (!plan->count || (sig!=SIGTERM && sig!=SIGKILL)) { snprintf(message,size,"Empty plan or unsupported signal"); return -1; }
    int *fds=malloc(plan->count*sizeof(*fds));
    if (!fds) { snprintf(message,size,"Cannot allocate process handles"); return -1; }
    for (size_t i=0;i<plan->count;i++) fds[i]=-1;
    size_t exited=0,sent=0,failed=0; int result=-1;
    /* Acquire and validate every handle before any mutation. A changed identity
       or a permissions failure cancels the entire operation during preflight. */
    for (size_t i=0;i<plan->count;i++) {
        const ProcessTarget *t=&plan->targets[i];
        if (process_protected(t->pid)) { snprintf(message,size,"Protected PID %d; no signal sent",t->pid); goto done; }
        fds[i]=(int)syscall(SYS_pidfd_open,t->pid,0);
        if (fds[i]<0) {
            if (errno==ESRCH && i>0) { ++exited; continue; }
            snprintf(message,size,"Cannot lock PID %d: %s; no signal sent",t->pid,strerror(errno)); goto done;
        }
        Process p;
        if (process_read(t->pid,&p)<0 || p.start!=t->start) {
            snprintf(message,size,"PID %d exited or identity changed; no signal sent",t->pid); goto done;
        }
        if (syscall(SYS_pidfd_send_signal,fds[i],0,NULL,0)<0) {
            if (errno==ESRCH && i>0) { close(fds[i]); fds[i]=-1; ++exited; continue; }
            snprintf(message,size,"Cannot signal PID %d: %s; no signal sent",t->pid,strerror(errno)); goto done;
        }
    }
    for (size_t i=0;i<plan->count;i++) if (fds[i]>=0) {
        if (syscall(SYS_pidfd_send_signal,fds[i],sig,NULL,0)==0) ++sent;
        else if (errno==ESRCH) ++exited; else ++failed;
    }
    if (!plan->tree && sent==1)
        snprintf(message,size,"%s sent to PID %d (exit not yet confirmed)",sig==SIGTERM?"SIGTERM":"SIGKILL",plan->targets[0].pid);
    else snprintf(message,size,"%s subtree: %zu sent, %zu exited, %zu failed (exit not yet confirmed)",sig==SIGTERM?"SIGTERM":"SIGKILL",sent,exited,failed);
    result=failed?-1:0;
done:
    for (size_t i=0;i<plan->count;i++) if (fds[i]>=0) close(fds[i]);
    free(fds); return result;
#else
    (void)plan; (void)sig; snprintf(message,size,"This build lacks pidfd support; no signal sent"); return -1;
#endif
}
