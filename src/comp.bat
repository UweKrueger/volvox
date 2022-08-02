@if not exist %2 goto compile
@set srcdate=%~t1
@set destdate=%~t2
@set srctime=%srcdate:~9,1%%srcdate:~3,2%%srcdate:~0,2%%srcdate:~11,2%%srcdate:~14,2%
@set desttime=%destdate:~9,1%%destdate:~3,2%%destdate:~0,2%%destdate:~11,2%%destdate:~14,2%
@if %desttime% GTR %srctime% goto skip
:compile
cl /MP /we"4238" /GS /Zc:rvalueCast /W4 /wd"4201" /wd"4189" /wd"4141" /wd"4146" /wd"4244" /wd"4267" /wd"4291" /wd"4351" /wd"4456" /wd"4457" /wd"4458" /wd"4459" /wd"4503" /wd"4624" /wd"4722" /wd"4100" /wd"4127" /wd"4512" /wd"4505" /wd"4610" /wd"4510" /wd"4702" /wd"4245" /wd"4706" /wd"4310" /wd"4701" /wd"4703" /wd"4389" /wd"4611" /wd"4805" /wd"4204" /wd"4577" /wd"4091" /wd"4592" /wd"4319" /wd"4709" /wd"4324" /Zc:wchar_t /Gm- /O2 /Ob2 /Zc:inline /fp:precise /D "WIN32" /D "_WINDOWS" /D "NDEBUG" /D "_HAS_EXCEPTIONS=0" /D "GTEST_HAS_RTTI=0" /D "_CRT_SECURE_NO_DEPRECATE" /D "_CRT_SECURE_NO_WARNINGS" /D "_CRT_NONSTDC_NO_DEPRECATE" /D "_CRT_NONSTDC_NO_WARNINGS" /D "_SCL_SECURE_NO_DEPRECATE" /D "_SCL_SECURE_NO_WARNINGS" /D "__STDC_CONSTANT_MACROS" /D "__STDC_FORMAT_MACROS" /D "__STDC_LIMIT_MACROS" /errorReport:prompt /WX- /Zc:forScope /Gd /Oi /MD /std:c++20 /nologo /diagnostics:column /c /I"C:\Program Files\LLVM\include" /I"../include" /I"../include/editline" /EHs-c- %3 %4 %5 %6 %7 %8 %9 %1
:skip
