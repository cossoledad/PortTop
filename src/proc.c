#include "porttop.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Grow in blocks, with allocation errors propagated to the caller. */
static int append(void **array, size_t *count, size_t width, const void *value) {
    if (*count % 64 == 0) {
        if (*count > SIZE_MAX / width - 64) { errno=ENOMEM; return -1; }
        void *p=realloc(*array,(*count+64)*width);
        if (!p) return -1;
        *array=p;
    }
    memcpy((char *)*array+*count*width,value,width); ++*count; return 0;
}
double monotonic_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
void text_safe(char *s) {
    /* ASCII-only display prevents terminal escapes and invalid UTF-8 widths. */
    for (;*s;++s) if ((unsigned char)*s<32 || (unsigned char)*s>=127) *s='?';
}
static ssize_t read_file(const char *path, char *buf, size_t size) {
    FILE *f=fopen(path,"re"); if (!f) return -1;
    size_t n=fread(buf,1,size-1,f); int bad=ferror(f); fclose(f); buf[n]=0;
    if (bad) return -1;
    return (ssize_t)n;
}
int process_read(pid_t pid, Process *p) {
    char path[128], statbuf[8192]; memset(p,0,sizeof(*p)); p->pid=pid;
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    if (read_file(path,statbuf,sizeof(statbuf))<0) return -1;
    char *left=strchr(statbuf,'('), *right=strrchr(statbuf,')');
    if (!left || !right || right<=left || right[1]!=' ') { errno=EINVAL; return -1; }
    size_t n=(size_t)(right-left-1); if (n>=sizeof(p->name)) n=sizeof(p->name)-1;
    memcpy(p->name,left+1,n); text_safe(p->name);
    char *save=NULL, *tok=strtok_r(right+2," ", &save);
    uint64_t user=0, system=0;
    for (int field=3;tok; ++field,tok=strtok_r(NULL," ",&save)) {
        if (field==3) p->state=*tok;
        if (field==5) p->group=(pid_t)strtol(tok,NULL,10);
        if (field==6) p->session=(pid_t)strtol(tok,NULL,10);
        if (field==7) p->tty=strtol(tok,NULL,10);
        if (field==4) p->ppid=(pid_t)strtol(tok,NULL,10);
        if (field==14) user=strtoull(tok,NULL,10);
        if (field==15) system=strtoull(tok,NULL,10);
        if (field==22) p->start=strtoull(tok,NULL,10);
        if (field==24) { long long pages=strtoll(tok,NULL,10); if (pages>0) p->rss=(uint64_t)pages*(uint64_t)sysconf(_SC_PAGESIZE); }
    }
    if (!p->start) { errno=ESRCH; return -1; }
    p->ticks=user+system;
    snprintf(path,sizeof(path),"/proc/%d",pid);
    struct stat st; if (stat(path,&st)<0) return -1;
    p->uid=st.st_uid;
    struct passwd pw, *found=NULL; char pwbuf[16384];
    if (!getpwuid_r(p->uid,&pw,pwbuf,sizeof(pwbuf),&found) && found)
        snprintf(p->user,sizeof(p->user),"%s",found->pw_name);
    else snprintf(p->user,sizeof(p->user),"%u",p->uid);
    text_safe(p->user);
    snprintf(path,sizeof(path),"/proc/%d/cmdline",pid);
    ssize_t bytes=read_file(path,p->command,sizeof(p->command));
    if (bytes>0) {
        p->truncated=(size_t)bytes==sizeof(p->command)-1;
        for (ssize_t i=0;i<bytes-1;i++) if (!p->command[i]) p->command[i]=' ';
        text_safe(p->command);
    } else snprintf(p->command,sizeof(p->command),"[%s]",p->name);
    snprintf(path,sizeof(path),"/proc/%d/cwd",pid);
    ssize_t len=readlink(path,p->cwd,sizeof(p->cwd)-1);
    if (len>=0) p->cwd[len]=0; else strcpy(p->cwd,"(unavailable)");
    snprintf(path,sizeof(path),"/proc/%d/exe",pid);
    len=readlink(path,p->exe,sizeof(p->exe)-1);
    if (len>=0) p->exe[len]=0; else strcpy(p->exe,"(unavailable)");
    text_safe(p->cwd); text_safe(p->exe); return 0;
}
static bool address_decode(const char *hex, bool ipv6, char *out, size_t size) {
    uint32_t words[4]={0};
    if (strlen(hex)!=(ipv6?32u:8u)) return false;
    for (unsigned i=0;i<(ipv6?4u:1u);i++) {
        char chunk[9]; memcpy(chunk,hex+i*8,8); chunk[8]=0;
        char *end; unsigned long value=strtoul(chunk,&end,16); if (*end) return false;
        words[i]=(uint32_t)value; /* /proc uses native byte order within each word. */
    }
    return inet_ntop(ipv6?AF_INET6:AF_INET,words,out,(socklen_t)size)!=NULL;
}
static int read_sockets(Snapshot *s, const char *table, bool ipv6, bool udp, bool all) {
    char path[128]; snprintf(path,sizeof(path),"/proc/net/%s",table);
    FILE *f=fopen(path,"re"); if (!f) { ++s->unavailable_tables; return 0; }
    char line[1024];
    if (!fgets(line,sizeof(line),f)) { fclose(f); errno=EIO; return -1; }
    while (fgets(line,sizeof(line),f)) {
        char local[40], remote[40]; unsigned port, peer, state; uint64_t inode;
        int n=sscanf(line," %*u: %32[0-9A-Fa-f]:%x %32[0-9A-Fa-f]:%x %x %*s %*s %*s %*u %*u %" SCNu64,
                     local,&port,remote,&peer,&state,&inode);
        if (n!=6 || !port || (!all && (udp?state!=7:state!=10))) continue;
        Socket sock={.port=port,.remote_port=peer,.state=state,.inode=inode};
        snprintf(sock.protocol,sizeof(sock.protocol),"%s%s",udp?"udp":"tcp",ipv6?"6":"");
        if (!address_decode(local,ipv6,sock.address,sizeof(sock.address)) ||
            !address_decode(remote,ipv6,sock.remote,sizeof(sock.remote))) continue;
        if (append((void **)&s->sockets,&s->nsockets,sizeof(sock),&sock)<0) { fclose(f); return -1; }
    }
    if (ferror(f)) { fclose(f); errno=EIO; return -1; }
    fclose(f); return 0;
}
static int inode_cmp(const void *a,const void *b) {
    const Socket *x=a,*y=b; return (x->inode>y->inode)-(x->inode<y->inode);
}
static size_t inode_find(const Snapshot *s,uint64_t inode) {
    size_t lo=0, hi=s->nsockets;
    while (lo<hi) { size_t mid=lo+(hi-lo)/2;
        if (s->sockets[mid].inode<inode) lo=mid+1; else hi=mid;
    }
    return lo;
}
static bool cpu_delta(uint64_t ticks,uint64_t idle,uint64_t old_ticks,uint64_t old_idle,double *usage) {
    if (ticks<=old_ticks || idle<old_idle || idle-old_idle>ticks-old_ticks) return false;
    *usage=100.*(1.-(double)(idle-old_idle)/(double)(ticks-old_ticks)); return true;
}
static int host_read(Snapshot *s,const Snapshot *prev) {
    FILE *f=fopen("/proc/stat","re");
    if (f) {
        char line[512];
        while (fgets(line,sizeof(line),f)) {
            char name[32]; unsigned long long v[8]={0};
            if (sscanf(line,"%31s %llu %llu %llu %llu %llu %llu %llu %llu",name,v,v+1,v+2,v+3,v+4,v+5,v+6,v+7)!=9) continue;
            if (strncmp(name,"cpu",3)) continue;
            uint64_t ticks=0,idle=(uint64_t)(v[3]+v[4]);
            for (int i=0;i<8;i++) ticks+=(uint64_t)v[i];
            if (!strcmp(name,"cpu")) {
                s->host_ticks=ticks; s->host_idle=idle;
                if (prev && prev->sampled_at>0)
                    s->host_cpu_valid=cpu_delta(ticks,idle,prev->host_ticks,prev->host_idle,&s->host_cpu);
            } else {
                char *end; unsigned long id=strtoul(name+3,&end,10);
                if (*end || !name[3] || id>UINT32_MAX) continue;
                Cpu cpu={.id=(unsigned)id,.ticks=ticks,.idle=idle};
                if (prev) for (size_t i=0;i<prev->ncores;i++) if (prev->cores[i].id==cpu.id) {
                    cpu.valid=cpu_delta(ticks,idle,prev->cores[i].ticks,prev->cores[i].idle,&cpu.usage); break;
                }
                if (append((void **)&s->cores,&s->ncores,sizeof(cpu),&cpu)<0) { fclose(f); return -1; }
            }
        }
        fclose(f);
    }
    f=fopen("/proc/meminfo","re");
    if (f) { char line[256]; unsigned long long n;
        while (fgets(line,sizeof(line),f)) {
            if (sscanf(line,"MemTotal: %llu kB",&n)==1) s->mem_total=(uint64_t)n*1024;
            if (sscanf(line,"MemAvailable: %llu kB",&n)==1) s->mem_available=(uint64_t)n*1024;
        } fclose(f);
    }
    return 0;
}
int snapshot_read(Snapshot *s,const Snapshot *prev,bool all) {
    memset(s,0,sizeof(*s)); s->sampled_at=monotonic_seconds(); if (host_read(s,prev)<0) goto fail;
    if (read_sockets(s,"tcp",false,false,all)<0 || read_sockets(s,"tcp6",true,false,all)<0 ||
        read_sockets(s,"udp",false,true,all)<0 || read_sockets(s,"udp6",true,true,all)<0) goto fail;
    if (s->unavailable_tables==4) { errno=EACCES; goto fail; }
    if (s->nsockets>1) qsort(s->sockets,s->nsockets,sizeof(Socket),inode_cmp);
    bool *owned=calloc(s->nsockets?s->nsockets:1,sizeof(bool)); if (!owned) goto fail;
    DIR *proc=opendir("/proc"); if (!proc) { free(owned); goto fail; }
    struct dirent *entry;
    while ((entry=readdir(proc))) {
        char *end; long id=strtol(entry->d_name,&end,10); if (*end || id<=0 || id>INT32_MAX) continue;
        char path[128]; snprintf(path,sizeof(path),"/proc/%ld/fd",id);
        DIR *fds=opendir(path);
        if (!fds) { if (errno==EACCES || errno==EPERM) ++s->denied; continue; }
        /* Read identity before enumerating fds, then verify it after. */
        Process p; if (process_read((pid_t)id,&p)<0) { closedir(fds); continue; }
        size_t *matches=NULL,nmatches=0; struct dirent *fd;
        while ((fd=readdir(fds))) {
            if (fd->d_name[0]=='.') continue;
            char linkpath[384],target[128];
            snprintf(linkpath,sizeof(linkpath),"%s/%s",path,fd->d_name);
            ssize_t n=readlink(linkpath,target,sizeof(target)-1); if (n<0) continue; target[n]=0;
            uint64_t inode; if (sscanf(target,"socket:[%" SCNu64 "]",&inode)!=1 || !inode) continue;
            size_t idx=inode_find(s,inode);
            for (;idx<s->nsockets && s->sockets[idx].inode==inode;idx++) {
                bool duplicate=false; for (size_t j=0;j<nmatches;j++) if (matches[j]==idx) duplicate=true;
                if (!duplicate && append((void **)&matches,&nmatches,sizeof(idx),&idx)<0) {
                    free(matches); closedir(fds); closedir(proc); free(owned); goto fail;
                }
            }
        }
        closedir(fds);
        if (nmatches) {
            Process verified;
            if (process_read((pid_t)id,&verified)<0 || verified.start!=p.start) { free(matches); continue; }
            double dt=prev?s->sampled_at-prev->sampled_at:0;
            if (dt>0) for (size_t j=0;j<prev->nprocesses;j++) {
                const Process *old=&prev->processes[j];
                if (old->pid==p.pid && old->start==p.start && p.ticks>=old->ticks) {
                    p.cpu_valid=true;
                    p.cpu=100.*(double)(p.ticks-old->ticks)/(double)sysconf(_SC_CLK_TCK)/dt; break;
                }
            }
            size_t pi=s->nprocesses;
            if (append((void **)&s->processes,&s->nprocesses,sizeof(p),&p)<0) {
                free(matches); closedir(proc); free(owned); goto fail;
            }
            for (size_t j=0;j<nmatches;j++) {
                Row row={matches[j],pi}; owned[matches[j]]=true;
                if (append((void **)&s->rows,&s->nrows,sizeof(row),&row)<0) {
                    free(matches); closedir(proc); free(owned); goto fail;
                }
            }
        }
        free(matches);
    }
    closedir(proc);
    for (size_t i=0;i<s->nsockets;i++) if (!owned[i]) {
        Row row={i,SIZE_MAX}; ++s->unknown;
        if (append((void **)&s->rows,&s->nrows,sizeof(row),&row)<0) { free(owned); goto fail; }
    }
    free(owned); return 0;
fail: { int saved=errno; snapshot_free(s); errno=saved; return -1; }
}
void snapshot_free(Snapshot *s) { free(s->cores); free(s->sockets); free(s->processes); free(s->rows); memset(s,0,sizeof(*s)); }
const char *socket_state(const Socket *s) {
    static const char *names[]={"UNKNOWN","ESTAB","SYN-SENT","SYN-RECV","FIN-WAIT1","FIN-WAIT2","TIME-WAIT","UNCONN","CLOSE-WAIT","LAST-ACK","LISTEN","CLOSING","NEW-SYN"};
    return s->state<sizeof(names)/sizeof(*names)?names[s->state]:"UNKNOWN";
}
int process_signal(pid_t pid,uint64_t start,int sig,char *message,size_t size) {
    if (process_protected(pid) || (sig!=SIGTERM && sig!=SIGKILL)) {
        snprintf(message,size,"Refusing protected PID or unsupported signal"); return -1;
    }
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int fd=(int)syscall(SYS_pidfd_open,pid,0);
    if (fd<0) { snprintf(message,size,"Cannot lock PID %d: %s",pid,strerror(errno)); return -1; }
    Process p;
    if (process_read(pid,&p)<0 || p.start!=start) {
        close(fd); snprintf(message,size,"Process exited or identity changed; signal cancelled"); return -1;
    }
    int result=(int)syscall(SYS_pidfd_send_signal,fd,sig,NULL,0); int saved=errno; close(fd);
    if (result<0) snprintf(message,size,"Signal failed for PID %d: %s",pid,strerror(saved));
    else snprintf(message,size,"%s sent to PID %d (exit not yet confirmed)",sig==SIGTERM?"SIGTERM":"SIGKILL",pid);
    return result;
#else
    (void)start; snprintf(message,size,"This build lacks pidfd support; signal cancelled"); return -1;
#endif
}
static const Snapshot *sort_snapshot;
static const Options *sort_options;
static int row_cmp(const void *a,const void *b) {
    const Row *x=&sort_snapshot->rows[*(const size_t *)a],*y=&sort_snapshot->rows[*(const size_t *)b];
    const Socket *sx=&sort_snapshot->sockets[x->socket],*sy=&sort_snapshot->sockets[y->socket];
    const Process *px=x->process==SIZE_MAX?NULL:&sort_snapshot->processes[x->process];
    const Process *py=y->process==SIZE_MAX?NULL:&sort_snapshot->processes[y->process];
    int c=0;
    if (sort_options->sort==SORT_PID) { int vx=px?px->pid:INT32_MAX,vy=py?py->pid:INT32_MAX; c=(vx>vy)-(vx<vy); }
    if (sort_options->sort==SORT_CPU) { double vx=px?px->cpu:-1,vy=py?py->cpu:-1; c=(vx<vy)-(vx>vy); }
    if (sort_options->sort==SORT_MEMORY) { uint64_t vx=px?px->rss:0,vy=py?py->rss:0; c=(vx<vy)-(vx>vy); }
    if (!c) c=(sx->port>sy->port)-(sx->port<sy->port);
    if (!c) c=strcmp(sx->protocol,sy->protocol);
    if (!c) c=strcmp(sx->address,sy->address);
    if (!c) c=((px?px->pid:0)>(py?py->pid:0))-((px?px->pid:0)<(py?py->pid:0));
    if (!c) c=(sx->inode>sy->inode)-(sx->inode<sy->inode);
    return sort_options->reverse?-c:c;
}
bool process_visible(const Process *p,uid_t viewer) { return viewer==0 || (p && p->uid==viewer); }
size_t *rows_select(const Snapshot *s,const Options *o,size_t *count) {
    *count=0;
    size_t *list=malloc((s->nrows?s->nrows:1)*sizeof(size_t)); if (!list) return NULL;
    for (size_t i=0;i<s->nrows;i++) {
        const Row *r=&s->rows[i]; const Socket *sock=&s->sockets[r->socket];
        const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
        if (!process_visible(p,geteuid())) continue;
        if (o->port && o->port!=sock->port) continue;
        if (*o->filter) {
            char nums[64]; snprintf(nums,sizeof(nums),"%u %d",sock->port,p?p->pid:0);
            if (!strcasestr(nums,o->filter) && !strcasestr(sock->address,o->filter) &&
                !strcasestr(sock->protocol,o->filter) && !strcasestr(socket_state(sock),o->filter) &&
                !(p && (strcasestr(p->command,o->filter) || strcasestr(p->user,o->filter)))) continue;
        }
        list[(*count)++]=i;
    }
    sort_snapshot=s; sort_options=o;
    if (*count>1) qsort(list,*count,sizeof(*list),row_cmp);
    return list;
}
static void json_string(const char *s) {
    putchar('"'); for (;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if (c=='"' || c=='\\') { putchar('\\'); putchar(c); }
        else if (c<32) printf("\\u%04x",c); else putchar(c);
    } putchar('"');
}
void print_snapshot(const Snapshot *s,const Options *o) {
    size_t count; size_t *rows=rows_select(s,o,&count);
    if (!rows) { fputs("porttop: out of memory\n",stderr); exit(1); }
    if (o->json) {
        printf("{\"scope\":\"%s\",\"viewer_uid\":%u,\"system\":{\"cpu_count\":%zu,\"memory_total_bytes\":%" PRIu64 ",\"memory_available_bytes\":%" PRIu64 "},",geteuid()==0?"all":"current-user",geteuid(),s->ncores,s->mem_total,s->mem_available);
        printf("\"version\":\"%s\",\"denied_processes\":%zu,\"unknown_sockets\":%zu,\"unavailable_tables\":%zu,\"cpu_sampled\":false,\"rows\":[",PT_VERSION,s->denied,s->unknown,s->unavailable_tables);
    } else puts("PORT  PROTO LOCAL ADDRESS                           STATE           PID USER         RSS MiB COMMAND");
    for (size_t i=0;i<count;i++) {
        const Row *r=&s->rows[rows[i]]; const Socket *sock=&s->sockets[r->socket];
        const Process *p=r->process==SIZE_MAX?NULL:&s->processes[r->process];
        if (o->json) {
            printf("%s{\"port\":%u,\"protocol\":",i?",":"",sock->port); json_string(sock->protocol);
            fputs(",\"address\":",stdout); json_string(sock->address);
            printf(",\"remote_port\":%u,\"remote_address\":",sock->remote_port); json_string(sock->remote);
            fputs(",\"state\":",stdout); json_string(socket_state(sock));
            printf(",\"inode\":%" PRIu64 ",\"process\":",sock->inode);
            if (!p) fputs("null",stdout);
            else {
                printf("{\"pid\":%d,\"ppid\":%d,\"uid\":%u,\"start_ticks\":%" PRIu64 ",\"rss_bytes\":%" PRIu64 ",\"user\":",p->pid,p->ppid,p->uid,p->start,p->rss); json_string(p->user);
                fputs(",\"command\":",stdout); json_string(p->command);
                fputs(",\"cwd\":",stdout); json_string(p->cwd);
                fputs(",\"exe\":",stdout); json_string(p->exe);
                printf(",\"command_truncated\":%s}",p->truncated?"true":"false");
            } putchar('}');
        } else printf("%-5u %-5s %-39s %-11s %7d %-12.12s %7.1f %s%s\n",sock->port,sock->protocol,sock->address,socket_state(sock),p?p->pid:0,p?p->user:"?",p?(double)p->rss/1048576.:0.,p?p->command:"(owner unavailable)",p&&p->truncated?" [truncated]":"");
    }
    if (o->json) puts("]}");
    else fprintf(stderr,"%zu rows; %zu sockets with unknown owner; %zu inaccessible process fd directories; %zu unavailable socket tables\n",count,s->unknown,s->denied,s->unavailable_tables);
    free(rows);
}
