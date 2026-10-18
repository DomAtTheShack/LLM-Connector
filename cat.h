//
// Created by dooli on 2026/10/07.
//

#ifndef HAMPROOF_CAT_H
#define HAMPROOF_CAT_H

#include <string>

#include <hamlib/rig.h>

// CAT control of a radio through Hamlib. Every call talks to the radio and throws
// std::runtime_error with Hamlib's message if the radio rejects it or doesn't answer.
class Cat {
public:
    // model: Hamlib model number (1 = dummy rig, 2 = rigctld)
    // port:  serial device (COM5, /dev/ttyACM0, /dev/pts/N) or host:port for model 2
    Cat(rig_model_t model, const std::string& port, int baud = 19200);
    ~Cat();

    Cat(const Cat&) = delete;
    Cat& operator=(const Cat&) = delete;

    double frequency();                 // Hz
    void setFrequency(double hz);

    rmode_t mode();
    void setMode(rmode_t mode, pbwidth_t width = RIG_PASSBAND_NORMAL);

    bool transmitting();
    void setPtt(bool on);

    int sMeter();                       // dB relative to S9 (S9 = 0, S0 = -54)

private:
    RIG* rig_;
};

// Keys the transmitter for as long as it exists, so the radio is unkeyed even if
// the code in between throws or returns early.
class PttGuard {
public:
    explicit PttGuard(Cat& cat) : cat_(cat) { cat_.setPtt(true); }
    ~PttGuard() {
        try { cat_.setPtt(false); } catch (...) {}
    }

    PttGuard(const PttGuard&) = delete;
    PttGuard& operator=(const PttGuard&) = delete;

private:
    Cat& cat_;
};


#endif //HAMPROOF_CAT_H
