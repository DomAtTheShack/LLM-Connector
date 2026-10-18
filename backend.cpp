// backend.cpp - Windows, Linux and macOS implementations of backend.h.
// Shared code comes first, then one #ifdef block per OS.
#include "backend.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <utility>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <mmsystem.h>
    #include <io.h>
#else
    #ifdef __APPLE__
        #include <pulse/pulseaudio.h>
    #else
        #include <alsa/asoundlib.h>
    #endif
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <glob.h>
    #include <poll.h>
    #include <signal.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace os {

// ---- Shared ------------------------------------------------------------------

Process::~Process() {
    closeStdin();
    closePipe();
    // Don't leave a child running (or a zombie) behind once nothing can talk to it
    if (running()) kill();
    wait();
}

Process::Process(Process&& other) noexcept { *this = std::move(other); }

int runAndPipe(const std::vector<std::string>& argv,
               const std::function<void(std::string_view)>& onOutput) {
    Process process;
    if (!process.start(argv)) {
        std::cerr << process.error() << std::endl;
        return -1;
    }
    char buffer[256];
    long bytesRead;
    while ((bytesRead = process.read(buffer, sizeof buffer)) > 0) {
        onOutput(std::string_view(buffer, static_cast<size_t>(bytesRead)));
    }
    if (bytesRead == -1) std::cerr << process.error() << std::endl;
    return process.wait();
}

#ifdef _WIN32
// ==== Windows =================================================================

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);
    return result;
}

std::string narrow(const wchar_t* s) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(len - 1, '\0');   // len counts the null terminator
    WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// "CreatePipe: The system cannot find the file specified." style messages
std::string lastErrorMessage(const char* what) {
    DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = std::string(what) + ": " + (text ? narrow(text) : std::to_string(code));
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message;
}

// Windows passes one command line string, which the child splits back into argv.
// Quotes each argument so it splits back exactly (the CommandLineToArgvW rules)
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');   // Escape them so the closing quote survives
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += *it;
    }
    cmd += L'"';
}

} // namespace

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        std::swap(processHandle, other.processHandle);
        std::swap(pipeRead, other.pipeRead);
        std::swap(pipeWrite, other.pipeWrite);
        std::swap(exitCode, other.exitCode);
        std::swap(lastError, other.lastError);
    }
    return *this;
}

void Process::closePipe() {
    if (pipeRead) {
        CloseHandle(static_cast<HANDLE>(pipeRead));
        pipeRead = nullptr;
    }
}

bool Process::start(const std::vector<std::string>& argv, bool captureStdout, bool pipeStdin) {
    if (argv.empty()) {
        lastError = "start: no program given";
        return false;
    }
    std::wstring cmdLine;
    for (const std::string& arg : argv) {
        if (!cmdLine.empty()) cmdLine += L' ';
        appendQuoted(cmdLine, widen(arg));
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};   // Inheritable, so the child gets them
    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE inRead  = nullptr, inWrite  = nullptr;
    if (captureStdout) {
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
            lastError = lastErrorMessage("CreatePipe");
            return false;
        }
        // Only the write end goes to the child
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }
    if (pipeStdin) {
        if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
            lastError = lastErrorMessage("CreatePipe");
            if (outRead) { CloseHandle(outRead); CloseHandle(outWrite); }
            return false;
        }
        // Only the read end goes to the child
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (captureStdout || pipeStdin) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = pipeStdin     ? inRead   : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = captureStdout ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    // No application name: Windows takes the program from the command line and searches PATH
    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0,
                             nullptr, nullptr, &si, &pi);
    if (!ok) lastError = lastErrorMessage(argv[0].c_str());
    // Our copies of the child's ends have to close, or read() never sees end of output
    // and the child never sees end of input
    if (outWrite) CloseHandle(outWrite);
    if (inRead)   CloseHandle(inRead);
    if (!ok) {
        if (outRead) CloseHandle(outRead);
        if (inWrite) CloseHandle(inWrite);
        return false;
    }
    CloseHandle(pi.hThread);
    processHandle = pi.hProcess;
    pipeRead      = outRead;
    pipeWrite     = inWrite;
    exitCode      = -1;
    return true;
}

bool Process::write(std::string_view data) {
    if (!pipeWrite) return false;
    while (!data.empty()) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(pipeWrite), data.data(), static_cast<DWORD>(data.size()),
                       &written, nullptr)) {
            lastError = lastErrorMessage("WriteFile");
            return false;
        }
        data.remove_prefix(written);
    }
    return true;
}

void Process::closeStdin() {
    if (pipeWrite) {
        CloseHandle(static_cast<HANDLE>(pipeWrite));
        pipeWrite = nullptr;
    }
}

long Process::read(char* buf, size_t size) {
    if (!pipeRead) return 0;
    DWORD bytesRead = 0;
    if (!ReadFile(static_cast<HANDLE>(pipeRead), buf, static_cast<DWORD>(size), &bytesRead, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;   // The child closed its stdout
        lastError = lastErrorMessage("ReadFile");
        return -1;
    }
    return static_cast<long>(bytesRead);
}

int Process::wait() {
    if (!processHandle) return exitCode;
    HANDLE h = static_cast<HANDLE>(processHandle);
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    exitCode = GetExitCodeProcess(h, &code) ? static_cast<int>(code) : -1;
    CloseHandle(h);
    processHandle = nullptr;
    return exitCode;
}

bool Process::running() const {
    return processHandle && WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) == WAIT_TIMEOUT;
}

void Process::kill() {
    if (processHandle) TerminateProcess(static_cast<HANDLE>(processHandle), 1);
}

std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    if (direction == AudioDirection::Capture) {
        UINT count = waveInGetNumDevs();
        for (UINT i = 0; i < count; i++) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
                devices.push_back({std::to_string(i), narrow(caps.szPname)});
        }
    } else {
        UINT count = waveOutGetNumDevs();
        for (UINT i = 0; i < count; i++) {
            WAVEOUTCAPSW caps{};
            if (waveOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
                devices.push_back({std::to_string(i), narrow(caps.szPname)});
        }
    }
    return devices;
}

bool fileExists(const std::string& path) {
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool fileReadable(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool isTerminal(FILE* stream) {
    return _isatty(_fileno(stream)) != 0;
}

void setRawMode(bool enable) {
    static DWORD oldMode;
    static bool active = false;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!isTerminal(stdin)) return;
    if (enable && !active) {
        GetConsoleMode(in, &oldMode);
        DWORD newMode = oldMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        // Arrow keys then arrive as "ESC [ A", the same as a Linux terminal sends
        newMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(in, newMode);
        active = true;
    } else if (!enable && active) {
        SetConsoleMode(in, oldMode);
        active = false;
    }
}

void setInterruptKey(bool enable) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!isTerminal(stdin) || !GetConsoleMode(in, &mode)) return;
    // Processed input is what turns Ctrl-C into a signal instead of a key
    SetConsoleMode(in, enable ? mode | ENABLE_PROCESSED_INPUT : mode & ~ENABLE_PROCESSED_INPUT);
}

int readInputByte() {
    unsigned char c;
    DWORD bytesRead = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &bytesRead, nullptr) || bytesRead == 0)
        return -1;
    return c;
}

bool waitForInput(int timeoutMs) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!isTerminal(stdin)) return WaitForSingleObject(in, timeoutMs) == WAIT_OBJECT_0;
    // The console also wakes up for key releases, focus changes and mouse moves, which
    // ReadFile skips over. Drop those so a wakeup means a byte is really there
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        INPUT_RECORD record;
        DWORD count = 0;
        while (PeekConsoleInputW(in, &record, 1, &count) && count == 1) {
            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
                record.Event.KeyEvent.uChar.UnicodeChar != 0) return true;
            ReadConsoleInputW(in, &record, 1, &count);
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        WaitForSingleObject(in, static_cast<DWORD>(deadline - now));
    }
}

std::vector<std::string> listSerialPorts() {
    std::vector<std::string> ports;
    HKEY key;
    // Every serial port Windows knows about, including USB ones, is listed here
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) return ports;
    for (DWORD i = 0;; i++) {
        wchar_t name[256], data[64];
        DWORD nameLen = std::size(name), dataSize = sizeof data, type;
        LONG status = RegEnumValueW(key, i, name, &nameLen, nullptr, &type,
                                    reinterpret_cast<BYTE*>(data), &dataSize);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || type != REG_SZ) continue;
        data[std::size(data) - 1] = L'\0';
        ports.push_back(narrow(data));
    }
    RegCloseKey(key);
    // COM2 before COM10
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

void enableAnsiEscapes() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}

namespace {

bool isFile(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// A Cygwin program needs cygwin1.dll, which lives in <cygwin>\bin. Windows only finds it
// through PATH, so when minimodem sits under a Cygwin install put that bin folder on our
// PATH; the child inherits it
void addCygwinDllToPath(const std::wstring& exePath) {
    for (const wchar_t* suffix : {L"\\usr\\local\\bin\\minimodem.exe", L"\\bin\\minimodem.exe"}) {
        std::wstring tail = suffix;
        if (exePath.size() <= tail.size() ||
            _wcsicmp(exePath.c_str() + exePath.size() - tail.size(), tail.c_str()) != 0) continue;
        std::wstring binDir = exePath.substr(0, exePath.size() - tail.size()) + L"\\bin";
        if (!isFile(binDir + L"\\cygwin1.dll")) return;
        DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring path(len, L'\0');
        GetEnvironmentVariableW(L"PATH", path.data(), len);
        path.resize(len ? len - 1 : 0);
        SetEnvironmentVariableW(L"PATH", (binDir + L";" + path).c_str());
        return;
    }
}

// PATH first, then the usual Cygwin install folders. A program started before minimodem's
// folder was added to PATH (e.g. CLion left open since then) still has the old PATH
std::string findMinimodem() {
    wchar_t found[MAX_PATH];
    std::wstring exe;
    if (SearchPathW(nullptr, L"minimodem.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        exe = found;
    } else {
        for (const wchar_t* candidate : {L"C:\\cygwin64\\usr\\local\\bin\\minimodem.exe",
                                         L"C:\\cygwin64\\bin\\minimodem.exe",
                                         L"C:\\cygwin\\usr\\local\\bin\\minimodem.exe",
                                         L"C:\\cygwin\\bin\\minimodem.exe"}) {
            if (isFile(candidate)) { exe = candidate; break; }
        }
    }
    if (exe.empty()) return "minimodem.exe";   // start() then reports it can't be found
    addCygwinDllToPath(exe);
    return narrow(exe.c_str());
}

} // namespace

const char* minimodemPath() {
    static const std::string path = findMinimodem();
    return path.c_str();
}

namespace {
BOOL WINAPI swallowInterrupt(DWORD type) {
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}
} // namespace

// Children share our console, so they still get Ctrl-C. Unlike a null handler (which
// children inherit), a handler function only applies to us
void catchInterrupt(bool enable) {
    SetConsoleCtrlHandler(swallowInterrupt, enable ? TRUE : FALSE);
}

#else
// ==== Linux and macOS =========================================================

namespace {

std::string errnoMessage(const std::string& what, int err) {
    return what + ": " + std::strerror(err);
}

#ifndef __APPLE__

// ALSA descriptions span several lines, e.g. "HDA Intel PCH, ALC892 Analog\nFront speakers".
// Joins the non-empty lines into one: "HDA Intel PCH, ALC892 Analog - Front speakers"
std::string formatDescription(const char* desc) {
    std::string result;
    std::string line;
    auto addLine = [&] {
        size_t start = line.find_first_not_of(" \t");
        size_t end   = line.find_last_not_of(" \t");
        if (start != std::string::npos) {
            if (!result.empty()) result += " - ";
            result += line.substr(start, end - start + 1);
        }
        line.clear();
    };
    for (const char* c = desc; *c != '\0'; ++c) {
        if (*c == '\n') addLine();
        else line += *c;
    }
    addLine();
    return result;
}

// Many hinted devices (lavrate, speex, upmix, ...) wrap a hardware card that doesn't
// exist under WSL, so only keep the ones that really open in the direction we need
bool canOpen(const char* name, snd_pcm_stream_t stream) {
    // Failed opens print errors (ALSA, and JACK directly), so mute stderr while probing
    fflush(stderr);
    int savedStderr = dup(STDERR_FILENO);
    int devNull = open("/dev/null", O_WRONLY);
    dup2(devNull, STDERR_FILENO);
    close(devNull);

    snd_pcm_t* pcm = nullptr;
    bool ok = snd_pcm_open(&pcm, name, stream, SND_PCM_NONBLOCK) == 0;
    if (ok) snd_pcm_close(pcm);

    fflush(stderr);
    dup2(savedStderr, STDERR_FILENO);
    close(savedStderr);
    return ok;
}
#endif

} // namespace

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        std::swap(pid, other.pid);
        std::swap(pipeFd, other.pipeFd);
        std::swap(stdinFd, other.stdinFd);
        std::swap(exitCode, other.exitCode);
        std::swap(lastError, other.lastError);
    }
    return *this;
}

void Process::closePipe() {
    if (pipeFd != -1) {
        close(pipeFd);
        pipeFd = -1;
    }
}

bool Process::start(const std::vector<std::string>& argv, bool captureStdout, bool pipeStdin) {
    if (argv.empty()) {
        lastError = "start: no program given";
        return false;
    }
    std::vector<std::string> argStrings = argv;
    std::vector<char*> args;
    for (std::string& s : argStrings) args.push_back(s.data());
    args.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int inPipe[2]  = {-1, -1};
    auto closePipes = [&] {
        for (int fd : {outPipe[0], outPipe[1], inPipe[0], inPipe[1]})
            if (fd != -1) close(fd);
    };
    if (captureStdout && pipe(outPipe) == -1) {
        lastError = errnoMessage("pipe", errno);
        return false;
    }
    if (pipeStdin) {
        if (pipe(inPipe) == -1) {
            lastError = errnoMessage("pipe", errno);
            closePipes();
            return false;
        }
        // Later children mustn't inherit our end, or this child never sees end of input
        fcntl(inPipe[1], F_SETFD, FD_CLOEXEC);
        // Writing after the child has exited would otherwise kill us with SIGPIPE;
        // write() then just fails with EPIPE
        signal(SIGPIPE, SIG_IGN);
    }
    // If execv fails the child writes errno here. On success the pipe closes on exec, so
    // the parent reads nothing and knows the program started
    int errPipe[2];
    if (pipe(errPipe) == -1) {
        lastError = errnoMessage("pipe", errno);
        closePipes();
        return false;
    }
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    pid_t child = fork();
    if (child == -1) {
        lastError = errnoMessage("fork", errno);
        closePipes();
        close(errPipe[0]);
        close(errPipe[1]);
        return false;
    }

    if (child == 0) {
        close(errPipe[0]);
        if (captureStdout) {
            close(outPipe[0]);
            dup2(outPipe[1], STDOUT_FILENO);
            close(outPipe[1]);
        }
        if (pipeStdin) {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[0]);
        }
        execv(args[0], args.data());
        int err = errno;                     // Only gets here if execv failed
        ssize_t ignored = ::write(errPipe[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }

    close(errPipe[1]);
    if (captureStdout) close(outPipe[1]);
    if (pipeStdin)     close(inPipe[0]);

    int childErr = 0;
    ssize_t n;
    do { n = ::read(errPipe[0], &childErr, sizeof childErr); } while (n == -1 && errno == EINTR);
    close(errPipe[0]);
    if (n == sizeof childErr) {
        lastError = errnoMessage(argv[0], childErr);
        if (captureStdout) close(outPipe[0]);
        if (pipeStdin)     close(inPipe[1]);
        waitpid(child, nullptr, 0);
        return false;
    }

    pid      = child;
    pipeFd   = captureStdout ? outPipe[0] : -1;
    stdinFd  = pipeStdin ? inPipe[1] : -1;
    exitCode = -1;
    return true;
}

bool Process::write(std::string_view data) {
    if (stdinFd == -1) return false;
    while (!data.empty()) {
        ssize_t n = ::write(stdinFd, data.data(), data.size());
        if (n == -1) {
            if (errno == EINTR) continue;
            lastError = errnoMessage("write", errno);
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void Process::closeStdin() {
    if (stdinFd != -1) {
        close(stdinFd);
        stdinFd = -1;
    }
}

long Process::read(char* buf, size_t size) {
    if (pipeFd == -1) return 0;
    ssize_t n;
    do { n = ::read(pipeFd, buf, size); } while (n == -1 && errno == EINTR);
    if (n == -1) lastError = errnoMessage("read", errno);
    return static_cast<long>(n);
}

int Process::wait() {
    if (pid == -1) return exitCode;
    int status = 0;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r == -1 && errno == EINTR);
    if (r == -1) {
        lastError = errnoMessage("waitpid", errno);
        exitCode = -1;
    } else if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = 128 + WTERMSIG(status);   // Same convention as the shell
    }
    pid = -1;
    return exitCode;
}

bool Process::running() const {
    // WNOWAIT only peeks, so the exit status is still there for wait() to collect
    if (pid == -1) return false;
    siginfo_t info{};
    if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == -1)
        return false;
    return info.si_pid == 0;   // Nothing has exited yet
}

void Process::kill() {
    if (pid != -1) ::kill(pid, SIGTERM);
}

#ifdef __APPLE__
namespace {

void addSource(pa_context*, const pa_source_info* info, int eol, void* userdata) {
    // Monitors replay what an output is playing; they aren't microphones or line-ins
    if (eol || info->monitor_of_sink != PA_INVALID_INDEX) return;
    static_cast<std::vector<AudioDevice>*>(userdata)->push_back(
        {info->name, info->description ? info->description : info->name});
}

void addSink(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    if (eol) return;
    static_cast<std::vector<AudioDevice>*>(userdata)->push_back(
        {info->name, info->description ? info->description : info->name});
}

} // namespace

// Homebrew's minimodem only has PulseAudio, so list what the PulseAudio server offers
// (it picks up the Mac's CoreAudio devices). The names are what PULSE_SOURCE/PULSE_SINK take
std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    pa_mainloop* loop = pa_mainloop_new();
    pa_context* context = pa_context_new(pa_mainloop_get_api(loop), "RTTY-Connector");

    bool ready = context && pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0;
    while (ready && pa_context_get_state(context) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context)) ||
            pa_mainloop_iterate(loop, 1, nullptr) < 0) ready = false;
    }

    if (ready) {
        pa_operation* op = direction == AudioDirection::Capture
                               ? pa_context_get_source_info_list(context, addSource, &devices)
                               : pa_context_get_sink_info_list(context, addSink, &devices);
        while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            if (pa_mainloop_iterate(loop, 1, nullptr) < 0) break;
        }
        if (op) pa_operation_unref(op);
    } else {
        std::cerr << "Error: Cannot reach PulseAudio. Start it with: brew services start pulseaudio"
                  << std::endl;
    }

    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    pa_mainloop_free(loop);
    return devices;
}
#else
std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    snd_pcm_stream_t stream = direction == AudioDirection::Capture
                                  ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
    void** hints;

    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        std::cerr << "Error: Failed to fetch ALSA device hints." << std::endl;
        return devices;
    }

    for (void** n = hints; *n != nullptr; ++n) {
        // The name used to open the device (e.g. hw:0,0) and the user-friendly description
        char* name = snd_device_name_get_hint(*n, "NAME");
        char* desc = snd_device_name_get_hint(*n, "DESC");
        // "Input", "Output", or null when the device does both
        char* ioid = snd_device_name_get_hint(*n, "IOID");

        const char* wrongWay = stream == SND_PCM_STREAM_CAPTURE ? "Output" : "Input";
        bool rightDirection = (ioid == nullptr || strcmp(ioid, wrongWay) != 0);
        if (name != nullptr && rightDirection && strcmp(name, "null") != 0 && canOpen(name, stream)) {
            std::string description = desc ? formatDescription(desc) : "";
            devices.push_back({name, description.empty() ? name : description});
        }

        if (name) free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }

    snd_device_name_free_hint(hints);
    return devices;
}
#endif

bool fileExists(const std::string& path)   { return access(path.c_str(), F_OK) == 0; }
bool fileReadable(const std::string& path) { return access(path.c_str(), R_OK) == 0; }

bool isTerminal(FILE* stream) {
    return isatty(fileno(stream)) != 0;
}

void setRawMode(bool enable) {
    static termios oldt;
    static bool active = false;
    if (!isatty(STDIN_FILENO)) return;
    if (enable && !active) {
        tcgetattr(STDIN_FILENO, &oldt);
        termios newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        active = true;
    } else if (!enable && active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        active = false;
    }
}

void setInterruptKey(bool enable) {
    termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &t) == -1) return;
    if (enable) t.c_lflag |= ISIG;   // ISIG is what turns Ctrl-C into SIGINT
    else        t.c_lflag &= ~ISIG;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

int readInputByte() {
    unsigned char c;
    ssize_t n;
    do { n = ::read(STDIN_FILENO, &c, 1); } while (n == -1 && errno == EINTR);
    return n == 1 ? c : -1;
}

bool waitForInput(int timeoutMs) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    int n;
    do { n = poll(&fd, 1, timeoutMs); } while (n == -1 && errno == EINTR);
    return n > 0;
}

std::vector<std::string> listSerialPorts() {
    std::vector<std::string> ports;
#ifdef __APPLE__
    // USB serial adapters (FTDI, CP210x, CH340) and radios with built-in USB. The cu.*
    // ("call-up") devices open straight away; tty.* ones wait for a carrier-detect signal
    const char* patterns[] = {"/dev/cu.usbserial*", "/dev/cu.usbmodem*",
                              "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*"};
#else
    // USB serial adapters and radios with built-in USB. ttyS* are left out: Linux creates
    // them whether or not anything is there
    const char* patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*"};
#endif
    for (const char* pattern : patterns) {
        glob_t matches;
        if (glob(pattern, 0, nullptr, &matches) == 0) {
            for (size_t i = 0; i < matches.gl_pathc; i++) ports.emplace_back(matches.gl_pathv[i]);
        }
        globfree(&matches);
    }
    return ports;
}

void enableAnsiEscapes() {}   // Linux and macOS terminals handle them already

namespace {

// PATH first, then where `make install` (/usr/local) and Homebrew on Apple Silicon
// (/opt/homebrew) put it
std::string findMinimodem() {
    std::vector<std::string> dirs;
    if (const char* path = getenv("PATH")) {
        std::string list = path;
        size_t start = 0, end;
        while ((end = list.find(':', start)) != std::string::npos) {
            dirs.push_back(list.substr(start, end - start));
            start = end + 1;
        }
        dirs.push_back(list.substr(start));
    }
    dirs.insert(dirs.end(), {"/usr/local/bin", "/opt/homebrew/bin"});
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/minimodem";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "/usr/local/bin/minimodem";   // start() then reports it can't be found
}

void swallowInterrupt(int) {}

} // namespace

const char* minimodemPath() {
    static const std::string path = findMinimodem();
    return path.c_str();
}

// A handler function rather than SIG_IGN: exec resets handlers to the default, so the
// programs we start still stop on Ctrl-C, but an ignored signal would stay ignored in them
void catchInterrupt(bool enable) {
    struct sigaction action{};
    action.sa_handler = enable ? swallowInterrupt : SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
}

#endif

} // namespace os
