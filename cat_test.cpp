// Exercises Cat against a radio, the X6100 simulator, or the dummy rig.
// Usage: cat_test <port> [model]     e.g. cat_test /dev/pts/9   or   cat_test x 1

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

#include "cat.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [hamlib model, default 3087]\n", argv[0]);
        return EXIT_FAILURE;
    }
    rig_model_t model = argc > 2 ? atoi(argv[2]) : RIG_MODEL_X6100;

    try {
        Cat radio(model, argv[1]);
        printf("frequency: %.0f Hz\n", radio.frequency());

        radio.setFrequency(14080000);
        radio.setMode(RIG_MODE_PKTUSB);
        printf("now: %.0f Hz, %s\n", radio.frequency(), rig_strrmode(radio.mode()));

        {
            PttGuard keyed(radio);
            printf("PTT on:  transmitting=%d\n", radio.transmitting());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        printf("PTT off: transmitting=%d\n", radio.transmitting());
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
