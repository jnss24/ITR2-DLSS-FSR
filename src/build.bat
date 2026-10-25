@echo off
rem Builds version.dll (proxy) and itr2_dlss.dll (core):
rem   ..\bin          debug build (all keys, diagnostics) - the one installed for testing
rem   ..\bin\release  release build for Nexus (ITR2_RELEASE: only F5-F9, F9 = off / PLAIN edges, no diagnostics)
rem                   with its own ini made from the template (make_release_ini.py)
rem   ..\bin\amdtest  the release plus test keys and the image probe (ITR2_AMDTEST), same ini as the release
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set NGX=..\third_party\ngx
set FFX=..\third_party\ffx
set OUT=..\bin
set REL=..\bin\release
set OBJ=obj
set OBJREL=obj\release
set AMD=..\bin\amdtest
set OBJAMD=obj\amdtest
if not exist %OUT% mkdir %OUT%
if not exist %REL% mkdir %REL%
if not exist %OBJ% mkdir %OBJ%
if not exist %OBJREL% mkdir %OBJREL%
if not exist %AMD% mkdir %AMD%
if not exist %OBJAMD% mkdir %OBJAMD%
set SRC=core\util.cpp core\engine.cpp core\upscaler.cpp core\dlss.cpp core\fsr.cpp core\drs.cpp core\tweaks.cpp core\shot.cpp core\capture.cpp core\motion.cpp core\bench.cpp core\benchreport.cpp core\stress.cpp core\heartbeat.cpp core\hitch.cpp core\perf.cpp core\announce.cpp core\profile.cpp core\main.cpp
set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /GS /Zi /LD /D_CRT_SECURE_NO_WARNINGS /Icore /I%NGX%\include /I%FFX%\api\include /I%FFX%\upscalers\include
set LIBS=%NGX%\lib\nvsdk_ngx_s.lib user32.lib advapi32.lib ole32.lib shell32.lib sapi.lib winmm.lib windowscodecs.lib gdi32.lib mfplat.lib mfreadwrite.lib mfuuid.lib

echo [1/5] shader
fxc /nologo /T cs_5_1 /E main /O3 /Fh core\velocity_combine_cs.h /Vn g_VelocityCombineCS core\velocity_combine.hlsl || exit /b 1
fxc /nologo /T cs_5_1 /E main /O3 /Fh core\fovea_blend_cs.h /Vn g_FoveaBlendCS core\fovea_blend.hlsl || exit /b 1
fxc /nologo /T cs_5_1 /E main /O3 /Fh core\sharpen_cs.h /Vn g_SharpenCS core\sharpen.hlsl || exit /b 1
fxc /nologo /T cs_5_1 /E main /O3 /Fh core\fovea_compose_cs.h /Vn g_FoveaComposeCS core\fovea_compose.hlsl || exit /b 1
fxc /nologo /T cs_5_1 /E main /O3 /Fh core\edge_aa_cs.h /Vn g_EdgeAaCS core\edge_aa.hlsl || exit /b 1
fxc /nologo /T cs_5_1 /E main /O3 /Gis /Fh core\dlss5_codec_cs.h /Vn g_Dlss5CodecCS core\dlss5_codec.hlsl || exit /b 1
rem /Gis: IEEE strictness, or fxc may fold the probe's isnan / isinf away
fxc /nologo /T cs_5_1 /E main /O3 /Gis /Fh core\probe_cs.h /Vn g_ProbeCS core\probe.hlsl || exit /b 1

echo [2/5] itr2_dlss.dll (debug)
cl %CFLAGS% %SRC% /Fo%OBJ%\ /Fd%OBJ%\core.pdb /Fe%OUT%\itr2_dlss.dll ^
   /link /DEBUG /OPT:REF /OPT:ICF /PDB:%OUT%\itr2_dlss.pdb %LIBS% || exit /b 1

echo [3/5] itr2_dlss.dll (release)
cl %CFLAGS% /DITR2_RELEASE %SRC% /Fo%OBJREL%\ /Fd%OBJREL%\core.pdb /Fe%REL%\itr2_dlss.dll ^
   /link /DEBUG /OPT:REF /OPT:ICF /PDB:%REL%\itr2_dlss.pdb /PDBALTPATH:itr2_dlss.pdb %LIBS% || exit /b 1
cl %CFLAGS% /DITR2_RELEASE /DITR2_AMDTEST %SRC% /Fo%OBJAMD%\ /Fd%OBJAMD%\core.pdb /Fe%AMD%\itr2_dlss.dll ^
   /link /DEBUG /OPT:REF /OPT:ICF /PDB:%AMD%\itr2_dlss.pdb /PDBALTPATH:itr2_dlss.pdb %LIBS% || exit /b 1

echo [4/5] version.dll (proxy)
ml64 /nologo /c /Fo%OBJ%\proxy_asm.obj proxy\proxy.asm || exit /b 1
cl /nologo /O2 /MT /W3 /LD proxy\proxy.cpp %OBJ%\proxy_asm.obj /Fo%OBJ%\ /Fe%OUT%\version.dll ^
   /link /DEF:proxy\version.def /OPT:REF || exit /b 1

echo [5/5] package
for %%D in (%OUT% %REL% %AMD%) do (
   copy /y %NGX%\bin\nvngx_dlss.dll %%D\ >nul
   copy /y %FFX%\bin\amd_fidelityfx_loader_dx12.dll %%D\ >nul
   copy /y %FFX%\bin\amd_fidelityfx_upscaler_dx12.dll %%D\ >nul
   copy /y %FFX%\LICENSE.md %%D\AMD_FidelityFX_LICENSE.md >nul
   copy /y %NGX%\LICENSE.txt %%D\NVIDIA_DLSS_LICENSE.txt >nul
)
copy /y %OUT%\version.dll %REL%\ >nul
copy /y %OUT%\version.dll %AMD%\ >nul
if not exist %OUT%\itr2_dlss.ini copy /y itr2_dlss.ini %OUT%\ >nul
python -I make_release_ini.py itr2_dlss.ini %REL%\itr2_dlss.ini || exit /b 1
copy /y %REL%\itr2_dlss.ini %AMD%\ >nul
rem the manual goes into the zip as plain text for Notepad (make_readme_txt.py), not as Markdown
python -I make_readme_txt.py ..\MANUAL.md %REL%\ITR2_DLSS_README.txt || exit /b 1
if exist %REL%\ITR2_DLSS_MANUAL.md del /q %REL%\ITR2_DLSS_MANUAL.md
del /q %OUT%\*.exp %OUT%\*.lib %REL%\*.exp %REL%\*.lib %AMD%\*.exp %AMD%\*.lib 2>nul
echo done.
