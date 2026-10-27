// Dataset paths are deliberately ASCII. Extended OEM names are rejected.
#include "ff.h"
WCHAR ff_oem2uni(WCHAR c, WORD cp) { (void)cp; return c < 128 ? c : 0; }
WCHAR ff_uni2oem(DWORD c, WORD cp) { (void)cp; return c < 128 ? (WCHAR)c : 0; }
DWORD ff_wtoupper(DWORD c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
