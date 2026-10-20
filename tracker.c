#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

/* ─── constants ─────────────────────────────────────────────── */
#define MAX_ENTRIES       256
#define MAX_CATEGORIES     32
#define MAX_NAME           64
#define MAX_FIELD_VAL      64
#define MAX_FIELD_LABEL    32
#define MAX_FIELDS         16
#define MAX_CAT            32
#define MAX_SESSION_ITEMS  64
#define MAX_SESSIONS      128
#define MAX_LABEL          48
#define DATA_FILE         "tracker.json"

/* ─── ansi ──────────────────────────────────────────────────── */
#define CLR      "\033[2J\033[H"
#define HIDE_CUR "\033[?25l"
#define SHOW_CUR "\033[?25h"
#define RESET    "\033[0m"
#define BOLD     "\033[1m"
#define DIM      "\033[2m"
#define STRIKE   "\033[9m"

#define FG_STONE "\033[38;2;180;174;165m"
#define FG_SAGE  "\033[38;2;130;155;130m"
#define FG_SLATE "\033[38;2;100;130;155m"
#define FG_SAND  "\033[38;2;195;175;140m"
#define FG_SMOKE "\033[38;2;100;100;100m"
#define FG_CLAY  "\033[38;2;185;110;90m"

/* ─── labels ─────────────────────────────────────────────────── */
static char entry_label   [MAX_LABEL] = "ingredient";
static char entries_label [MAX_LABEL] = "pantry";
static char session_label [MAX_LABEL] = "basket";
static char sessions_label[MAX_LABEL] = "baskets";
static char was_label     [MAX_LABEL] = "was";
static char now_label     [MAX_LABEL] = "now";

static char field_labels[MAX_FIELDS][MAX_FIELD_LABEL];
static int  field_count = 0;

/* ─── structs ───────────────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME];
    char cat[MAX_CAT];
    char fields[MAX_FIELDS][MAX_FIELD_VAL];
} Entry;

typedef struct {
    char name[MAX_NAME];
    char was[MAX_FIELDS][MAX_FIELD_VAL];
    char now[MAX_FIELDS][MAX_FIELD_VAL];
    int  changed[MAX_FIELDS]; /* 1 if this field was actually updated this session */
    int  done;
} SItem;

typedef struct {
    char  date[32];
    SItem items[MAX_SESSION_ITEMS];
    int   count;
} Session;

/* ─── globals ───────────────────────────────────────────────── */
static Entry   entries[MAX_ENTRIES];  static int ent_n = 0;
static Session sessions[MAX_SESSIONS]; static int ses_n = 0;
static int     cur_ses = -1;

/* ─── terminal ──────────────────────────────────────────────── */
static struct termios orig;
static void raw_off(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig); }
static void raw_on(void) {
    tcgetattr(STDIN_FILENO, &orig); atexit(raw_off);
    struct termios r = orig;
    r.c_lflag &= ~(ECHO|ICANON); r.c_cc[VMIN]=1; r.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
}
#define KEY_UP 1000
#define KEY_DN 1001
#define KEY_EN 1002
#define KEY_ES 1003
static int gkey(void) {
    char c; if (read(STDIN_FILENO,&c,1)!=1) return -1;
    if (c=='\033') {
        char s[3]; if (read(STDIN_FILENO,s,1)!=1) return KEY_ES;
        if (read(STDIN_FILENO,s+1,1)!=1) return KEY_ES;
        if (s[0]=='[') { if (s[1]=='A') return KEY_UP; if (s[1]=='B') return KEY_DN; }
        return KEY_ES;
    }
    if (c=='\n'||c=='\r') return KEY_EN;
    return (unsigned char)c;
}
static int tcols(void) { struct winsize w; ioctl(1,TIOCGWINSZ,&w); return w.ws_col>0?w.ws_col:80; }

/* ─── draw helpers ──────────────────────────────────────────── */
static void hline(const char *l,const char *f,const char *r,int w,const char *col){
    printf("%s%s",col,l); for(int i=0;i<w-2;i++) printf("%s",f); printf("%s"RESET"\n",r);
}
static void centered(const char *t,int w,const char *col){
    int len=strlen(t),pad=(w-len)/2; if(pad<0)pad=0;
    printf("%s%*s%s%*s"RESET"\n",col,pad,"",t,w-pad-len,"");
}
static void spaced_title(const char *src, char *dst, int dsz){
    int d=0;
    for(int i=0;src[i]&&d<dsz-2;i++){
        char c=(src[i]>='a'&&src[i]<='z')?src[i]-32:src[i];
        dst[d++]=c;
        if(src[i+1]) dst[d++]=' ';
    }
    dst[d]=0;
}

/* ─── JSON helpers ──────────────────────────────────────────── */
static void jstr(FILE *f,const char *s){
    fputc('"',f);
    for(;*s;s++){
        if(*s=='"')  fputs("\\\"",f);
        else if(*s=='\\') fputs("\\\\",f);
        else fputc(*s,f);
    }
    fputc('"',f);
}

static const char *find_close_brace(const char *open){
    int depth=0; const char *p=open;
    while(*p){
        if(*p=='"'){ p++;
            while(*p&&*p!='"'){ if(*p=='\\')p++; if(*p)p++; }
            if(*p) p++; continue;
        }
        if(*p=='{') depth++;
        if(*p=='}'){ depth--; if(!depth) return p; }
        p++;
    }
    return NULL;
}

static const char *find_close_bracket(const char *open){
    int depth=0; const char *p=open;
    while(*p){
        if(*p=='"'){ p++;
            while(*p&&*p!='"'){ if(*p=='\\')p++; if(*p)p++; }
            if(*p) p++; continue;
        }
        if(*p=='[') depth++;
        if(*p==']'){ depth--; if(!depth) return p; }
        p++;
    }
    return NULL;
}

static int jget_b(const char *start,const char *end,
                   const char *key,char *buf,int bsz){
    char k[128]; snprintf(k,sizeof(k),"\"%s\":",key);
    size_t klen=strlen(k);
    const char *p=start;
    while(p<end){
        const char *found=strstr(p,k);
        if(!found||found>=end) return 0;
        const char *after=found+klen;
        while(after<end&&*after==' ') after++;
        if(after>=end||*after!='"'){ p=found+1; continue; }
        after++; int i=0;
        while(after<end&&*after&&*after!='"'&&i<bsz-1){
            if(*after=='\\') after++;
            if(after<end) buf[i++]=*after++;
        }
        buf[i]=0; return 1;
    }
    return 0;
}

static int jgeti(const char *j,const char *key){
    char k[128]; snprintf(k,sizeof(k),"\"%s\":",key);
    const char *p=strstr(j,k); if(!p) return 0;
    p+=strlen(k); while(*p==' ')p++; return atoi(p);
}

/* parse a JSON string array ["a","b","c"] into buf[][sz], returns count */
static int parse_str_array(const char *arr_start,const char *arr_end,
                            char buf[][MAX_FIELD_VAL],int max_items){
    int n=0;
    const char *p=arr_start;
    while(p<arr_end&&n<max_items){
        while(p<arr_end&&*p!='"'&&*p!=']') p++;
        if(p>=arr_end||*p==']') break;
        p++; /* skip opening quote */
        int i=0;
        while(p<arr_end&&*p&&*p!='"'&&i<MAX_FIELD_VAL-1){
            if(*p=='\\') p++;
            if(p<arr_end) buf[n][i++]=*p++;
        }
        buf[n][i]=0; n++;
        if(p<arr_end&&*p=='"') p++;
    }
    return n;
}

/* parse field labels (narrower buffer) */
static int parse_label_array(const char *arr_start,const char *arr_end,
                              char buf[][MAX_FIELD_LABEL],int max_items){
    int n=0;
    const char *p=arr_start;
    while(p<arr_end&&n<max_items){
        while(p<arr_end&&*p!='"'&&*p!=']') p++;
        if(p>=arr_end||*p==']') break;
        p++;
        int i=0;
        while(p<arr_end&&*p&&*p!='"'&&i<MAX_FIELD_LABEL-1){
            if(*p=='\\') p++;
            if(p<arr_end) buf[n][i++]=*p++;
        }
        buf[n][i]=0; n++;
        if(p<arr_end&&*p=='"') p++;
    }
    return n;
}

/* ─── persistence ───────────────────────────────────────────── */
static void save(void){
    FILE *f=fopen(DATA_FILE,"w"); if(!f) return;
    fputs("{\n\"labels\":{\n",f);
    fputs("  \"entry\":",f);    jstr(f,entry_label);    fputs(",\n",f);
    fputs("  \"entries\":",f);  jstr(f,entries_label);  fputs(",\n",f);
    fputs("  \"session\":",f);  jstr(f,session_label);  fputs(",\n",f);
    fputs("  \"sessions\":",f); jstr(f,sessions_label); fputs(",\n",f);
    fputs("  \"was\":",f);      jstr(f,was_label);      fputs(",\n",f);
    fputs("  \"now\":",f);      jstr(f,now_label);      fputs(",\n",f);
    fputs("  \"fields\":[",f);
    for(int i=0;i<field_count;i++){
        jstr(f,field_labels[i]);
        if(i<field_count-1) fputc(',',f);
    }
    fputs("]\n},\n",f);

    fputs("\"entries\":[\n",f);
    for(int i=0;i<ent_n;i++){
        fputs("{",f);
        fputs("\"name\":",f); jstr(f,entries[i].name);
        fputs(",\"cat\":",f);  jstr(f,entries[i].cat);
        fputs(",\"fields\":[",f);
        for(int fi=0;fi<field_count;fi++){
            jstr(f,entries[i].fields[fi]);
            if(fi<field_count-1) fputc(',',f);
        }
        fputs("]}",f);
        fprintf(f,"%s\n",i<ent_n-1?",":"");
    }
    fputs("],\n\"sessions\":[\n",f);
    for(int b=0;b<ses_n;b++){
        fputs("{",f); fputs("\"date\":",f); jstr(f,sessions[b].date);
        fputs(",\"items\":[\n",f);
        for(int i=0;i<sessions[b].count;i++){
            SItem *it=&sessions[b].items[i];
            fputs("{",f); fputs("\"name\":",f); jstr(f,it->name);
            fputs(",\"was\":[",f);
            for(int fi=0;fi<field_count;fi++){
                jstr(f,it->was[fi]); if(fi<field_count-1) fputc(',',f);
            }
            fputs("],\"now\":[",f);
            for(int fi=0;fi<field_count;fi++){
                jstr(f,it->now[fi]); if(fi<field_count-1) fputc(',',f);
            }
            fprintf(f,"],\"done\":%d}%s\n",it->done,i<sessions[b].count-1?",":"");
        }
        fprintf(f,"]}%s\n",b<ses_n-1?",":"");
    }
    fputs("]\n}\n",f); fclose(f);
}

static void load(void){
    FILE *f=fopen(DATA_FILE,"r"); if(!f) return;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc(sz+1); if(!buf){fclose(f);return;}
    size_t rd=fread(buf,1,sz,f); buf[rd]=0; fclose(f);

    const char *after_labels=buf;
    const char *lsec=strstr(buf,"\"labels\":");
    if(lsec){
        const char *lb=strchr(lsec,'{');
        if(lb){
            const char *le=find_close_brace(lb);
            if(le){
                after_labels=le+1;
                jget_b(lb,le,"entry",    entry_label,   MAX_LABEL);
                jget_b(lb,le,"entries",  entries_label, MAX_LABEL);
                jget_b(lb,le,"session",  session_label,  MAX_LABEL);
                jget_b(lb,le,"sessions", sessions_label, MAX_LABEL);
                jget_b(lb,le,"was",      was_label,     MAX_LABEL);
                jget_b(lb,le,"now",      now_label,     MAX_LABEL);
                const char *fa=strstr(lb,"\"fields\":");
                if(fa&&fa<le){
                    const char *ab=strchr(fa,'[');
                    const char *ae=ab?find_close_bracket(ab):NULL;
                    if(ab&&ae)
                        field_count=parse_label_array(ab+1,ae,field_labels,MAX_FIELDS);
                }
            }
        }
    }

    const char *ep=strstr(after_labels,"\"entries\":");
    const char *sp=strstr(after_labels,"\"sessions\":");
    if(ep&&sp){
        const char *arr=strchr(ep,'['); if(arr) arr++;
        const char *p=arr?arr:ep;
        while(p&&p<sp&&ent_n<MAX_ENTRIES){
            const char *ob=strchr(p,'{'); if(!ob||ob>=sp) break;
            const char *cb=find_close_brace(ob); if(!cb||cb>=sp) break;
            Entry *g=&entries[ent_n];
            jget_b(ob,cb,"name",g->name,MAX_NAME);
            jget_b(ob,cb,"cat", g->cat, MAX_CAT);
            const char *fa=strstr(ob,"\"fields\":");
            if(fa&&fa<cb){
                const char *ab=strchr(fa,'[');
                const char *ae=ab?find_close_bracket(ab):NULL;
                if(ab&&ae) parse_str_array(ab+1,ae,g->fields,MAX_FIELDS);
            }
            if(strlen(g->name)>0) ent_n++;
            p=cb+1;
        }
    }

    if(sp){
        const char *arr=strchr(sp,'['); if(arr) arr++;
        const char *p=arr?arr:sp;
        while(p&&*p&&ses_n<MAX_SESSIONS){
            const char *ob=strchr(p,'{'); if(!ob) break;
            const char *cb=find_close_brace(ob); if(!cb) break;
            Session *sk=&sessions[ses_n];
            jget_b(ob,cb,"date",sk->date,32);
            const char *isec=strstr(ob,"\"items\":");
            if(isec&&isec<cb){
                const char *iarr=strchr(isec,'['); if(iarr) iarr++;
                const char *ip=iarr?iarr:isec;
                while(ip&&ip<cb&&sk->count<MAX_SESSION_ITEMS){
                    const char *iob=strchr(ip,'{'); if(!iob||iob>=cb) break;
                    const char *icb=find_close_brace(iob); if(!icb||icb>=cb) break;
                    SItem *it=&sk->items[sk->count];
                    jget_b(iob,icb,"name",it->name,MAX_NAME);
                    const char *wa=strstr(iob,"\"was\":");
                    if(wa&&wa<icb){
                        const char *ab=strchr(wa,'[');
                        const char *ae=ab?find_close_bracket(ab):NULL;
                        if(ab&&ae) parse_str_array(ab+1,ae,it->was,MAX_FIELDS);
                    }
                    const char *na=strstr(iob,"\"now\":");
                    if(na&&na<icb){
                        const char *ab=strchr(na,'[');
                        const char *ae=ab?find_close_bracket(ab):NULL;
                        if(ab&&ae) parse_str_array(ab+1,ae,it->now,MAX_FIELDS);
                    }
                    it->done=jgeti(iob,"done");
                    if(strlen(it->name)>0) sk->count++;
                    ip=icb+1;
                }
            }
            ses_n++; p=cb+1;
        }
    }
    free(buf);
}

/* ─── input ─────────────────────────────────────────────────── */
static void do_readline(const char *prompt,char *buf,int sz){
    raw_off(); printf(SHOW_CUR"%s"FG_STONE,prompt); fflush(stdout);
    if(fgets(buf,sz,stdin)){ int l=strlen(buf); if(l>0&&buf[l-1]=='\n') buf[l-1]=0; }
    else buf[0]=0;
    raw_on(); printf(HIDE_CUR);
}
static void today(char *out,int sz){
    time_t t=time(NULL); struct tm *tm=localtime(&t); strftime(out,sz,"%Y-%m-%d  %H:%M",tm);
}

/* ─── first-run setup ───────────────────────────────────────── */
static void first_run_setup(void){
    printf(CLR"\n");
    printf(FG_SAGE BOLD"  ╭── welcome to your cornucopia ──╮\n\n"RESET);
    printf(FG_SMOKE"  Quick setup before you begin. Press enter to keep the default [ ].\n\n"RESET);

    char tmp[MAX_LABEL];

    printf(FG_STONE"  Name of the individual items?\n"RESET);
    printf(FG_SMOKE"  (ingredient, title, part)\n"RESET);
    do_readline("  entry name    [ingredient] : ",tmp,MAX_LABEL);
    if(strlen(tmp)) strncpy(entry_label,tmp,MAX_LABEL-1);

    printf(FG_STONE"\n  Name of the main list?\n"RESET);
    printf(FG_SMOKE"  (pantry, library, workshop)\n"RESET);
    do_readline("  list name     [pantry]     : ",tmp,MAX_LABEL);
    if(strlen(tmp)) strncpy(entries_label,tmp,MAX_LABEL-1);

    printf(FG_STONE"\n  Name of a single log event?\n"RESET);
    printf(FG_SMOKE"  (basket, session, run)\n"RESET);
    do_readline("  session name  [basket]     : ",tmp,MAX_LABEL);
    if(strlen(tmp)){
        strncpy(session_label,tmp,MAX_LABEL-1);
        snprintf(sessions_label,MAX_LABEL,"%ss",tmp);
    }

    printf(FG_STONE"\n  What fields do you want to track per entry?\n"RESET);
    printf(FG_SMOKE"  Enter one field name at a time. Leave blank to finish.\n"
                   "  (quantity, location, status, rating)\n\n"RESET);
    field_count=0;
    while(field_count<MAX_FIELDS){
        char prompt[64];
        snprintf(prompt,sizeof(prompt),"  field %d : ",field_count+1);
        do_readline(prompt,tmp,MAX_FIELD_LABEL);
        if(!strlen(tmp)) break;
        strncpy(field_labels[field_count],tmp,MAX_FIELD_LABEL-1);
        field_count++;
    }
    if(!field_count){
        strncpy(field_labels[0],"quantity",MAX_FIELD_LABEL-1);
        field_count=1;
        printf(FG_SMOKE"  (defaulting to: quantity)\n"RESET);
    }

    printf(FG_STONE"\n  Labels for the before/after snapshot per session.\n"RESET);
    printf(FG_SMOKE"  before [was] : "RESET);
    raw_off(); printf(SHOW_CUR FG_STONE); fflush(stdout);
    if(fgets(tmp,MAX_LABEL,stdin)){int l=strlen(tmp);if(l>0&&tmp[l-1]=='\n')tmp[l-1]=0;}
    raw_on(); printf(HIDE_CUR);
    if(strlen(tmp)) strncpy(was_label,tmp,MAX_LABEL-1);

    printf(FG_SMOKE"  after  [now] : "RESET);
    raw_off(); printf(SHOW_CUR FG_STONE); fflush(stdout);
    if(fgets(tmp,MAX_LABEL,stdin)){int l=strlen(tmp);if(l>0&&tmp[l-1]=='\n')tmp[l-1]=0;}
    raw_on(); printf(HIDE_CUR);
    if(strlen(tmp)) strncpy(now_label,tmp,MAX_LABEL-1);

    printf("\n"FG_SAGE BOLD"  ready. let's go.\n\n"RESET);
    sleep(1);
    save();
}

/* ═══════════════════════════════════════════════════
   ENTRIES VIEW
═══════════════════════════════════════════════════ */
static void draw_entries(void){
    int cols=tcols();
    char title[MAX_LABEL*2];
    spaced_title(entries_label,title,sizeof(title));

    printf(CLR HIDE_CUR);
    hline("╭","─","╮",cols,FG_SAGE BOLD);
    centered(title,cols,FG_SAGE BOLD);
    hline("╰","─","╯",cols,FG_SAGE BOLD);
    printf("\n");

    if(!ent_n){
        char msg[256];
        snprintf(msg,sizeof(msg),"  (empty — press n to add a %s)",entry_label);
        printf(DIM FG_SMOKE"%s\n"RESET,msg);
    } else {
        char seen[MAX_CATEGORIES][MAX_CAT]; int nc=0;
        for(int i=0;i<ent_n;i++){
            int f=0; for(int c=0;c<nc;c++) if(!strcasecmp(seen[c],entries[i].cat)){f=1;break;}
            if(!f&&nc<MAX_CATEGORIES) strncpy(seen[nc++],entries[i].cat,MAX_CAT-1);
        }
        for(int c=0;c<nc;c++){
            printf("\n"FG_SAGE BOLD"  · %s\n"RESET,seen[c]);
            int box_w=cols-4;
            int inner=box_w-3;
            hline("  ╭","─","╮",box_w,FG_SMOKE);
            for(int i=0;i<ent_n;i++){
                if(strcasecmp(entries[i].cat,seen[c])) continue;
                int name_w=28;
                printf(FG_SMOKE"  │ "RESET FG_STONE"%-*s"RESET,name_w,entries[i].name);
                int used=name_w;
                for(int fi=0;fi<field_count;fi++){
                    int chunk=strlen(field_labels[fi])+2+16;
                    if(used+chunk>inner) break;
                    printf(DIM FG_SMOKE"%s: "RESET FG_SAND"%-16s"RESET,
                           field_labels[fi],entries[i].fields[fi]);
                    used+=chunk;
                }
                int rem=inner-used; if(rem<0)rem=0;
                printf("%*s"FG_SMOKE"│"RESET"\n",rem,"");
            }
            hline("  ╰","─","╯",box_w,FG_SMOKE);
        }
    }

    printf("\n"DIM FG_SMOKE"  ──────────────────────────────────────────\n"RESET);
    printf("  "BOLD FG_SAND"n"RESET FG_SMOKE" add %s    "
               BOLD FG_SAND"b"RESET FG_SMOKE" %s    "
               BOLD FG_SAND"q"RESET FG_SMOKE" quit\n"RESET,
           entry_label,sessions_label);
}

static void add_entry(void){
    if(ent_n>=MAX_ENTRIES) return;
    Entry *g=&entries[ent_n]; memset(g,0,sizeof(*g));
    char header[128]; snprintf(header,sizeof(header),"  ╭── new %s ──╮",entry_label);
    printf(CLR"\n"FG_SAGE BOLD"%s\n\n"RESET,header);

    char prompt[128];
    do_readline("  name       : ",g->name,MAX_NAME);
    if(!strlen(g->name)) return;
    for(int fi=0;fi<field_count;fi++){
        snprintf(prompt,sizeof(prompt),"  %-12s : ",field_labels[fi]);
        do_readline(prompt,g->fields[fi],MAX_FIELD_VAL);
    }
    do_readline("  category   : ",g->cat,MAX_CAT);
    if(!strlen(g->cat)) strncpy(g->cat,"General",MAX_CAT-1);
    ent_n++; save();
}

/* ═══════════════════════════════════════════════════
   SESSIONS VIEW
═══════════════════════════════════════════════════ */
static void draw_sessions(void){
    int cols=tcols();
    char title[MAX_LABEL*2];
    spaced_title(sessions_label,title,sizeof(title));

    printf(CLR HIDE_CUR);
    hline("╭","─","╮",cols,FG_SLATE BOLD);
    centered(title,cols,FG_SLATE BOLD);
    hline("╰","─","╯",cols,FG_SLATE BOLD);
    printf("\n");

    if(!ses_n){ printf(DIM FG_SMOKE"  (no %s yet)\n"RESET,sessions_label); }
    else{
        for(int b=ses_n-1;b>=0;b--){
            int cur=(b==cur_ses);
            printf(FG_SAND BOLD"  %s %s\n"RESET,cur?"▶":"  ",sessions[b].date);
            for(int i=0;i<sessions[b].count;i++){
                SItem *it=&sessions[b].items[i];
                printf(FG_SMOKE"    │ "RESET);
                if(it->done)
                    printf(DIM STRIKE FG_SMOKE"%-22s"RESET, it->name);
                else
                    printf(FG_STONE"%-22s"RESET, it->name);
                for(int fi=0;fi<field_count;fi++){
                    if(!it->changed[fi]) continue;
                    if(it->done)
                        printf(DIM FG_SMOKE"  %s: %s→%s"RESET,
                               field_labels[fi],it->was[fi],it->now[fi]);
                    else
                        printf("  "DIM FG_SMOKE"%s: "RESET
                               FG_CLAY"%-10s"RESET"→"FG_SLATE"%s"RESET,
                               field_labels[fi],it->was[fi],it->now[fi]);
                }
                printf("\n");
            }
            printf("\n");
        }
    }

    printf(DIM FG_SMOKE"  ──────────────────────────────────────────\n"RESET);
    printf("  "BOLD FG_SAND"n"RESET FG_SMOKE" new %s    "
               BOLD FG_SAND"ESC"RESET FG_SMOKE" back\n"RESET,session_label);
}

/* ═══════════════════════════════════════════════════
   SELECTION MODE
═══════════════════════════════════════════════════ */
static void draw_select(int cursor){
    int cols=tcols();
    char header[256];
    snprintf(header,sizeof(header),
             "select %s    ↑↓ move   enter log   esc done",entry_label);

    printf(CLR HIDE_CUR);
    hline("╭","─","╮",cols,FG_SAND BOLD);
    centered(header,cols,FG_SAND BOLD);
    hline("╰","─","╯",cols,FG_SAND BOLD);

    if(cur_ses>=0&&sessions[cur_ses].count>0){
        printf("\n"FG_SAGE BOLD"  in %s:\n"RESET,session_label);
        for(int i=0;i<sessions[cur_ses].count;i++){
            SItem *it=&sessions[cur_ses].items[i];
            printf(FG_SAGE"    ✓ "RESET FG_STONE"%-22s"RESET,it->name);
            for(int fi=0;fi<field_count;fi++){
                if(!it->changed[fi]) continue;
                printf("  "DIM FG_SMOKE"%s: "RESET
                       FG_CLAY"%-10s"RESET"→"FG_SLATE"%s"RESET,
                       field_labels[fi],it->was[fi],it->now[fi]);
            }
            printf("\n");
        }
    }

    printf("\n"FG_SMOKE BOLD"  %s:\n"RESET,entries_label);
    for(int i=0;i<ent_n;i++){
        if(i==cursor){
            printf(FG_SAND BOLD"  ▶ %-26s"RESET,entries[i].name);
            for(int fi=0;fi<field_count;fi++)
                printf(FG_SAND" %s: %-12s"RESET,field_labels[fi],entries[i].fields[fi]);
            printf("\n");
        } else {
            printf("    "FG_STONE"%-26s"RESET,entries[i].name);
            for(int fi=0;fi<field_count;fi++)
                printf(DIM FG_SMOKE" %s: %-12s"RESET,field_labels[fi],entries[i].fields[fi]);
            printf("\n");
        }
    }
}

static void session_mode(void){
    if(!ent_n){
        char msg[128]; snprintf(msg,sizeof(msg),"\n  no %ss yet.\n",entry_label);
        printf(CLR FG_CLAY"%s"RESET,msg); sleep(1); return;
    }

    char ds[32]; today(ds,sizeof(ds));
    cur_ses=ses_n; memset(&sessions[ses_n],0,sizeof(Session));
    strncpy(sessions[ses_n].date,ds,32); ses_n++;

    int cursor=0;
    while(1){
        draw_select(cursor);
        int k=gkey();
        if(k==KEY_UP&&cursor>0) cursor--;
        if(k==KEY_DN&&cursor<ent_n-1) cursor++;
        if(k==KEY_ES){ save(); break; }
        if(k==KEY_EN){
            Session *sk=&sessions[cur_ses]; if(sk->count>=MAX_SESSION_ITEMS) continue;
            SItem *it=&sk->items[sk->count]; memset(it,0,sizeof(*it));
            strncpy(it->name,entries[cursor].name,MAX_NAME-1);

            printf("\n"FG_SLATE"  logging: "RESET BOLD FG_STONE"%s\n"RESET,it->name);
            printf(FG_SMOKE"  Press enter to keep current value.\n\n"RESET);

            int any_done=0;
            char prompt[128];
            for(int fi=0;fi<field_count;fi++){
                printf(FG_SMOKE"  current %-12s : "RESET FG_SAND"%s\n"RESET,
                       field_labels[fi],entries[cursor].fields[fi]);
                snprintf(prompt,sizeof(prompt),"  new     %-12s : ",field_labels[fi]);
                char tmp[MAX_FIELD_VAL]={0};
                do_readline(prompt,tmp,MAX_FIELD_VAL);

                /* snapshot was value always */
                strncpy(it->was[fi],entries[cursor].fields[fi],MAX_FIELD_VAL-1);

                if(!strlen(tmp)){
                    /* no change — copy current, mark unchanged */
                    strncpy(it->now[fi],entries[cursor].fields[fi],MAX_FIELD_VAL-1);
                    it->changed[fi]=0;
                } else {
                    strncpy(it->now[fi],tmp,MAX_FIELD_VAL-1);
                    it->changed[fi]=1;
                    int done_val=(!strcasecmp(tmp,"0")||!strcasecmp(tmp,"none")||
                                  !strcasecmp(tmp,"done")||!strcasecmp(tmp,"empty"));
                    if(done_val) any_done=1;
                    strncpy(entries[cursor].fields[fi],tmp,MAX_FIELD_VAL-1);
                }
                printf("\n");
            }
            it->done=any_done;
            sk->count++;
            save();
        }
    }
}

/* ─── main ──────────────────────────────────────────────────── */
typedef enum { VE, VS } View;
int main(void){
    load();
    if(ent_n==0&&ses_n==0){
        FILE *check=fopen(DATA_FILE,"r");
        if(!check){ raw_on(); first_run_setup(); }
        else fclose(check);
    }
    raw_on();
    View v=VE;
    while(1){
        if(v==VE){
            draw_entries(); int k=gkey();
            if(k=='q'||k=='Q') break;
            if(k=='n'||k=='N') add_entry();
            if(k=='b'||k=='B') v=VS;
        } else {
            draw_sessions(); int k=gkey();
            if(k==KEY_ES) v=VE;
            if(k=='n'||k=='N') session_mode();
        }
    }
    raw_off();
    printf(CLR SHOW_CUR FG_SAGE"  famine is upon us.\n"RESET);
    return 0;
}