// O libnx não traz sysconf(); o recompilador do PCSX ReARMed só pergunta o tamanho da página de memória
// (e às vezes o número de núcleos). Respostas do Switch: páginas de 4 KB e 4 núcleos (3 livres para apps).
#include <unistd.h>
#include <errno.h>

long sysconf(int name) {
    switch (name) {
#ifdef _SC_PAGESIZE
        case _SC_PAGESIZE: return 0x1000;
#endif
#if defined(_SC_PAGE_SIZE) && (!defined(_SC_PAGESIZE) || _SC_PAGE_SIZE != _SC_PAGESIZE)
        case _SC_PAGE_SIZE: return 0x1000;
#endif
#ifdef _SC_NPROCESSORS_ONLN
        case _SC_NPROCESSORS_ONLN: return 3;
#endif
#ifdef _SC_NPROCESSORS_CONF
        case _SC_NPROCESSORS_CONF: return 3;
#endif
        default: errno = EINVAL; return -1;
    }
}
