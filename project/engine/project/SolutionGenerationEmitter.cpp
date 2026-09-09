#include "SolutionGenerationEmitter.h"

#include "../utility/StringUtility.h"
#include "../../externals/nlohmann/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <sstream>

namespace {
	using json = nlohmann::json;

	bool IsReparsePoint(const std::filesystem::path& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	}

	bool HasReparsePointInExistingPath(const std::filesystem::path& path) {
		std::filesystem::path current = path.root_path();
		for (const std::filesystem::path& part : path.relative_path()) {
			current /= part;
			std::error_code error;
			if (!std::filesystem::exists(current, error) || error) break;
			if (IsReparsePoint(current)) return true;
		}
		return !current.empty() && IsReparsePoint(current);
	}

	bool IsPathWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
		const std::wstring childText = child.lexically_normal().generic_wstring();
		std::wstring parentText = parent.lexically_normal().generic_wstring();
		while (parentText.size() > 1 && parentText.back() == L'/') parentText.pop_back();
		if (CompareStringOrdinal(childText.data(), static_cast<int>(childText.size()), parentText.data(), static_cast<int>(parentText.size()), TRUE) == CSTR_EQUAL) return true;
		return childText.size() > parentText.size() &&
			CompareStringOrdinal(childText.data(), static_cast<int>(parentText.size()), parentText.data(), static_cast<int>(parentText.size()), TRUE) == CSTR_EQUAL &&
			childText[parentText.size()] == L'/';
	}

	std::string EscapeXml(const std::string& value) {
		std::string result;
		for (const char character : value) {
			switch (character) {
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '\"': result += "&quot;"; break;
			case '\'': result += "&apos;"; break;
			default: result += character; break;
			}
		}
		return result;
	}

	std::string MsBuildPath(const std::filesystem::path& path) {
		std::string result = StringUtility::ToUtf8(path);
		std::replace(result.begin(), result.end(), '/', '\\');
		return EscapeXml(result);
	}

	std::string MsBuildSourcePath(const std::filesystem::path& path) {
		return "$(CGProjectSourceRoot)" + MsBuildPath(path);
	}

	std::string Fnv1a64(const std::string& content) {
		std::uint64_t value = 14695981039346656037ull;
		for (const unsigned char character : content) {
			value ^= character;
			value *= 1099511628211ull;
		}
		std::ostringstream stream;
		stream << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << value;
		return stream.str();
	}

	std::string GeneratedObjectFileName(const std::filesystem::path& projectRelativePath, std::size_t ordinal) {
		std::ostringstream stream;
		stream << "$(IntDir)src_" << ordinal << '_' << Fnv1a64(projectRelativePath.lexically_normal().generic_string()) << ".obj";
		return stream.str();
	}

	bool WriteUtf8File(const std::filesystem::path& path, const std::string& content, std::string& errorMessage) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			errorMessage = "Preview artifact could not be opened.";
			return false;
		}
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		output.flush();
		if (!output.good()) {
			errorMessage = "Preview artifact could not be written.";
			return false;
		}
		return true;
	}

	std::string BuildProps(const SolutionGenerationModel& model) {
		std::ostringstream output;
		output << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n";
		output << "  <PropertyGroup>\r\n"
			<< "    <CGProjectSourceRoot>$(MSBuildThisFileDirectory)..\\..\\..\\</CGProjectSourceRoot>\r\n"
			<< "    <CGGameRoot>$(CGProjectSourceRoot)..\\</CGGameRoot>\r\n"
			<< "  </PropertyGroup>\r\n";
		for (const std::string& configuration : model.configurations) {
			const bool debug = configuration == "Debug";
			const bool development = configuration == "Development";
			output << "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='" << configuration << "|x64'\">\r\n"
				<< "    <OutDir>$(CGGameRoot)generated\\outputs\\$(ProjectName)\\$(Configuration)\\</OutDir>\r\n"
				<< "    <IntDir>$(CGGameRoot)generated\\obj\\$(ProjectName)\\$(Configuration)\\</IntDir>\r\n"
				<< "  </PropertyGroup>\r\n  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='" << configuration << "|x64'\">\r\n"
				<< "    <ClCompile><WarningLevel>Level3</WarningLevel><SDLCheck>true</SDLCheck><ConformanceMode>true</ConformanceMode><LanguageStandard>stdcpp20</LanguageStandard><AdditionalOptions>/utf-8 /FS /bigobj %(AdditionalOptions)</AdditionalOptions><MultiProcessorCompilation>false</MultiProcessorCompilation><RuntimeLibrary>" << (debug ? "MultiThreadedDebug" : "MultiThreadedDLL") << "</RuntimeLibrary><PreprocessorDefinitions>" << (debug ? "_DEBUG;_WINDOWS" : development ? "NDEBUG;DEVELOPMENT;_WINDOWS" : "NDEBUG;_WINDOWS") << ";%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>$(CGProjectSourceRoot)externals\\DirectXTex;$(CGProjectSourceRoot)externals\\assimp\\include;$(CGProjectSourceRoot)externals\\imgui;$(CGProjectSourceRoot)externals\\ImGuizmo;$(CGProjectSourceRoot);$(CGProjectSourceRoot)Vector;$(CGProjectSourceRoot)Matrix</AdditionalIncludeDirectories></ClCompile>\r\n"
				<< "    <Link><SubSystem>Windows</SubSystem><GenerateDebugInformation>true</GenerateDebugInformation><AdditionalDependencies>DirectXTex.lib;" << (debug ? "assimp-vc143-mtd.lib" : "assimp-vc143-md.lib") << ";%(AdditionalDependencies)</AdditionalDependencies><AdditionalLibraryDirectories>$(CGProjectSourceRoot)externals\\DirectXTex\\Bin\\Desktop_2022_Win10\\x64\\" << (debug ? "Debug" : "Release") << "\\;$(CGProjectSourceRoot)externals\\assimp\\lib\\" << (debug ? "Debug" : "Release") << "</AdditionalLibraryDirectories></Link>\r\n"
				<< "  </ItemDefinitionGroup>\r\n";
		}
		return output.str() + "</Project>\r\n";
	}

	const char* FilterName(SolutionGenerationTargetKind kind) {
		switch (kind) {
		case SolutionGenerationTargetKind::Engine: return "Engine";
		case SolutionGenerationTargetKind::Game: return "Application";
		case SolutionGenerationTargetKind::Host: return "Host";
		default: return "Source";
		}
	}

	std::string BuildProject(const SolutionGenerationModel& model, const SolutionGenerationTarget& target, const SolutionGenerationTarget& engine, const SolutionGenerationTarget& game, const SolutionGenerationTarget& host) {
		const bool isHost = target.kind == SolutionGenerationTargetKind::Host;
		const bool isGame = target.kind == SolutionGenerationTargetKind::Game;
		std::ostringstream output;
		output << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n  <ItemGroup Label=\"ProjectConfigurations\">\r\n";
		for (const std::string& configuration : model.configurations) output << "    <ProjectConfiguration Include=\"" << configuration << "|x64\"><Configuration>" << configuration << "</Configuration><Platform>x64</Platform></ProjectConfiguration>\r\n";
		output << "  </ItemGroup>\r\n  <PropertyGroup Label=\"Globals\"><VCProjectVersion>17.0</VCProjectVersion><Keyword>Win32Proj</Keyword><ProjectGuid>" << target.stableGuid << "</ProjectGuid><RootNamespace>" << EscapeXml(target.name) << "</RootNamespace><WindowsTargetPlatformVersion>" << model.toolchain.windowsSdk << "</WindowsTargetPlatformVersion></PropertyGroup>\r\n  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\r\n";
		for (const std::string& configuration : model.configurations) output << "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='" << configuration << "|x64'\" Label=\"Configuration\"><ConfigurationType>" << (isHost ? "Application" : "StaticLibrary") << "</ConfigurationType><UseDebugLibraries>" << (configuration == "Debug" ? "true" : "false") << "</UseDebugLibraries><PlatformToolset>" << model.toolchain.platformToolset << "</PlatformToolset><CharacterSet>Unicode</CharacterSet></PropertyGroup>\r\n";
		output << "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\r\n  <Import Project=\"$(MSBuildThisFileDirectory)" << EscapeXml(host.name) << ".Common.props\" />\r\n  <ItemGroup>\r\n";
		std::size_t compileOrdinal = 0;
		if (target.kind == SolutionGenerationTargetKind::Engine) {
			const std::initializer_list<std::filesystem::path> engineExternalSources = {
				"externals/ImGuizmo/ImGuizmo.cpp",
				"externals/imgui/imgui.cpp",
				"externals/imgui/imgui_demo.cpp",
				"externals/imgui/imgui_draw.cpp",
				"externals/imgui/imgui_impl_dx12.cpp",
				"externals/imgui/imgui_impl_win32.cpp",
				"externals/imgui/imgui_tables.cpp",
				"externals/imgui/imgui_widgets.cpp"
			};
			for (const std::filesystem::path& externalSource : engineExternalSources) output << "    <ClCompile Include=\"" << MsBuildSourcePath(externalSource) << "\"><ObjectFileName>" << GeneratedObjectFileName(externalSource, compileOrdinal++) << "</ObjectFileName></ClCompile>\r\n";
		}
		for (const SolutionGenerationSourceFile& file : target.sourceFiles) if (file.kind == SolutionGenerationFileKind::Compile) output << "    <ClCompile Include=\"" << MsBuildSourcePath(file.projectRelativePath) << "\"><ObjectFileName>" << GeneratedObjectFileName(file.projectRelativePath, compileOrdinal++) << "</ObjectFileName></ClCompile>\r\n";
		output << "  </ItemGroup>\r\n  <ItemGroup>\r\n";
		for (const SolutionGenerationSourceFile& file : target.sourceFiles) if (file.kind == SolutionGenerationFileKind::Include) output << "    <ClInclude Include=\"" << MsBuildSourcePath(file.projectRelativePath) << "\" />\r\n";
		output << "  </ItemGroup>\r\n";
		if (isHost) output << "  <ItemGroup><ProjectReference Include=\"" << EscapeXml(game.name) << ".vcxproj\"><Project>" << game.stableGuid << "</Project></ProjectReference><ProjectReference Include=\"" << EscapeXml(engine.name) << ".vcxproj\"><Project>" << engine.stableGuid << "</Project></ProjectReference></ItemGroup>\r\n";
		else if (isGame) output << "  <ItemGroup><ProjectReference Include=\"" << EscapeXml(engine.name) << ".vcxproj\"><Project>" << engine.stableGuid << "</Project></ProjectReference></ItemGroup>\r\n";
		output << "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\r\n";
		if (isHost) output << "  <Target Name=\"CopyRuntimeDlls\" AfterTargets=\"Build\"><ItemGroup><RuntimeDll Include=\"$(WindowsSdkDir)Redist\\D3D\\x64\\dxcompiler.dll\" Condition=\"!Exists('$(TargetDir)dxcompiler.dll')\" /><RuntimeDll Include=\"$(WindowsSdkDir)Redist\\D3D\\x64\\dxil.dll\" Condition=\"!Exists('$(TargetDir)dxil.dll')\" /></ItemGroup><Copy SourceFiles=\"@(RuntimeDll)\" DestinationFolder=\"$(TargetDir)\" SkipUnchangedFiles=\"true\" Condition=\"'@(RuntimeDll)' != ''\" /></Target>\r\n  <Target Name=\"CopyRuntimeAssets\" AfterTargets=\"Build\"><ItemGroup><RuntimeResource Include=\"$(CGProjectSourceRoot)resources\\**\\*\" /></ItemGroup><Copy SourceFiles=\"@(RuntimeResource)\" DestinationFiles=\"@(RuntimeResource->'$(TargetDir)resources\\%(RecursiveDir)%(Filename)%(Extension)')\" SkipUnchangedFiles=\"true\" /></Target>\r\n";
		return output.str() + "</Project>\r\n";
	}

	std::string BuildFilters(const SolutionGenerationTarget& target) {
		std::ostringstream output;
		output << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n  <ItemGroup><Filter Include=\"" << FilterName(target.kind) << "\" /></ItemGroup>\r\n  <ItemGroup>\r\n";
		for (const SolutionGenerationSourceFile& file : target.sourceFiles) {
			const char* item = file.kind == SolutionGenerationFileKind::Compile ? "ClCompile" : "ClInclude";
			output << "    <" << item << " Include=\"" << MsBuildSourcePath(file.projectRelativePath) << "\"><Filter>" << FilterName(target.kind) << "</Filter></" << item << ">\r\n";
		}
		return output.str() + "  </ItemGroup>\r\n</Project>\r\n";
	}

	std::string BuildSolution(const SolutionGenerationModel& model, const SolutionGenerationTarget& engine, const SolutionGenerationTarget& game, const SolutionGenerationTarget& host) {
		std::ostringstream output;
		output << "Microsoft Visual Studio Solution File, Format Version 12.00\r\n# Visual Studio Version 17\r\nVisualStudioVersion = 17.13.35931.197\r\nMinimumVisualStudioVersion = 10.0.40219.1\r\n"
			<< "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"" << engine.name << "\", \"" << engine.name << ".vcxproj\", \"" << engine.stableGuid << "\"\r\nEndProject\r\n"
			<< "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"" << game.name << "\", \"" << game.name << ".vcxproj\", \"" << game.stableGuid << "\"\r\n\tProjectSection(ProjectDependencies) = postProject\r\n\t\t" << engine.stableGuid << " = " << engine.stableGuid << "\r\n\tEndProjectSection\r\nEndProject\r\n"
			<< "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"" << host.name << "\", \"" << host.name << ".vcxproj\", \"" << host.stableGuid << "\"\r\n\tProjectSection(ProjectDependencies) = postProject\r\n\t\t" << game.stableGuid << " = " << game.stableGuid << "\r\n\t\t" << engine.stableGuid << " = " << engine.stableGuid << "\r\n\tEndProjectSection\r\nEndProject\r\nGlobal\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n";
		for (const std::string& configuration : model.configurations) output << "\t\t" << configuration << "|x64 = " << configuration << "|x64\r\n";
		output << "\tEndGlobalSection\r\n\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n";
		for (const std::string& configuration : model.configurations) {
			const std::initializer_list<std::string> projectGuids = { engine.stableGuid, game.stableGuid, host.stableGuid };
			for (const std::string& projectGuid : projectGuids) {
				output << "\t\t" << projectGuid << "." << configuration << "|x64.ActiveCfg = " << configuration << "|x64\r\n"
					<< "\t\t" << projectGuid << "." << configuration << "|x64.Build.0 = " << configuration << "|x64\r\n";
			}
		}
		return output.str() + "\tEndGlobalSection\r\nEndGlobal\r\n";
	}
}

bool SolutionGenerationEmitter::EmitPreview(const SolutionGenerationModel& model, const std::filesystem::path& stagingRoot, SolutionGenerationPreview& output, std::string& errorMessage) const {
	output = {};
	std::error_code error;
	const std::filesystem::path root = std::filesystem::absolute(stagingRoot).lexically_normal();
	if (root.empty() || !std::filesystem::is_directory(root, error) || error || HasReparsePointInExistingPath(root) || !std::filesystem::is_empty(root, error) || error) {
		errorMessage = "Preview staging root must be an empty non-reparse directory.";
		return false;
	}
	const SolutionGenerationTarget* engine = nullptr;
	const SolutionGenerationTarget* game = nullptr;
	const SolutionGenerationTarget* host = nullptr;
	for (const SolutionGenerationTarget& target : model.targets) {
		if (target.kind == SolutionGenerationTargetKind::Engine) engine = &target;
		else if (target.kind == SolutionGenerationTargetKind::Game) game = &target;
		else if (target.kind == SolutionGenerationTargetKind::Host) host = &target;
	}
	if (!engine || !game || !host || model.sourceDirectory.empty() || model.artifactDirectory.empty() || model.configurations.empty()) {
		errorMessage = "Solution generation model is incomplete.";
		return false;
	}
	std::error_code relativeError;
	const std::filesystem::path artifactRelativeDirectory = std::filesystem::relative(model.artifactDirectory, model.projectRoot, relativeError).lexically_normal();
	if (relativeError || artifactRelativeDirectory.empty() || artifactRelativeDirectory.is_absolute() || artifactRelativeDirectory.has_root_name() || artifactRelativeDirectory.has_root_directory()) {
		errorMessage = "Solution generation artifact directory is invalid.";
		return false;
	}
	const std::filesystem::path generated = root / artifactRelativeDirectory;
	std::filesystem::create_directories(generated, error);
	if (error || !IsPathWithin(generated, root)) {
		errorMessage = "Preview staging directory could not be created.";
		return false;
	}
	const std::map<std::filesystem::path, std::string> artifacts = {
		{ artifactRelativeDirectory / StringUtility::ToPath(host->name + ".sln"), BuildSolution(model, *engine, *game, *host) },
		{ artifactRelativeDirectory / StringUtility::ToPath(engine->name + ".vcxproj"), BuildProject(model, *engine, *engine, *game, *host) },
		{ artifactRelativeDirectory / StringUtility::ToPath(engine->name + ".vcxproj.filters"), BuildFilters(*engine) },
		{ artifactRelativeDirectory / StringUtility::ToPath(game->name + ".vcxproj"), BuildProject(model, *game, *engine, *game, *host) },
		{ artifactRelativeDirectory / StringUtility::ToPath(game->name + ".vcxproj.filters"), BuildFilters(*game) },
		{ artifactRelativeDirectory / StringUtility::ToPath(host->name + ".vcxproj"), BuildProject(model, *host, *engine, *game, *host) },
		{ artifactRelativeDirectory / StringUtility::ToPath(host->name + ".vcxproj.filters"), BuildFilters(*host) },
		{ artifactRelativeDirectory / StringUtility::ToPath(host->name + ".Common.props"), BuildProps(model) }
	};
	output.stagingRoot = root;
	output.inputIdentity = ComputeInputIdentity(model);
	for (const auto& [relativePath, content] : artifacts) {
		const std::filesystem::path target = (root / relativePath).lexically_normal();
		if (!IsPathWithin(target, root)) {
			errorMessage = "Preview artifact path is outside the staging root.";
			output = {};
			return false;
		}
		std::filesystem::create_directories(target.parent_path(), error);
		if (error || !WriteUtf8File(target, content, errorMessage)) {
			output = {};
			return false;
		}
		output.artifacts.push_back({ relativePath, Fnv1a64(content) });
	}
	json manifest = { { "artifacts", json::array() }, { "inputIdentity", output.inputIdentity }, { "schemaVersion", 1 } };
	for (const SolutionGenerationArtifact& artifact : output.artifacts) manifest["artifacts"].push_back({ { "contentHash", artifact.contentHash }, { "path", StringUtility::ToUtf8(artifact.relativePath) } });
	output.manifestPath = std::filesystem::path(L"project") / L"build" / L"generated" / L"solution-generation.json";
	const std::filesystem::path manifestTarget = root / output.manifestPath;
	if (!WriteUtf8File(manifestTarget, manifest.dump(2) + "\n", errorMessage)) {
		output = {};
		return false;
	}
	return true;
}

std::string SolutionGenerationEmitter::ComputeInputIdentity(const SolutionGenerationModel& model) const {
	std::ostringstream input;
	input << "cg3.solution-generation/input/v8\n"
		<< "layout=grouped-v1\n";
	std::error_code relativeError;
	const std::filesystem::path artifactRelativeDirectory = std::filesystem::relative(model.artifactDirectory, model.projectRoot, relativeError).lexically_normal();
	if (!relativeError) {
		input << "artifactDirectory=" << artifactRelativeDirectory.generic_string() << '\n';
	}
	for (const std::string& configuration : model.configurations) input << "configuration=" << configuration << '\n';
	input << "platform=" << model.toolchain.platform << '\n'
		<< "toolset=" << model.toolchain.platformToolset << '\n'
		<< "sdk=" << model.toolchain.windowsSdk << '\n'
		<< "assimp=" << model.externals.assimp << '\n'
		<< "directXTex=" << model.externals.directXTexProject << '\n'
		<< "imgui=" << model.externals.imguiProject << '\n'
		<< "copyResources=" << (model.runtime.copyResources ? "true" : "false") << '\n'
		<< "resourceRoot=" << model.runtime.resourceRoot << '\n';
	for (const SolutionGenerationTarget& target : model.targets) {
		input << "target=" << static_cast<int>(target.kind) << ':' << target.name << ':' << target.stableGuid << '\n';
		for (const SolutionGenerationSourceFile& file : target.sourceFiles) {
			input << "source=" << static_cast<int>(file.kind) << ':' << file.projectRelativePath.generic_string() << '\n';
		}
	}
	return Fnv1a64(input.str());
}
