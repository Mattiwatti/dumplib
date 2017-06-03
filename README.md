# Overview
Dumplib is a helper tool to create import libraries (.lib files) with correct `extern "C"` name decorations for **x86** PE files compiled by MSVC. (This means not x64 because x64 files do not have decorated export names; for x64, see Remarks below.) Its main use is to allow creating import libraries for DLLs for which no import library is available, and to customize existing import libraries, e.g. to remove unwanted exports that cause linker collisions.

Dumplib mostly just applies a bunch of regexes and hacks. Most of the actual work is done by `dumpbin.exe` which generates the export listing dumplib parses. Executing dumplib generates three files:
- A **.cpp** file containing dummy function definitions;
- A **.def** file containing the names and ordinals of functions to be exported;
- A **.bat** file to invoke the compiler and linker to produce a (useless) DLL file and the import library for it, which will be compatible with the original DLL and can be linked against.

# Usage
1. Create an export listing of the target file using the **x86** version of `dumpbin`. To do so, open a 'VS2017 **x86** Native Tools Command Prompt' (or similar depending on your version). Note the part that says **x86**, this is important because each tool invoked from this prompt will generate a different (wrong) output in x64 mode. Assuming you want to create an import library for `ucrtbase.dll`, execute `dumpbin /EXPORTS ucrtbase.dll > ucrtbase-exports.txt`.
2. Execute `dumplib ucrtbase-exports.txt ucrtbase.dll`. The second argument supplies the name of the executable to be generated, which will usually be the same as the original, but can be changed if desired.
3. Depending on the size and complexity of your file, you may see a number of warnings scroll by. If you missed the part that said to open an **x86** prompt, this number will be large. Otherwise, take note of what, if anything, was not parsed correctly so you can make manual fixups later if needed.
4. The aforementioned three files (.cpp, .def, .bat) should now be present in the directory. You can edit these to remove any declarations you don't want. Removing the declaration from the .def file is sufficient for this.
5. Once you are done, open the .bat file in notepad to modify the first path to point to the same batch file you used to open the VS command prompt. The other filenames and paths should already be correct.
6. Run the .bat file.
7. There should now be an `output` directory (to prevent accidental overwriting of the input files) containing your import library and DLL.

# Remarks
- Dumplib does not and will not ever parse C++ exports. It is intended for Windows system files, which tend to use `extern "C"` exports. `Dumpbin` on the other hand **does** undecorate C++ function names, so if your DLL has a small number of C++ functions it is still possible to do this with some added manual copy and paste work (this is why dumpbin generates .cpp files, not .c). I am not interested in adding support for this however.
- The 'x64 version' of dumplib is significantly more ghetto and fits in this README, because `extern "C"` declarations are not decorated by MSVC on x64. The following batch file will suffice in most cases:
  ```Batch
  @echo off
  call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

  dumpbin /EXPORTS "%1" > %2-exports.txt

  echo LIBRARY %2 > %2-exports.def
  echo EXPORTS >> %2-exports.def
  for /f "skip=19 tokens=4" %%a in (%2-exports.txt) do (
      if "%%a" NEQ "" echo %%a>> %2-exports.def
  )

  lib /nologo /DEF:%2-exports.def /OUT:%2.lib /MACHINE:X64

  del %2.exp 1>nul
  pause
  ```
  Example usage: `dumplib.bat C:\Windows\System32\ntdll.dll ntdll.dll`. It should hopefully be clear that the first path should in fact point to the **x64** version of your VS vars batch file this time.

  One limitation of this method compared to dumplib is that it does not understand forward exports. I may add some minimal amount of x64 support in the future to correct this.
