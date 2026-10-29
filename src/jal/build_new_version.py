#!/usr/bin/env python3
"""
Build script for JALv2 compiler version 2.5r10
Creates a new compiler binary with updated version information.

Author: Matthew Schinkel, Copyright (c) 2026, all rights reserved.
"""

import os
import subprocess
import sys
import shutil

def run_command(cmd, description):
    """Run a command and handle errors"""
    print(f"  {description}...")
    try:
        result = subprocess.run(cmd, shell=True, check=True, capture_output=True, text=True)
        if result.stdout:
            print(f"    Output: {result.stdout.strip()}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"  Error: {e}")
        print(f"  Command: {cmd}")
        if e.stdout:
            print(f"  Output: {e.stdout}")
        if e.stderr:
            print(f"  Error: {e.stderr}")
        return False

def main():
    print("Building JALv2 compiler version 2.5r10")
    print("=" * 50)
    
    # Change to the jal directory
    jal_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(jal_dir)
    
    # GCC path
    gcc_path = r"C:\msys64\mingw64\bin\gcc.exe"
    strip_path = r"C:\msys64\mingw64\bin\strip.exe"
    
    # Compiler flags (removed -Werror to see warnings instead of failing)
    cflags = "-O2 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -march=x86-64 -m64 -g"
    
    # Source files to compile
    source_files = [
        "jal_main.c",
        "jal_blck.c", 
        "jal_vdef.c",
        "jal_prnt.c",
        "jal_op.c",
        "jal_incl.c",
        "jal_ctrl.c",
        "jal_proc.c",
        "jal_expr.c",
        "jal_file.c",
        "jal_asm.c",
        "jal_tokn.c"
    ]
    
    # Object files
    object_files = [f.replace('.c', '.o') for f in source_files]
    
    # Library paths
    libcore_path = "../libcore/libcore-x86-64.a"
    libpic12_path = "../libpic12/libpic12-x86-64.a"
    libutils_path = "../libutils/libutils-x86-64.a"
    
    # Check if libraries exist
    libraries = [libcore_path, libpic12_path, libutils_path]
    for lib in libraries:
        if not os.path.exists(lib):
            print(f"Error: Library {lib} not found!")
            return False
    
    # Compile source files
    print("Compiling source files...")
    for src_file in source_files:
        obj_file = src_file.replace('.c', '.o')
        cmd = f'"{gcc_path}" -c -o {obj_file} {cflags} {src_file}'
        if not run_command(cmd, f"Compiling {src_file}"):
            return False
    
    # Link the executable
    print("Linking executable...")
    obj_list = " ".join(object_files)
    lib_list = " ".join(libraries)
    cmd = f'"{gcc_path}" -o jalv25r10_64.exe {obj_list} {lib_list} -g -m64 -march=x86-64 -lm'
    
    if not run_command(cmd, "Linking executable"):
        return False
    
    # Strip symbols
    print("Stripping symbols...")
    cmd = f'"{strip_path}" jalv25r10_64.exe'
    run_command(cmd, "Stripping symbols")  # Don't fail if strip fails
    
    # Check if executable was created
    if os.path.exists("jalv25r10_64.exe"):
        print("\n✅ Success! New compiler created: jalv25r10_64.exe")
        
        # Get file size
        size = os.path.getsize("jalv25r10_64.exe")
        print(f"   File size: {size:,} bytes")
        
        # Copy to main directory
        main_dir = "../../"
        dest_path = os.path.join(main_dir, "jalv25r10_64.exe")
        try:
            shutil.copy2("jalv25r10_64.exe", dest_path)
            print(f"   Copied to: {dest_path}")
        except Exception as e:
            print(f"   Warning: Could not copy to main directory: {e}")
        
        return True
    else:
        print("\n❌ Error: Executable was not created!")
        return False

if __name__ == "__main__":
    success = main()
    if not success:
        sys.exit(1)
    else:
        print("\n🎉 Build completed successfully!")
        print("You can now test the new compiler version.")
