#include "porttop.h"
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    Process p;
    assert(prctl(PR_SET_NAME,"pt (odd) name",0,0,0)==0);
    assert(process_read(getpid(),&p)==0);
    assert(!strcmp(p.name,"pt (odd) name"));
    assert(p.ppid==getppid() && p.start>0 && p.uid==getuid());
    char msg[512];
    assert(process_signal(getpid(),p.start,SIGTERM,msg,sizeof(msg))<0);
    assert(process_signal(1,0,SIGKILL,msg,sizeof(msg))<0);
    pid_t child=fork(); assert(child>=0);
    if (!child) { for (;;) pause(); }
    assert(process_read(child,&p)==0);
    assert(process_signal(child,p.start+1,SIGKILL,msg,sizeof(msg))<0);
    assert(kill(child,0)==0);
    assert(process_signal(child,p.start,SIGTERM,msg,sizeof(msg))==0);
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM);
    assert(process_signal(child,p.start,SIGKILL,msg,sizeof(msg))<0);
    assert(process_signal(getppid(),0,SIGTERM,msg,sizeof(msg))<0);
    assert(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)==0);
    int ready[2]; assert(pipe(ready)==0);
    child=fork(); assert(child>=0);
    if (!child) {
        close(ready[0]);
        pid_t grand=fork(); assert(grand>=0);
        if (!grand) { close(ready[1]); for (;;) pause(); }
        assert(write(ready[1],&grand,sizeof(grand))==(ssize_t)sizeof(grand));
        close(ready[1]); for (;;) pause();
    }
    close(ready[1]); pid_t grand;
    assert(read(ready[0],&grand,sizeof(grand))==(ssize_t)sizeof(grand)); close(ready[0]);
    assert(process_read(child,&p)==0);
    SignalPlan plan;
    assert(signal_plan_build(&plan,child,p.start,true,msg,sizeof(msg))==0);
    assert(plan.count==2 && plan.targets[1].pid==grand);
    plan.targets[1].start++;
    assert(signal_plan_send(&plan,SIGTERM,msg,sizeof(msg))<0);
    assert(kill(child,0)==0 && kill(grand,0)==0);
    plan.targets[1].start--;
    assert(signal_plan_send(&plan,SIGTERM,msg,sizeof(msg))==0);
    assert(waitpid(child,&status,0)==child && WIFSIGNALED(status));
    assert(waitpid(grand,&status,0)==grand && WIFSIGNALED(status));
    signal_plan_free(&plan);
    assert(process_read(getppid(),&p)==0);
    assert(signal_plan_build(&plan,p.pid,p.start,true,msg,sizeof(msg))<0);
    char unsafe[]="a\033[31m\n\xff"; text_safe(unsafe);
    assert(!strcmp(unsafe,"a?[31m??"));
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(listener,(struct sockaddr *)&addr,sizeof(addr))==0);
    assert(listen(listener,1)==0);
    Snapshot first={0},second={0};
    assert(snapshot_read(&first,NULL,false)==0);
    double deadline=monotonic_seconds()+0.15;
    volatile unsigned long work=0;
    while (monotonic_seconds()<deadline) work++;
    assert(snapshot_read(&second,&first,false)==0);
    bool measured=false;
    for (size_t i=0;i<second.nprocesses;i++) if (second.processes[i].pid==getpid()) {
        assert(second.processes[i].cpu>0);
        measured=true;
    }
    assert(measured);
    snapshot_free(&first); snapshot_free(&second); close(listener);
    puts("PASS core: process stat parsing, identity checks, pidfd signals, terminal text, live CPU sampling, subtree preflight and ancestor protection");
    return 0;
}
