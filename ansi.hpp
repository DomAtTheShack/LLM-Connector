// ansi.hpp - Small header-only helper for colored/styled terminal output
// using ANSI escape codes. Escape codes are skipped automatically when stdout
// is not a terminal (e.g. "./HamProof > out.txt") or NO_COLOR is set.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <string>

#include "backend.h"

// printf format checking where the compiler supports it (GCC/Clang, not MSVC)
#if defined(__GNUC__) || defined(__clang__)
    #define ANSI_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
    #define ANSI_PRINTF_FORMAT(fmt, args)
#endif

namespace ansi {

enum class Color {
    Default = 39,
    Black = 30, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    Gray = 90, BrightRed, BrightGreen, BrightYellow, BrightBlue,
    BrightMagenta, BrightCyan, BrightWhite
};

// Styles can be combined: ansi::Bold | ansi::Underline
enum Style : unsigned {
    None      = 0,
    Bold      = 1 << 0,
    Dim       = 1 << 1,
    Italic    = 1 << 2,
    Underline = 1 << 3,
    Blink     = 1 << 4,
    Inverse   = 1 << 5,
};

inline Style operator|(Style a, Style b) {
    return static_cast<Style>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

// ---- Enable / disable --------------------------------------------------------

inline bool& enabledFlag() {
    static bool enabled = [] {
        bool on = os::isTerminal(stdout) && std::getenv("NO_COLOR") == nullptr;
        if (on) os::enableAnsiEscapes();   // Windows consoles need this before escapes work
        return on;
    }();
    return enabled;
}

inline bool enabled() { return enabledFlag(); }
inline void setEnabled(bool on) { enabledFlag() = on; }

// Writes a raw escape sequence only when escapes are enabled
inline void esc(const char* seq) {
    if (enabled()) std::fputs(seq, stdout);
}

// ---- Colors and styles -------------------------------------------------------

inline void reset() { esc("\033[0m"); }

inline void setColor(Color fg, Color bg = Color::Default) {
    if (!enabled()) return;
    std::printf("\033[%d;%dm", static_cast<int>(fg), static_cast<int>(bg) + 10);
}

// 24-bit color, supported by most modern terminals (Windows Terminal included)
inline void setRGB(int r, int g, int b) {
    if (!enabled()) return;
    std::printf("\033[38;2;%d;%d;%dm", r, g, b);
}

inline void setStyle(Style style) {
    if (style & Bold)      esc("\033[1m");
    if (style & Dim)       esc("\033[2m");
    if (style & Italic)    esc("\033[3m");
    if (style & Underline) esc("\033[4m");
    if (style & Blink)     esc("\033[5m");
    if (style & Inverse)   esc("\033[7m");
}

// ---- Printing (printf-style) -------------------------------------------------

inline void vprint(Color fg, Style style, const char* fmt, va_list args) {
    setColor(fg);
    setStyle(style);
    std::vprintf(fmt, args);
    reset();
}

ANSI_PRINTF_FORMAT(2, 3)
inline void print(Color fg, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fg, None, fmt, args);
    va_end(args);
}

ANSI_PRINTF_FORMAT(3, 4)
inline void print(Color fg, Style style, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fg, style, fmt, args);
    va_end(args);
}

// ---- Screen and cursor -------------------------------------------------------

inline void clearScreen() { esc("\033[2J\033[H"); }  // Clear and move cursor to top-left
inline void clearLine()   { esc("\033[2K\r"); }      // Clear current line, cursor to column 1
inline void clearToEnd()  { esc("\033[0J"); }        // Clear from cursor to end of screen

inline void moveTo(int row, int col) {               // 1-based coordinates
    if (!enabled()) return;
    std::printf("\033[%d;%dH", row, col);
}

inline void hideCursor() { esc("\033[?25l"); }
inline void showCursor() { esc("\033[?25h"); }

inline void flush() { std::fflush(stdout); }

// ---- Keyboard input ----------------------------------------------------------

// Raw mode: keys arrive one at a time, without waiting for Enter and without echo
inline void setRawMode(bool enable) { os::setRawMode(enable); }

enum class Key { Up, Down, Enter, Quit, Other };

// Reads one key press. Arrow keys arrive as "ESC [ A" (or "ESC O A")
inline Key readKey() {
    int c = std::getchar();
    switch (c) {
        case EOF:
        case 'q':
            return Key::Quit;
        case '\n':
        case '\r':
            return Key::Enter;
        case 'k': return Key::Up;
        case 'j': return Key::Down;
        case 27: {
            int next = std::getchar();
            if (next != '[' && next != 'O') return Key::Other;
            switch (std::getchar()) {
                case 'A': return Key::Up;
                case 'B': return Key::Down;
                default:  return Key::Other;
            }
        }
        default:
            return Key::Other;
    }
}

// ---- Selection menu ----------------------------------------------------------

struct MenuColors {
    Color title  = Color::BrightRed;
    Color item   = Color::BrightGreen;
    Color cursor = Color::BrightRed;
};

inline const char* toCStr(const char* s)        { return s; }
inline const char* toCStr(const std::string& s) { return s.c_str(); }

// Core menu. label(i) returns the text for option i, so options can be any type.
// Returns the chosen index, or -1 if the user pressed 'q'.
template <class LabelFn>
int selectWith(const char* title, int count, LabelFn label, int initial = 0,
               MenuColors colors = {}) {
    if (count <= 0) return -1;
    int selection = (initial >= 0 && initial < count) ? initial : 0;
    int result = -1;
    bool done = false;

    setRawMode(true);
    hideCursor();
    while (!done) {
        clearScreen();                      // Redraw the whole menu each keypress
        print(colors.title, "%s (Up/Down, Enter, q to quit):\n", title);
        for (int i = 0; i < count; i++) {
            if (i == selection) {
                print(colors.cursor, "-> ");
                print(colors.item, Bold | Inverse, "%s", toCStr(label(i)));
                std::printf("\n");
            } else {
                print(colors.item, "   %s\n", toCStr(label(i)));
            }
        }
        flush();

        switch (readKey()) {
            case Key::Up:                   // Wrap around at the top and bottom
                selection = (selection - 1 + count) % count;
                break;
            case Key::Down:
                selection = (selection + 1) % count;
                break;
            case Key::Enter:
                result = selection;
                done = true;
                break;
            case Key::Quit:
                done = true;
                break;
            case Key::Other:
                break;
        }
    }
    showCursor();
    setRawMode(false);
    return result;
}

// Any container of strings: C arrays, std::array, std::vector<const char*>, std::vector<std::string>
template <class Container>
int select(const char* title, const Container& options, int initial = 0, MenuColors colors = {}) {
    return selectWith(title, static_cast<int>(std::size(options)),
                      [&](int i) { return std::begin(options)[i]; }, initial, colors);
}

// Inline list: ansi::select("Mode", {"RTTY", "Bell 103", "Bell 202"})
inline int select(const char* title, std::initializer_list<const char*> options,
                  int initial = 0, MenuColors colors = {}) {
    return selectWith(title, static_cast<int>(options.size()),
                      [&](int i) { return options.begin()[i]; }, initial, colors);
}

} // namespace ansi
