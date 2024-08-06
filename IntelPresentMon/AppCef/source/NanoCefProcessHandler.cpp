// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#include "NanoCefBrowserClient.h"
#include <Core/source/kernel/Kernel.h>
#include <include/wrapper/cef_helpers.h>
#include "NanoCefProcessHandler.h"
#include "SchemeHandlerFactory.h"
#include "DataBindAccessor.h"
#include <Core/source/infra/Logging.h>
#include "include/wrapper/cef_closure_task.h"
#include <include/cef_task.h>
#include "include/base/cef_callback.h"
#include "util/AsyncEndpointManager.h"
#include "util/CefValues.h"
#include <Core/source/cli/CliOptions.h>
#include <CommonUtilities/mt/Thread.h>
#include <CommonUtilities/log/NamedPipeMarshallReceiver.h>
#include <CommonUtilities/log/EntryMarshallInjector.h>
#include <include/cef_parser.h>

using namespace pmon::util;
using namespace std::chrono_literals;

namespace p2c::client::cef
{
    void NanoCefProcessHandler::OnContextInitialized()
    {
#ifdef NDEBUG
        constexpr bool is_debug = false;
#else
        constexpr bool is_debug = true;
#endif
        const auto& opt = cli::Options::Get();
        std::string host;
        std::string port;

        // parse cli-passed url
        if (opt.url) {
            CefURLParts url_parts;
            if (CefParseURL(*opt.url, url_parts)) {
                host = CefString(&url_parts.host);
                port = CefString(&url_parts.port);
            }
            else {
                pmlog_warn("Bad cli-passed url");
            }
        }
        // use url parts to determine the scheme mode
        using Mode = SchemeHandlerFactory::SchemeMode;
        Mode mode;
        if (host.empty()) {
            mode = Mode::File;
        }
        else if (host == "localhost") {
            mode = Mode::Local;
        }
        else {
            mode = Mode::Web;
        }

        const auto hardFail = is_debug && !opt.noNetFail;

        CefRegisterSchemeHandlerFactory("https", "", new SchemeHandlerFactory{ mode, hardFail, host, port });
        CefRegisterSchemeHandlerFactory("http", "", new SchemeHandlerFactory{ mode, hardFail, std::move(host), std::move(port) });
    }

    CefRefPtr<CefBrowserProcessHandler> NanoCefProcessHandler::GetBrowserProcessHandler()
    {
        return this;
    }

    CefRefPtr<CefRenderProcessHandler> NanoCefProcessHandler::GetRenderProcessHandler()
    {
        return this;
    }

    void NanoCefProcessHandler::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> pChildCommandLine)
    {
        auto& opt = cli::Options::Get();
        // propagate custom cli switches to children
        auto pCmdLine = CefCommandLine::GetGlobalCommandLine();
        for (auto&&[name, val] : opt.GetForwardedOptions()) {
            if (val.empty()) {
                pChildCommandLine->AppendSwitch(std::move(name));
            }
            else {
                pChildCommandLine->AppendSwitchWithValue(std::move(name), std::move(val));
            }
        }
        // initiate logging ipc connection for renderer children
        if (pChildCommandLine->GetSwitchValue("type") == "renderer") {
            // inject logging ipc pipe cli option to child
            static int count = 0;
            std::string pipePrefix = std::format("p2c-logpipe-{}-{}", GetCurrentProcessId(), ++count);
            pChildCommandLine->AppendSwitchWithValue(opt.logPipeName.GetName(), pipePrefix);
            // launch connector thread
            mt::Thread{ "logconn", count, [pipePrefix] {
                try {
                    // wait maximum 1.5sec for pipe to be created
                    if (!pipe::DuplexPipe::WaitForAvailability(R"(\\.\pipe\)" + pipePrefix, 1500)) {
                        pmlog_warn(std::format("Failed to connect to logging source server {} after waiting 1.5s", pipePrefix));
                        return;
                    }
                    // retry connection maximum 3 times, every 50ms
                    const int nAttempts = 3;
                    for (int i = 0; i < nAttempts; i++) {
                        try {
                            auto pChan = log::GetDefaultChannel();
                            auto pReceiver = std::make_shared<log::NamedPipeMarshallReceiver>(pipePrefix, log::IdentificationTable::GetPtr());
                            auto pInjector = std::make_shared<log::EntryMarshallInjector>(pChan, std::move(pReceiver));
                            pChan->AttachComponent(std::move(pInjector));
                            return;
                        }
                        catch (const pipe::PipeError&) {
                            std::this_thread::sleep_for(50ms);
                        }
                    }
                    pmlog_warn(std::format("Failed to connect to logging source server {} after {} attempts", pipePrefix, nAttempts));
                }
                catch (...) {
                    pmlog_error(ReportException());
                }
            } }.detach();
        }
    }

    void NanoCefProcessHandler::AddFunctionToObject_(CefString name, CefRefPtr<CefV8Value>& pObj, CefRefPtr<DataBindAccessor>& pAccessor)
    {
        pObj->SetValue(name, CefV8Value::CreateFunction(name, pAccessor), V8_PROPERTY_ATTRIBUTE_NONE);
    }

    void NanoCefProcessHandler::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
    {
        pAccessor = new DataBindAccessor{ pBrowser, pKernelWrapper.get() };

        auto core = CefV8Value::CreateObject(nullptr, nullptr);
        AddFunctionToObject_("invokeEndpoint", core, pAccessor);
        AddFunctionToObject_("registerSignalHandler", core, pAccessor);
        context->GetGlobal()->SetValue("core", std::move(core), V8_PROPERTY_ATTRIBUTE_NONE);
    }

    void NanoCefProcessHandler::OnBrowserCreated(CefRefPtr<CefBrowser> browser_, CefRefPtr<CefDictionaryValue> extra_info)
    {
        log::IdentificationTable::AddThisThread("cef-proc");

        pBrowser = browser_;

        CefRenderProcessHandler::OnBrowserCreated(std::move(browser_), std::move(extra_info));
    }

    bool NanoCefProcessHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
    {
        if (message->GetName() == util::AsyncEndpointManager::GetResolveMessageName())
        {
            pAccessor->ResolveAsyncEndpoint(
                message->GetArgumentList()->GetInt(0),
                message->GetArgumentList()->GetBool(1),
                message->GetArgumentList()->GetValue(2)
            );
            return true;
        }
        else if (message->GetName() == GetShutdownMessageName())
        {
            // release important resources
            pAccessor->ClearKernelWrapper();
            pKernelWrapper.reset();

            // send shutdown ack
            auto pMsg = CefProcessMessage::Create(GetShutdownMessageName());
            pBrowser->GetMainFrame()->SendProcessMessage(PID_BROWSER, std::move(pMsg));

            return true;
        }
        return false;
    }
}