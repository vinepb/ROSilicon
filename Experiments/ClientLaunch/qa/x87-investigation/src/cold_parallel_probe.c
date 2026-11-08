/* Owned first-entry / prewarmed cross-modification stress. No game, remote
 * target, guest trap or exception handler. This is not an architectural
 * cache-ordering conformance test. Four consumers enter distinct own code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WORKERS=4, CYCLES=8, TAG=0x543210, MAX_ROUNDS=64 };
struct state { volatile LONG release,ticks,math,seen,errors,ever,checksum; };
_Static_assert(sizeof(void *)==4, "PE32 fixture only");
_Static_assert(offsetof(struct state,seen)==12, "seen offset");
_Static_assert(offsetof(struct state,errors)==16, "errors offset");
_Static_assert(offsetof(struct state,ever)==20, "ever offset");
_Static_assert(offsetof(struct state,checksum)==24, "checksum offset");
typedef int (__cdecl *function)(struct state *);
struct fixture;
struct worker {
    struct fixture *fixture;
    struct state state;
    unsigned id,calls;
    int last_result;
};
struct fixture {
    volatile LONG phase,done,abort;
    unsigned rounds;
    function *entries;
    struct worker workers[WORKERS];
};
enum operation { IDLE,SAME,CHANGED,NEIGHBOR,MODE_COUNT };
static const char *modes[]={"idle","same-byte","changed-byte","neighbor-changed"};
static LONG read_long(volatile LONG *p) { return InterlockedCompareExchange(p,0,0); }
static uint32_t seed(unsigned slot) { return UINT32_C(0x12345678)+slot; }
static uint32_t checksum(unsigned slot) {
    return seed(slot) + 128u*UINT32_C(0x01010101) + 128u*127u/2u;
}
static void emit(unsigned char *p,size_t *n,const void *bytes,size_t size) { memcpy(p+*n,bytes,size);*n+=size; }
static size_t make_code(unsigned char *p,int x87,unsigned slot,size_t *version) {
    size_t n=0;
    const unsigned char argument[]={0x8b,0x4c,0x24,4};emit(p,&n,argument,sizeof(argument));
    const size_t loop=n;
    memset(p+n,0x90,16);n+=16;
    p[n++]=0xb8;*version=n;const uint32_t v=1;emit(p,&n,&v,4);
    const unsigned char observe[]={0x83,0xf8,1,0x74,9,0x83,0xf8,2,0x74,4,
        0xf0,0xff,0x41,16,0xf0,0x09,0x41,12,0xf0,0x09,0x41,20};
    emit(p,&n,observe,sizeof(observe));
    p[n++]=0xb8;uint32_t value=seed(slot);emit(p,&n,&value,4);
    for (unsigned i=0;i<128;++i) { p[n++]=0x05;value=UINT32_C(0x01010101)+i;emit(p,&n,&value,4); }
    const unsigned char store[]={0x89,0x41,24};emit(p,&n,store,sizeof(store));
    p[n++]=0x3d;value=checksum(slot);emit(p,&n,&value,4);
    const unsigned char check[]={0x74,4,0xf0,0xff,0x41,16};emit(p,&n,check,sizeof(check));
    if (x87) {
        const unsigned char math[]={0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,0xdb,0x59,8};emit(p,&n,math,sizeof(math));
    } else {
        const unsigned char math[]={0xb8,1,0,0,0,0x83,0xc0,1,0x89,0x41,8};emit(p,&n,math,sizeof(math));
    }
    const unsigned char finish[]={0x83,0x79,8,2,0x74,4,0xf0,0xff,0x41,16,
        0xf0,0xff,0x41,4,0x83,0x39,0};emit(p,&n,finish,sizeof(finish));
    p[n++]=0x0f;p[n++]=0x84;const int32_t back=(int32_t)loop-(int32_t)(n+4);emit(p,&n,&back,4);
    p[n++]=0xb8;value=TAG+slot;emit(p,&n,&value,4);p[n++]=0xc3;
    return n;
}
static int query(const void *p) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(p,&info,sizeof(info))==sizeof(info) && info.State==MEM_COMMIT && info.Protect==PAGE_EXECUTE_READWRITE;
}
static DWORD WINAPI consume(void *parameter) {
    struct worker *w=parameter;struct fixture *f=w->fixture;
    const ULONGLONG deadline=GetTickCount64()+18000;
    for (unsigned round=0;round<f->rounds;++round) {
        while (read_long(&f->phase)<(LONG)round+1) {
            if (read_long(&f->abort) || GetTickCount64()>=deadline) return 88;
            Sleep(0);
        }
        if (read_long(&f->abort)) return 88;
        const unsigned slot=round*WORKERS+w->id;
        w->last_result=f->entries[slot](&w->state);++w->calls;
        const int valid=w->last_result==TAG+(int)slot && w->state.math==2 &&
            !w->state.errors && (uint32_t)w->state.checksum==checksum(slot);
        InterlockedIncrement(&f->done);
        if (!valid) return 86;
    }
    return 0;
}
static int all_alive(HANDLE *handles) {
    for (unsigned i=0;i<WORKERS;++i) if (WaitForSingleObject(handles[i],0)!=WAIT_TIMEOUT) return 0;
    return 1;
}
static int wait_seen(struct fixture *f,HANDLE *handles,LONG wanted,ULONGLONG deadline) {
    for (;;) {
        int ready=1;
        for (unsigned i=0;i<WORKERS;++i) {
            struct state *s=&f->workers[i].state;
            if (read_long(&s->errors)) return 0;
            if (!(read_long(&s->seen)&wanted) || !read_long(&s->ticks)) ready=0;
        }
        if (ready) return 1;
        if (GetTickCount64()>=deadline || !all_alive(handles)) return 0;
        Sleep(1);
    }
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc!=5 || (strcmp(argv[2],"integer") && strcmp(argv[2],"x87")) ||
        (strcmp(argv[3],"first") && strcmp(argv[3],"warm"))) return 2;
    char *end;unsigned long got=strtoul(argv[4],&end,10);
    if (!*argv[4] || *end || got<1 || got>MAX_ROUNDS) return 2;
    const unsigned rounds=(unsigned)got;unsigned mode=MODE_COUNT;
    for (unsigned i=0;i<MODE_COUNT;++i) if (!strcmp(argv[1],modes[i])) mode=i;
    if (mode==MODE_COUNT) return 2;
    const int warm=!strcmp(argv[3],"warm"),x87=!strcmp(argv[2],"x87");
    SYSTEM_INFO info;GetSystemInfo(&info);const size_t page=info.dwPageSize,g=info.dwAllocationGranularity;
    if (page<4096 || g<65536 || g%page) return 87;
    const unsigned slots=rounds*WORKERS;const size_t total=slots*2*g;
    unsigned char *mapping=VirtualAlloc((void *)(uintptr_t)0x25000000,total,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    unsigned char *expected=malloc(total);function *entries=malloc(slots*sizeof(*entries));
    if (!mapping || mapping!=(void *)(uintptr_t)0x25000000 || !expected || !entries) return 87;
    size_t version_offset=0,code_size=0;unsigned char bytes[2048];
    for (unsigned slot=0;slot<slots;++slot) {
        size_t offset;size_t n=make_code(bytes,x87,slot,&offset);
        if (n>sizeof(bytes) || n+32>page || (slot && (offset!=version_offset || n!=code_size))) return 87;
        version_offset=offset;code_size=n;unsigned char *address=mapping+slot*2*g;
        memcpy(address+32,bytes,n);entries[slot]=(function)(void *)(address+32);
        DWORD previous;
        if (!VirtualProtect(address,page,PAGE_EXECUTE_READWRITE,&previous) ||
            !VirtualProtect(address+g,page,PAGE_EXECUTE_READWRITE,&previous) ||
            !FlushInstructionCache(GetCurrentProcess(),address,page)) return 87;
    }
    memcpy(expected,mapping,total);
    printf("BEGIN case=cold-parallel mode=%s kind=%s temperature=%s rounds=%u workers=%u code-size=%u version-offset=%u\n",
        modes[mode],argv[2],argv[3],rounds,WORKERS,(unsigned)code_size,(unsigned)version_offset);
    printf("CODE ");for (size_t i=0;i<code_size;++i) printf("%02x",mapping[32+i]);printf("\n");
    unsigned warm_calls=0;
    if (warm) for (unsigned slot=0;slot<slots;++slot) {
        struct state s={0};s.release=1;
        const int result=entries[slot](&s);++warm_calls;
        if (result!=TAG+(int)slot || s.math!=2 || s.errors || s.ever!=1 || (uint32_t)s.checksum!=checksum(slot)) return 86;
    }
    struct fixture f={0};f.rounds=rounds;f.entries=entries;HANDLE handles[WORKERS]={0};
    for (unsigned i=0;i<WORKERS;++i) {
        f.workers[i].id=i;f.workers[i].fixture=&f;
        handles[i]=CreateThread(NULL,0,consume,&f.workers[i],0,NULL);
        if (!handles[i]) return 87;
    }
    Sleep(300);
    unsigned failures=0,batches=0,writes=0,changed=0,flushes=0,before_first_tick=0,progress=0,version_checks=0;
    DWORD api_error=0;const ULONGLONG deadline=GetTickCount64()+15000;
    for (unsigned round=0;round<rounds && !failures;++round) {
        for (unsigned i=0;i<WORKERS;++i) { memset(&f.workers[i].state,0,sizeof(struct state));f.workers[i].state.math=-1; }
        InterlockedExchange(&f.done,0);InterlockedExchange(&f.phase,(LONG)round+1);
        for (unsigned cycle=0;cycle<CYCLES && !failures;++cycle) {
            const unsigned char value=mode==SAME ? 1 : (cycle%2 ? 1 : 2);
            LONG ticks[WORKERS];
            for (unsigned i=0;i<WORKERS;++i) {
                ticks[i]=read_long(&f.workers[i].state.ticks);
                InterlockedExchange(&f.workers[i].state.seen,0);
                if (mode!=IDLE) {
                    unsigned char *base=mapping+(round*WORKERS+i)*2*g;
                    unsigned char *target=mode==NEIGHBOR ? base+g+32 : base+32+version_offset;
                    const size_t offset=(size_t)(target-mapping);SIZE_T written=0;
                    if (!read_long(&f.workers[i].state.ticks)) ++before_first_tick;
                    if (!WriteProcessMemory(GetCurrentProcess(),target,&value,1,&written) || written!=1) { api_error=GetLastError();failures|=1;break; }
                    ++writes;changed+=expected[offset]!=value;expected[offset]=value;
                    if (!FlushInstructionCache(GetCurrentProcess(),target,1)) { api_error=GetLastError();failures|=1;break; }
                    ++flushes;
                    if (*target!=value || !query(base) || !query(base+g)) { failures|=2;break; }
                }
            }
            const LONG wanted=mode==CHANGED ? value : 1;
            if (!failures && !wait_seen(&f,handles,wanted,GetTickCount64()+2000)) { failures|=4;break; }
            if (failures) break;
            version_checks+=WORKERS;Sleep(1);
            for (unsigned i=0;i<WORKERS;++i) if (read_long(&f.workers[i].state.ticks)!=ticks[i]) ++progress;
            if (GetTickCount64()>=deadline) { failures|=8;break; }
        }
        for (unsigned i=0;i<WORKERS;++i) {
            if (read_long(&f.workers[i].state.ever)!=(mode==CHANGED ? 3 : 1) || read_long(&f.workers[i].state.errors)) failures|=16;
            InterlockedExchange(&f.workers[i].state.release,1);
        }
        while (read_long(&f.done)!=WORKERS && !failures) {
            if (GetTickCount64()>=deadline) { failures|=8;break; }
            for (unsigned i=0;i<WORKERS;++i) if (WaitForSingleObject(handles[i],0)!=WAIT_TIMEOUT && f.workers[i].calls<round+1) failures|=32;
            Sleep(1);
        }
        if (!failures) ++batches;
    }
    InterlockedExchange(&f.abort,1);
    for (unsigned i=0;i<WORKERS;++i) InterlockedExchange(&f.workers[i].state.release,1);
    unsigned joined=0,calls=0;DWORD exits=0;
    for (unsigned i=0;i<WORKERS;++i) {
        if (WaitForSingleObject(handles[i],2000)!=WAIT_OBJECT_0) { failures|=64;continue; }
        ++joined;DWORD exit_code;if (!GetExitCodeThread(handles[i],&exit_code)) return 87;exits|=exit_code;
        calls+=f.workers[i].calls;
        if (f.workers[i].calls!=rounds || f.workers[i].last_result!=TAG+(int)((rounds-1)*WORKERS+i) || f.workers[i].state.errors) failures|=128;
        printf("OBS worker id=%u calls=%u result=%d math=%ld checksum=%lu errors=%ld ever=%ld ticks=%lu\n",i,
            f.workers[i].calls,f.workers[i].last_result,f.workers[i].state.math,(DWORD)f.workers[i].state.checksum,
            f.workers[i].state.errors,f.workers[i].state.ever,(DWORD)f.workers[i].state.ticks);
        CloseHandle(handles[i]);
    }
    const int bytes_ok=joined==WORKERS && !memcmp(mapping,expected,total);
    unsigned protection_checks=0;
    for (unsigned slot=0;slot<slots;++slot) if (query(mapping+slot*2*g) && query(mapping+slot*2*g+g)) ++protection_checks;
    const unsigned operations=mode==IDLE ? 0 : rounds*WORKERS*CYCLES;
    const unsigned changes=mode==CHANGED || mode==NEIGHBOR ? operations : 0;
    if (batches!=rounds || joined!=WORKERS || exits || !bytes_ok || protection_checks!=slots || writes!=operations || flushes!=operations ||
        changed!=changes || !progress || version_checks!=rounds*WORKERS*CYCLES || warm_calls!=(warm ? slots : 0)) failures|=256;
    printf("OBS writer batches=%u writes=%u changed=%u flushes=%u before-first-tick=%u progress=%u version-checks=%u bytes-ok=%u protection-checks=%u api-error=%lu\n",
        batches,writes,changed,flushes,before_first_tick,progress,version_checks,bytes_ok,protection_checks,api_error);
    printf("OBS consumer calls=%u warm-calls=%u joined=%u exit=%lu\n",calls,warm_calls,joined,exits);
    if (joined==WORKERS) { free(entries);free(expected);if (!VirtualFree(mapping,0,MEM_RELEASE)) return 87; }
    printf("END case=cold-parallel failure-mask=%u status=%s\n",failures,failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
