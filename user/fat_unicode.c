#include "ff.h"

WCHAR ff_oem2uni(WCHAR oem, WORD cp) {
    (void)cp;
    return oem < 0x80 ? oem : 0;
}

WCHAR ff_uni2oem(DWORD uni, WORD cp) {
    (void)cp;
    return uni < 0x80 ? (WCHAR)uni : 0;
}

DWORD ff_wtoupper(DWORD uni) {
    if (uni >= 'a' && uni <= 'z') {
        return uni - 'a' + 'A';
    }
    return uni;
}
