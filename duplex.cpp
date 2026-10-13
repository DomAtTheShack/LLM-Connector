// duplex.cpp - Duplex mode. Each turn runs its own minimodem (--tx, or --rx --rx-one) while
// one thread reads the keyboard for the whole session. Keys and minimodem's output both
// arrive through one queue, so a turn just waits on it and reacts to whatever comes next.
#include "duplex.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ansi.hpp"
#include "backend.h"
#include "cat.h"

namespace {

const ansi::Color statusColor   = ansi::Color::BrightYellow;
const ansi::Color sentColor     = ansi::Color::BrightCyan;
const ansi::Color receivedColor = ansi::Color::BrightGreen;
const ansi::Color errorColor    = ansi::Color::BrightRed;

enum class Key { Char, Enter, Backspace, Tab, Esc };

// Something for a turn to react to
struct Event {
    enum class Type { Key, Output, OutputEnded } type;
    Key key = Key::Char;
    char ch = 0;          // For Key::Char
    std::string text;     // For Output
};

class EventQueue {
public:
    void push(Event event) {
        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
        ready.notify_one();
    }

    Event pop() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return !events.empty(); });
        Event event = std::move(events.front());
        events.pop_front();
        return event;
    }

private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
};

void pushKey(EventQueue& events, Key key, char ch = 0) {
    events.push({Event::Type::Key, key, ch, {}});
}

// Turns keyboard bytes into Keys for the rest of the program. Arrow and function keys
// arrive as escape sequences and are skipped; an ESC with nothing straight after it is
// the Esc key itself
void readKeys(EventQueue& events) {
    for (;;) {
        int c = os::readInputByte();
        switch (c) {
            case -1:             // stdin closed, so nothing more can be typed: quit
                pushKey(events, Key::Esc);
                return;
            case 3:              // Ctrl-C, which setInterruptKey(false) turns into a byte
                pushKey(events, Key::Esc);
                break;
            case '\t':
                pushKey(events, Key::Tab);
                break;
            case '\r':
            case '\n':
                pushKey(events, Key::Enter);
                break;
            case 8:
            case 127:
                pushKey(events, Key::Backspace);
                break;
            case 27:
                if (!os::waitForInput(50)) {
                    pushKey(events, Key::Esc);
                    break;
                }
                c = os::readInputByte();
                if (c == '[' || c == 'O') {
                    // Parameter bytes run up to a final byte between '@' and '~'
                    do c = os::readInputByte(); while (c != -1 && (c < 0x40 || c > 0x7e));
                }
                break;
            default:
                // RTTY and Bell202 here carry plain ASCII
                if (c >= 32 && c < 127) pushKey(events, Key::Char, static_cast<char>(c));
                break;
        }
    }
}

void printColored(ansi::Color color, const std::string& text) {
    ansi::setColor(color);
    std::fwrite(text.data(), 1, text.size(), stdout);
    ansi::reset();
    ansi::flush();
}

// One minimodem run. A reader thread forwards its output to the queue, then posts
// OutputEnded once minimodem closes its output, which it does by exiting
class Modem {
public:
    explicit Modem(EventQueue& events) : events(events) {}

    ~Modem() {
        // Only on an early exit from a turn; normally wait() has already joined
        if (reader.joinable()) {
            process.kill();
            reader.join();
        }
    }

    Modem(const Modem&) = delete;
    Modem& operator=(const Modem&) = delete;

    bool start(const std::vector<std::string>& args, bool pipeStdin) {
        if (!process.start(args, true, pipeStdin)) {
            ansi::print(errorColor, "%s\n", process.error().c_str());
            return false;
        }
        reader = std::thread([this] {
            char buffer[256];
            long n;
            while ((n = process.read(buffer, sizeof buffer)) > 0)
                events.push({Event::Type::Output, Key::Char, 0, std::string(buffer, n)});
            events.push({Event::Type::OutputEnded});
        });
        return true;
    }

    // Call once OutputEnded has arrived. Returns minimodem's exit code
    int wait() {
        reader.join();
        return process.wait();
    }

    os::Process process;

private:
    EventQueue& events;
    std::thread reader;
};

enum class TurnEnd { Next, Quit, Failed };

TurnEnd transmitTurn(const std::vector<std::string>& args, Cat* radio, EventQueue& events) {
    ansi::print(statusColor, ansi::Bold, "\n== TX ==");
    ansi::print(statusColor, "  Enter sends a line | Tab: done, start receiving | Esc: quit\n");
    ansi::flush();

    // Declared before the modem, so on an early return minimodem is stopped before the
    // radio is unkeyed
    std::optional<PttGuard> keyed;
    if (radio) {
        try {
            keyed.emplace(*radio);
        } catch (const std::exception& e) {
            ansi::print(errorColor, "Cannot key the radio: %s\n", e.what());
            return TurnEnd::Failed;
        }
    }

    Modem modem(events);
    if (!modem.start(args, true)) return TurnEnd::Failed;

    std::string line;
    bool finishing = false;   // Tab pressed: minimodem is sending what's left, then exits
    bool quitting  = false;
    auto sendLine = [&] {
        modem.process.write(line + "\n");
        line.clear();
        std::fputs("\n", stdout);
    };

    for (;;) {
        Event event = events.pop();
        if (event.type == Event::Type::OutputEnded) break;
        if (event.type == Event::Type::Output) {
            // minimodem prints nothing while sending, so this is almost always a problem
            printColored(errorColor, event.text);
            continue;
        }
        if (finishing || quitting) {
            // Esc still cuts a finishing transmission short; everything else waits for Rx
            if (event.key == Key::Esc && !quitting) {
                modem.process.kill();
                quitting = true;
            }
            continue;
        }
        switch (event.key) {
            case Key::Char:
                line += event.ch;
                ansi::print(sentColor, "%c", event.ch);
                break;
            case Key::Backspace:
                if (!line.empty()) {
                    line.pop_back();
                    std::fputs("\b \b", stdout);
                }
                break;
            case Key::Enter:
                sendLine();
                break;
            case Key::Tab:
                if (!line.empty()) sendLine();
                // End of input: minimodem sends whatever it still has queued, then exits
                modem.process.closeStdin();
                finishing = true;
                ansi::print(statusColor, "(finishing transmission...)\n");
                break;
            case Key::Esc:
                modem.process.kill();
                quitting = true;
                break;
        }
        ansi::flush();
    }
    int status = modem.wait();

    // PttGuard would unkey too, but it has to swallow errors; this one must be seen
    if (radio) {
        try {
            radio->setPtt(false);
        } catch (const std::exception& e) {
            ansi::print(errorColor, "Cannot unkey the radio: %s - it may still be transmitting!\n",
                        e.what());
            return TurnEnd::Failed;
        }
    }
    if (quitting) return TurnEnd::Quit;
    if (!finishing) {
        ansi::print(errorColor, "minimodem stopped unexpectedly (exit %d)\n", status);
        return TurnEnd::Failed;
    }
    return TurnEnd::Next;
}

TurnEnd receiveTurn(const std::vector<std::string>& args, EventQueue& events) {
    ansi::print(statusColor, ansi::Bold, "\n== RX ==");
    ansi::print(statusColor, "  Back to Tx when the other station's carrier drops | "
                "Tab: Tx now | Esc: quit\n");
    ansi::flush();

    Modem modem(events);
    if (!modem.start(args, false)) return TurnEnd::Failed;

    bool stopping = false;    // Tab pressed: back to Tx without waiting for the carrier
    bool quitting = false;
    for (;;) {
        Event event = events.pop();
        if (event.type == Event::Type::OutputEnded) break;
        if (event.type == Event::Type::Output) {
            printColored(receivedColor, event.text);
            continue;
        }
        if (stopping || quitting) continue;
        // Anything typed now is dropped; only Tab and Esc mean something while receiving
        if (event.key == Key::Tab) {
            modem.process.kill();
            stopping = true;
        } else if (event.key == Key::Esc) {
            modem.process.kill();
            quitting = true;
        }
    }
    int status = modem.wait();

    if (quitting) return TurnEnd::Quit;
    if (stopping) return TurnEnd::Next;
    // --rx-one exits cleanly after the first carrier it heard has dropped
    if (status != 0) {
        ansi::print(errorColor, "\nminimodem stopped unexpectedly (exit %d)\n", status);
        return TurnEnd::Failed;
    }
    ansi::print(statusColor, "\n(carrier lost)\n");
    return TurnEnd::Next;
}

} // namespace

int runDuplex(const std::vector<std::string>& txArgs, const std::vector<std::string>& rxArgs,
              Cat* radio) {
    ansi::setRawMode(true);
    // Ctrl-C would kill us with the radio still keyed, so it comes in as a key instead
    os::setInterruptKey(false);
    ansi::print(statusColor, "PTT: %s\n", radio ? "CAT" : "VOX (the radio keys itself on the audio)");

    // The keyboard thread blocks in a read for the rest of the program, so the queue it
    // fills has to outlive this function; it is never freed on purpose
    auto* events = new EventQueue;
    std::thread(readKeys, std::ref(*events)).detach();

    TurnEnd end = TurnEnd::Next;
    for (bool transmitting = true; end == TurnEnd::Next; transmitting = !transmitting) {
        end = transmitting ? transmitTurn(txArgs, radio, *events) : receiveTurn(rxArgs, *events);
    }

    ansi::setRawMode(false);   // Also turns the interrupt key back on
    std::printf("\n");
    return end == TurnEnd::Quit ? 0 : 1;
}
