/* minimal freestanding runtime for the unicorn PPC64 test */
#include <stddef.h>
#include <stdarg.h>
void ws_lr_log2(const char*f,va_list ap);
void ws_exit(int);
#include <stdint.h>
static long sys3(long n,long a,long b,long c){ register long r0 __asm__("r0")=n; register long r3 __asm__("r3")=a; register long r4 __asm__("r4")=b; register long r5 __asm__("r5")=c;
 __asm__ volatile("sc" : "+r"(r0),"+r"(r3) : "r"(r4),"r"(r5) : "memory","cr0","r6","r7","r8","r9","r10","r11","r12","ctr"); return r3; }
long ws_mprotect(void*p,unsigned long n,int pr){ return sys3(125,(long)p,n,pr); }
void ws_exit(int c){ sys3(234,c,0,0); for(;;); }
#define EXIT_NOP 0
struct exitw { int dummy; };
void out_c(char c){ sys3(4,1,(long)&c,1); }
void out_s(const char *s){ while(*s) out_c(*s++); }
void out_x(unsigned long v){ int i; for(i=60;i>=0;i-=4) out_c("0123456789abcdef"[(v>>i)&15]); }
void out_d(long v){ char b[24]; int n=0; if(v<0){out_c('-');v=-v;} if(!v)out_c('0'); while(v){b[n++]='0'+v%10;v/=10;} while(n)out_c(b[--n]); }
static unsigned char heap[256<<20] __attribute__((aligned(16)));
static size_t hp;
#define NB 40
static void *fl[NB];
static int cls(size_t n){ int c=4; while(((size_t)1<<c)<n) c++; return c; }
void *malloc(size_t n){ int c=cls(n+16); char*q=fl[c]; if(q){ fl[c]=*(void**)(q+16); return q+16; }
 size_t sz=(size_t)1<<c; if(hp+sz>sizeof heap){out_s("OOM\n");ws_exit(2);} q=(char*)heap+hp; hp+=sz; *(size_t*)(q+8)=c; return q+16; }
void free(void*p){ if(!p) return; char*q=(char*)p-16; int c=*(size_t*)(q+8); *(void**)(q+16)=fl[c]; fl[c]=q; }
void *calloc(size_t a,size_t b){ size_t n=a*b; void*p=malloc(n); unsigned char*q=p; size_t i; for(i=0;i<n;i++)q[i]=0; return p; }
void *realloc(void*p,size_t n){ if(!p) return malloc(n); char*h=(char*)p-16; size_t old=((size_t)1<<*(size_t*)(h+8))-16; if(n<=old) return p; void*q=malloc(n); unsigned char*x=q,*y=p; size_t i; for(i=0;i<old;i++)x[i]=y[i]; free(p); return q; }
void *memset(void*d,int c,size_t n){ unsigned char*p=d; while(n--)*p++=c; return d; }
void *memcpy(void*d,const void*s,size_t n){ unsigned char*p=d;const unsigned char*q=s; while(n--)*p++=*q++; return d; }
void *memmove(void*d,const void*s,size_t n){ unsigned char*p=d;const unsigned char*q=s; if(p<q) while(n--)*p++=*q++; else {p+=n;q+=n; while(n--)*--p=*--q;} return d; }
int memcmp(const void*a,const void*b,size_t n){ const unsigned char*p=a,*q=b; while(n--){ if(*p!=*q) return *p-*q; p++;q++;} return 0; }
size_t strlen(const char*s){ size_t n=0; while(s[n])n++; return n; }
int strcmp(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return *(unsigned char*)a-*(unsigned char*)b; }
char *strcpy(char*d,const char*s){ char*r=d; while((*d++=*s++)); return r; }
int snprintf(char*b,size_t n,const char*f,...){ if(n)b[0]=0; return 0; }
int puts(const char*s){ out_s(s); out_c('\n'); return 0; }
void abort(void){ out_s("ABORT\n"); ws_exit(3); }
void __assert_fail(const char*a,const char*b,unsigned c,const char*d){ out_s("ASSERT "); out_s(a); out_c('\n'); abort(); }
static char reent_buf[4096] __attribute__((aligned(16)));
void *__getreent(void){ return reent_buf; }
int fputc(int c, void *f){ out_c(c); return c; }
int isatty(int fd){ return 0; }
void ws_lr_dump(const char*t,const void*p,unsigned n){}
void ws_lr_log(const char*f,...){ va_list ap; va_start(ap,f); ws_lr_log2(f,ap); va_end(ap); }

#include <stdarg.h>
static void pf_num(unsigned long v,int base,int width,char pad,int neg){ char b[24]; int n=0; if(!v) b[n++]='0'; while(v){ b[n++]="0123456789abcdef"[v%base]; v/=base; } if(neg) b[n++]='-'; while(n<width) b[n++]=pad; while(n) out_c(b[--n]); }
static int vpf(const char*f,va_list ap){ while(*f){ if(*f!='%'){ out_c(*f++); continue; } f++; char pad=' '; int w=0,l=0; if(*f=='0'){pad='0';f++;} while(*f>='0'&&*f<='9') w=w*10+*f++-'0'; while(*f=='l'||*f=='z'){l=1;f++;}
  switch(*f){ case 'd': case 'i': { long v=l?va_arg(ap,long):va_arg(ap,int); if(v<0) pf_num(-v,10,w,pad,1); else pf_num(v,10,w,pad,0);} break;
   case 'u': pf_num(l?va_arg(ap,unsigned long):va_arg(ap,unsigned),10,w,pad,0); break;
   case 'x': pf_num(l?va_arg(ap,unsigned long):va_arg(ap,unsigned),16,w,pad,0); break;
   case 'p': out_s("0x"); pf_num((unsigned long)va_arg(ap,void*),16,0,'0',0); break;
   case 's': { const char*p=va_arg(ap,const char*); out_s(p?p:"(null)"); } break;
   case 'c': out_c(va_arg(ap,int)); break; case '%': out_c('%'); break; default: out_c('?'); }
  f++; } return 0; }
int printf(const char*f,...){ va_list ap; va_start(ap,f); vpf(f,ap); va_end(ap); return 0; }
int fprintf(void*s,const char*f,...){ va_list ap; va_start(ap,f); vpf(f,ap); va_end(ap); return 0; }
void ws_lr_log2(const char*f,va_list ap){ out_s("[lr] "); vpf(f,ap); }
char *getenv(const char*n){ return 0; }
int atoi(const char*s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+*s++-'0'; return v; }
void exit(int c){ ws_exit(c); }
void perror(const char*s){ out_s(s); out_c('\n'); }
void *stderr=0,*stdout=0;
