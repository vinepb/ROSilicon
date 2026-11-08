/* Only the child CREATED BY THIS PROGRAM is a WriteProcessMemory target. */
#define main layout_unused_main
#include "layout_parallel_probe.c"
#undef main
#include "exception_reporter.h"

enum { REMOTE_MAGIC=0x524f574e };
struct remote_control {
    DWORD magic,mode,x87,cycles,base,code_size,version_offset;
    volatile LONG ready;
    struct state states[WORKERS];
    int returns[WORKERS];
    LONG child_ok;
};
struct remote_worker { struct remote_control *control;function entry;unsigned id; };
static DWORD WINAPI remote_consume(void *parameter) {
    struct remote_worker *w=parameter;
    w->control->returns[w->id]=w->entry(&w->control->states[w->id]);
    return 0;
}
static int remote_child(const char *name) {
    HANDLE shared=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);
    if (!shared) return 87;
    struct remote_control *r=MapViewOfFile(shared,FILE_MAP_ALL_ACCESS,0,0,sizeof(*r));
    if (!r || r->magic!=REMOTE_MAGIC || r->mode>1 || r->x87>1 || r->cycles<1 || r->cycles>64) return 87;
    SYSTEM_INFO info;GetSystemInfo(&info);size_t g=info.dwAllocationGranularity,page=info.dwPageSize,total=WORKERS*2*g;
    if (g<65536 || page<4096) return 87;
    unsigned char *mapping=VirtualAlloc((void *)(uintptr_t)0x25000000,total,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    unsigned char *expected=malloc(total);
    if (!mapping || mapping!=(void *)(uintptr_t)0x25000000 || !expected) return 87;
    struct remote_worker workers[WORKERS];HANDLE threads[WORKERS];unsigned char code[2048];
    for (unsigned i=0;i<WORKERS;++i) {
        size_t offset,n=make_code(code,r->x87,i,&offset);DWORD old;
        if (n>sizeof(code) || n+32>page) return 87;
        memcpy(mapping+i*2*g+32,code,n);
        if (!VirtualProtect(mapping+i*2*g,page,PAGE_EXECUTE_READWRITE,&old) ||
            !FlushInstructionCache(GetCurrentProcess(),mapping+i*2*g,page)) return 87;
        r->code_size=(DWORD)n;r->version_offset=(DWORD)offset;
        r->states[i].math=-1;
        workers[i]=(struct remote_worker){r,(function)(void *)(mapping+i*2*g+32),i};
    }
    memcpy(expected,mapping,total);r->base=(DWORD)(uintptr_t)mapping;
    for (unsigned i=0;i<WORKERS;++i) {
        threads[i]=CreateThread(NULL,0,remote_consume,&workers[i],0,NULL);
        if (!threads[i]) return 87;
    }
    InterlockedExchange(&r->ready,1);
    unsigned failures=0,joined=0,protections=0;
    for (unsigned i=0;i<WORKERS;++i) {
        if (WaitForSingleObject(threads[i],20000)!=WAIT_OBJECT_0) {failures|=1;continue;}
        ++joined;DWORD exit_code;
        if (!GetExitCodeThread(threads[i],&exit_code) || exit_code) failures|=2;
        struct state *s=&r->states[i];
        if (r->returns[i]!=TAG+(int)i || s->math!=2 || s->errors ||
            (DWORD)s->checksum!=checksum(i) || s->ever!=(r->mode ? 3 : 2) || !s->ticks) failures|=4;
        printf("REMOTE_CHILD worker id=%u result=%d math=%ld checksum=%lu errors=%ld ever=%ld ticks=%lu\n",
            i,r->returns[i],s->math,(DWORD)s->checksum,s->errors,s->ever,(DWORD)s->ticks);
        CloseHandle(threads[i]);
    }
    const unsigned char final_value=r->mode && r->cycles%2 ? 0x90 : 0xb8;
    for (unsigned i=0;i<WORKERS;++i) {
        expected[i*2*g+32+r->version_offset]=final_value;
        protections+=query(mapping+i*2*g);
    }
    const int bytes_ok=joined==WORKERS && !memcmp(mapping,expected,total);
    if (joined!=WORKERS || protections!=WORKERS || !bytes_ok) failures|=8;
    InterlockedExchange(&r->child_ok,!failures);
    printf("REMOTE_CHILD joined=%u protections=%u bytes-ok=%u failure-mask=%u status=%s\n",
        joined,protections,bytes_ok,failures,failures ? "FAIL" : "PASS");
    if (joined==WORKERS) {free(expected);VirtualFree(mapping,0,MEM_RELEASE);}
    UnmapViewOfFile(r);CloseHandle(shared);
    return failures ? 1 : 0;
}
static int remote_parent(int argc,char **argv) {
    if (argc!=4 || (strcmp(argv[1],"same-byte") && strcmp(argv[1],"changed-byte")) ||
        (strcmp(argv[2],"integer") && strcmp(argv[2],"x87"))) return 2;
    char *end;unsigned long cycles=strtoul(argv[3],&end,10);
    if (!*argv[3] || *end || cycles<1 || cycles>64 || (cycles==1 && !strcmp(argv[1],"changed-byte"))) return 2;
    char name[96],module[MAX_PATH],command[MAX_PATH+128];
    snprintf(name,sizeof(name),"Local\\ROSiliconOwnedRemote_%lu",GetCurrentProcessId());
    DWORD path_size=GetModuleFileNameA(NULL,module,sizeof(module));
    if (!path_size || path_size>=sizeof(module)) return 87;
    HANDLE shared=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(struct remote_control),name);
    if (!shared || GetLastError()==ERROR_ALREADY_EXISTS) return 87;
    struct remote_control *r=MapViewOfFile(shared,FILE_MAP_ALL_ACCESS,0,0,sizeof(*r));
    if (!r) return 87;
    memset(r,0,sizeof(*r));r->magic=REMOTE_MAGIC;r->mode=!strcmp(argv[1],"changed-byte");
    r->x87=!strcmp(argv[2],"x87");r->cycles=(DWORD)cycles;
    snprintf(command,sizeof(command),"\"%s\" --child %s",module,name);
    STARTUPINFOA startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION child={0};
    if (!CreateProcessA(module,command,NULL,NULL,TRUE,0,NULL,NULL,&startup,&child)) return 87;
    CloseHandle(child.hThread);
    printf("BEGIN case=remote-layout mode=%s kind=%s cycles=%lu workers=%u child=%lu\n",argv[1],argv[2],cycles,WORKERS,child.dwProcessId);
    const ULONGLONG deadline=GetTickCount64()+15000;
    unsigned failures=0,writes=0,changes=0,flushes=0,progress=0,completed=0,version_checks=0;
    DWORD api_error=0;
    while (!read_long(&r->ready)) {
        if (WaitForSingleObject(child.hProcess,0)!=WAIT_TIMEOUT || GetTickCount64()>=deadline) {failures|=1;break;}
        Sleep(1);
    }
    if (!failures && (r->base!=0x25000000 || r->code_size!=767 || r->version_offset!=32)) failures|=2;
    SYSTEM_INFO info;GetSystemInfo(&info);size_t g=info.dwAllocationGranularity;
    unsigned char previous=0xb8;
    for (unsigned cycle=0;cycle<cycles && !failures;++cycle) {
        const unsigned char value=r->mode && !(cycle%2) ? 0x90 : 0xb8;
        LONG ticks[WORKERS];
        for (unsigned i=0;i<WORKERS;++i) {ticks[i]=read_long(&r->states[i].ticks);InterlockedExchange(&r->states[i].seen,0);}
        for (unsigned i=0;i<WORKERS;++i) {
            SIZE_T written=0;void *target=(void *)(uintptr_t)(r->base+i*2*g+32+r->version_offset);
            if (!WriteProcessMemory(child.hProcess,target,&value,1,&written) || written!=1) {api_error=GetLastError();failures|=4;break;}
            ++writes;changes+=previous!=value;
            if (!FlushInstructionCache(child.hProcess,target,1)) {api_error=GetLastError();failures|=4;break;}
            ++flushes;
        }
        previous=value;
        if (failures) break;
        const LONG wanted=r->mode ? (value==0x90 ? 1 : 2) : 2;
        const ULONGLONG seen_deadline=GetTickCount64()+2000;
        for (;;) {
            int seen=1;
            for (unsigned i=0;i<WORKERS;++i) {
                if (!(read_long(&r->states[i].seen)&wanted) || !read_long(&r->states[i].ticks)) seen=0;
                if (read_long(&r->states[i].errors)) failures|=8;
            }
            if (seen || failures) break;
            if (WaitForSingleObject(child.hProcess,0)!=WAIT_TIMEOUT || GetTickCount64()>=seen_deadline || GetTickCount64()>=deadline) {failures|=16;break;}
            Sleep(1);
        }
        if (failures) break;
        version_checks+=WORKERS;Sleep(1);
        for (unsigned i=0;i<WORKERS;++i) progress+=read_long(&r->states[i].ticks)!=ticks[i];
        ++completed;
    }
    for (unsigned i=0;i<WORKERS;++i) InterlockedExchange(&r->states[i].release,1);
    unsigned forced=0;
    if (WaitForSingleObject(child.hProcess,3000)!=WAIT_OBJECT_0) {
        forced=1;failures|=32;TerminateProcess(child.hProcess,90);WaitForSingleObject(child.hProcess,2000);
    }
    DWORD exit_code=STILL_ACTIVE;
    if (!GetExitCodeProcess(child.hProcess,&exit_code)) {api_error=GetLastError();failures|=64;}
    if (exit_code || !read_long(&r->child_ok) || completed!=cycles || writes!=cycles*WORKERS ||
        flushes!=writes || changes!=(r->mode ? writes : 0) || !progress || version_checks!=cycles*WORKERS) failures|=128;
    printf("REMOTE_PARENT completed=%u writes=%u changed=%u flushes=%u progress=%u version-checks=%u api-error=%lu child-exit=%lu forced=%u child-ok=%ld\n",
        completed,writes,changes,flushes,progress,version_checks,api_error,exit_code,forced,read_long(&r->child_ok));
    printf("END case=remote-layout failure-mask=%u status=%s\n",failures,failures ? "FAIL" : "PASS");
    CloseHandle(child.hProcess);UnmapViewOfFile(r);CloseHandle(shared);
    return failures ? 1 : 0;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(report_unhandled);
    if (argc==2 && !strcmp(argv[1],"--reporter-selfcheck")) {
        void *page=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
        if (!page) return 87;
        printf("OWNED_EXPECTED_FAULT allocation=%08lx\n",(DWORD)(uintptr_t)page);
        *(volatile unsigned char *)page=1;return 94;
    }
    if (argc==3 && !strcmp(argv[1],"--child")) return remote_child(argv[2]);
    return remote_parent(argc,argv);
}
