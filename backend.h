// backend.h - OS abstraction layer. Everything that differs between Windows and Linux
// (running programs, pipes, audio devices, file checks, terminal modes) goes through here,
// so the rest of HamProof calls the same functions on either OS.
// The implementations for both live in backend.cpp.
#ifndef HAMPROOF_BACKEND_H
#define HAMPROOF_BACKEND_H

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace os {

// ---- Processes ---------------------------------------------------------------

// A child program, optionally with its stdout piped back to us and its stdin fed by us
class Process {
public:
    Process() = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // argv[0] is the program to run. With captureStdout the child's stdout goes to a pipe
    // that read() drains; otherwise it shares our stdout. With pipeStdin the child reads
    // what write() sends, until closeStdin(); otherwise it shares our stdin
    bool start(const std::vector<std::string>& argv, bool captureStdout = true,
               bool pipeStdin = false);
    long read(char* buf, size_t size);   // >0 bytes read, 0 = end of output, -1 = error
    bool write(std::string_view data);   // false if the child has gone or stdin isn't piped
    void closeStdin();                   // The child sees end of input
    int  wait();                         // Blocks until it exits; returns its exit code or -1
    bool running() const;
    void kill();
    const std::string& error() const { return lastError; }  // Why the last call failed

private:
    void closePipe();

#ifdef _WIN32
    void* processHandle = nullptr;       // HANDLE
    void* pipeRead      = nullptr;       // HANDLE
    void* pipeWrite     = nullptr;       // HANDLE
#else
    int pid     = -1;
    int pipeFd  = -1;
    int stdinFd = -1;
#endif
    int exitCode = -1;
    std::string lastError;
};

// Runs a program and hands each chunk of its stdout to onOutput as it arrives.
// Returns the exit code, or -1 if it couldn't be started (the reason goes to stderr)
int runAndPipe(const std::vector<std::string>& argv,
               const std::function<void(std::string_view)>& onOutput);

// ---- Audio devices -----------------------------------------------------------

enum class AudioDirection { Capture, Playback };

// A sound device: the name used to open it plus the text shown in the menu
struct AudioDevice {
    std::string name;   // Linux: ALSA name, e.g. "plughw:CARD=PCH,DEV=0". Windows: WinMM index
    std::string desc;   // e.g. "HDA Intel PCH, ALC892 Analog - Front speakers"
};

// Only devices that really open in the requested direction
std::vector<AudioDevice> listAudioDevices(AudioDirection direction);

// ---- Serial ports ------------------------------------------------------------

// Ports a radio's CAT could be on. Windows: "COM5". Linux: "/dev/ttyUSB0", "/dev/ttyACM0"
std::vector<std::string> listSerialPorts();

// ---- Files -------------------------------------------------------------------

bool fileExists(const std::string& path);
bool fileReadable(const std::string& path);

// ---- Terminal ----------------------------------------------------------------

bool isTerminal(FILE* stream);
// Raw mode: keys arrive one at a time, without waiting for Enter and without echo.
// Arrow keys arrive as "ESC [ A" style sequences on both OSes
void setRawMode(bool enable);
// With enable false, Ctrl-C arrives as byte 3 instead of killing us. setRawMode(false)
// turns it back on
void setInterruptKey(bool enable);
// One byte from the keyboard, unbuffered (stdio's getchar may read ahead), or -1 when
// stdin has closed
int readInputByte();
// True once readInputByte() has something to return, false after timeoutMs without
bool waitForInput(int timeoutMs);
// Lets the console understand ANSI escape codes (always true on Linux terminals)
void enableAnsiEscapes();

// ---- Misc --------------------------------------------------------------------

// Where minimodem is installed
const char* minimodemPath();

} // namespace os

#endif //HAMPROOF_BACKEND_H
