/* Diagnostic copy of our owned layout stress; no recovery or remote target. */
#define main layout_original_main
#include "layout_parallel_probe.c"
#undef main

static char *report_text(char *p,const char *s) {
    while (*s) *p++=*s++;
    return p;
}
static char *report_hex(char *p,DWORD value) {
    const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<8;++i) *p++=digits[(value>>(28-4*i))&15];
    return p;
}
static char *report_field(char *p,const char *label,DWORD value) {
    return report_hex(report_text(p,label),value);
}
static LONG WINAPI report_unhandled(EXCEPTION_POINTERS *exception) {
    char buffer[512],*p=report_text(buffer,"OWNED_UNHANDLED");
    EXCEPTION_RECORD *r=exception->ExceptionRecord;
    CONTEXT *c=exception->ContextRecord;
    p=report_field(p," code=",r->ExceptionCode);
    p=report_field(p," address=",(DWORD)(uintptr_t)r->ExceptionAddress);
    p=report_field(p," eip=",c->Eip);
    p=report_field(p," eax=",c->Eax);
    p=report_field(p," ecx=",c->Ecx);
    p=report_field(p," esp=",c->Esp);
    p=report_field(p," thread=",GetCurrentThreadId());
    p=report_field(p," parameters=",r->NumberParameters);
    if (r->NumberParameters>=2) {
        p=report_field(p," access=",(DWORD)r->ExceptionInformation[0]);
        p=report_field(p," fault-address=",(DWORD)r->ExceptionInformation[1]);
    }
    *p++='\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),buffer,(DWORD)(p-buffer),&written,NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(report_unhandled);
    if (argc==2 && !strcmp(argv[1],"--reporter-selfcheck")) {
        void *page=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
        if (!page) return 93;
        char buffer[128],*p=report_field(buffer,"OWNED_EXPECTED_FAULT allocation=",(DWORD)(uintptr_t)page);
        *p++='\n';DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE),buffer,(DWORD)(p-buffer),&written,NULL);
        *(volatile unsigned char *)page=1;
        return 94; /* The deliberate invalid write must not return. */
    }
    return layout_original_main(argc,argv);
}
