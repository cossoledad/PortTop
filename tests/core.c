#include "porttop.h"
#include "text_view.h"
#include <assert.h>
#include <limits.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void check_history(void) {
    History h={0}; Process p={.pid=12345,.uid=geteuid(),.start=100,.cpu=150.,.rss=250,.cpu_valid=true};
    Snapshot s={.processes=&p,.nprocesses=1,.ncores=2,.host_cpu=70.,.host_cpu_valid=true,
        .mem_total=1000,.mem_available=400,.sampled_at=1.};
    assert(history_push(&h,&s)==0 && h.count==1);
    assert(h.points[0].cpu==70. && h.points[0].memory==60. && h.points[0].cores==2);
    const ProcessHistory *series=history_process(&h,p.pid,p.start);
    double cpu_axis; uint64_t memory_axis;
    history_scale(&h,series,&cpu_axis,&memory_axis);
    assert(cpu_axis==200. && memory_axis==32*1024*1024);
    assert(series && series->points[0].cpu==150. && series->points[0].rss==250);
    s.nprocesses=0; s.sampled_at++;
    assert(history_push(&h,&s)==0);
    series=history_process(&h,p.pid,p.start); assert(series && !series->points[1].present);
    s.nprocesses=1; p.start=200; p.cpu=25.;
    assert(history_push(&h,&s)==0);
    assert(history_process(&h,p.pid,100) && history_process(&h,p.pid,200));
    assert(!history_process(&h,p.pid,200)->points[0].present);
    for (int i=0;i<PT_HISTORY;i++) assert(history_push(&h,&s)==0);
    assert(h.count==PT_HISTORY && h.nprocesses==1 && !history_process(&h,p.pid,100));
    history_free(&h);
    Process owned={.uid=1000},other={.uid=2000};
    assert(process_visible(&owned,1000) && !process_visible(&other,1000) && !process_visible(NULL,1000));
    assert(process_visible(&owned,0) && process_visible(&other,0) && process_visible(NULL,0));
}

static void check_text_view(void) {
    TextView view={0}; char token[5001]; memset(token,'x',sizeof(token)-1); token[5000]=0;
    assert(text_view_append(&view,"[ COMMAND ]\n%s\n\nargv with spaces and 100%%%% literal\nTAIL_MARKER",token)==0);
    assert(text_view_wrap(&view,69)==0 && view.count>70);
    char *joined=malloc(view.length+1),*expected=malloc(view.length+1); assert(joined && expected);
    size_t length=0,flat=0;
    for (size_t i=0;i<view.count;i++) {
        assert(view.lines[i].length<=69);
        memcpy(joined+length,view.text+view.lines[i].start,view.lines[i].length); length+=view.lines[i].length;
    }
    for (size_t i=0;i<view.length;i++) if (view.text[i]!='\n') expected[flat++]=view.text[i];
    assert(length==flat && !memcmp(joined,expected,length)); free(joined); free(expected);
    view.scroll=25; size_t anchor=view.lines[view.scroll].start;
    assert(text_view_wrap(&view,40)==0 && view.lines[view.scroll].start<=anchor);
    assert(view.scroll+1==view.count || view.lines[view.scroll+1].start>anchor);
    text_view_move(&view,LONG_MAX,10); assert(view.scroll==view.count-10);
    text_view_move(&view,LONG_MIN,10); assert(view.scroll==0);
    assert(text_view_wrap(&view,0)<0);
    text_view_free(&view);
}

int main(void) {
    check_text_view();
    check_history();
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
    assert(first.ncores>0 && !first.host_cpu_valid);
    double deadline=monotonic_seconds()+0.15;
    volatile unsigned long work=0;
    while (monotonic_seconds()<deadline) work++;
    assert(snapshot_read(&second,&first,false)==0);
    assert(second.host_cpu_valid && second.host_cpu>=0 && second.host_cpu<=100);
    assert(second.ncores>0);
    for (size_t i=0;i<second.ncores;i++) assert(second.cores[i].usage>=0 && second.cores[i].usage<=100);
    bool measured=false;
    for (size_t i=0;i<second.nprocesses;i++) if (second.processes[i].pid==getpid()) {
        assert(second.processes[i].cpu>0 && second.processes[i].cpu_valid);
        measured=true;
    }
    assert(measured);
    snapshot_free(&first); snapshot_free(&second); close(listener);
    puts("PASS core: process stat parsing, identity checks, pidfd signals, terminal text, live per-core sampling, history ring and UID scope, wrapped details, subtree preflight and ancestor protection");
    return 0;
}
