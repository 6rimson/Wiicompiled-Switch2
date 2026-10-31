#include "hbxl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No console or networking is involved. Unexpected platform calls fail. */
uint64_t hbxl_imp[IMP_COUNT];
char HBXL_FS_PORT;
char HBXL_FS_HOST;
static void unexpected(void) { fputs("unexpected platform call\n", stderr); exit(2); }
long hbxl_gettid(void) { unexpected(); return 0; }
void hbxl_svc_sleep(int64_t ns) { (void)ns; unexpected(); }
void hbxl_log(const char *s) { (void)s; unexpected(); }
void hbxl_logf(const char *s, ...) { (void)s; unexpected(); }
void hbxl_ring_logf(const char *s, ...) { (void)s; unexpected(); }
void *hbxl_mmap(size_t n) { (void)n; unexpected(); return NULL; }
int hbxl_munmap(void *p, size_t n) { (void)p; (void)n; unexpected(); return 0; }
long __hbx_syscall(long n, long a, long b, long c, long d, long e, long f) {
    (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; unexpected(); return 0;
}
void hbxl_memcpy(void *d, const void *s, size_t n) { memcpy(d,s,n); }
void hbxl_memset(void *d, int c, size_t n) { memset(d,c,n); }
size_t hbxl_strlen(const char *s) { return strlen(s); }

int main(void) {
    unsigned cases=0;
    const long sizes[]={4096,2,0,1,0x40000000};
    const long third[]={0,1,4096};
    for (unsigned i=0; i<sizeof sizes/sizeof sizes[0]; i++) {
        for (unsigned j=0; j<sizeof third/sizeof third[0]; j++) {
            unsigned char bytes[]={0x55,0x55,0x55,0x55};
            int handled=0;
            long got=hbxl_vfs_syscall(17,(long)(bytes+1),sizes[i],third[j],0,0,0,&handled);
            long expected=sizes[i]<2?-34:2;
            int ok=handled && got==expected && bytes[0]==0x55 && bytes[3]==0x55;
            ok &= sizes[i]<2 ? (bytes[1]==0x55 && bytes[2]==0x55)
                             : (bytes[1]=='/' && bytes[2]==0);
            if (!ok) {
                fprintf(stderr,"FAIL size=%ld third=%ld result=%ld expected=%ld\n",sizes[i],third[j],got,expected);
                return 1;
            }
            cases++;
        }
    }
    printf("PASS: %u getcwd cases (argument order, small buffers, guards)\n",cases);
    return 0;
}
