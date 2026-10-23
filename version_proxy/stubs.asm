; auto-generated, do not edit
OPTION CASEMAP:NONE

EXTERN g_pfn_GetFileVersionInfoA:QWORD
EXTERN g_pfn_GetFileVersionInfoByHandle:QWORD
EXTERN g_pfn_GetFileVersionInfoExA:QWORD
EXTERN g_pfn_GetFileVersionInfoExW:QWORD
EXTERN g_pfn_GetFileVersionInfoSizeA:QWORD
EXTERN g_pfn_GetFileVersionInfoSizeExA:QWORD
EXTERN g_pfn_GetFileVersionInfoSizeExW:QWORD
EXTERN g_pfn_GetFileVersionInfoSizeW:QWORD
EXTERN g_pfn_GetFileVersionInfoW:QWORD
EXTERN g_pfn_VerFindFileA:QWORD
EXTERN g_pfn_VerFindFileW:QWORD
EXTERN g_pfn_VerInstallFileA:QWORD
EXTERN g_pfn_VerInstallFileW:QWORD
EXTERN g_pfn_VerLanguageNameA:QWORD
EXTERN g_pfn_VerLanguageNameW:QWORD
EXTERN g_pfn_VerQueryValueA:QWORD
EXTERN g_pfn_VerQueryValueW:QWORD

.code

GetFileVersionInfoA PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoA]
GetFileVersionInfoA ENDP

GetFileVersionInfoByHandle PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoByHandle]
GetFileVersionInfoByHandle ENDP

GetFileVersionInfoExA PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoExA]
GetFileVersionInfoExA ENDP

GetFileVersionInfoExW PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoExW]
GetFileVersionInfoExW ENDP

GetFileVersionInfoSizeA PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoSizeA]
GetFileVersionInfoSizeA ENDP

GetFileVersionInfoSizeExA PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoSizeExA]
GetFileVersionInfoSizeExA ENDP

GetFileVersionInfoSizeExW PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoSizeExW]
GetFileVersionInfoSizeExW ENDP

GetFileVersionInfoSizeW PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoSizeW]
GetFileVersionInfoSizeW ENDP

GetFileVersionInfoW PROC
    jmp qword ptr [g_pfn_GetFileVersionInfoW]
GetFileVersionInfoW ENDP

VerFindFileA PROC
    jmp qword ptr [g_pfn_VerFindFileA]
VerFindFileA ENDP

VerFindFileW PROC
    jmp qword ptr [g_pfn_VerFindFileW]
VerFindFileW ENDP

VerInstallFileA PROC
    jmp qword ptr [g_pfn_VerInstallFileA]
VerInstallFileA ENDP

VerInstallFileW PROC
    jmp qword ptr [g_pfn_VerInstallFileW]
VerInstallFileW ENDP

VerLanguageNameA PROC
    jmp qword ptr [g_pfn_VerLanguageNameA]
VerLanguageNameA ENDP

VerLanguageNameW PROC
    jmp qword ptr [g_pfn_VerLanguageNameW]
VerLanguageNameW ENDP

VerQueryValueA PROC
    jmp qword ptr [g_pfn_VerQueryValueA]
VerQueryValueA ENDP

VerQueryValueW PROC
    jmp qword ptr [g_pfn_VerQueryValueW]
VerQueryValueW ENDP

END
