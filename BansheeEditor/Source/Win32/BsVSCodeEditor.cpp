#include "Win32/BsVSCodeEditor.h"
#include <windows.h>
#include <atlbase.h>
#include "BsUtil.h"

// Import EnvDTE
#import "libid:80cc9f66-e7d8-4ddd-85b6-d9e6cd0e93e2" version("8.0") lcid("0") raw_interfaces_only named_guids

namespace BansheeEngine
{
	enum class VisualStudioVersion
	{
		VS2008,
		VS2010,
		VS2012,
		VS2013,
		VS2015
	};

	LONG getRegistryStringValue(HKEY hKey, const WString& name, WString& value, const WString& defaultValue)
	{
		value = defaultValue;

		wchar_t strBuffer[512];
		DWORD strBufferSize = sizeof(strBuffer);
		ULONG result = RegQueryValueExW(hKey, name.c_str(), 0, nullptr, (LPBYTE)strBuffer, &strBufferSize);
		if (result == ERROR_SUCCESS)
			value = strBuffer;

		return result;
	}

	struct VSProjectInfo
	{
		WString GUID;
		WString name;
		Path path;
	};

	class VisualStudio
	{
	private:
		static const WString SLN_TEMPLATE;
		static const WString PROJ_ENTRY_TEMPLATE;
		static const WString PROJ_PLATFORM_TEMPLATE;

		static const WString PROJ_TEMPLATE;
		static const WString REFERENCE_ENTRY_TEMPLATE;
		static const WString REFERENCE_PATH_ENTRY_TEMPLATE;
		static const WString CODE_ENTRY_TEMPLATE;
		static const WString NON_CODE_ENTRY_TEMPLATE;

	public:
		static CComPtr<EnvDTE::_DTE> findRunningInstance(const CLSID& clsID, const Path& solutionPath)
		{
			CComPtr<IRunningObjectTable> runningObjectTable = nullptr;
			if (FAILED(GetRunningObjectTable(0, &runningObjectTable)))
				return nullptr;

			CComPtr<IEnumMoniker> enumMoniker = nullptr;
			if (FAILED(runningObjectTable->EnumRunning(&enumMoniker)))
				return nullptr;

			CComPtr<IMoniker> dteMoniker = nullptr;
			if (FAILED(CreateClassMoniker(clsID, &dteMoniker)))
				return nullptr;

			CComBSTR bstrSolution(solutionPath.toWString(Path::PathType::Windows).c_str());
			CComPtr<IMoniker> moniker;
			ULONG count = 0;
			while (enumMoniker->Next(1, &moniker, &count) == S_OK)
			{
				if (moniker->IsEqual(dteMoniker))
				{
					CComPtr<IUnknown> curObject = nullptr;
					HRESULT result = runningObjectTable->GetObject(moniker, &curObject);
					moniker = nullptr;

					if (result != S_OK)
						continue;

					CComPtr<EnvDTE::_DTE> dte = curObject;
					if (dte == nullptr)
						return nullptr;

					CComPtr<EnvDTE::_Solution> solution;
					if (FAILED(dte->get_Solution(&solution)))
						continue;

					CComBSTR fullName;
					if (FAILED(solution->get_FullName(&fullName)))
						continue;

					if (fullName == bstrSolution)
						return dte;
				}
			}

			return nullptr;
		}

		static CComPtr<EnvDTE::_DTE> openInstance(const CLSID& clsid, const Path& solutionPath)
		{
			CComPtr<IUnknown> newInstance = nullptr;
			if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, EnvDTE::IID__DTE, (LPVOID*)&newInstance)))
				return nullptr;

			CComPtr<EnvDTE::_DTE> dte = newInstance;
			if (dte == nullptr)
				return nullptr;

			dte->put_UserControl(TRUE);

			CComPtr<EnvDTE::_Solution> solution;
			if (FAILED(dte->get_Solution(&solution)))
				return nullptr;

			CComBSTR bstrSolution(solutionPath.toWString(Path::PathType::Windows).c_str());
			if (FAILED(solution->Open(bstrSolution)))
				return nullptr;

			// Wait until VS opens
			UINT32 elapsed = 0;
			while (elapsed < 10000)
			{
				EnvDTE::Window* window = nullptr;
				if (SUCCEEDED(dte->get_MainWindow(&window)))
					return dte;

				Sleep(100);
				elapsed += 100;
			}

			return nullptr;
		}

		static bool openFile(CComPtr<EnvDTE::_DTE> dte, const Path& filePath, UINT32 line)
		{
			// Open file
			CComPtr<EnvDTE::ItemOperations> itemOperations;
			if (FAILED(dte->get_ItemOperations(&itemOperations)))
				return false;

			CComBSTR bstrFilePath(filePath.toWString(Path::PathType::Windows).c_str());
			CComBSTR bstrKind(EnvDTE::vsViewKindPrimary);
			CComPtr<EnvDTE::Window> window = nullptr;
			if (FAILED(itemOperations->OpenFile(bstrFilePath, bstrKind, &window)))
				return false;

			// Scroll to line
			CComPtr<EnvDTE::Document> activeDocument;
			if (SUCCEEDED(dte->get_ActiveDocument(&activeDocument)))
			{
				CComPtr<IDispatch> selection;
				if (SUCCEEDED(activeDocument->get_Selection(&selection)))
				{
					CComPtr<EnvDTE::TextSelection> textSelection;
					if (SUCCEEDED(selection->QueryInterface(&textSelection)))
					{
						textSelection->GotoLine(line, TRUE);
					}
				}
			}

			// Bring the window in focus
			window = nullptr;
			if (SUCCEEDED(dte->get_MainWindow(&window)))
			{
				window->Activate();

				HWND hWnd;
				window->get_HWnd((LONG*)&hWnd);
				SetForegroundWindow(hWnd);
			}

			return true;
		}

		static String getSolutionGUID(const WString& solutionName)
		{
			static const String guidTemplate = "{0}-{1}-{2}-{3}-{4}";
			String hash = md5(L"SLN_" + solutionName);

			return StringUtil::format(guidTemplate, hash.substr(0, 8), hash.substr(8, 4), hash.substr(12, 4), hash.substr(16, 4), hash.substr(20, 12));
		}

		static String getProjectGUID(const WString& projectName)
		{
			static const String guidTemplate = "{0}-{1}-{2}-{3}-{4}";
			String hash = md5(L"PRJ_" + projectName);

			return StringUtil::format(guidTemplate, hash.substr(0, 8), hash.substr(8, 4), hash.substr(12, 4), hash.substr(16, 4), hash.substr(20, 12));
		}

		static WString writeSolution(VisualStudioVersion version, const WString& name, const Vector<VSProjectInfo>& projects)
		{
			struct VersionData
			{
				WString formatVersion;
			};

			Map<VisualStudioVersion, VersionData> versionData =
			{
				{ VisualStudioVersion::VS2008, { L"10.0" } },
				{ VisualStudioVersion::VS2010, { L"11.0" } },
				{ VisualStudioVersion::VS2012, { L"12.0" } },
				{ VisualStudioVersion::VS2013, { L"12.0" } },
				{ VisualStudioVersion::VS2015, { L"12.0" } }
			};

			String solutionGUID = getSolutionGUID(name);

			WStringStream projectEntriesStream;
			WStringStream projectPlatformsStream;
			for (auto& project : projects)
			{
				projectEntriesStream << StringUtil::format(PROJ_ENTRY_TEMPLATE, toWString(solutionGUID), project.name, project.path.toWString(), project.GUID) << std::endl;
				projectPlatformsStream << StringUtil::format(PROJ_PLATFORM_TEMPLATE, project.GUID) << std::endl;
			}

			WString projectEntries = projectEntriesStream.str();
			WString projectPlatforms = projectPlatformsStream.str();

			return StringUtil::format(SLN_TEMPLATE, versionData[version].formatVersion, projectEntries, projectPlatforms);
		}

		static WString writeProject(VisualStudioVersion version, const CodeProjectData& projectData)
		{
			struct VersionData
			{
				WString toolsVersion;
			};

			Map<VisualStudioVersion, VersionData> versionData =
			{
				{ VisualStudioVersion::VS2008, { L"3.5" } },
				{ VisualStudioVersion::VS2010, { L"4.0" } },
				{ VisualStudioVersion::VS2012, { L"4.0" } },
				{ VisualStudioVersion::VS2013, { L"12.0" } },
				{ VisualStudioVersion::VS2015, { L"13.0" } }
			};

			WStringStream tempStream;
			for (auto& codeEntry : projectData.codeFiles)
				tempStream << StringUtil::format(CODE_ENTRY_TEMPLATE, codeEntry.toWString()) << std::endl;

			WString codeEntries = tempStream.str();
			tempStream.str(L"");
			tempStream.clear();

			for (auto& nonCodeEntry : projectData.nonCodeFiles)
				tempStream << StringUtil::format(NON_CODE_ENTRY_TEMPLATE, nonCodeEntry.toWString()) << std::endl;

			WString nonCodeEntries = tempStream.str();
			tempStream.str(L"");
			tempStream.clear();

			for (auto& referenceEntry : projectData.references)
			{
				if (referenceEntry.path.isEmpty())
					tempStream << StringUtil::format(REFERENCE_ENTRY_TEMPLATE, referenceEntry.name) << std::endl;
				else
					tempStream << StringUtil::format(REFERENCE_PATH_ENTRY_TEMPLATE, referenceEntry.name, referenceEntry.path.toWString()) << std::endl;
			}

			WString referenceEntries = tempStream.str();
			tempStream.str(L"");
			tempStream.clear();

			for (auto& define : projectData.defines)
				tempStream << define << L";";

			WString defines = tempStream.str();
			WString projectGUID = toWString(getProjectGUID(projectData.name));

			return StringUtil::format(PROJ_ENTRY_TEMPLATE, versionData[version].toolsVersion, projectGUID, 
				projectData.name, defines, referenceEntries, codeEntries, nonCodeEntries);
		}
	};

	const WString VisualStudio::SLN_TEMPLATE =
		LR"(Microsoft Visual Studio Solution File, Format Version {0}
{1}
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|Any CPU = Debug|Any CPU
		Release|Any CPU = Release|Any CPU
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{2}
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
EndGlobal)";

	const WString VisualStudio::PROJ_ENTRY_TEMPLATE =
		LR"(Project("\{{0}\}") = "{1}", "{2}", "\{{3}\}"
EndProject)";

	const WString VisualStudio::PROJ_PLATFORM_TEMPLATE =
		LR"(\{{0}\}.Debug|Any CPU.ActiveCfg = Debug|Any CPU
		\{{0}\}.Debug|Any CPU.Build.0 = Debug|Any CPU
		\{{0}\}.Release|Any CPU.ActiveCfg = Release|Any CPU
		\{{0}\}.Release|Any CPU.Build.0 = Release|Any CPU)";

	const WString VisualStudio::PROJ_TEMPLATE =
		LR"literal(<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="{0}" DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(MSBuildExtensionsPath)\$(MSBuildToolsVersion)\Microsoft.Common.props" Condition="Exists('$(MSBuildExtensionsPath)\$(MSBuildToolsVersion)\Microsoft.Common.props')" />
  <PropertyGroup>
    <Configuration Condition = " '$(Configuration)' == '' ">Debug</Configuration>
    <Platform Condition = " '$(Platform)' == '' ">AnyCPU</Platform>
    <ProjectGuid>\{{1}\}</ProjectGuid>
    <OutputType>Library</OutputType>
    <AppDesignerFolder>Properties</AppDesignerFolder>
    <RootNamespace></RootNamespace>
    <AssemblyName>{2}</AssemblyName>
    <TargetFrameworkVersion>v4.0</TargetFrameworkVersion>
    <FileAlignment>512</FileAlignment>
    <BaseDirectory>Resources</BaseDirectory>
    <SchemaVersion>2.0</SchemaVersion>
  </PropertyGroup>
    <PropertyGroup Condition = " '$(Configuration)|$(Platform)' == 'Debug|AnyCPU' ">
    <DebugSymbols>true</DebugSymbols>
    <DebugType>full</DebugType>
    <Optimize>false</Optimize>
    <OutputPath>Internal\Temp\Assemblies\Debug\</OutputPath>
    <DefineConstants>DEBUG;TRACE;{3}</DefineConstants>
    <ErrorReport>prompt</ErrorReport>
    <WarningLevel>4</WarningLevel >
  </PropertyGroup>
  <PropertyGroup Condition = " '$(Configuration)|$(Platform)' == 'Release|AnyCPU' ">
    <DebugType>pdbonly</DebugType>
    <Optimize>true</Optimize>
    <OutputPath>Internal\Temp\Assemblies\Release\</OutputPath>
    <DefineConstants>TRACE;{3}</DefineConstants>
    <ErrorReport>prompt</ErrorReport>
    <WarningLevel>4</WarningLevel>
  </PropertyGroup>
  <ItemGroup>
{4}
  </ItemGroup>
  <ItemGroup>
{5}
  </ItemGroup>
  <ItemGroup>
{6}
  </ItemGroup>
  <Import Project = "$(MSBuildToolsPath)\Microsoft.CSharp.targets"/>
</Project>)literal";

	const WString VisualStudio::REFERENCE_ENTRY_TEMPLATE =
		LR"(    <Reference Include="{0}"/>)";

	const WString VisualStudio::REFERENCE_PATH_ENTRY_TEMPLATE =
		LR"(    <Reference Include="{0}">
      <HintPath>{1}</HintPath>
    </Reference>)";

	const WString VisualStudio::CODE_ENTRY_TEMPLATE =
		LR"(    <Compile Include="{0}"/>)";

	const WString VisualStudio::NO_CODE_ENTRY_TEMPLATE =
		LR"(    <None Include="{0}"/>)";

	VSCodeEditor::VSCodeEditor(const Path& execPath, const WString& CLSID)
		:mCLSID(CLSID), mExecPath(execPath)
	{
		
	}

	void VSCodeEditor::openFile(const Path& solutionPath, const Path& filePath, UINT32 lineNumber) const
	{
		CLSID clsID;
		if (FAILED(CLSIDFromString(mCLSID.toWString().c_str(), &clsID)))
			return;

		CComPtr<EnvDTE::_DTE> dte = VisualStudio::findRunningInstance(clsID, solutionPath);
		if (dte == nullptr)
			dte = VisualStudio::openInstance(clsID, solutionPath);

		if (dte == nullptr)
			return;

		VisualStudio::openFile(dte, filePath, lineNumber);
	}

	void VSCodeEditor::syncSolution(const CodeSolutionData& data, const Path& outputPath) const
	{
		// TODO
	}

	VSCodeEditorFactory::VSCodeEditorFactory()
		:mAvailableVersions(getAvailableVersions())
	{ 
		for (auto& version : mAvailableVersions)
			mAvailableEditors.push_back(version.first);
	}

	Map<WString, VSCodeEditorFactory::VSVersionInfo> VSCodeEditorFactory::getAvailableVersions() const
	{
#if BS_ARCH_TYPE == BS_ARCHITECTURE_x86_64
		bool is64bit = true;
#else
		bool is64bit = false;
		IsWow64Process(GetCurrentProcess(), &is64bit);
#endif

		WString registryKeyRoot;
		if (is64bit)
			registryKeyRoot = L"SOFTWARE\\Microsoft";
		else
			registryKeyRoot = L"SOFTWARE\\Wow6432Node\\Microsoft";

		struct VersionData
		{
			WString registryKey;
			WString name;
			WString executable;
		};

		Map<VisualStudioVersion, VersionData> versionToVersionNumber =
		{ 
			{ VisualStudioVersion::VS2008, { L"VisualStudio\\9.0", L"Visual Studio 2008", L"devenv.exe" } },
			{ VisualStudioVersion::VS2010, { L"VisualStudio\\10.0", L"Visual Studio 2010", L"devenv.exe" } },
			{ VisualStudioVersion::VS2012, { L"VisualStudio\\11.0", L"Visual Studio 2012", L"devenv.exe" } },
			{ VisualStudioVersion::VS2013, { L"VisualStudio\\12.0", L"Visual Studio 2013", L"devenv.exe" } },
			{ VisualStudioVersion::VS2015, { L"VisualStudio\\13.0", L"Visual Studio 2015", L"devenv.exe" } }
		};

		Map<WString, VSVersionInfo> versionInfo;
		for(auto version : versionToVersionNumber)
		{
			WString registryKey = registryKeyRoot + L"\\" + version.second.registryKey;

			HKEY regKey;
			LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, registryKeyRoot.c_str(), 0, KEY_READ, &regKey);
			if (result != ERROR_SUCCESS)
				continue;

			WString installPath;
			getRegistryStringValue(regKey, L"InstallDir", installPath, StringUtil::WBLANK);
			if (installPath.empty())
				continue;

			WString clsID;
			getRegistryStringValue(regKey, L"ThisVersionDTECLSID", clsID, StringUtil::WBLANK);

			VSVersionInfo info;
			info.name = version.second.name;
			info.execPath = installPath.append(version.second.executable);
			info.CLSID = clsID;

			versionInfo[info.name] = info;
		}

		// TODO - Also query for VSExpress and VSCommunity (their registry keys are different)

		return versionInfo;
	}

	CodeEditor* VSCodeEditorFactory::create(const WString& editor) const
	{
		auto findIter = mAvailableVersions.find(editor);
		if (findIter == mAvailableVersions.end())
			return nullptr;

		// TODO - Also create VSExpress and VSCommunity editors

		return bs_new<VSCodeEditor>(findIter->second.execPath, findIter->second.CLSID);
	}
}