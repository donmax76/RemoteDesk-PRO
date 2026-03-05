#pragma once
#include "host.h"
#include "logger.h"
#include <winsvc.h>

class ProcessManager {
public:
    // Returns JSON list of running processes
    static std::string get_process_list() {
        std::ostringstream json;
        json << "{\"cmd\":\"process_list_result\",\"processes\":[";

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            json << "]}";
            return json.str();
        }

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        bool first = true;

        if (Process32FirstW(snap, &pe)) {
            do {
                char name[260];
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name), nullptr, nullptr);

                // Get memory usage
                DWORD pid = pe.th32ProcessID;
                SIZE_T mem = 0;
                HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ, FALSE, pid);
                if (ph) {
                    PROCESS_MEMORY_COUNTERS pmc{};
                    if (GetProcessMemoryInfo(ph, &pmc, sizeof(pmc)))
                        mem = pmc.WorkingSetSize;
                    CloseHandle(ph);
                }

                if (!first) json << ",";
                json << "{\"pid\":" << pid
                     << ",\"name\":\"" << json_escape(name) << "\""
                     << ",\"memory\":" << mem
                     << ",\"threads\":" << pe.cntThreads << "}";
                first = false;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        json << "]}";
        return json.str();
    }

    // Kill process by PID
    static bool kill_process(DWORD pid) {
        HANDLE ph = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!ph) return false;
        bool ok = TerminateProcess(ph, 1) != 0;
        CloseHandle(ph);
        return ok;
    }

    // Launch process
    static bool launch_process(const std::string& path, const std::string& args,
                               const std::string& workdir, bool as_admin = false)
    {
        std::wstring wpath(path.begin(), path.end());
        std::wstring wargs(args.begin(), args.end());
        std::wstring wwork(workdir.begin(), workdir.end());

        if (as_admin) {
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = wpath.c_str();
            sei.lpParameters = wargs.c_str();
            sei.lpDirectory  = workdir.empty() ? nullptr : wwork.c_str();
            sei.nShow = SW_SHOW;
            return ShellExecuteExW(&sei) != 0;
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = wpath;
        if (!wargs.empty()) cmd += L" " + wargs;

        bool ok = CreateProcessW(nullptr, (LPWSTR)cmd.c_str(),
            nullptr, nullptr, FALSE, 0,
            nullptr, workdir.empty() ? nullptr : wwork.c_str(),
            &si, &pi) != 0;
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return ok;
    }

    // Run command via cmd.exe and capture stdout+stderr (for terminal)
    static std::string run_cmd_capture(const std::string& command) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;
        HANDLE hRead = nullptr, hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};

        std::wstring wcmd(L"cmd.exe /c ");
        int wlen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), (int)command.size(), nullptr, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_UTF8, 0, command.c_str(), (int)command.size(), wbuf.data(), wlen);
            wbuf[wlen] = 0;
            wcmd += wbuf.data();
        } else {
            wcmd += std::wstring(command.begin(), command.end());
        }
        std::vector<wchar_t> cmdLine(wcmd.size() + 1);
        wcscpy_s(cmdLine.data(), cmdLine.size(), wcmd.c_str());

        if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0,
                nullptr, nullptr, &si, &pi)) {
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return "";
        }
        CloseHandle(hWrite);
        hWrite = nullptr;

        std::string output;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
            buf[n] = '\0';
            output += buf;
        }
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(hRead);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return output;
    }
};

class ServiceManager {
public:
    static std::string get_services_list() {
        std::ostringstream json;
        json << "{\"cmd\":\"service_list_result\",\"services\":[";

        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (!scm) { json << "]}"; return json.str(); }

        DWORD needed=0, count=0, resume=0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &needed, &count, &resume, nullptr);

        std::vector<uint8_t> buf(needed);
        resume=0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.data(), needed, &needed, &count, &resume, nullptr);

        auto* arr = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
        bool first = true;
        for (DWORD i=0; i<count; ++i) {
            char name[256]={}, disp[256]={};
            WideCharToMultiByte(CP_UTF8, 0, arr[i].lpServiceName, -1, name, 256, nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, arr[i].lpDisplayName, -1, disp, 256, nullptr, nullptr);
            bool running = arr[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;

            if (!first) json << ",";
            json << "{\"name\":\"" << json_escape(name) << "\""
                 << ",\"display\":\"" << json_escape(disp) << "\""
                 << ",\"running\":" << (running?"true":"false") << "}";
            first = false;
        }
        CloseServiceHandle(scm);
        json << "]}";
        return json.str();
    }

    static bool control_service(const std::string& name, const std::string& action) {
        std::wstring wname(name.begin(), name.end());
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) return false;
        SC_HANDLE svc = OpenServiceW(scm, wname.c_str(), SERVICE_ALL_ACCESS);
        if (!svc) { CloseServiceHandle(scm); return false; }

        bool ok = false;
        SERVICE_STATUS ss{};
        if (action == "start")   ok = StartServiceW(svc, 0, nullptr) != 0;
        else if (action == "stop")    ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss) != 0;
        else if (action == "restart") {
            ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            Sleep(2000);
            ok = StartServiceW(svc, 0, nullptr) != 0;
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok;
    }
};
