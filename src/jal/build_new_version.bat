@echo off
rem Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
rem (jalv3compiler fork build helper)
echo Building JALv2 compiler version 2.5r10

cd /d "%~dp0"

echo Compiling main jal module...
C:\msys64\mingw64\bin\gcc.exe -c -o jal_main.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_main.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_blck.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_blck.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_vdef.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_vdef.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_prnt.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_prnt.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_op.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_op.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_incl.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_incl.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_ctrl.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_ctrl.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_proc.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_proc.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_expr.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_expr.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_file.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_file.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_asm.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_asm.c
C:\msys64\mingw64\bin\gcc.exe -c -o jal_tokn.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g jal_tokn.c

echo Linking...
C:\msys64\mingw64\bin\gcc.exe -o jalv25r10_64.exe jal_main.o jal_blck.o jal_vdef.o jal_prnt.o jal_op.o jal_incl.o jal_ctrl.o jal_proc.o jal_expr.o jal_file.o jal_asm.o jal_tokn.o ../libcore/libcore-x86-64.a ../libpic12/libpic12-x86-64.a ../libutils/libutils-x86-64.a -g -m64 -march=x86-64 -lm

echo Stripping symbols...
C:\msys64\mingw64\bin\strip.exe jalv25r10_64.exe

echo Done! New compiler: jalv25r10_64.exe
pause

