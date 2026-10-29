@echo off
rem Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
rem (jalv3compiler fork build helper)
echo Building JAL 64-bit compiler for Windows using MSYS2

set GCC=C:\msys64\mingw64\bin\gcc.exe
set AR=C:\msys64\mingw64\bin\ar.exe
set STRIP=C:\msys64\mingw64\bin\strip.exe

echo Clean up old stuff
if exist libutils\.obj-x86-64 rmdir /s /q libutils\.obj-x86-64
if exist libutils\.dep rmdir /s /q libutils\.dep
if exist libcore\.obj-x86-64 rmdir /s /q libcore\.obj-x86-64
if exist libcore\.dep rmdir /s /q libcore\.dep
if exist libpic12\.obj-x86-64 rmdir /s /q libpic12\.obj-x86-64
if exist libpic12\.dep rmdir /s /q libpic12\.dep
if exist jal\.obj-x86-64 rmdir /s /q jal\.obj-x86-64
if exist jal\.dep rmdir /s /q jal\.dep
if exist ..\bin\jalv2_64.exe del /q ..\bin\jalv2_64.exe

rem Build the compiler
echo Building Libutils
cd libutils
mkdir .obj-x86-64
mkdir .dep
%GCC% -MMD -MF .dep/array.d -c -o .obj-x86-64/array.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g array.c
%GCC% -MMD -MF .dep/cache.d -c -o .obj-x86-64/cache.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cache.c
%GCC% -MMD -MF .dep/codefile.d -c -o .obj-x86-64/cod_file.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cod_file.c
%GCC% -MMD -MF .dep/mem.d -c -o .obj-x86-64/mem.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g mem.c
echo creating libutils-x86-64.a
%AR% -r libutils-x86-64.a .obj-x86-64/array.o .obj-x86-64/cache.o .obj-x86-64/cod_file.o .obj-x86-64/mem.o
echo Building Libcore
cd ..\libcore
mkdir .obj-x86-64
mkdir .dep
%GCC% -MMD -MF .dep/fp_op.d -c -o .obj-x86-64/pf_op.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_op.c
%GCC% -MMD -MF .dep/operator.d -c -o .obj-x86-64/operator.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g operator.c
%GCC% -MMD -MF .dep/vararray.d -c -o .obj-x86-64/vararray.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g vararray.c
%GCC% -MMD -MF .dep/expr.d -c -o .obj-x86-64/expr.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g expr.c
%GCC% -MMD -MF .dep/pf_src.d -c -o .obj-x86-64/pf_src.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_src.c
%GCC% -MMD -MF .dep/cmd_op.d -c -o .obj-x86-64/cmd_op.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_op.c
%GCC% -MMD -MF .dep/pf_cmd.d -c -o .obj-x86-64/pf_cmd.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_cmd.c
%GCC% -MMD -MF .dep/variable.d -c -o .obj-x86-64/variable.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g variable.c
%GCC% -MMD -MF .dep/cmdarray.d -c -o .obj-x86-64/cmdarray.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmdarray.c
%GCC% -MMD -MF .dep/pf_proca.d -c -o .obj-x86-64/pf_proca.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_proca.c
%GCC% -MMD -MF .dep/cmd.d -c -o .obj-x86-64/cmd.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd.c
%GCC% -MMD -MF .dep/pf_token.d -c -o .obj-x86-64/pf_token.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_token.c
%GCC% -MMD -MF .dep/tag.d -c -o .obj-x86-64/tag.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g tag.c
%GCC% -MMD -MF .dep/cmd_brch.d -c -o .obj-x86-64/cmd_brch.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_brch.c
%GCC% -MMD -MF .dep/cmd_proc.d -c -o .obj-x86-64/cmd_proc.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_proc.c
%GCC% -MMD -MF .dep/cmddelay.d -c -o .obj-x86-64/cmddelay.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmddelay.c
%GCC% -MMD -MF .dep/pfile.d -c -o .obj-x86-64/pfile.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pfile.c
%GCC% -MMD -MF .dep/pf_block.d -c -o .obj-x86-64/pf_block.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_block.c
%GCC% -MMD -MF .dep/cmd_optm.d -c -o .obj-x86-64/cmd_optm.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_optm.c
%GCC% -MMD -MF .dep/vardef.d -c -o .obj-x86-64/vardef.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g vardef.c
%GCC% -MMD -MF .dep/cmd_lbl.d -c -o .obj-x86-64/cmd_lbl.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_lbl.c
%GCC% -MMD -MF .dep/cmd_log.d -c -o .obj-x86-64/cmd_log.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_log.c
%GCC% -MMD -MF .dep/value.d -c -o .obj-x86-64/value.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g value.c
%GCC% -MMD -MF .dep/cmd_asm.d -c -o .obj-x86-64/cmd_asm.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmd_asm.c
%GCC% -MMD -MF .dep/pf_proc.d -c -o .obj-x86-64/pf_proc.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_proc.c
%GCC% -MMD -MF .dep/pf_expr.d -c -o .obj-x86-64/pf_expr.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pf_expr.c
%GCC% -MMD -MF .dep/label.d -c -o .obj-x86-64/label.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g label.c
%GCC% -MMD -MF .dep/cmdassrt.d -c -o .obj-x86-64/cmdassrt.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g cmdassrt.c
%GCC% -MMD -MF .dep/valarray.d -c -o .obj-x86-64/valarray.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g valarray.c
%GCC% -MMD -MF .dep/exprnode.d -c -o .obj-x86-64/exprnode.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g exprnode.c
echo creating libcore-x86-64.a
%AR% -r libcore-x86-64.a .obj-x86-64/expr.o .obj-x86-64/cmd_op.o .obj-x86-64/cmd_lbl.o .obj-x86-64/variable.o .obj-x86-64/pf_token.o .obj-x86-64/pf_src.o .obj-x86-64/cmddelay.o .obj-x86-64/pf_proca.o .obj-x86-64/cmd_brch.o .obj-x86-64/cmd_proc.o .obj-x86-64/tag.o .obj-x86-64/value.o .obj-x86-64/pf_op.o .obj-x86-64/pfile.o .obj-x86-64/cmd_log.o .obj-x86-64/vararray.o .obj-x86-64/cmd_asm.o .obj-x86-64/pf_proc.o .obj-x86-64/label.o .obj-x86-64/cmd_optm.o .obj-x86-64/valarray.o .obj-x86-64/exprnode.o .obj-x86-64/operator.o .obj-x86-64/cmdarray.o .obj-x86-64/pf_cmd.o .obj-x86-64/cmd.o .obj-x86-64/vardef.o .obj-x86-64/pf_block.o .obj-x86-64/pf_expr.o .obj-x86-64/cmdassrt.o
echo Building Libpic12
cd ..\libpic12
mkdir .obj-x86-64
mkdir .dep
%GCC% -MMD -MF .dep/pic_code.d -c -o .obj-x86-64/pic_code.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_code.c
%GCC% -MMD -MF .dep/pic_gop.d -c -o .obj-x86-64/pic_gop.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_gop.c
%GCC% -MMD -MF .dep/pic.d -c -o .obj-x86-64/pic.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic.c
%GCC% -MMD -MF .dep/pic16.d -c -o .obj-x86-64/pic16.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic16.c
%GCC% -MMD -MF .dep/picdelay.d -c -o .obj-x86-64/picdelay.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g picdelay.c
%GCC% -MMD -MF .dep/pic12.d -c -o .obj-x86-64/pic12.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic12.c
%GCC% -MMD -MF .dep/pic_stvar.d -c -o .obj-x86-64/pic_stvar.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_stvar.c
%GCC% -MMD -MF .dep/pic_daop.d -c -o .obj-x86-64/pic_daop.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_daop.c
%GCC% -MMD -MF .dep/piccolst.d -c -o .obj-x86-64/piccolst.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g piccolst.c
%GCC% -MMD -MF .dep/pic_stk.d -c -o .obj-x86-64/pic_stk.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_stk.c
%GCC% -MMD -MF .dep/pic_emu.d -c -o .obj-x86-64/pic_emu.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_emu.c
%GCC% -MMD -MF .dep/pic_opfn.d -c -o .obj-x86-64/pic_opfn.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_opfn.c
%GCC% -MMD -MF .dep/pic_wopt.d -c -o .obj-x86-64/pic_wopt.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_wopt.c
%GCC% -MMD -MF .dep/pic_cmdo.d -c -o .obj-x86-64/pic_cmdo.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_cmdo.c
%GCC% -MMD -MF .dep/picmovlpop.d -c -o .obj-x86-64/picmovlpop.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g picmovlpop.c
%GCC% -MMD -MF .dep/pic_inst.d -c -o .obj-x86-64/pic_inst.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_inst.c
%GCC% -MMD -MF .dep/pic14.d -c -o .obj-x86-64/pic14.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic14.c
%GCC% -MMD -MF .dep/picbsrop.d -c -o .obj-x86-64/picbsrop.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g picbsrop.c
%GCC% -MMD -MF .dep/pic_op.d -c -o .obj-x86-64/pic_op.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_op.c
%GCC% -MMD -MF .dep/pic14h.d -c -o .obj-x86-64/pic14h.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic14h.c
%GCC% -MMD -MF .dep/pic_var.d -c -o .obj-x86-64/pic_var.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_var.c
%GCC% -MMD -MF .dep/pic_brop.d -c -o .obj-x86-64/pic_brop.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g pic_brop.c
echo creating libpic12-x86-64.a
%AR% -r libpic12-x86-64.a .obj-x86-64/pic_gop.o .obj-x86-64/pic16.o .obj-x86-64/pic_daop.o .obj-x86-64/pic_stk.o .obj-x86-64/pic_op.o .obj-x86-64/pic_brop.o .obj-x86-64/pic_code.o .obj-x86-64/pic_opfn.o .obj-x86-64/picmovlpop.o .obj-x86-64/pic_emu.o .obj-x86-64/pic_var.o .obj-x86-64/picdelay.o .obj-x86-64/pic12.o .obj-x86-64/pic_cmdo.o .obj-x86-64/pic_stvar.o .obj-x86-64/pic_inst.o .obj-x86-64/pic.o .obj-x86-64/piccolst.o .obj-x86-64/pic14.o .obj-x86-64/picbsrop.o .obj-x86-64/pic_wopt.o .obj-x86-64/pic14h.o
echo Building Jal
cd ..\jal
mkdir .obj-x86-64
mkdir .dep
%GCC% -MMD -MF .dep/jal_main.d -c -o .obj-x86-64/jal_main.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_main.c
%GCC% -MMD -MF .dep/jal_blck.d -c -o .obj-x86-64/jal_blck.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_blck.c
%GCC% -MMD -MF .dep/jal_vdef.d -c -o .obj-x86-64/jal_vdef.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_vdef.c
%GCC% -MMD -MF .dep/jal_prnt.d -c -o .obj-x86-64/jal_prnt.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_prnt.c
%GCC% -MMD -MF .dep/jal_op.d -c -o .obj-x86-64/jal_op.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_op.c
%GCC% -MMD -MF .dep/jal_incl.d -c -o .obj-x86-64/jal_incl.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_incl.c
%GCC% -MMD -MF .dep/jal_ctrl.d -c -o .obj-x86-64/jal_ctrl.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_ctrl.c
%GCC% -MMD -MF .dep/jal_proc.d -c -o .obj-x86-64/jal_proc.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_proc.c
%GCC% -MMD -MF .dep/jal_expr.d -c -o .obj-x86-64/jal_expr.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_expr.c
%GCC% -MMD -MF .dep/jal_file.d -c -o .obj-x86-64/jal_file.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_file.c
%GCC% -MMD -MF .dep/jal_asm.d -c -o .obj-x86-64/jal_asm.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_asm.c
%GCC% -MMD -MF .dep/jal_tokn.d -c -o .obj-x86-64/jal_tokn.o -O2 -ansi -pedantic -W -Werror -Wall -Wshadow -Wcast-qual  -march=x86-64 -m64 -g jal_tokn.c
%GCC% -o ../../bin/jalv2-x86-64 .obj-x86-64/jal_op.o .obj-x86-64/jal_incl.o .obj-x86-64/jal_proc.o .obj-x86-64/jal_main.o .obj-x86-64/jal_blck.o .obj-x86-64/jal_vdef.o .obj-x86-64/jal_asm.o .obj-x86-64/jal_tokn.o .obj-x86-64/jal_ctrl.o .obj-x86-64/jal_prnt.o .obj-x86-64/jal_expr.o .obj-x86-64/jal_file.o ../libpic12/libpic12-x86-64.a ../libcore/libcore-x86-64.a ../libutils/libutils-x86-64.a -g -m64 -march=x86-64 -lm 
echo Strip symbols from the object file to shrink size
cd ../../bin
%STRIP% jalv2-x86-64.exe
rename jalv2-x86-64.exe jalv2_64.exe
echo All done!
pause

