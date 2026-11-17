#ifndef PORTTOP_TEXT_VIEW_H
#define PORTTOP_TEXT_VIEW_H
#include <stddef.h>

typedef struct { size_t start, length; } TextLine;
typedef struct {
    char *text;
    TextLine *lines;
    size_t length, count, scroll, width;
} TextView;
int text_view_append(TextView *v,const char *format,...) __attribute__((format(printf,2,3)));
int text_view_wrap(TextView *v,size_t width);
size_t text_view_max_scroll(const TextView *v,size_t visible);
void text_view_move(TextView *v,long delta,size_t visible);
void text_view_free(TextView *v);
#endif
