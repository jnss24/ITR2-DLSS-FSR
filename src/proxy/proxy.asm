; proxy.asm - export trampolines into the real version.dll (table filled in DllMain)
EXTERN g_real:QWORD
.code
GetFileVersionInfoA PROC
    jmp qword ptr [g_real + 0]
GetFileVersionInfoA ENDP
GetFileVersionInfoByHandle PROC
    jmp qword ptr [g_real + 8]
GetFileVersionInfoByHandle ENDP
GetFileVersionInfoExA PROC
    jmp qword ptr [g_real + 16]
GetFileVersionInfoExA ENDP
GetFileVersionInfoExW PROC
    jmp qword ptr [g_real + 24]
GetFileVersionInfoExW ENDP
GetFileVersionInfoSizeA PROC
    jmp qword ptr [g_real + 32]
GetFileVersionInfoSizeA ENDP
GetFileVersionInfoSizeExA PROC
    jmp qword ptr [g_real + 40]
GetFileVersionInfoSizeExA ENDP
GetFileVersionInfoSizeExW PROC
    jmp qword ptr [g_real + 48]
GetFileVersionInfoSizeExW ENDP
GetFileVersionInfoSizeW PROC
    jmp qword ptr [g_real + 56]
GetFileVersionInfoSizeW ENDP
GetFileVersionInfoW PROC
    jmp qword ptr [g_real + 64]
GetFileVersionInfoW ENDP
VerFindFileA PROC
    jmp qword ptr [g_real + 72]
VerFindFileA ENDP
VerFindFileW PROC
    jmp qword ptr [g_real + 80]
VerFindFileW ENDP
VerInstallFileA PROC
    jmp qword ptr [g_real + 88]
VerInstallFileA ENDP
VerInstallFileW PROC
    jmp qword ptr [g_real + 96]
VerInstallFileW ENDP
VerLanguageNameA PROC
    jmp qword ptr [g_real + 104]
VerLanguageNameA ENDP
VerLanguageNameW PROC
    jmp qword ptr [g_real + 112]
VerLanguageNameW ENDP
VerQueryValueA PROC
    jmp qword ptr [g_real + 120]
VerQueryValueA ENDP
VerQueryValueW PROC
    jmp qword ptr [g_real + 128]
VerQueryValueW ENDP
END
