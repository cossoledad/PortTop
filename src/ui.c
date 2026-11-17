#include "porttop.h"
#include "text_view.h"
#include <ncurses.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_CHAIN 12
static struct termios saved;
static SCREEN *screen;
static bool active,colors,unicode,dimmed;
static volatile sig_atomic_t stopping,suspending,resized;
static int width,height;

typedef struct {
    bool open,tree,confirm;
    int sig;
    Process chain[MAX_CHAIN];
    size_t length,target,preview;
    unsigned port;
    SignalPlan plan;
    char error[512];
    WINDOW *window;
} Dialog;
static void handler(int sig) {
    if (sig==SIGTSTP) suspending=1;
    else if (sig==SIGWINCH) resized=1;
    else stopping=1;
}
static void restore(void) {
    if (!active) return;
    endwin(); tcsetattr(STDIN_FILENO,TCSAFLUSH,&saved); active=false;
}
static attr_t style(int color) {
    attr_t a=colors?COLOR_PAIR(color):A_NORMAL;
    if (color==1 || color==5 || color==16) a|=A_BOLD;
    if (dimmed || (!colors && color>=17)) a|=A_DIM;
    if ((color==5 || color==13 || color==16) && !colors) a|=A_REVERSE;
    return a;
}
static void put(WINDOW *w,int y,int x,int max,int color,const char *fmt,...)
    __attribute__((format(printf,6,7)));
static void put(WINDOW *w,int y,int x,int max,int color,const char *fmt,...) {
    int h,b; getmaxyx(w,h,b);
    if (y<0 || y>=h || x<0 || x>=b || max<=0) return;
    if (max>b-x) max=b-x;
    char text[32768]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    wattrset(w,(int)style(color)); mvwaddnstr(w,y,x,text,max); wattrset(w,A_NORMAL);
}
static void glyph(WINDOW *w,int y,int x,wchar_t ch,int color) {
    if (y<0 || y>=getmaxy(w) || x<0 || x>=getmaxx(w)) return;
    wchar_t text[2]={ch,0}; cchar_t cell;
    setcchar(&cell,text,style(color)&~A_COLOR,(short)(colors?color:0),NULL);
    mvwadd_wch(w,y,x,&cell);
}
static void frame(WINDOW *w,int y,int x,int rows,int cols,const char *title,int color) {
    if (rows<2 || cols<2) return;
    wchar_t hz=unicode?L'─':L'-',vt=unicode?L'│':L'|';
    for (int j=1;j<cols-1;j++) { glyph(w,y,x+j,hz,color); glyph(w,y+rows-1,x+j,hz,color); }
    for (int i=1;i<rows-1;i++) { glyph(w,y+i,x,vt,color); glyph(w,y+i,x+cols-1,vt,color); }
    glyph(w,y,x,unicode?L'╭':L'+',color); glyph(w,y,x+cols-1,unicode?L'╮':L'+',color);
    glyph(w,y+rows-1,x,unicode?L'╰':L'+',color); glyph(w,y+rows-1,x+cols-1,unicode?L'╯':L'+',color);
    put(w,y,x+2,cols-4,color," %s ",title);
}
static void clipped(char *out,size_t size,const char *text,int cols) {
    if (cols<1 || !size) { if (size) *out=0; return; }
    size_t n=strlen(text),limit=(size_t)cols;
    if (limit>=size) limit=size-1;
    if (n<=limit) snprintf(out,size,"%s",text);
    else if (limit<7) snprintf(out,size,"%.*s",(int)limit,text);
    else {
        size_t left=(limit-3)/2,right=limit-3-left;
        snprintf(out,size,"%.*s...%s",(int)left,text,text+n-right);
    }
}
static void field(int y,const char *label,const char *value,int color) {
    char text[8192]; clipped(text,sizeof(text),value,width-15);
    put(stdscr,y,3,8,2,"%s",label); put(stdscr,y,11,width-14,color,"%s",text);
}
static const char *sort_name(Sort sort) {
    static const char *names[]={"port","pid","cpu","memory"}; return names[sort];
}
static size_t read_chain(const Process *owner,Process *chain) {
    size_t n=1; chain[0]=*owner;
    pid_t parent=owner->ppid;
    while (parent>1 && n<MAX_CHAIN) {
        if (process_protected(parent)) break;
        bool seen=false; for (size_t i=0;i<n;i++) if (chain[i].pid==parent) seen=true;
        if (seen || process_read(parent,&chain[n])<0) break;
        if (!process_visible(&chain[n],geteuid())) break;
        if (chain[n].tty!=0 && chain[n].pid==chain[n].session) break;
        /* Parent could have exited and its PID been reused. */
        if (chain[n].start>chain[n-1].start) break;
        parent=chain[n].ppid; ++n;
    }
    return n;
}
static void dialog_close(Dialog *d) {
    signal_plan_free(&d->plan);
    if (d->window) delwin(d->window);
    d->window=NULL; d->open=false;
}
static void dialog_plan(Dialog *d) {
    signal_plan_free(&d->plan); d->preview=0; d->confirm=false; d->error[0]=0;
    Process *p=&d->chain[d->target];
    (void)signal_plan_build(&d->plan,p->pid,p->start,d->tree,d->error,sizeof(d->error));
}
static void port_list(const Snapshot *s,pid_t pid,char *out,size_t size) {
    size_t used=0; out[0]=0;
    for (size_t i=0;i<s->nrows;i++) {
        const Row *r=&s->rows[i]; if (r->process==SIZE_MAX || s->processes[r->process].pid!=pid) continue;
        if (used>=size-1) break;
        int n=snprintf(out+used,size-used,"%s%u/%s",used?",":"",s->sockets[r->socket].port,s->sockets[r->socket].protocol);
        if (n<0 || (size_t)n>=size-used) break;
        used+=(size_t)n;
    }
    if (!used) snprintf(out,size,"no visible ports");
}
static void dialog_preview_move(Dialog *d,long delta) {
    size_t visible=d->window?(size_t)(getmaxy(d->window)-17):1;
    size_t max=d->plan.count>visible?d->plan.count-visible:0;
    if (d->preview>max) d->preview=max;
    if (delta<0) d->preview=(size_t)(-delta)>d->preview?0:d->preview-(size_t)(-delta);
    else d->preview=(size_t)delta>max-d->preview?max:d->preview+(size_t)delta;
}
static void draw_dialog(Dialog *d,const Snapshot *s) {
    int h=height-2; if (h>26) h=26;
    int w=width-6; if (w>112) w=112;
    int top=(height-h)/2,left=(width-w)/2;
    if (d->window && (getmaxy(d->window)!=h || getmaxx(d->window)!=w ||
                      getbegy(d->window)!=top || getbegx(d->window)!=left)) {
        delwin(d->window); d->window=NULL;
    }
    if (!d->window) d->window=newwin(h,w,top,left);
    WINDOW *win=d->window; if (!win) return;
    dialog_preview_move(d,0);
    werase(win); frame(win,0,0,h,w,"TERMINATE PROCESS",9);
    const Process *target=&d->chain[d->target];
    put(win,1,3,w-6,4,"%s  | origin port %u | target PID %d",d->sig==SIGTERM?"SIGTERM":"SIGKILL",d->port,target->pid);
    put(win,2,3,w-6,2,"Launch chain: Up selects parent, Down selects child");
    size_t last=d->target+2;
    if (last<4) last=4;
    if (last>d->length) last=d->length;
    size_t first=last>4?last-4:0;
    for (size_t j=0;j<last-first;j++) {
        size_t i=last-1-j; const Process *p=&d->chain[i];
        put(win,3+(int)j,3,w-6,i==d->target?5:0,"%c %7d %-18.18s %.*s",i==d->target?'>':' ',p->pid,p->name,w-36,p->command);
    }
    size_t chunk=(size_t)(w-6),len=strlen(target->command);
    put(win,7,3,w-6,2,"COMMAND preview%s | i: full process details",target->truncated?" (truncated)":"");
    for (size_t i=0;i<2;i++) if (i*chunk<len)
        put(win,8+(int)i,3,w-6,0,"%.*s",(int)chunk,target->command+i*chunk);
    put(win,10,3,w-6,3,"Scope: %s   [s switches scope]",d->tree?"[ Subtree ]   Process only":"[ Process only ]   Subtree");
    size_t affected=0;
    for (size_t i=0;i<s->nrows;i++) {
        const Row *r=&s->rows[i]; if (r->process==SIZE_MAX) continue;
        for (size_t j=0;j<d->plan.count;j++) if (s->processes[r->process].pid==d->plan.targets[j].pid &&
            s->processes[r->process].start==d->plan.targets[j].start) { ++affected; break; }
    }
    put(win,11,3,w-6,1,"Affected: %zu processes, %zu visible socket owners | wheel/PgUp/PgDn",d->plan.count,affected);
    int visible=h-17; if (visible<1) visible=1;
    for (int j=0;j<visible;j++) {
        size_t i=d->preview+(size_t)j; if (i>=d->plan.count) break;
        const ProcessTarget *p=&d->plan.targets[i]; char ports[256]; port_list(s,p->pid,ports,sizeof(ports));
        put(win,12+j,3,w-6,0,"%7d %-18.18s ports: %s",p->pid,p->name,ports);
    }
    if (*d->error) put(win,h-5,3,w-6,4,"%s",d->error);
    else if (d->plan.denied) put(win,h-5,3,w-6,4,"%zu unreadable process records; subtree preview may be incomplete",d->plan.denied);
    else put(win,h-5,3,w-6,4,"%s",d->tree?"Includes descendants and their other ports. Reviewed PIDs only.":"A supervisor may restart it. Select a parent + Subtree to stop its services.");
    put(win,h-4,3,w-6,2,"Tab/Left/Right: button | Enter: activate | y: confirm | Esc/n: cancel");
    put(win,h-2,w/2-18,16,d->confirm?0:5,"  [ Cancel ]  ");
    put(win,h-2,w/2+2,18,d->confirm?16:12," [ Terminate ] ");
    /* Overlay the complete virtual popup, then commit the whole frame once. */
    touchwin(win); wnoutrefresh(win);
}
typedef struct {
    bool open;
    pid_t pid;
    char name[256];
    TextView content;
    WINDOW *window;
} InfoDialog;
static void info_close(InfoDialog *info) {
    if (info->window) delwin(info->window);
    text_view_free(&info->content); memset(info,0,sizeof(*info));
}
static int info_open(InfoDialog *info,const Process *p,const Snapshot *s,const Process *chain,size_t nchain) {
    info_close(info);
    TextView *v=&info->content;
    char cpu[32]="--"; if (p->cpu_valid) snprintf(cpu,sizeof(cpu),"%.1f%%",p->cpu);
    if (text_view_append(v,"[ PROCESS ]\nPID %d   PPID %d   USER %s (UID %u)\nCPU %s (one core = 100%%)   RSS %.2f MiB   State %c\n\n[ COMMAND ]\n%s\n%s\n[ WORKING DIRECTORY ]\n%s\n\n[ EXECUTABLE ]\n%s\n\n[ IDENTITY ]\nProcess group %d   Session %d   TTY %ld\nStart ticks %" PRIu64 "\n\n[ PORTS / current network namespace ]\n",
        p->pid,p->ppid,p->user,p->uid,cpu,(double)p->rss/1048576.,p->state,p->command,
        p->truncated?"[command truncated at 16 KiB]\n":"",p->cwd,p->exe,p->group,p->session,p->tty,p->start)<0) goto fail;
    size_t ports=0;
    for (size_t i=0;i<s->nrows;i++) {
        const Row *r=&s->rows[i]; if (r->process==SIZE_MAX) continue;
        const Process *owner=&s->processes[r->process];
        if (owner->pid!=p->pid || owner->start!=p->start) continue;
        const Socket *sock=&s->sockets[r->socket]; ++ports;
        if (text_view_append(v,"%s  %s:%u -> %s:%u  %s  inode %" PRIu64 "\n",sock->protocol,sock->address,sock->port,sock->remote,sock->remote_port,socket_state(sock),sock->inode)<0) goto fail;
    }
    if (!ports && text_view_append(v,"No visible ports in the current socket mode.\n")<0) goto fail;
    if (text_view_append(v,"\n[ LAUNCH CHAIN ]\n")<0) goto fail;
    for (size_t i=0;i<nchain;i++)
        if (text_view_append(v,"PID %d (%s), PPID %d\n%s\n\n",chain[i].pid,chain[i].name,chain[i].ppid,chain[i].command)<0) goto fail;
    if (text_view_append(v,"[ END OF DETAILS ]\nRead-only snapshot. Close to resume the previous view.\n")<0) goto fail;
    info->pid=p->pid; snprintf(info->name,sizeof(info->name),"%s",p->name); info->open=true; return 0;
fail:
    info_close(info); return -1;
}
static void scroll_bar(WINDOW *win,int top,int left,int rows,size_t position,size_t count,size_t visible) {
    if (count<=visible || rows<1) return;
    int thumb=(int)((size_t)rows*visible/count); if (thumb<1) thumb=1;
    size_t max=count-visible;
    int first=(int)((size_t)(rows-thumb)*position/max);
    for (int i=0;i<rows;i++) glyph(win,top+i,left,unicode?(i>=first && i<first+thumb?L'┃':L'│'):(i>=first && i<first+thumb?L'#':L'|'),i>=first && i<first+thumb?1:2);
}
static void draw_info(InfoDialog *info) {
    int h=height-2; if (h>28) h=28;
    int w=width-4; if (w>110) w=110;
    int top=(height-h)/2,left=(width-w)/2;
    if (info->window && (getmaxy(info->window)!=h || getmaxx(info->window)!=w ||
        getbegy(info->window)!=top || getbegx(info->window)!=left)) { delwin(info->window); info->window=NULL; }
    if (!info->window) info->window=newwin(h,w,top,left);
    WINDOW *win=info->window; if (!win) return;
    werase(win); frame(win,0,0,h,w,"PROCESS INFORMATION",10);
    put(win,1,3,w-6,2,"PID %d | %s | read-only snapshot",info->pid,info->name);
    TextView *v=&info->content;
    if (text_view_wrap(v,(size_t)(w-7))<0) {
        put(win,3,3,w-6,4,"Unable to wrap details: %s",strerror(errno));
    } else {
        size_t visible=(size_t)(h-6); text_view_move(v,0,visible);
        for (size_t j=0;j<visible && v->scroll+j<v->count;j++) {
            const TextLine *line=&v->lines[v->scroll+j];
            bool heading=v->text[line->start]=='[' && (line->start==0 || v->text[line->start-1]=='\n');
            put(win,3+(int)j,3,w-7,heading?1:0,"%.*s",(int)line->length,v->text+line->start);
        }
        size_t last=v->scroll+visible<v->count?v->scroll+visible:v->count;
        char progress[80]; snprintf(progress,sizeof(progress)," %zu-%zu / %zu ",v->count?v->scroll+1:0,last,v->count);
        put(win,0,w-(int)strlen(progress)-3,(int)strlen(progress),10,"%s",progress);
        scroll_bar(win,3,w-2,h-6,v->scroll,v->count,visible);
    }
    put(win,h-2,3,w-6,2,"wheel/arrows: scroll  PgUp/Dn: page  Home/End  Esc: close");
    touchwin(win); wnoutrefresh(win);
}
static bool mouse_inside(const MEVENT *event,WINDOW *win) {
    return win && event->x>=getbegx(win) && event->x<getbegx(win)+getmaxx(win) &&
        event->y>=getbegy(win) && event->y<getbegy(win)+getmaxy(win);
}
static int wheel_delta(const MEVENT *event) {
    if (event->bstate&BUTTON4_PRESSED) return -3;
#ifdef BUTTON5_PRESSED
    if (event->bstate&BUTTON5_PRESSED) return 3;
#endif
    return 0;
}
/* All panel geometry depends only on terminal dimensions, never live data. */
static int metrics_height(void) { return height>=32?10:7; }
static int graph_height(void) { return metrics_height()-5; }
static int cpu_width(void) { return width*2/3; }
static int core_columns(void) {
    int n=(cpu_width()-cpu_width()/2-3)/11; return n>0?n:1;
}
static int table_begin(void) { return 1+metrics_height()+2; }
static int detail_begin(void) { return height-2-(height>=32?9:6); }
static int detail_command_width(void) { return width>15?width-14:1; }
static size_t cores_per_page(void) { return (size_t)(core_columns()*graph_height()); }
static double bounded(double v) { return v<0?0:(v>100?100:v); }
static void graph_values(const History *h,const ProcessHistory *series,size_t slot,bool memory,
                         bool shared_scale,double cpu_axis,uint64_t memory_axis,
                         double *host,double *process,bool *host_valid,bool *process_valid) {
    const HostPoint *point=&h->points[slot];
    *host=bounded(memory?point->memory:point->cpu);
    *host_valid=memory?point->memory_valid:point->cpu_valid;
    *process=0; *process_valid=false;
    if (!series || !series->points[slot].present) return;
    const ProcessPoint *p=&series->points[slot];
    *process_valid=memory?(!shared_scale || point->memory_valid):(p->cpu_valid && (!shared_scale || point->cores>0));
    if (!*process_valid) return;
    if (memory) *process=100.*(double)p->rss/(double)(shared_scale?point->mem_total:memory_axis);
    else *process=shared_scale?p->cpu/(double)point->cores:100.*p->cpu/cpu_axis;
    *process=bounded(*process);
}
static size_t graph_points(const History *h,int cols) {
    size_t capacity=(size_t)(cols>4?cols-4:1)*2;
    return h->count<capacity?h->count:capacity;
}
static double graph_span(const History *h,int cols) {
    size_t n=graph_points(h,cols); if (n<2) return 0;
    return h->points[(h->next+PT_HISTORY-1)%PT_HISTORY].sampled_at-
        h->points[(h->next+PT_HISTORY-n)%PT_HISTORY].sampled_at;
}
static void graph(const History *h,const ProcessHistory *series,bool memory,int x,int cols,
                  bool shared_scale,double cpu_axis,uint64_t memory_axis) {
    int rows=graph_height(),pixels=rows*4;
    int plot_cols=cols-4; if (plot_cols<1) return;
    size_t n=graph_points(h,cols),first=(size_t)plot_cols*2-n;
    static const unsigned bits_for[2][4]={{1,2,4,64},{8,16,32,128}};
    static const char shades[]=" .:-=+#@";
    put(stdscr,3,x,3,2,"100"); put(stdscr,3+rows-1,x,3,2,"  0");
    if (rows>=4) put(stdscr,3+rows/2,x,3,2," 50");
    for (int col=0;col<plot_cols;col++) {
        unsigned dots[5]={0},host_dots[5]={0}; bool trace[5]={0},edge[5]={0};
        for (int half=0;half<2;half++) {
            size_t pixel=(size_t)col*2+(size_t)half;
            if (pixel<first) continue;
            size_t index=pixel-first,slot=(h->next+PT_HISTORY-n+index)%PT_HISTORY;
            double host,process; bool host_valid,process_valid;
            graph_values(h,series,slot,memory,shared_scale,cpu_axis,memory_axis,&host,&process,&host_valid,&process_valid);
            int fill=host_valid?(int)(host*(double)pixels/100.+0.5):0;
            if (host_valid && host>0 && !fill) fill=1;
            int level=(int)(process*(double)(pixels-1)/100.+0.5),low=level,high=level;
            if (process_valid && index>0) {
                double old_host,old_process; bool old_host_valid,old_valid;
                graph_values(h,series,(slot+PT_HISTORY-1)%PT_HISTORY,memory,shared_scale,cpu_axis,memory_axis,
                    &old_host,&old_process,&old_host_valid,&old_valid);
                if (old_valid) {
                    int previous=(int)(old_process*(double)(pixels-1)/100.+0.5);
                    if (previous<low) low=previous;
                    if (previous>high) high=previous;
                }
            }
            for (int row=0;row<rows;row++) for (int dot=0;dot<4;dot++) {
                int y=(rows-1-row)*4+3-dot; unsigned bit=bits_for[half][dot];
                if (y<fill) { dots[row]|=bit; host_dots[row]|=bit; if (y==fill-1) edge[row]=true; }
                /* Join adjacent measured points; missing samples remain a gap. */
                if (process_valid && y>=low && y<=high) { dots[row]|=bit; trace[row]=true; }
            }
        }
        for (int row=0;row<rows;row++) {
            int color=trace[row]?6:(edge[row]?(memory?3:1):(memory?18:17));
            if (dots[row]) {
                if (unicode) glyph(stdscr,3+row,x+4+col,(wchar_t)(0x2800+dots[row]),color);
                else {
                    unsigned value=host_dots[row],count=0; while (value) { count+=value&1u; value>>=1; }
                    put(stdscr,3+row,x+4+col,1,color,"%c",trace[row]?'*':shades[count<8?count:7]);
                }
            } else if ((row==0 || row==rows-1 || (rows>=4 && row==rows/2)) && col%3==0)
                glyph(stdscr,3+row,x+4+col,unicode?L'·':L'.',19);
        }
    }
}
static void draw_metrics(const Snapshot *s,const History *history,const Process *p,size_t core_page,bool shared_scale) {
    int split=cpu_width(),rows=metrics_height(),chart_cols=split/2-3;
    frame(stdscr,1,1,rows,split-1,"CPU HISTORY",7);
    frame(stdscr,1,split+1,rows,width-split-2,"MEM HISTORY",8);
    char proc_cpu[32]="--",proc_mem[32]="--";
    if (p && p->cpu_valid) snprintf(proc_cpu,sizeof(proc_cpu),"%.1f%%",p->cpu);
    if (p) snprintf(proc_mem,sizeof(proc_mem),"%.1f MiB",(double)p->rss/1048576.);
    put(stdscr,2,3,chart_cols,0,"TOTAL %5.1f%% | PID %s",s->host_cpu,proc_cpu);
    uint64_t used=s->mem_available<=s->mem_total?s->mem_total-s->mem_available:0;
    double mem=s->mem_total?100.*(double)used/(double)s->mem_total:0;
    put(stdscr,1,width-11,8,3," %5.1f%% ",mem);
    put(stdscr,2,split+3,width-split-6,0,"%.2f / %.2f GiB",(double)used/1073741824.,(double)s->mem_total/1073741824.);
    const ProcessHistory *series=p?history_process(history,p->pid,p->start):NULL;
    double cpu_axis; uint64_t memory_axis;
    history_scale(history,series,&cpu_axis,&memory_axis);
    put(stdscr,1,split-15,11,7," %.0fs view ",graph_span(history,chart_cols));
    graph(history,series,false,3,chart_cols,shared_scale,cpu_axis,memory_axis);
    graph(history,series,true,split+3,width-split-6,shared_scale,cpu_axis,memory_axis);
    size_t per_page=cores_per_page(),pages=(s->ncores+per_page-1)/per_page;
    size_t first=(pages?core_page%pages:0)*per_page;
    int columns=core_columns(),core_x=split/2+2;
    put(stdscr,2,core_x,split-core_x-2,2,"CORES %zu-%zu/%zu [c]",s->ncores?first+1:0,
        first+per_page<s->ncores?first+per_page:s->ncores,s->ncores);
    for (size_t j=0;j<per_page && first+j<s->ncores;j++) {
        const Cpu *cpu=&s->cores[first+j];
        int y=3+(int)(j/(size_t)columns),x=core_x+(int)(j%(size_t)columns)*11;
        if (cpu->valid) put(stdscr,y,x,10,cpu->usage>=75?4:3,"c%02u %5.1f%%",cpu->id,cpu->usage);
        else put(stdscr,y,x,10,2,"c%02u    --%%",cpu->id);
    }
    int legend=rows-2;
    if (p) put(stdscr,legend,3,split-5,6,"* PID %d   %s",p->pid,p->name);
    else put(stdscr,legend,3,split-5,2,"system average / 0-100%%");
    if (p && shared_scale) put(stdscr,legend+1,3,split-5,2,"CPU scaled to %zu cores [g]",s->ncores);
    else if (p) put(stdscr,legend+1,3,split-5,2,"axes: CPU %.0f%%  [g] independent",cpu_axis);
    else put(stdscr,legend+1,3,split-5,2,"2 samples/column");
    put(stdscr,legend,split+3,width-split-6,6,"PID %s | %.0fs",proc_mem,graph_span(history,width-split-6));
    if (p && !shared_scale) put(stdscr,legend+1,split+3,width-split-6,2,"RSS axis %.0f MiB",(double)memory_axis/1048576.);
    else put(stdscr,legend+1,split+3,width-split-6,2,"RSS / total RAM");
}
typedef struct { int port,proto,address,state,pid,user,cpu,memory,command,addr_width; bool show_user; } Columns;
static Columns table_columns(void) {
    Columns c={.port=3,.show_user=width>=112};
    c.proto=c.port+6; c.address=c.proto+6; c.addr_width=width>=120?25:(width>=100?18:14);
    c.state=c.address+c.addr_width+1; c.pid=c.state+(width>=100?11:8);
    c.user=c.pid+8; c.cpu=c.user+(c.show_user?10:0); c.memory=c.cpu+7; c.command=c.memory+9;
    return c;
}
static const char *short_command(const Process *p) {
    const char *start=p->command;
    for (const char *s=p->command;*s && *s!=' ';s++) if (*s=='/') start=s+1;
    return start;
}
static void draw_table(const Snapshot *s,const Options *o,const size_t *list,size_t count,size_t selected,size_t *offset) {
    int body=table_begin(),top=body-2,bottom=detail_begin(),visible=bottom-body-1;
    if (visible<1) visible=1;
    frame(stdscr,top,1,bottom-top,width-2,"PORTS",9);
    char title[160]; snprintf(title,sizeof(title)," %s | %s %s | %zu %s ",o->all?"all sockets":"listeners + UDP",sort_name(o->sort),o->reverse?"rev":"asc",count,count==1?"row":"rows");
    put(stdscr,top,width-(int)strlen(title)-4,width-14,9,"%s",title);
    Columns c=table_columns();
    put(stdscr,body-1,2,width-4,11,"%*s",width-4,"");
    put(stdscr,body-1,c.port,6,11,"PORT"); put(stdscr,body-1,c.proto,6,11,"PROTO");
    put(stdscr,body-1,c.address,c.addr_width,11,"LOCAL ADDRESS"); put(stdscr,body-1,c.state,c.pid-c.state-1,11,"STATE");
    put(stdscr,body-1,c.pid,7,11,"%7s","PID");
    if (c.show_user) put(stdscr,body-1,c.user,9,11,"USER");
    put(stdscr,body-1,c.cpu,6,11,"%6s","CPU%"); put(stdscr,body-1,c.memory,8,11,"%8s","RSS MiB");
    put(stdscr,body-1,c.command,width-c.command-3,11,"COMMAND");
    if (selected<*offset) *offset=selected;
    if (selected>=*offset+(size_t)visible) *offset=selected-(size_t)visible+1;
    for (int j=0;j<visible;j++) {
        size_t i=*offset+(size_t)j; if (i>=count) break;
        const Row *r=&s->rows[list[i]]; const Socket *sock=&s->sockets[r->socket];
        const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
        int y=body+j; bool chosen=i==selected; int text=chosen?5:0;
        if (chosen) put(stdscr,y,2,width-4,5,"%*s",width-4,"");
        put(stdscr,y,2,7,chosen?13:1,"%c%5u",chosen?'>':' ',sock->port);
        put(stdscr,y,c.proto,5,chosen?5:2,"%s",sock->protocol);
        char address[80]; clipped(address,sizeof(address),sock->address,c.addr_width);
        put(stdscr,y,c.address,c.addr_width,text,"%s",address);
        put(stdscr,y,c.state,c.pid-c.state-1,chosen?5:(sock->state==10 || sock->state==7?3:2),"%s",socket_state(sock));
        char pid[24]="-"; if (p) snprintf(pid,sizeof(pid),"%d",p->pid);
        put(stdscr,y,c.pid,7,text,"%7s",pid);
        if (c.show_user) put(stdscr,y,c.user,9,chosen?5:2,"%s",p?p->user:"?");
        put(stdscr,y,c.cpu,6,chosen?5:(p && p->cpu>=50?4:2),"%6.1f",p?p->cpu:0.);
        put(stdscr,y,c.memory,8,text,"%8.1f",p?(double)p->rss/1048576.:0.);
        put(stdscr,y,c.command,width-c.command-3,text,"%s",p?short_command(p):"(owner unavailable)");
    }
    if (!count) put(stdscr,body,4,width-8,2,"No matching ports. / edit filter | a toggle all sockets");
    put(stdscr,bottom-1,width-21,16,9," %zu/%zu ",count?selected+1:0,count);
}
static void draw_details(const Snapshot *s,const size_t *list,size_t count,size_t selected,
                         const Process *chain,size_t nchain) {
    int top=detail_begin(),rows=height-2-top;
    frame(stdscr,top,1,rows,width-2,"PROCESS DETAILS",10);
    put(stdscr,top,width-22,18,10," Enter/i: inspect ");
    if (!count) { put(stdscr,top+2,4,width-8,2,"Select a port to inspect its process and launch chain."); return; }
    const Row *r=&s->rows[list[selected]]; const Socket *sock=&s->sockets[r->socket];
    const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
    if (!p) { put(stdscr,top+2,4,width-8,4,"Owner unavailable: permissions, closing socket, or outside visible PID namespace."); return; }
    if (p->truncated) put(stdscr,top,width-31,27,4," CMD truncated | i: full ");
    put(stdscr,top+1,3,width-6,0,"PID %d  PPID %d  USER %s  CPU %.1f%%  RSS %.1f MiB  PORT %u/%s",p->pid,p->ppid,p->user,p->cpu,(double)p->rss/1048576.,sock->port,sock->protocol);
    int cmd=top+3,lines=1;
    if (rows>=9) {
        put(stdscr,top+2,3,width-6,3,"%s %s:%u -> %s:%u | %s | inode %" PRIu64,sock->protocol,sock->address,sock->port,sock->remote,sock->remote_port,socket_state(sock),sock->inode);
        field(top+3,"CWD",p->cwd,0); field(top+4,"EXE",p->exe,2); cmd=top+5; lines=2;
    } else field(top+2,"CWD",p->cwd,0);
    size_t len=strlen(p->command),chunk=(size_t)detail_command_width(),start=0;
    put(stdscr,cmd,3,8,2,"CMD");
    for (int j=0;j<lines;j++) if (start+(size_t)j*chunk<len)
        put(stdscr,cmd+j,11,(int)chunk,0,"%.*s",(int)chunk,p->command+start+(size_t)j*chunk);
    char parents[4096]; snprintf(parents,sizeof(parents),"PARENTS %d",p->pid);
    for (size_t j=1;j<nchain;j++) {
        size_t used=strlen(parents); snprintf(parents+used,sizeof(parents)-used," <- %d (%s)",chain[j].pid,chain[j].name);
    }
    char summary[4096]; clipped(summary,sizeof(summary),parents,width-6);
    put(stdscr,top+rows-2,3,width-6,2,"%s",summary);
}
static void shortcut(int *x,const char *key,const char *label,int color) {
    if (!key || !label) return;
    int len=(int)(strlen(key)+strlen(label)+3);
    if (*x+len>width-1) return;
    put(stdscr,height-1,*x,(int)strlen(key)+2,color," %s ",key); *x+=(int)strlen(key)+2;
    put(stdscr,height-1,*x,(int)strlen(label)+1,2,"%s ",label); *x+=(int)strlen(label)+1;
}
static void draw_help(void) {
    int w=width-6; if (w>104) w=104;
    int h=20,top=(height-h)/2,left=(width-w)/2;
    for (int i=0;i<h;i++) put(stdscr,top+i,left,w,0,"%*s",w,"");
    frame(stdscr,top,left,h,w,"KEYBOARD HELP",10);
    const char *lines[]={
        "Up/Down or j     Select a port (j moves down; k terminates)",
        "PgUp/PgDn        Move one page; Home/End jump to first/last",
        "/                Filter by port, PID, user, address or command",
        "Enter / Esc      Apply / cancel filter; Esc clears an applied filter",
        "s / r            Cycle sort (port/PID/CPU/memory) / reverse",
        "a                Toggle listeners + unconnected UDP / all sockets",
        "Space / R / c    Pause sampling / refresh now / next CPU core page",
        "Enter / i / g    Process details / process axes vs host axes",
        "k / K            SIGTERM / SIGKILL confirmation popup",
        "Popup Up/Down    Choose parent/child in launch chain",
        "Popup s / i      Process/Subtree scope / full target details",
        "Popup Tab/Enter  Select Cancel/Terminate (default Cancel) and activate",
        "Popup y / Esc    Confirm / cancel",
        "? / q            Close help / quit",
        "CPU: 100% = one core. Only current network namespace."};
    for (size_t i=0;i<sizeof(lines)/sizeof(*lines);i++) put(stdscr,top+2+(int)i,left+3,w-6,0,"%s",lines[i]);
}
static void draw(const Snapshot *s,const Options *o,const size_t *list,size_t count,size_t selected,
                 size_t *offset,bool paused,bool filtering,bool help,Dialog *dialog,
                 const Process *chain,size_t nchain,const char *message,
                 const History *history,size_t core_page,bool shared_scale,InfoDialog *info) {
    getmaxyx(stdscr,height,width); erase();
    if (width<80 || height<24) {
        put(stdscr,0,0,width,1,"PortTop | terminal too small");
        put(stdscr,2,0,width,0,"Resize to at least 80 columns x 24 rows. q: quit");
        wnoutrefresh(stdscr); doupdate(); return;
    }
    dimmed=dialog->open || info->open || help;
    put(stdscr,0,2,8,1,"PORTTOP"); put(stdscr,0,11,9,2,"v%s",PT_VERSION);
    if (width>=110) put(stdscr,0,23,width-65,2,"ports first, processes in context");
    char status[128]; snprintf(status,sizeof(status)," %s | %ums | %s ",geteuid()==0?"all users":"current user",o->interval_ms,paused?"PAUSED":"LIVE");
    put(stdscr,0,width-(int)strlen(status)-2,(int)strlen(status),2,"%s",status);
    put(stdscr,0,width-(paused?9:7),paused?6:4,paused?4:3,"%s",paused?"PAUSED":"LIVE");
    const Process *picked=NULL;
    if (count && s->rows[list[selected]].process!=SIZE_MAX) picked=&s->processes[s->rows[list[selected]].process];
    draw_metrics(s,history,picked,core_page,shared_scale);
    draw_table(s,o,list,count,selected,offset); draw_details(s,list,count,selected,chain,nchain);
    if (*message && !filtering) put(stdscr,height-2,2,width-4,4,"%s",message);
    else put(stdscr,height-2,2,width-4,filtering?3:2,"%s%s",filtering?"FILTER > ":"Filter: ",o->filter);
    int x=1;
    shortcut(&x,"arrows","move",1); shortcut(&x,"/","filter",1); shortcut(&x,"i","details",1); shortcut(&x,"k","terminate",12);
    shortcut(&x,"K","kill",12); shortcut(&x,"?","help",1); shortcut(&x,"q","quit",1);
    if (width>=110) { shortcut(&x,"g","scale",1); shortcut(&x,"c","cores",1); shortcut(&x,"Space","pause",1); }
    dimmed=false;
    if (help) draw_help();
    wnoutrefresh(stdscr);
    if (dialog->open) { dimmed=info->open; draw_dialog(dialog,s); dimmed=false; }
    if (info->open) draw_info(info);
    doupdate();
}
static void selected_chain(const Snapshot *s,const size_t *list,size_t count,size_t selected,Process *chain,size_t *nchain) {
    *nchain=0;
    if (count) {
        const Row *r=&s->rows[list[selected]];
        if (r->process!=SIZE_MAX) *nchain=read_chain(&s->processes[r->process],chain);
    }
}
int ui_run(Options *o) {
    if (tcgetattr(STDIN_FILENO,&saved)<0) { perror("porttop: terminal"); return 1; }
    setlocale(LC_ALL,"");
    unicode=MB_CUR_MAX>1 && wcwidth(L'⣿')==1 && wcwidth(L'╭')==1;
    const char *term=getenv("TERM");
    if (!term || !*term || !strcmp(term,"dumb")) { fputs("porttop: set TERM to your terminal type, or use --once\n",stderr); return 1; }
    screen=newterm(NULL,stdout,stdin);
    if (!screen) { fputs("porttop: ncurses could not initialize this terminal\n",stderr); return 1; }
    active=true; atexit(restore);
    cbreak(); noecho(); nonl(); keypad(stdscr,TRUE); curs_set(0); set_escdelay(25); leaveok(stdscr,TRUE);
    colors=o->color && has_colors();
    if (colors) {
        start_color(); use_default_colors();
        if (COLORS>=256) {
            init_pair(1,80,-1); init_pair(2,244,-1); init_pair(3,114,-1); init_pair(4,179,-1);
            init_pair(5,255,23); init_pair(6,176,-1); init_pair(7,65,-1); init_pair(8,143,-1);
            init_pair(9,174,-1); init_pair(10,67,-1); init_pair(11,250,235); init_pair(12,203,-1);
            init_pair(13,117,23); init_pair(16,255,88);
            init_pair(17,30,-1); init_pair(18,64,-1); init_pair(19,236,-1);
        } else {
            init_pair(1,COLOR_CYAN,-1); init_pair(2,COLOR_WHITE,-1); init_pair(3,COLOR_GREEN,-1);
            init_pair(4,COLOR_YELLOW,-1); init_pair(5,COLOR_WHITE,COLOR_BLUE); init_pair(6,COLOR_MAGENTA,-1);
            init_pair(7,COLOR_GREEN,-1); init_pair(8,COLOR_YELLOW,-1); init_pair(9,COLOR_RED,-1);
            init_pair(10,COLOR_CYAN,-1); init_pair(11,COLOR_WHITE,COLOR_BLACK); init_pair(12,COLOR_RED,-1);
            init_pair(13,COLOR_CYAN,COLOR_BLUE); init_pair(16,COLOR_WHITE,COLOR_RED);
            init_pair(17,COLOR_CYAN,-1); init_pair(18,COLOR_GREEN,-1); init_pair(19,COLOR_BLACK,-1);
        }
    }
    struct sigaction sa={0}; sa.sa_handler=handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
    sigaction(SIGTSTP,&sa,NULL); sigaction(SIGWINCH,&sa,NULL);
    History history={0}; size_t core_page=0;
    Snapshot s={0}; size_t *list=NULL,count=0,selected=0,offset=0,nchain=0;
    InfoDialog info={0}; bool mouse_enabled=false;
    Process chain[MAX_CHAIN]; Dialog *dialog=calloc(1,sizeof(*dialog));
    if (!dialog) { restore(); delscreen(screen); return 1; }
    bool shared_scale=false;
    bool paused=false,filtering=false,help=false,refresh=true,dirty=true;
    char message[512]="",old_filter[256]=""; int result=0; double next=0;
    while (!stopping) {
        if (suspending) {
            def_prog_mode(); restore(); suspending=0; raise(SIGSTOP);
            reset_prog_mode(); active=true; curs_set(0); mouse_enabled=false; resized=1; dirty=true;
        }
        struct winsize ws;
        if (!ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws) && ws.ws_col && ws.ws_row &&
            (resized || ws.ws_col!=getmaxx(stdscr) || ws.ws_row!=getmaxy(stdscr))) {
            resizeterm(ws.ws_row,ws.ws_col); resized=0; dirty=true;
        }
        double now=monotonic_seconds();
        if ((refresh || (!paused && now>=next)) && !dialog->open && !info.open) {
            Snapshot fresh;
            if (snapshot_read(&fresh,&s,o->all)<0) {
                snprintf(message,sizeof(message),"Sampling failed: %s",strerror(errno));
                if (s.sampled_at==0.) { result=1; break; }
            } else {
                uint64_t inode=0,start=0; pid_t pid=0;
                size_t screen_row=selected>=offset?selected-offset:0;
                if (count && list) {
                    Row r=s.rows[list[selected]]; inode=s.sockets[r.socket].inode;
                    if (r.process!=SIZE_MAX) { pid=s.processes[r.process].pid; start=s.processes[r.process].start; }
                }
                size_t fresh_count; size_t *fresh_list=rows_select(&fresh,o,&fresh_count);
                if (!fresh_list) { snapshot_free(&fresh); result=1; break; }
                snapshot_free(&s); s=fresh; free(list); list=fresh_list; count=fresh_count;
                if (selected>=count) selected=count?count-1:0;
                for (size_t i=0;i<count;i++) {
                    Row r=s.rows[list[i]]; Process *p=r.process==SIZE_MAX?NULL:&s.processes[r.process];
                    if (s.sockets[r.socket].inode==inode && (p?p->pid:0)==pid && (p?p->start:0)==start) { selected=i; break; }
                }
                if (history_push(&history,&s)<0) { result=1; break; }
                offset=selected>screen_row?selected-screen_row:0;
                selected_chain(&s,list,count,selected,chain,&nchain);
            }
            refresh=false; next=monotonic_seconds()+(double)o->interval_ms/1000.; dirty=true;
        }
        bool needs_mouse=dialog->open || info.open;
        if (needs_mouse!=mouse_enabled) {
            mmask_t mask=BUTTON4_PRESSED;
#ifdef BUTTON5_PRESSED
            mask|=BUTTON5_PRESSED;
#endif
            mousemask(needs_mouse?mask:0,NULL); mouseinterval(0); mouse_enabled=needs_mouse;
        }
        if (dirty) {
            draw(&s,o,list,count,selected,&offset,paused,filtering,help,dialog,chain,nchain,message,&history,core_page,shared_scale,&info);
            dirty=false;
        }
        int delay=250;
        if (!paused && !dialog->open && !info.open) { double ms=(next-monotonic_seconds())*1000.; if (ms<delay) delay=ms>0?(int)ms:0; }
        timeout(delay); int key=getch();
        if (key==ERR) {
            struct pollfd fd={STDIN_FILENO,POLLIN,0}; poll(&fd,1,0);
            if (fd.revents&(POLLHUP|POLLERR|POLLNVAL)) break;
            continue;
        }
        if (key==KEY_RESIZE) { dirty=true; continue; }
        dirty=true;
        MEVENT mouse_event; bool has_mouse_event=key==KEY_MOUSE && getmouse(&mouse_event)==OK;
        if (info.open) {
            size_t visible=info.window?(size_t)(getmaxy(info.window)-6):1;
            if (key==27 || key=='q' || key=='i' || key=='\r' || key=='\n' || key==KEY_ENTER) info_close(&info);
            else if (width>=80 && height>=24) {
                if (key==KEY_UP) text_view_move(&info.content,-1,visible);
                else if (key==KEY_DOWN || key=='j') text_view_move(&info.content,1,visible);
                else if (key==KEY_PPAGE) text_view_move(&info.content,-(long)visible,visible);
                else if (key==KEY_NPAGE) text_view_move(&info.content,(long)visible,visible);
                else if (key==KEY_HOME) info.content.scroll=0;
                else if (key==KEY_END) info.content.scroll=text_view_max_scroll(&info.content,visible);
                else if (key==KEY_MOUSE) {
                    if (has_mouse_event && mouse_inside(&mouse_event,info.window))
                        text_view_move(&info.content,wheel_delta(&mouse_event),visible);
                }
            }
            continue;
        }
        if (dialog->open) {
            if (key==27 || key=='n' || key=='q' || ((key=='\n' || key=='\r' || key==KEY_ENTER) && !dialog->confirm)) {
                dialog_close(dialog); snprintf(message,sizeof(message),"Signal cancelled");
            } else if (width<80 || height<24) {
                /* Hidden confirmation controls must never execute an action. */
            } else if (key=='i') {
                if (info_open(&info,&dialog->chain[dialog->target],&s,dialog->chain+dialog->target,dialog->length-dialog->target)<0)
                    snprintf(dialog->error,sizeof(dialog->error),"Cannot open details: %s",strerror(errno));
            } else if (key==KEY_MOUSE) {
                if (has_mouse_event && mouse_inside(&mouse_event,dialog->window)) {
                    int delta=wheel_delta(&mouse_event);
                    dialog_preview_move(dialog,delta);
                }
            } else if (key=='y' || ((key=='\n' || key=='\r' || key==KEY_ENTER) && dialog->confirm)) {
                if (!*dialog->error) {
                    (void)signal_plan_send(&dialog->plan,dialog->sig,message,sizeof(message));
                    dialog_close(dialog); refresh=true;
                }
            } else if (key=='\t' || key==KEY_LEFT || key==KEY_RIGHT) dialog->confirm=!dialog->confirm;
            else if (key=='s') { dialog->tree=!dialog->tree; dialog_plan(dialog); }
            else if (key==KEY_UP && dialog->target+1<dialog->length) { ++dialog->target; dialog->tree=true; dialog_plan(dialog); }
            else if (key==KEY_DOWN && dialog->target) { --dialog->target; dialog_plan(dialog); }
            else if (key==KEY_NPAGE) dialog_preview_move(dialog,dialog->window?getmaxy(dialog->window)-17:1);
            else if (key==KEY_PPAGE) dialog_preview_move(dialog,dialog->window?17-getmaxy(dialog->window):-1);

            continue;
        }
        if (filtering) {
            size_t len=strlen(o->filter);
            if (key==27) { snprintf(o->filter,sizeof(o->filter),"%s",old_filter); filtering=false; }
            else if (key=='\r' || key=='\n' || key==KEY_ENTER) filtering=false;
            else if (key==KEY_BACKSPACE || key==127 || key==8) { if (len) o->filter[len-1]=0; }
            else if (key>=32 && key<127 && len<sizeof(o->filter)-1) { o->filter[len]=(char)key; o->filter[len+1]=0; }
            selected=offset=0;
        } else if (help) { if (key=='?' || key==27) help=false; else if (key=='q') break; else continue; }
        else {
            message[0]=0;
            switch(key) {
            case 'q': goto done;
            case '?': help=true; break;
            case '/': filtering=true; snprintf(old_filter,sizeof(old_filter),"%s",o->filter); break;
            case 27: o->filter[0]=0; selected=offset=0; break;
            case 'j': case KEY_DOWN: if (selected+1<count) ++selected; break;
            case KEY_UP: if (selected) --selected; break;
            case KEY_HOME: selected=0; break;
            case KEY_END: selected=count?count-1:0; break;
            case KEY_PPAGE: { size_t page=(size_t)(detail_begin()-table_begin()-1); selected=selected>page?selected-page:0; break; }
            case KEY_NPAGE: { size_t page=(size_t)(detail_begin()-table_begin()-1); selected=selected+page<count?selected+page:(count?count-1:0); break; }
            case ' ': paused=!paused; break;
            case 'R': refresh=true; break;
            case 'c': ++core_page; break;
            case 'g': shared_scale=!shared_scale; break;
            case 's': o->sort=(Sort)((o->sort+1)%4); selected=offset=0; break;
            case 'r': o->reverse=!o->reverse; selected=offset=0; break;
            case 'a': o->all=!o->all; refresh=true; selected=offset=0; break;
            case '\r': case '\n': case KEY_ENTER: case 'i':
                if (count && nchain) {
                    const Row *r=&s.rows[list[selected]];
                    if (info_open(&info,&s.processes[r->process],&s,chain,nchain)<0)
                        snprintf(message,sizeof(message),"Cannot open details: %s",strerror(errno));
                } else snprintf(message,sizeof(message),"Owner unavailable; no process details");
                break;
            case 'k': case 't': case 'K':
                if (count && nchain) {
                    dialog->length=nchain; memcpy(dialog->chain,chain,nchain*sizeof(*chain));
                    dialog->sig=key=='K'?SIGKILL:SIGTERM; dialog->tree=false; dialog->target=0;
                    dialog->port=s.sockets[s.rows[list[selected]].socket].port; dialog->open=true; dialog_plan(dialog);
                } else snprintf(message,sizeof(message),"Owner unavailable; cannot signal this socket");
                break;
            default: break;
            }
        }
        if (!dialog->open && !info.open) {
            free(list); list=rows_select(&s,o,&count);
            if (!list) { result=1; break; }
            if (selected>=count) selected=count?count-1:0;
            selected_chain(&s,list,count,selected,chain,&nchain);
        }
    }
done:
    mousemask(0,NULL); info_close(&info); dialog_close(dialog); free(dialog); free(list); snapshot_free(&s); history_free(&history); restore(); delscreen(screen);
    if (result) fprintf(stderr,"porttop: terminal or sampling failure: %s\n",strerror(errno));
    return result;
}
