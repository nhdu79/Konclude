// Minimal live-heap profiler (LD_PRELOAD): records size + backtrace of every live allocation,
// prints the top call stacks by live bytes at exit. Not thread-scalable, just for diagnosis.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEPTH 14
#define PTRCAP (1u<<24)      /* open-addressing table of live pointers */
#define SITECAP (1u<<18)

typedef struct { void* p; size_t sz; uint32_t site; } entry_t;
typedef struct { uint64_t hash; void* frames[DEPTH]; int n; size_t live; size_t count; } site_t;

static void* (*real_malloc)(size_t); static void (*real_free)(void*);
static void* (*real_calloc)(size_t,size_t); static void* (*real_realloc)(void*,size_t);
static entry_t* ptrs; static site_t* sites;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int inhook = 0;
static char bootbuf[1<<20]; static size_t bootpos = 0;

static void init(void) {
  if (real_malloc) return;
  real_malloc = dlsym(RTLD_NEXT,"malloc"); real_free = dlsym(RTLD_NEXT,"free");
  real_calloc = dlsym(RTLD_NEXT,"calloc"); real_realloc = dlsym(RTLD_NEXT,"realloc");
  ptrs = real_calloc(PTRCAP, sizeof(entry_t)); sites = real_calloc(SITECAP, sizeof(site_t));
}
static inline uint32_t ph(void* p){ uint64_t x=(uint64_t)p; x^=x>>33; x*=0xff51afd7ed558ccdULL; x^=x>>33; return (uint32_t)x & (PTRCAP-1); }
static uint32_t site_of(void** fr, int n) {
  uint64_t h=1469598103934665603ULL; for (int i=0;i<n;i++){ h^=(uint64_t)fr[i]; h*=1099511628211ULL; }
  uint32_t i=(uint32_t)h & (SITECAP-1);
  while (sites[i].hash && sites[i].hash!=h) i=(i+1)&(SITECAP-1);
  if (!sites[i].hash){ sites[i].hash=h; memcpy(sites[i].frames,fr,sizeof(void*)*n); sites[i].n=n; }
  return i;
}
static void record(void* p, size_t sz) {
  if (!p || !ptrs || inhook) return;
  inhook=1; void* fr[DEPTH+2]; int n=backtrace(fr,DEPTH+2); inhook=0;
  pthread_mutex_lock(&mu);
  uint32_t s=site_of(fr+2, n>2?n-2:0);
  uint32_t i=ph(p); while (ptrs[i].p && ptrs[i].p!=(void*)1) i=(i+1)&(PTRCAP-1);
  ptrs[i].p=p; ptrs[i].sz=sz; ptrs[i].site=s; sites[s].live+=sz; sites[s].count++;
  pthread_mutex_unlock(&mu);
}
static void forget(void* p) {
  if (!p || !ptrs) return;
  pthread_mutex_lock(&mu);
  uint32_t i=ph(p);
  while (ptrs[i].p) { if (ptrs[i].p==p){ sites[ptrs[i].site].live-=ptrs[i].sz; sites[ptrs[i].site].count--; ptrs[i].p=(void*)1; break; } i=(i+1)&(PTRCAP-1); }
  pthread_mutex_unlock(&mu);
}
void* malloc(size_t sz){ if(!real_malloc){ if(inhook||!dlsym){} } if(!real_malloc){ static int busy=0; if(busy){ void* r=bootbuf+bootpos; bootpos+=(sz+15)&~15ul; return r; } busy=1; init(); busy=0; }
  void* p=real_malloc(sz); record(p,sz); return p; }
void free(void* p){ if((char*)p>=bootbuf && (char*)p<bootbuf+sizeof(bootbuf)) return; if(!real_free) init(); forget(p); real_free(p); }
void* calloc(size_t n,size_t sz){ if(!real_calloc){ void* r=bootbuf+bootpos; bootpos+=((n*sz)+15)&~15ul; memset(r,0,n*sz); return r; } void* p=real_calloc(n,sz); record(p,n*sz); return p; }
void* realloc(void* q,size_t sz){ if(!real_realloc) init(); if((char*)q>=bootbuf && (char*)q<bootbuf+sizeof(bootbuf)){ void* p=malloc(sz); memcpy(p,q,sz); return p; } forget(q); void* p=real_realloc(q,sz); record(p,sz); return p; }

__attribute__((constructor)) static void eager_init(void) { init(); }

static int cmp(const void* a,const void* b){ size_t x=((site_t*)a)->live, y=((site_t*)b)->live; return x<y?1:x>y?-1:0; }
__attribute__((destructor)) static void dump(void) {
  const char* outbase=getenv("HEAPPROF_OUT"); if(!outbase||!sites) return;
  char out[1024]; snprintf(out,sizeof(out),"%s.%d",outbase,(int)getpid());
  inhook=1; pthread_mutex_lock(&mu);
  site_t* copy=real_malloc(sizeof(site_t)*SITECAP); memcpy(copy,sites,sizeof(site_t)*SITECAP);
  pthread_mutex_unlock(&mu);
  qsort(copy,SITECAP,sizeof(site_t),cmp);
  FILE* f=fopen(out,"w");
  for (int k=0;k<60 && copy[k].live>0;k++) {
    fprintf(f,"SITE %d live=%zu count=%zu\n",k,copy[k].live,copy[k].count);
    for (int j=0;j<copy[k].n;j++){ Dl_info di; if(dladdr(copy[k].frames[j],&di)&&di.dli_fname) fprintf(f,"  %s+0x%lx\n",di.dli_fname,(unsigned long)((char*)copy[k].frames[j]-(char*)di.dli_fbase)); }
  }
  fclose(f);
}
