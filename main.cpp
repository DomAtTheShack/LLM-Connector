#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ansi.hpp"
#include "backend.h"
#include "cat.h"
#include "duplex.h"
#include "options.h"

using namespace std;

// Exits cleanly if the user pressed 'q' in a menu
int requireSelection(int selection) {
    if (selection == -1) {
        ansi::clearScreen();
        exit(EXIT_SUCCESS);
    }
    return selection;
}

// minimodem judges each bit over 1/baud seconds, which smears every tone across roughly
// +/- baud Hz. Mark and space need to be at least that far apart to be told apart, and
// twice that leaves margin for noise, drift and filtering.
enum class ShiftFit { TooNarrow, Marginal, Comfortable };

ShiftFit shiftFit(float shift, float baud) {
    float ratio = shift / baud;
    if (ratio < 1.0f) return ShiftFit::TooNarrow;
    if (ratio < 2.0f) return ShiftFit::Marginal;
    return ShiftFit::Comfortable;
}

// Asks which serial port the radio's CAT is on and opens it. nullptr means no CAT: the
// radio keys itself on VOX
unique_ptr<Cat> chooseRadio() {
    vector<string> ports = os::listSerialPorts();
    vector<string> labels = {"VOX"};
    labels.insert(labels.end(), ports.begin(), ports.end());
    labels.emplace_back("Type a port...");
    int choice = requireSelection(ansi::selectWith(
        "Select the radio's CAT port for PTT", static_cast<int>(labels.size()),
        [&](int i) { return labels[i]; }));
    if (choice == 0) return nullptr;

    string port;
    if (choice == static_cast<int>(labels.size()) - 1) {
        ansi::clearScreen();
        ansi::print(ansi::Color::BrightRed, "CAT port (e.g. COM5, /dev/ttyACM0, /dev/pts/3), "
                    "empty for VOX: ");
        ansi::flush();
        // selectWith leaves the terminal in normal mode, so this reads a whole echoed line
        char buf[256];
        if (!fgets(buf, sizeof buf, stdin)) exit(EXIT_SUCCESS);
        port = buf;
        while (!port.empty() && (port.back() == '\n' || port.back() == '\r' || port.back() == ' '))
            port.pop_back();
        if (port.empty()) return nullptr;
    } else {
        port = ports[choice - 1];
    }

    int baud = static_cast<int>(catBaudRates[requireSelection(ansi::selectWith(
        "Select the radio's CAT baud rate", std::size(catBaudRates),
        [](int i) { return catBaudRates[i].label; }, defaultCatBaud))].value);
    try {
        return make_unique<Cat>(RIG_MODEL_X6100, port, baud);
    } catch (const exception& e) {
        ansi::clearScreen();
        fprintf(stderr, "Cannot use the radio on %s: %s\n", port.c_str(), e.what());
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char** argv) {
    auto optionLabel = [](const Option* options) {
        return [options](int i) { return options[i].label; };
    };

    // Usage: hamproof [-q] [file.wav]   (no file means record from an ALSA device)
    bool quiet = false;
    string inputFile;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = true;
        } else if (inputFile.empty()) {
            inputFile = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [-q] [file.wav]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    ansi::setRawMode(true);
    ansi::clearScreen();
    ansi::print(ansi::Color::BrightGreen, "Welcome to the RTTY Ham Proof\nPress any key to continue...\n");
    ansi::flush();
    std::getchar();

    int selection = modes[requireSelection(ansi::selectWith(
        "Select a transmission type", std::size(modes), optionLabel(modes)))].value;
    bool duplex = selection >= 4;
    bool rtty = selection == 0 || selection == 1 || selection == 4;
    bool transmitting = !duplex && selection % 2 == 1;   // Duplex both transmits and receives

    // Receiving reads the file; transmitting makes minimodem write audio into it, so refuse
    // to start rather than overwrite a recording that was probably meant to be decoded
    if (!inputFile.empty()) {
        if (duplex) {
            ansi::clearScreen();
            fprintf(stderr, "Duplex mode needs a sound card, not a file\n");
            exit(EXIT_FAILURE);
        }
        if (!transmitting && !os::fileReadable(inputFile)) {
            ansi::clearScreen();
            fprintf(stderr, "%s: cannot be read\n", inputFile.c_str());
            exit(EXIT_FAILURE);
        }
        if (transmitting && os::fileExists(inputFile)) {
            ansi::clearScreen();
            fprintf(stderr, "%s already exists; transmitting would overwrite it\n", inputFile.c_str());
            exit(EXIT_FAILURE);
        }
    }

    char baudStr[16], markStr[16], spaceStr[16], squelchStr[16] = "";
    float mark, space, baud, centre, shift, squelch = 0;

    if (rtty) {
        baud = baudRates[requireSelection(ansi::selectWith(
            "Select a baud rate", std::size(baudRates), optionLabel(baudRates)))].value;
        centre = centreFreqs[requireSelection(ansi::selectWith(
            "Select a centre frequency", std::size(centreFreqs), optionLabel(centreFreqs)))].value;

        // Rate every shift against the chosen baud and start the cursor on the narrowest
        // comfortable one, so faster speeds steer towards wider shifts
        int recommendedShift = -1;
        vector<string> shiftLabels;

        for (int i = 0; i < static_cast<int>(std::size(shifts)); i++) {
            string label = shifts[i].label;
            switch (shiftFit(shifts[i].value, baud)) {
                case ShiftFit::TooNarrow:   label += "  [too narrow for this baud]"; break;
                case ShiftFit::Marginal:    label += "  [marginal, expect errors]";  break;
                case ShiftFit::Comfortable:
                    if (recommendedShift == -1 || shifts[i].value < shifts[recommendedShift].value)
                        recommendedShift = i;

                    break;
            }
            shiftLabels.push_back(label);
        }
        if (recommendedShift != -1) shiftLabels[recommendedShift] += "  <- recommended";
        shift = shifts[requireSelection(ansi::selectWith(
            "Select a shift", static_cast<int>(shiftLabels.size()),
            [&](int i) { return shiftLabels[i]; }, recommendedShift))].value;
        // minimodem takes the two tones rather than centre + shift
        mark  = centre + shift / 2.0f;
        space = centre - shift / 2.0f;


        snprintf(baudStr,    sizeof baudStr,    "%g", baud);
        snprintf(markStr,    sizeof markStr,    "%g", mark);
        snprintf(spaceStr,   sizeof spaceStr,   "%g", space);
    }
    // Squelch only affects decoding, so there's nothing to ask when only transmitting
    if (!transmitting) {
        squelch = squelchLevels[requireSelection(ansi::selectWith(
            "Select a squelch level", std::size(squelchLevels), optionLabel(squelchLevels),
            defaultSquelch))].value;
        snprintf(squelchStr, sizeof squelchStr, "%g", squelch);
    }

    // minimodem's arguments for the sound card picked for one direction
    auto chooseAudioDevice = [&](os::AudioDirection direction, const char* title) {
        bool playback = direction == os::AudioDirection::Playback;
        vector<os::AudioDevice> devices = os::listAudioDevices(direction);
        if (devices.empty()) {
            ansi::setRawMode(false);
            ansi::clearScreen();
            fprintf(stderr, "No %s devices found\n", playback ? "playback" : "capture");
            exit(EXIT_FAILURE);
        }
        const os::AudioDevice& audioDevice = devices[requireSelection(ansi::selectWith(
            title, static_cast<int>(devices.size()),
            [&](int i) { return devices[i].desc + "  [" + devices[i].name + "]"; }))];
#ifdef _WIN32
        // The Windows (Cygwin) build of minimodem only has PulseAudio, which always uses the
        // default device, so there's no argument to pass. Make the device picked here the
        // default in Windows sound settings
        (void)audioDevice;
        return vector<string>{};
#else
        // minimodem turns a name without ':' into "plughw:<name>" (a hardware card), so
        // wrap names like "default" or "pulse" in the plug plugin to keep them as-is
        string alsaName = audioDevice.name;
        if (alsaName.find(':') == string::npos) alsaName = "plug:" + alsaName;
        return vector<string>{"--alsa=" + alsaName};
#endif
    };

    // minimodem uses the file if one was given (read on Rx, written on Tx), otherwise the
    // chosen sound card
    vector<string> rxAudioArgs, txAudioArgs;
    if (!inputFile.empty()) {
        (transmitting ? txAudioArgs : rxAudioArgs) = {"-f", inputFile};
    } else if (duplex) {
        rxAudioArgs = chooseAudioDevice(os::AudioDirection::Capture,
                                        "Select the receive (capture) audio device");
        txAudioArgs = chooseAudioDevice(os::AudioDirection::Playback,
                                        "Select the transmit (playback) audio device");
    } else if (transmitting) {
        txAudioArgs = chooseAudioDevice(os::AudioDirection::Playback, "Select an audio device");
    } else {
        rxAudioArgs = chooseAudioDevice(os::AudioDirection::Capture, "Select an audio device");
    }

    unique_ptr<Cat> duplexRadio;
    if (duplex) duplexRadio = chooseRadio();

    // The minimodem command line for one direction
    auto minimodemArgs = [&](bool tx, const vector<string>& audioArgs) {
        vector<string> args;
        // Numeric baud + Baudot 5-bit + 1.5 stop bits is what "rtty" mode means, at any rate
        if (rtty) {
            args = {
                os::minimodemPath(),
                "-i",                               // Mark is the lower tone (LSB reception)
                "-5", "--stopbits", "1.5",
                "-M", markStr, "-S", spaceStr,
                // minimodem rounds the tones to a grid of this spacing. Left alone it uses 10 Hz
                // below 100 baud but 50 Hz from 100 up, which moves 2125/2295 to 2150/2300
                "-b", "10",
            };
        } else { // Operating in Bell202 Tx/Rx Mode
            args = {os::minimodemPath()};
        }
        args.insert(args.end(), audioArgs.begin(), audioArgs.end());
        if (tx) {
            // Without this minimodem goes silent between lines, so VOX drops out and the
            // receiver has to resync, usually losing the first character. Idle on mark instead
            args.emplace_back("--tx-carrier");
        } else {
            args.insert(args.end(), {"-c", squelchStr});  // Minimum confidence to print a character
            // A duplex receive turn ends when the other station's carrier drops
            if (duplex) args.emplace_back("--rx-one");
        }
        args.emplace_back(tx ? "--tx" : "--rx");
        if (quiet) {
            args.emplace_back("-q");
        }
        args.emplace_back(rtty ? baudStr : "1200");  // minimodem expects the baud rate last
        return args;
    };

    ansi::clearScreen();
    if (rtty) {
        ansi::print(ansi::Color::BrightGreen, "Baud %s | Centre %g Hz | Shift %g Hz  (tones %s / %s Hz)",
                    baudStr, centre, shift, markStr, spaceStr);
        if (!transmitting) ansi::print(ansi::Color::BrightGreen, " | Squelch %s", squelchStr);
        ansi::print(ansi::Color::BrightGreen, "\n");
        if (shiftFit(shift, baud) != ShiftFit::Comfortable) {
            ansi::print(ansi::Color::BrightRed, "Warning: a %g Hz shift is narrow for %s baud; "
                        "use at least %g Hz for clean copy\n", shift, baudStr, 2.0f * baud);
        }
    }else {
        ansi::print(ansi::Color::BrightGreen, "Baud 1200 | Bell202 1200 bps\n");
    }

    if (duplex) {
        int status = runDuplex(minimodemArgs(true, txAudioArgs), minimodemArgs(false, rxAudioArgs),
                               duplexRadio.get());
        duplexRadio.reset();   // exit() skips destructors, and the radio's port should be closed
        exit(status);
    }

    if (transmitting) {
        ansi::print(ansi::Color::BrightGreen, "Type a line and press Enter to send it; Ctrl-D ends the transmission\n");
    }

    std::printf("\n");
    ansi::flush();

    // Show minimodem's output as it arrives
    int status = os::runAndPipe(minimodemArgs(transmitting, transmitting ? txAudioArgs : rxAudioArgs),
                                [](string_view chunk) {
        fwrite(chunk.data(), 1, chunk.size(), stdout);
        fflush(stdout);
    });

    exit(status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
