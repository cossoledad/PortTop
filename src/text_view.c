#include "text_view.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int text_view_append(TextView *v,const char *format,...) {
    va_list args,copy; va_start(args,format); va_copy(copy,args);
    int n=vsnprintf(NULL,0,format,copy); va_end(copy);
    if (n<0 || (size_t)n>SIZE_MAX-v->length-1) { va_end(args); errno=EOVERFLOW; return -1; }
    char *text=realloc(v->text,v->length+(size_t)n+1);
    if (!text) { va_end(args); return -1; }
    v->text=text; vsnprintf(text+v->length,(size_t)n+1,format,args); va_end(args);
    v->length+=(size_t)n; v->width=0; return 0;
}
static int line_add(TextLine **lines,size_t *count,size_t start,size_t length) {
    if (*count%64==0) {
        if (*count>SIZE_MAX/sizeof(**lines)-64) { errno=ENOMEM; return -1; }
        void *p=realloc(*lines,(*count+64)*sizeof(**lines)); if (!p) return -1; *lines=p;
    }
    (*lines)[(*count)++]=(TextLine){start,length}; return 0;
}
int text_view_wrap(TextView *v,size_t width) {
    if (!width) { errno=EINVAL; return -1; }
    if (v->width==width) return 0;
    TextLine *lines=NULL; size_t count=0,pos=0;
    while (pos<v->length) {
        const char *newline=memchr(v->text+pos,'\n',v->length-pos);
        size_t end=newline?(size_t)(newline-v->text):v->length;
        if (pos==end && line_add(&lines,&count,pos,0)<0) goto fail;
        while (pos<end) {
            size_t take=end-pos<width?end-pos:width;
            if (take<end-pos) {
                for (size_t i=take;i>width/3;i--) if (v->text[pos+i-1]==' ') { take=i; break; }
            }
            if (line_add(&lines,&count,pos,take)<0) goto fail;
            pos+=take;
        }
        pos=end+(newline?1u:0u);
    }
    /* Retain the byte at the top of the viewport when a terminal resize reflows it. */
    size_t anchor=v->count && v->scroll<v->count?v->lines[v->scroll].start:0,scroll=0;
    for (size_t i=0;i<count;i++) { if (lines[i].start>anchor) break; scroll=i; }
    free(v->lines); v->lines=lines; v->count=count; v->width=width; v->scroll=scroll; return 0;
fail:
    free(lines); return -1;
}
size_t text_view_max_scroll(const TextView *v,size_t visible) { return v->count>visible?v->count-visible:0; }
void text_view_move(TextView *v,long delta,size_t visible) {
    size_t max=text_view_max_scroll(v,visible); if (v->scroll>max) v->scroll=max;
    if (delta<0) { size_t n=(size_t)(-(delta+1))+1; v->scroll=n>v->scroll?0:v->scroll-n; }
    else { size_t n=(size_t)delta; v->scroll=n>max-v->scroll?max:v->scroll+n; }
}
void text_view_free(TextView *v) { free(v->text); free(v->lines); memset(v,0,sizeof(*v)); }
