﻿#include "../CommonUtilities/win/WinAPI.h"
#include <boost/process.hpp>
#include "../Core/source/kernel/Kernel.h"
#include "../Core/source/infra/util/FolderResolver.h"
#include "../Interprocess/source/act/SymmetricActionServer.h"
#include "kact/KernelExecutionContext.h"
#include "../AppCef/source/util/cact/TargetLostAction.h"
#include "../AppCef/source/util/cact/OverlayDiedAction.h"
#include "../AppCef/source/util/cact/PresentmonInitFailedAction.h"
#include "../AppCef/source/util/cact/StalePidAction.h"
#include <Core/source/cli/CliOptions.h>
#include <PresentMonAPI2Loader/Loader.h>
#include <Core/source/infra/LogSetup.h>
#include <CommonUtilities/win/Utilities.h>
#include <Versioning/BuildId.h>
#include <Shobjidl.h>
#include <boost/process/v2.hpp>
#include <boost/process/v2/windows/as_user_launcher.hpp>
#include <array>
#include <ranges>


using namespace pmon;
namespace vi = std::views;
namespace rn = std::ranges;
using namespace std::literals;

namespace kproc
{
	using KernelServer = ipc::act::SymmetricActionServer<kact::KernelExecutionContext>;

	class KernelHandler : public p2c::kern::KernelHandler
	{
	public:
		KernelHandler(KernelServer& server) : server_{ server } {}
		void OnTargetLost(uint32_t pid) override
		{
			server_.DispatchDetached(p2c::client::util::cact::TargetLostAction::Params{ pid });
		}
		void OnOverlayDied() override
		{
			server_.DispatchDetached(p2c::client::util::cact::OverlayDiedAction::Params{});
		}
		void OnPresentmonInitFailed() override
		{
			server_.DispatchDetached(p2c::client::util::cact::PresentmonInitFailedAction::Params{});
		}
		void OnStalePidSelected() override
		{
			server_.DispatchDetached(p2c::client::util::cact::StalePidAction::Params{});
		}
	private:
		// data
		KernelServer& server_;
	};
}



void PrintError(const char* fn, const char* role) {
	pmlog_error(std::format("[{}] {} failed, error {}", role, fn, GetLastError()));
}

// Enable a privilege in this process token (returns false if not present)
bool EnablePrivilege(LPCWSTR privName, const char* role) {
	HANDLE hToken = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(),
		TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
		&hToken)) {
		PrintError("EnablePrivilege: OpenProcessToken", role);
		return false;
	}

	LUID luid;
	if (!LookupPrivilegeValueW(nullptr, privName, &luid)) {
		PrintError("EnablePrivilege: LookupPrivilegeValue", role);
		CloseHandle(hToken);
		return false;
	}

	TOKEN_PRIVILEGES tp{ 1, {{luid, SE_PRIVILEGE_ENABLED}} };
	AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
	if (GetLastError() != ERROR_SUCCESS) {
		PrintError("EnablePrivilege: AdjustTokenPrivileges", role);
		CloseHandle(hToken);
		return false;
	}

	CloseHandle(hToken);
	return true;
}

// Returns a primary token at Medium integrity, or nullptr on error
HANDLE PrepareMediumIntegrityToken()
{
	const char* role = "kproc";
	// 0) Make sure we have the privileges to use this token
	if (EnablePrivilege(L"SeIncreaseQuotaPrivilege", role)) {
		std::wcout << L"[" << role << L"] [Spawn]  SeIncreaseQuotaPrivilege enabled\n";
	}
	else {
		PrintError("enable SE_INCREASE_QUOTA_NAME", role);
		return nullptr;
	}

	// 1) Open our elevated token
	HANDLE hElev{};
	if (!OpenProcessToken(
		GetCurrentProcess(),
		TOKEN_DUPLICATE | TOKEN_QUERY,
		&hElev))
	{
		PrintError("OpenProcessToken", role);
		return nullptr;
	}

	// 2) Strip admin rights & set medium IL
	HANDLE hLow{};
	if (!CreateRestrictedToken(
		hElev,
		LUA_TOKEN,   // UAC's built-in filter for medium IL
		0, nullptr,  // no per-SID disables
		0, nullptr,  // no priv removals
		0, nullptr,  // no extra restrict SIDs
		&hLow))
	{
		PrintError("CreateRestrictedToken", role);
		CloseHandle(hElev);
		return nullptr;
	}
	CloseHandle(hElev);

	// 3) Duplicate into a fully-qualified primary token
	HANDLE hDup{};
	DWORD rights =
		TOKEN_QUERY
		| TOKEN_DUPLICATE
		| TOKEN_ASSIGN_PRIMARY
		| TOKEN_ADJUST_DEFAULT
		| TOKEN_ADJUST_GROUPS
		| TOKEN_ADJUST_SESSIONID;
	if (!DuplicateTokenEx(
		hLow,
		rights,
		nullptr,
		SecurityImpersonation,
		TokenPrimary,
		&hDup))
	{
		PrintError("DuplicateTokenEx", role);
		CloseHandle(hLow);
		return nullptr;
	}
	CloseHandle(hLow);

	// 4) Remove High‐Mandatory group, add Medium‐Mandatory group
	SID_IDENTIFIER_AUTHORITY mAuth = SECURITY_MANDATORY_LABEL_AUTHORITY;
	PSID pHigh = nullptr, pMed = nullptr;
	if (AllocateAndInitializeSid(&mAuth, 1, SECURITY_MANDATORY_HIGH_RID,
		0, 0, 0, 0, 0, 0, 0, &pHigh) &&
		AllocateAndInitializeSid(&mAuth, 1, SECURITY_MANDATORY_MEDIUM_RID,
			0, 0, 0, 0, 0, 0, 0, &pMed))
	{
		// Deny the old High label
		TOKEN_GROUPS tg{};
		tg.GroupCount = 1;
		tg.Groups[0].Sid = pHigh;
		tg.Groups[0].Attributes = SE_GROUP_USE_FOR_DENY_ONLY;
		AdjustTokenGroups(hDup, FALSE, &tg, 0, nullptr, nullptr);

		// Enable the Medium label
		tg.Groups[0].Sid = pMed;
		tg.Groups[0].Attributes = SE_GROUP_INTEGRITY | SE_GROUP_ENABLED;
		AdjustTokenGroups(hDup, FALSE, &tg, 0, nullptr, nullptr);

		// Now set the official Mandatory Label
		TOKEN_MANDATORY_LABEL tml{};
		tml.Label.Sid = pMed;
		tml.Label.Attributes = SE_GROUP_INTEGRITY;
		DWORD tmlSize = sizeof(tml) + GetLengthSid(pMed);
		if (!SetTokenInformation(hDup, TokenIntegrityLevel, &tml, tmlSize)) {
			PrintError("SetTokenInformation(TokenIntegrityLevel)", role);
		}

		FreeSid(pHigh);
		// note: do NOT FreeSid(pMed) until after you spawn the process,
		// because the label structure references pMed in the token.
	}
	else {
		PrintError("AllocateAndInitializeSid", role);
		// we'll proceed anyway; CreateTokenRestricted often already did the right IL
	}

	return hDup;
}

bool WeAreElevated()
{
	HANDLE hElev = nullptr; TOKEN_ELEVATION elev{}; DWORD len = 0;
	OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hElev);
	GetTokenInformation(hElev, TokenElevation, &elev, sizeof(elev), &len);
	if (hElev) CloseHandle(hElev);
	return bool(elev.TokenIsElevated);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	using namespace kproc;
	using namespace p2c;

#ifdef NDEBUG
	constexpr bool is_debug = false;
#else
	constexpr bool is_debug = true;
#endif

	try {
		// parse the command line arguments and make them globally available
		if (auto err = cli::Options::Init(__argc, __argv, true)) {
			if (*err == 0) {
				MessageBoxA(nullptr, cli::Options::GetDiagnostics().c_str(), "Command Line Help",
					MB_ICONINFORMATION | MB_APPLMODAL | MB_SETFOREGROUND);
			}
			else {
				MessageBoxA(nullptr, cli::Options::GetDiagnostics().c_str(), "Command Line Parse Error",
					MB_ICONERROR | MB_APPLMODAL | MB_SETFOREGROUND);
			}
			return *err;
		}
		const auto& opt = cli::Options::Get();
		if (opt.filesWorking) {
			infra::util::FolderResolver::SetDevMode();
		}
		// optionally override the middleware dll path (typically when running from IDE in dev cycle)
		if (auto path = opt.middlewareDllPath.AsOptional()) {
			pmLoaderSetPathToMiddlewareDll_(path->c_str());
		}

		// create logging system and ensure cleanup before main ext
		LogChannelManager zLogMan_;

		// configure the logging system (partially based on command line options)
		ConfigureLogging();

		// set the app id so that windows get grouped
		// TODO: verify operation when multiple app instances running concurrently
		SetCurrentProcessExplicitAppUserModelID(L"Intel.PresentMon");

		pmlog_info(std::format("== kernel process starting build#{} clean:{} ==",
			bid::BuildIdShortHash(), !bid::BuildIdDirtyFlag()));

		// launch the service as a child process if desired (typically during development)
		const auto logSvcPipe = opt.logSvcPipe.AsOptional().value_or(
			std::format("pm2-child-svc-log-{}", GetCurrentProcessId()));
		boost::process::child childSvc;
		if (opt.svcAsChild) {
			using namespace std::literals;
			namespace bp = boost::process;

			childSvc = boost::process::child{
				"PresentMonService.exe"s,
				"--control-pipe"s, *opt.controlPipe,
				"--nsm-prefix"s, "pm-frame-nsm"s,
				"--intro-nsm"s, *opt.shmName,
				"--etw-session-name"s, *opt.etwSessionName,
				"--log-level"s, util::log::GetLevelName(util::log::GlobalPolicy::Get().GetLogLevel()),
				"--log-pipe-name"s, logSvcPipe,
				"--enable-stdio-log",
			};

			if (!::pmon::util::win::WaitForNamedPipe(*opt.controlPipe + "-in", 1500)) {
				pmlog_error("timeout waiting for child service control pipe to go online");
				return -1;
			}
		}

		if (opt.logSvcPipeEnable) {
			// connect to service's log pipe (best effort)
			ConnectToLoggingSourcePipe(logSvcPipe);
		}

		//// connect to the middleware diagnostic layer (not generally used by ipm since we connect
		//// to logging directly via copy channel in dev and log middleware to file in production)
		//std::optional<pmapi::DiagnosticHandler> diag;
		//try {
		//    if (opt.enableDiagnostic && opt.cefType && *opt.cefType == "renderer") {
		//        diag.emplace(
		//            (PM_DIAGNOSTIC_LEVEL)opt.logLevel.AsOptional().value_or(log::GlobalPolicy::Get().GetLogLevel()),
		//            PM_DIAGNOSTIC_OUTPUT_FLAGS_DEBUGGER | PM_DIAGNOSTIC_OUTPUT_FLAGS_QUEUE,
		//            [](const PM_DIAGNOSTIC_MESSAGE& msg) {
		//            auto ts = msg.pTimestamp ? msg.pTimestamp : std::string{};
		//            pmlog_(log::Level(msg.level)).note(std::format("@@ D I A G @@ => <{}> {}", ts, msg.pText));
		//        }
		//        );
		//    }
		//} pmcatch_report;

		// this pointer serves as a way to set the kernel on the server exec context after the server is created
		p2c::kern::Kernel* pKernel = nullptr;
		// this server receives a connection from the CEF render process
		const auto actName = std::format(R"(\\.\pipe\ipm-cef-channel-{})", GetCurrentProcessId());
		KernelServer server{ kact::KernelExecutionContext{.ppKernel = &pKernel }, actName, 1, "D:(A;;GA;;;WD)S:(ML;;NW;;;ME)" };
		// this handler receives events from the kernel and transmits them to the render process via the server
		KernelHandler kernHandler{ server };
		// the kernel manages the PresentMon data collection and the overlay rendering
		p2c::kern::Kernel kernel{ &kernHandler };
		// new we set this pointer, giving the server access to the Kernel
		pKernel = &kernel;

		// compose optional cli args for cef process tree
		auto args = std::vector<std::string>{
			opt.filesWorking ? "--p2c-files-working"s : ""s,
			opt.traceExceptions ? "--p2c-trace-exceptions"s : ""s,
			opt.logFolder ? "--p2c-log-folder"s : "", *opt.logFolder,
		} | vi::filter(std::not_fn(&std::string::empty)) | rn::to<std::vector>();
		bool allOriginsAllowed = false;
		for (auto& f : *opt.uiFlags) {
			if (f == "enable-chromium-debug") {
				// needed in order to connect Chrome debuggers to CEF
				args.push_back("--remote-allow-origins=*");
				allOriginsAllowed = true;
			}
			args.push_back("--p2c-" + f);
		}
		for (auto& o : *opt.uiOptions) {
			if (o.first == "url" && is_debug && !allOriginsAllowed) {
				// needed in order to connect Chrome debuggers to CEF
				args.push_back("--remote-allow-origins=*");
			}
			args.push_back("--p2c-" + o.first);
			args.push_back(o.second);
		}
		const auto cefLogPipe = std::format("pm-ui-log-{}", GetCurrentProcessId());
		// add fixed CLI options to the args vector
		args.append_range(std::vector{
			"--p2c-log-level"s, util::log::GetLevelName(*opt.logLevel),
			"--p2c-log-trace-level"s, util::log::GetLevelName(*opt.logTraceLevel),
			"--p2c-act-name"s, actName,
			"--p2c-log-pipe-name"s, cefLogPipe
		}); 
		namespace bp2 = boost::process::v2;
		// launch the CEF browser process, which in turn launches all the other processes in the CEF process constellation
		boost::asio::io_context ioctx;
		auto child = [&] {
			if (WeAreElevated()) {
				pmlog_info("detected elevation, attempting integrity downgrade");
				auto hMediumToken = PrepareMediumIntegrityToken();
				return bp2::windows::as_user_launcher{ hMediumToken }(
					ioctx, "PresentMonUI.exe"s, args
				);
			}
			else {
				return bp2::windows::default_launcher{}(
					ioctx, "PresentMonUI.exe"s, args
				);
			}
		}(); 

		// connect logging to the CEF process constellation
		ConnectToLoggingSourcePipe(cefLogPipe);

		// don't exit this process until the CEF control panel exits
		child.wait();

		pmlog_info("== kernel process exiting ==");
	}
	catch (...) {
		pmlog_error(util::ReportException());
		return -1;
	}

	return 0;
}