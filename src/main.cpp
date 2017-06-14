#include <Windows.h>
#include <stdio.h>
#include <fstream>
#include <unordered_set>
#include <regex>

enum class CallingConvention
{
	Cdecl,		// _Name
	Stdcall,	// _Name@4
	Fastcall	// @Name@4
};

struct Export
{
	bool ExternC;							// Normally true, but compiling as .cpp allows manually adding C++ exports
	CallingConvention CallingConvention;	// Cdecl for data exports
	std::string Name;						// Undecorated function name
	unsigned int NumberOfParameters;
	unsigned int Ordinal;
	bool NoName;
	bool IsIntrinsic;
	bool IsForwardExport;
	std::string ForwardDllName;
	std::string ForwardFunctionName;

	explicit Export(unsigned int ordinal)
	:	ExternC(true),
		CallingConvention(CallingConvention::Cdecl),
		NumberOfParameters(0),
		Ordinal(ordinal),
		NoName(false),
		IsIntrinsic(false),
		IsForwardExport(false)
	{}
};

static const std::unordered_set<std::string> Intrinsics // Known disallowed function names in MSVC
{
	"__std_terminate",
	"__CxxDetectRethrow",
	"__CxxExceptionFilter",
	"__CxxQueryExceptionSize",
	"__CxxRegisterExceptionObject",
	"__CxxUnregisterExceptionObject",
	"__RTCastToVoid",
	"__RTDynamicCast",
	"__RTtypeid",
	"_abnormal_termination",
	"_CxxThrowException",
	"_purecall",
	"_setjmp",
	"_setjmpex",
	"atexit"
};

static std::unordered_set<std::string> ForwardLibs;

// MS says PathRemoveFileSpec is unsafe and that I should upgrade to Windows 8/10 for PathCchRemoveFileSpec. Hmmm
static std::string BaseDir(const std::string& absolutePath)
{
	for (size_t i = absolutePath.length() - 1; i >= 1; --i)
	{
		if (absolutePath[i] == '\\')
			return std::string(absolutePath, 0, i);
	}
	return "";
}

static bool ReadDumpbinOutput(const std::string& filename, const std::string& libraryFilename, std::vector<Export>& exports)
{
	std::ifstream file(filename);
	if (!file.is_open())
	{
		fprintf(stderr, "Failed to open %s for reading\n", filename.c_str());
		return false;
	}

	const std::regex exportRegex("([0-9]+)  (.+) ([0-9A-F]*) (.+)");
	//                              ord     hint   (RVA)     decorated name

	const std::regex stdcallOrFastcallRegex("(.+) = (_|@)(.+)@([0-9]+)");	// AlpcGetHeaderSize = _AlpcGetHeaderSize@4
	const std::regex cdeclOrDataRegex("(.+) = (_|@)(.+)");					// wcstoul = _wcstoul
	const std::regex noNameRegex("\\[NONAME\\] (_|@)(.+)@([0-9]+)");		// [NONAME] _ZwCreateUserProcess@44

	std::string line;
	int i = 0;
	while (std::getline(file, line))
	{
		i++;
		if (i < 20) // dumpbin /EXPORT always generates 20 header lines
			continue;

		std::smatch exportMatches;
		if (!std::regex_search(line, exportMatches, exportRegex))
			break; // End of export listing

		// We have the ordinal and something resembling a name
		Export e(atoi(exportMatches[1].str().c_str()));
		std::string decoratedName = exportMatches[4].str();

		// Handle forwards first
		if (decoratedName.find(" (forwarded to") != std::string::npos)
		{
			e.IsForwardExport = true;

			// We have a forward export. This means we have to link against the .lib
			// of the DLL that exports the function. Possibilities from best to worst:
			const std::regex forwardedToDllRegex("(.+) \\(forwarded to (\\w+)\\.(.+)\\)");
			const std::regex forwardedToNonsense1Regex("(.+) \\(forwarded to api-ms-win-\\w+(.+)\\.(.+)\\)",
				std::regex_constants::icase);
			const std::regex forwardedToNonsense2Regex("(.+) \\(forwarded to api-ms-win-crt(.+)\\.(.+)\\)",
				std::regex_constants::icase);

			std::smatch forwardNameMatches;
			if (std::regex_search(decoratedName, forwardNameMatches, forwardedToDllRegex))
			{
				// Forward to a proper DLL. Usually this is either ntdll or KernelBase
				e.Name = forwardNameMatches[1].str();
				e.ForwardDllName = forwardNameMatches[2].str();
				e.ForwardFunctionName = forwardNameMatches[3].str();
				printf("%s forwards to %s.%s\n", e.Name.c_str(),
					e.ForwardDllName.c_str(), e.ForwardFunctionName.c_str());
			}
			else if (!std::regex_search(decoratedName, forwardNameMatches, forwardedToNonsense2Regex) &&
				std::regex_search(decoratedName, forwardNameMatches, forwardedToNonsense1Regex))
			{
				// Forward to a DLL that forwards to another DLL. This is why Unix people laugh at us
				e.Name = forwardNameMatches[1].str();
				e.ForwardDllName = forwardNameMatches[2].str();
				e.ForwardFunctionName = forwardNameMatches[3].str();
				if (e.ForwardDllName.find("rtlsupport-") != std::string::npos) // This one is easy
					e.ForwardDllName = "NTDLL";
				else
				{
					// Try kernel32.dll by default, unless that's what we're creating the .lib of.
					// Note: if you're creating kernel32.lib, you need to make kernelbase.lib first, and that's
					// actually probably only mostly correct since the api-ms crap points to kernel32.
					e.ForwardDllName = libraryFilename == "kernel32.dll" ? "kernelbase" : "kernel32";
				}
				printf("%s forwards to api-ms-win-core%s.%s\n", e.Name.c_str(),
					forwardNameMatches[2].str().c_str(), e.ForwardFunctionName.c_str());
			}
			else if (std::regex_search(decoratedName, forwardNameMatches, forwardedToNonsense2Regex))
			{
				// Forward to an MSVC or UCRT runtime DLL that forwards to (probably) ucrtbase.dll,
				// which itself only has forward imports of the previous kind. Good luck
				// Leaving the BS DLL name in the .def and linking against ucrt.lib and/or removing /NODEFAULTLIB may work.
				// Linking in "ucrtbase.lib" will fail for sure since it doesn't exist, but you could make it with dumplib...
				e.Name = forwardNameMatches[1].str();
				e.ForwardDllName = forwardNameMatches[2].str();
				e.ForwardFunctionName = forwardNameMatches[3].str();
				e.ForwardDllName = "ucrtbase";
				printf("!!! ACHTUNG !!! %s forwards to api-ms-win-crt%s.%s !!!\n", e.Name.c_str(),
					forwardNameMatches[2].str().c_str(), e.ForwardFunctionName.c_str());
			}
			else
			{
				fprintf(stderr, "Failed to parse forward export name \"%s\".\n", decoratedName.c_str());
				continue;
			}

			std::string libName = e.ForwardDllName == "ucrtbase" ? "ucrt" : e.ForwardDllName;
			std::transform(libName.begin(), libName.end(), libName.begin(), ::tolower);
			if (ForwardLibs.insert(libName).second)
				printf("Added %s.lib to the linker input files.\n", libName.c_str());
		}
		else
		{
			// Non-forward export. Figure out by regex what kind of calling convention and name we have
			std::smatch nameMatches;
			if (std::regex_search(decoratedName, nameMatches, stdcallOrFastcallRegex))
			{
				// Named stdcall/fastcall export
				e.Name = nameMatches[1].str();
				e.CallingConvention = nameMatches[2].str().c_str()[0] == '@'
					? CallingConvention::Fastcall : CallingConvention::Stdcall;
				e.NumberOfParameters = atoi(nameMatches[4].str().c_str()) / 4;
			}
			else if (std::regex_search(decoratedName, nameMatches, cdeclOrDataRegex))
			{
				// Named data or cdecl export
				e.Name = nameMatches[1].str();
				e.CallingConvention = CallingConvention::Cdecl;

			}
			else if (std::regex_search(decoratedName, nameMatches, noNameRegex))
			{
				// NONAME export. Fix up the name and find the calling convention
				e.CallingConvention = nameMatches[1].str().c_str()[0] == '@'
					? CallingConvention::Fastcall : CallingConvention::Stdcall;
				e.Name = nameMatches[2].str();
				e.NumberOfParameters = atoi(nameMatches[3].str().c_str()) / 4;
				e.NoName = true;
			}
			else
			{
				// This can happen if the decorated name *doesn't* have a prefix. Examples:
				// 'ExFetchLicenseData' and 'Kei386EoiHelper = Kei386EoiHelper@0' in ntoskrnl.exe
				// If you really need the export you can probably declare its public name using [M|N]ASM.
				// C++ declarations are another example but easy to do (dumpbin already undecorates them)
				fprintf(stderr, "Failed to parse export name \"%s\".\n", decoratedName.c_str());
				continue;
			}
		}

		if (Intrinsics.find(e.Name) != Intrinsics.end())
			e.IsIntrinsic = true;

		exports.push_back(e);
	}
	return true;
}

static bool CreateCppFile(const char *filename, const std::vector<Export>& exports)
{
	std::ofstream file(filename);
	if (!file.is_open())
	{
		fprintf(stderr, "Failed to open %s for writing\n", filename);
		return false;
	}

	for (auto const& e : exports)
	{
		if (e.IsForwardExport)
			continue;

		std::string declaration = e.ExternC ? "extern \"C\" " : "";
		declaration += "int ";

		// MSVC doesn't allow certain names. Hack pt. 1
		std::string name = e.IsIntrinsic ? "_INTRINSIC_" + e.Name : e.Name;

		if (e.CallingConvention == CallingConvention::Cdecl)
		{
			// Cdecl function or data export. The name decoration will be a leading underscore only
			declaration += " __cdecl " + name + "(";
		}
		else
		{
			// Fastcall or stdcall function
			declaration += (e.CallingConvention == CallingConvention::Fastcall
				? "__fastcall "
				: "__stdcall ") +
			name + "(";

			// Add some number of parameters to match the decorated name:
			// _AlpcGetMessageAttribute@8 -> AlpcGetMessageAttribute(int, int)
			std::string params;
			for (unsigned int i = 0; i < e.NumberOfParameters; ++i)
			{
				params += "int";
				if (i != e.NumberOfParameters - 1)
					params += ", ";
			}
			declaration += params;
		}

		// Make the return value the export ordinal, to prevent the compiler
		// from optimizing all exports to have the same RVA
		declaration += ") { return " + std::to_string(e.Ordinal) + "; }";

		file << declaration << std::endl;
	}
	return true;
}

static bool CreateDefFile(const char *filename, const char* libraryName, const std::vector<Export>& exports)
{
	std::ofstream file(filename);
	if (!file.is_open())
	{
		fprintf(stderr, "Failed to open %s for writing\n", filename);
		return false;
	}

	// Reference: https://msdn.microsoft.com/en-us/library/hyx1zcd3.aspx
	file << "LIBRARY " << libraryName << std::endl;
	file << "EXPORTS" << std::endl;

	for (auto const& e : exports)
	{
		// It's tempting to add the ordinal here to get correct import hints for the speedup (literally microseconds!)
		// Unfortunately MS decided that declaring the ordinal number also removes the name from the export table
		std::string declaration = "\t" + e.Name;

		if (e.IsForwardExport)
			declaration += " = " + e.ForwardDllName + "." + e.ForwardFunctionName;
		else if (e.IsIntrinsic)
			declaration += " = _INTRINSIC_" + e.Name; // Hack pt. 2

		if (e.NoName)
		{
			// If this is specified the ordinal *has* to be added and correct because it's the only way to
			// import the function (or GetProcAddress the ordinal - either way requires it to be correct)
			declaration += " @" + std::to_string(e.Ordinal) + " NONAME";
		}
		file << declaration << std::endl;
	}
	return true;
}

static bool CreateBatFile(const char *filename, const char *dumpbinInputFilename, const char* libraryName)
{
	std::ofstream file(filename);
	if (!file.is_open())
	{
		fprintf(stderr, "Failed to open %s for writing\n", filename);
		return false;
	}

	std::string libs;
	for (auto const& lib : ForwardLibs)
		libs += "\"" + lib + ".lib\" ";

	file << "@echo off" << std::endl << std::endl;
	file << ":: Set your *32 bit* VS vars path here. The rest should already be correct" << std::endl;
	file << "set VCVARSFILE=C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\"
		"Enterprise\\VC\\Auxiliary\\Build\\vcvars32.bat" << std::endl;
	file << "set WORKDIR=" << BaseDir(filename) << std::endl;
	file << "set CFILE=" << dumpbinInputFilename << ".cpp" << std::endl;
	file << "set DEFFILE=" << dumpbinInputFilename << ".def" << std::endl;
	file << "set LIBS=" << libs << std::endl;
	file << "set OUTPUTFILE=" << libraryName << std::endl << std::endl;

	file << "call \"%VCVARSFILE%\"" << std::endl;
	file << "rd /S /Q output 1>nul 2>&1" << std::endl;
	file << "mkdir output" << std::endl << std::endl;

	// Most flags below don't affect the .lib and are only meant to make the PE file as simple as possible
	// and make it easier to inspect. /NOCOFFGRPINFO kills the debug directory (needed for >= VS2015)
	file << "cl /nologo /c /MT /W4 /O1 /Os /GS- /guard:cf- \"%CFILE%\" /Fo\"%WORKDIR%\\output\\main.obj\"" << std::endl;
	file << "link /nologo /OUT:\"%WORKDIR%\\output\\%OUTPUTFILE%\" %LIBS% " <<
		"/IMPLIB:\"%WORKDIR%\\output\\%OUTPUTFILE%.lib\" /DLL /NOCOFFGRPINFO " <<
		"/MACHINE:X86 /SAFESEH /INCREMENTAL:NO /DEF:\"%WORKDIR%\\%DEFFILE%\" " <<
		"\"%WORKDIR%\\output\\main.obj\" /NODEFAULTLIB /NOENTRY /MERGE:.rdata=.text" << std::endl;
	file << "if %ERRORLEVEL% NEQ 0 goto drats" << std::endl << std::endl;

	file << "del output\\*.exp 1>nul" << std::endl;
	file << "del output\\main.obj 1>nul" << std::endl;
	file << "echo." << std::endl;
	file << "echo Your .lib file is ready sir/madam. It can be found at output\\%OUTPUTFILE%.lib." << std::endl;
	file << "goto end" << std::endl << std::endl;

	file << ":drats" << std::endl;
	file << "echo." << std::endl;
	file << "echo The compiler failed to do its job! Disgusting, sad!" << std::endl << std::endl;

	file << ":end" << std::endl;
	file << "echo." << std::endl;
	file << "pause" << std::endl;

	return true;
}

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		printf("Usage: dumplib <dumpbin-exports> <dllname>\n\n");
		printf("Where <dumpbin-exports> was created with the x86 version of dumpbin using:\n");
		printf("dumpbin /EXPORTS file.dll > <dumpbin-exports>\n\n");
		printf("Example: dumplib ntdll-exports.txt ntdll.dll\n");
		return 0;
	}

	const char *libraryFilename = argv[2];
	char absDumpbinPath[4096];
	if (GetFullPathNameA(argv[1], 4096, absDumpbinPath, nullptr) == 0)
	{
		fprintf(stderr, "GetFullPathName(\"%s\"): error %d\n", argv[1], GetLastError());
		return -1;
	}

	char drive[_MAX_DRIVE], dir[_MAX_DIR], dumpbinFilename[_MAX_FNAME], ext[_MAX_EXT];
	auto err = _splitpath_s(absDumpbinPath, drive, dir, dumpbinFilename, ext);
	if (err != 0)
	{
		fprintf(stderr, "_splitpath_s(\"%s\"): error %d\n", absDumpbinPath, err);
		return err;
	}

	printf("Reading dumpbin output from %s%s...\n", dumpbinFilename, ext);
	std::vector<Export> exports;
	if (!ReadDumpbinOutput(absDumpbinPath, libraryFilename, exports))
		return -1;
	if (exports.size() == 0)
	{
		fprintf(stderr, "No exports found. Did you use the x86 version of dumpbin?");
		return -1;
	}
	printf("Parsed %zu exports.\n", exports.size());

	char cPath[MAX_PATH], defPath[MAX_PATH], batPath[MAX_PATH];
	_makepath_s(cPath, drive, dir, dumpbinFilename, ".cpp");
	_makepath_s(defPath, drive, dir, dumpbinFilename, ".def");
	_makepath_s(batPath, drive, dir, dumpbinFilename, ".bat");

	printf("\nCreating C++ file %s.cpp...\n", dumpbinFilename);
	if (!CreateCppFile(cPath, exports))
		return -1;
	printf("Creating exports file %s.def...\n", dumpbinFilename);
	if (!CreateDefFile(defPath, libraryFilename, exports))
		return -1;
	printf("Creating .bat compilation file %s.bat...\n", dumpbinFilename);
	if (!CreateBatFile(batPath, dumpbinFilename, libraryFilename))
		return -1;

	printf("\nGreat success! You can modify the .cpp/.def files now to remove unwanted exports if needed.\n");
	printf("When finished, edit and run the .bat file to generate the import library.\n");
	return 0;
}
