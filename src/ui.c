#include "porttop.h"
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

#define MAX_CHAIN 12
static struct termios saved;
static SCREEN *screen;
static bool active,colors;
static volatile sig_atomic_t stopping,suspending,resized;
static int width,height;

typedef struct {
    bool open,tree,confirm;
    int sig;
    Process chain[MAX_CHAIN];
    size_t length,target,preview,command_offset;
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
    if (color==1 || color==5) a|=A_BOLD;
    if (color==5 && !colors) a|=A_REVERSE;
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
static void rule(int y,const char *title) {
    attrset((int)style(1)); mvhline(y,1,ACS_HLINE,width-2); attrset(A_NORMAL);
    put(stdscr,y,2,width-4,1," %s ",title);
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
    signal_plan_free(&d->plan); d->preview=0; d->command_offset=0; d->confirm=false; d->error[0]=0;
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
static void draw_dialog(Dialog *d,const Snapshot *s) {
    int h=height-2; if (h>30) h=30;
    int w=width-6; if (w>112) w=112;
    int top=(height-h)/2,left=(width-w)/2;
    if (d->window && (getmaxy(d->window)!=h || getmaxx(d->window)!=w ||
                      getbegy(d->window)!=top || getbegx(d->window)!=left)) {
        delwin(d->window); d->window=NULL;
    }
    if (!d->window) d->window=newwin(h,w,top,left);
    WINDOW *win=d->window; if (!win) return;
    werase(win); wattrset(win,(int)style(1)); box(win,0,0); wattrset(win,A_NORMAL);
    put(win,0,3,w-6,1," TERMINATE PROCESS ");
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
    size_t start=d->command_offset<len?d->command_offset:0;
    put(win,7,3,w-6,2,"COMMAND [%zu/%zu%s] | [ ] scroll",start,len,target->truncated?", truncated":"");
    for (size_t i=0;i<2;i++) if (start+i*chunk<len)
        put(win,8+(int)i,3,w-6,0,"%.*s",(int)chunk,target->command+start+i*chunk);
    put(win,10,3,w-6,3,"Scope: %s   [s switches scope]",d->tree?"[ Subtree ]   Process only":"[ Process only ]   Subtree");
    size_t affected=0;
    for (size_t i=0;i<s->nrows;i++) {
        const Row *r=&s->rows[i]; if (r->process==SIZE_MAX) continue;
        for (size_t j=0;j<d->plan.count;j++) if (s->processes[r->process].pid==d->plan.targets[j].pid &&
            s->processes[r->process].start==d->plan.targets[j].start) { ++affected; break; }
    }
    put(win,11,3,w-6,1,"Affected: %zu processes, %zu visible socket owners | PgUp/PgDn preview",d->plan.count,affected);
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
    put(win,h-2,w/2+2,18,d->confirm?5:0," [ Terminate ] ");
    /* Overlay the complete virtual popup, then commit the whole frame once. */
    touchwin(win); wnoutrefresh(win);
}
static void draw(const Snapshot *s,const Options *o,const size_t *list,size_t count,size_t selected,
                 size_t *offset,bool paused,bool filtering,bool help,Dialog *dialog,
                 size_t cmd_offset,const Process *chain,size_t nchain,const char *message) {
    getmaxyx(stdscr,height,width);
    /* werase only resets the virtual canvas; it does not clear the terminal. */
    erase();
    if (width<80 || height<24) {
        put(stdscr,0,0,width,1,"PortTop | terminal too small");
        put(stdscr,2,0,width,0,"Resize to at least 80 columns x 24 rows. q: quit");
        wnoutrefresh(stdscr); doupdate(); return;
    }
    put(stdscr,0,2,width-4,1,"PORTTOP  v%s",PT_VERSION);
    put(stdscr,0,21,width-23,2,"ports first, processes in context");
    rule(1,"SYSTEM");
    put(stdscr,2,3,width-6,0,"CPU %5.1f%%   RAM %5.1f / %.1f GiB   sockets %5zu   processes %5zu   %s",
         s->host_cpu,(double)(s->mem_total-s->mem_available)/1073741824.,(double)s->mem_total/1073741824.,s->nsockets,s->nprocesses,paused?"PAUSED":"LIVE");
    put(stdscr,3,3,width-6,4,"Unknown owners: %zu | inaccessible processes: %zu | unavailable tables: %zu",s->unknown,s->denied,s->unavailable_tables);
    char title[256]; snprintf(title,sizeof(title),"PORTS / %s / sort: %s %s / %zu rows",o->all?"all sockets":"listeners + UDP",sort_name(o->sort),o->reverse?"reversed":"",count);
    rule(4,title);
    put(stdscr,5,2,width-4,2," PORT  PROTO LOCAL ADDRESS             STATE          PID USER        CPU%%  RSS MiB COMMAND");
    int detail_top=height-13,visible=detail_top-7; if (visible<1) visible=1;
    if (selected<*offset) *offset=selected;
    if (selected>=*offset+(size_t)visible) *offset=selected-(size_t)visible+1;
    for (int j=0;j<visible;j++) {
        size_t i=*offset+(size_t)j; if (i>=count) break;
        const Row *r=&s->rows[list[i]]; const Socket *sock=&s->sockets[r->socket];
        const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
        char row[PT_COMMAND+512],pid[24]; if (p) snprintf(pid,sizeof(pid),"%d",p->pid); else strcpy(pid,"-");
        snprintf(row,sizeof(row),"%c%5u %-5s %-25.25s %-10s %7s %-10.10s %5.1f %8.1f %s",
            i==selected?'>':' ',sock->port,sock->protocol,sock->address,socket_state(sock),pid,p?p->user:"?",p?p->cpu:0.,p?(double)p->rss/1048576.:0.,p?p->command:"(owner unavailable)");
        put(stdscr,6+j,2,width-4,i==selected?5:0,"%-*.*s",width-4,width-4,row);
    }
    if (!count) put(stdscr,7,3,width-6,2,"No matching ports. / edit filter | a toggle all sockets");
    rule(detail_top,"PROCESS DETAILS / [ ] scroll command");
    if (count) {
        const Row *r=&s->rows[list[selected]]; const Socket *sock=&s->sockets[r->socket];
        const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
        put(stdscr,detail_top+1,3,width-6,3,"%s %s:%u -> %s:%u | %s | inode %" PRIu64,sock->protocol,sock->address,sock->port,sock->remote,sock->remote_port,socket_state(sock),sock->inode);
        if (p) {
            put(stdscr,detail_top+2,3,width-6,0,"PID %d  PPID %d  USER %s (%u)  CPU %.1f%%  RSS %.1f MiB",p->pid,p->ppid,p->user,p->uid,p->cpu,(double)p->rss/1048576.);
            put(stdscr,detail_top+3,3,width-6,0,"CWD  %s",p->cwd);
            put(stdscr,detail_top+4,3,width-6,0,"EXE  %s",p->exe);
            size_t len=strlen(p->command),chunk=(size_t)(width-10),start=cmd_offset<len?cmd_offset:0;
            put(stdscr,detail_top+5,3,width-6,2,"CMD  [%zu/%zu%s] (display joins argv with spaces)",start,len,p->truncated?", truncated at 16 KiB":"");
            for (size_t j=0;j<2;j++) if (start+j*chunk<len)
                put(stdscr,detail_top+6+(int)j,3,width-6,0,"     %.*s",(int)chunk,p->command+start+j*chunk);
            char parents[4096]; snprintf(parents,sizeof(parents),"PARENTS  %d",p->pid);
            for (size_t j=1;j<nchain;j++) {
                size_t used=strlen(parents); snprintf(parents+used,sizeof(parents)-used," <- %d (%s)",chain[j].pid,chain[j].name);
            }
            put(stdscr,detail_top+8,3,width-6,2,"%s",parents);
        } else put(stdscr,detail_top+3,3,width-6,4,"Owner unavailable: permissions, closing socket, or outside visible PID namespace.");
    }
    put(stdscr,height-3,2,width-4,filtering?3:2,"%s%s",filtering?"FILTER > ":"Filter: ",o->filter);
    put(stdscr,height-2,2,width-4,*message?4:2,"%s",*message?message:"arrows move  / filter  s sort  r reverse  a all  Space pause  k TERM  K KILL  ? help  q quit");
    if (help) {
        rule(5,"KEYBOARD HELP");
        const char *lines[]={
            "Up/Down or j     Select a port (j moves down; k terminates)",
            "PgUp/PgDn        Move one page; Home/End jump to first/last",
            "/                Filter by port, PID, user, address or command",
            "Enter / Esc      Apply / cancel filter; Esc clears an applied filter",
            "s / r            Cycle sort (port/PID/CPU/memory) / reverse",
            "a                Toggle listeners + unconnected UDP / all sockets",
            "Space / R        Pause sampling / refresh now",
            "[ / ]            Scroll command text by one line",
            "k / K            SIGTERM / SIGKILL confirmation popup",
            "Popup Up/Down    Choose parent/child in launch chain",
            "Popup s          Process only / Subtree with affected PIDs and ports",
            "Popup Tab/Enter  Select Cancel/Terminate and activate (default Cancel)",
            "Popup y / Esc    Confirm / cancel",
            "? / q            Close help / quit",
            "CPU: 100% = one core; first sample is zero. Only current network namespace."};
        for (size_t i=0;i<sizeof(lines)/sizeof(*lines);i++) put(stdscr,6+(int)i,3,width-6,0,"%s",lines[i]);
    }
    wnoutrefresh(stdscr);
    if (dialog->open) draw_dialog(dialog,s);
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
    const char *term=getenv("TERM");
    if (!term || !*term || !strcmp(term,"dumb")) { fputs("porttop: set TERM to your terminal type, or use --once\n",stderr); return 1; }
    screen=newterm(NULL,stdout,stdin);
    if (!screen) { fputs("porttop: ncurses could not initialize this terminal\n",stderr); return 1; }
    active=true; atexit(restore);
    cbreak(); noecho(); nonl(); keypad(stdscr,TRUE); curs_set(0); set_escdelay(25); leaveok(stdscr,TRUE);
    colors=o->color && has_colors();
    if (colors) {
        start_color(); use_default_colors();
        init_pair(1,COLOR_CYAN,-1); init_pair(2,COLOR_WHITE,-1); init_pair(3,COLOR_GREEN,-1);
        init_pair(4,COLOR_YELLOW,-1); init_pair(5,COLOR_WHITE,COLOR_BLUE);
    }
    struct sigaction sa={0}; sa.sa_handler=handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
    sigaction(SIGTSTP,&sa,NULL); sigaction(SIGWINCH,&sa,NULL);
    Snapshot s={0}; size_t *list=NULL,count=0,selected=0,offset=0,cmd_offset=0,nchain=0;
    Process chain[MAX_CHAIN]; Dialog *dialog=calloc(1,sizeof(*dialog));
    if (!dialog) { restore(); delscreen(screen); return 1; }
    bool paused=false,filtering=false,help=false,refresh=true,dirty=true;
    char message[512]="",old_filter[256]=""; int result=0; double next=0;
    while (!stopping) {
        if (suspending) {
            def_prog_mode(); restore(); suspending=0; raise(SIGSTOP);
            reset_prog_mode(); active=true; curs_set(0); resized=1; dirty=true;
        }
        struct winsize ws;
        if (!ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws) && ws.ws_col && ws.ws_row &&
            (resized || ws.ws_col!=getmaxx(stdscr) || ws.ws_row!=getmaxy(stdscr))) {
            resizeterm(ws.ws_row,ws.ws_col); resized=0; dirty=true;
        }
        double now=monotonic_seconds();
        if ((refresh || (!paused && now>=next)) && !dialog->open) {
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
                offset=selected>screen_row?selected-screen_row:0;
                selected_chain(&s,list,count,selected,chain,&nchain);
            }
            refresh=false; next=monotonic_seconds()+(double)o->interval_ms/1000.; dirty=true;
        }
        if (dirty) {
            draw(&s,o,list,count,selected,&offset,paused,filtering,help,dialog,cmd_offset,chain,nchain,message);
            dirty=false;
        }
        int delay=250;
        if (!paused && !dialog->open) { double ms=(next-monotonic_seconds())*1000.; if (ms<delay) delay=ms>0?(int)ms:0; }
        timeout(delay); int key=getch();
        if (key==ERR) {
            struct pollfd fd={STDIN_FILENO,POLLIN,0}; poll(&fd,1,0);
            if (fd.revents&(POLLHUP|POLLERR|POLLNVAL)) break;
            continue;
        }
        if (key==KEY_RESIZE) { dirty=true; continue; }
        dirty=true;
        if (dialog->open) {
            if (key==27 || key=='n' || key=='q' || ((key=='\n' || key=='\r' || key==KEY_ENTER) && !dialog->confirm)) {
                dialog_close(dialog); snprintf(message,sizeof(message),"Signal cancelled");
            } else if (width<80 || height<24) {
                /* Hidden confirmation controls must never execute an action. */
            } else if (key=='y' || ((key=='\n' || key=='\r' || key==KEY_ENTER) && dialog->confirm)) {
                if (!*dialog->error) {
                    (void)signal_plan_send(&dialog->plan,dialog->sig,message,sizeof(message));
                    dialog_close(dialog); refresh=true;
                }
            } else if (key=='\t' || key==KEY_LEFT || key==KEY_RIGHT) dialog->confirm=!dialog->confirm;
            else if (key=='s') { dialog->tree=!dialog->tree; dialog_plan(dialog); }
            else if (key==KEY_UP && dialog->target+1<dialog->length) { ++dialog->target; dialog->tree=true; dialog_plan(dialog); }
            else if (key==KEY_DOWN && dialog->target) { --dialog->target; dialog_plan(dialog); }
            else if (key==KEY_NPAGE && dialog->preview+1<dialog->plan.count) ++dialog->preview;
            else if (key==KEY_PPAGE && dialog->preview) --dialog->preview;
            else if (key=='[' || key==']') {
                size_t chunk=(size_t)(getmaxx(dialog->window)-6);
                if (key=='[') dialog->command_offset=dialog->command_offset>chunk?dialog->command_offset-chunk:0;
                else if (dialog->command_offset+chunk<strlen(dialog->chain[dialog->target].command)) dialog->command_offset+=chunk;
            }
            continue;
        }
        if (filtering) {
            size_t len=strlen(o->filter);
            if (key==27) { snprintf(o->filter,sizeof(o->filter),"%s",old_filter); filtering=false; }
            else if (key=='\r' || key=='\n' || key==KEY_ENTER) filtering=false;
            else if (key==KEY_BACKSPACE || key==127 || key==8) { if (len) o->filter[len-1]=0; }
            else if (key>=32 && key<127 && len<sizeof(o->filter)-1) { o->filter[len]=(char)key; o->filter[len+1]=0; }
            selected=offset=cmd_offset=0;
        } else if (help) { if (key=='?' || key==27) help=false; else if (key=='q') break; else continue; }
        else {
            message[0]=0;
            switch(key) {
            case 'q': goto done;
            case '?': help=true; break;
            case '/': filtering=true; snprintf(old_filter,sizeof(old_filter),"%s",o->filter); break;
            case 27: o->filter[0]=0; selected=offset=0; break;
            case 'j': case KEY_DOWN: if (selected+1<count) ++selected; cmd_offset=0; break;
            case KEY_UP: if (selected) --selected; cmd_offset=0; break;
            case KEY_HOME: selected=0; cmd_offset=0; break;
            case KEY_END: selected=count?count-1:0; cmd_offset=0; break;
            case KEY_PPAGE: { size_t page=(size_t)(height>24?height-20:4); selected=selected>page?selected-page:0; cmd_offset=0; break; }
            case KEY_NPAGE: { size_t page=(size_t)(height>24?height-20:4); selected=selected+page<count?selected+page:(count?count-1:0); cmd_offset=0; break; }
            case ' ': paused=!paused; break;
            case 'R': refresh=true; break;
            case 's': o->sort=(Sort)((o->sort+1)%4); selected=offset=0; break;
            case 'r': o->reverse=!o->reverse; selected=offset=0; break;
            case 'a': o->all=!o->all; refresh=true; selected=offset=0; break;
            case '[': { size_t n=(size_t)(width>10?width-10:1); cmd_offset=cmd_offset>n?cmd_offset-n:0; break; }
            case ']': if (cmd_offset<PT_COMMAND) cmd_offset+=(size_t)(width>10?width-10:1); break;
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
        if (!dialog->open) {
            free(list); list=rows_select(&s,o,&count);
            if (!list) { result=1; break; }
            if (selected>=count) selected=count?count-1:0;
            selected_chain(&s,list,count,selected,chain,&nchain);
        }
    }
done:
    dialog_close(dialog); free(dialog); free(list); snapshot_free(&s); restore(); delscreen(screen);
    if (result) fprintf(stderr,"porttop: terminal or sampling failure: %s\n",strerror(errno));
    return result;
}
