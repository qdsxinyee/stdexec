@echo off
cd /d "E:\code\cpp\library\stdexec\docs\kimidoc\iocptest"
call "E:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /EHsc /W3 iocp_blocking.cpp ws2_32.lib
cl /EHsc /W3 iocp_nonblocking_inline.cpp ws2_32.lib
cl /EHsc /W3 iocp_skip_sync.cpp ws2_32.lib
