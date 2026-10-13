//
// Created by dooli on 2026/10/07.
//

#include "cat.h"

#include <stdexcept>

namespace {

void check(int status, const char* what) {
    if (status != RIG_OK) {
        // rigerror() would append Hamlib's whole debug trace; rigerror2() is just the message
        throw std::runtime_error(std::string(what) + ": " + rigerror2(status));
    }
}

void setConf(RIG* rig, const char* name, const std::string& value) {
    check(rig_set_conf(rig, rig_token_lookup(rig, name), value.c_str()), name);
}

}

Cat::Cat(rig_model_t model, const std::string& port, int baud) {
    // Hamlib logs to stderr by default, which would scribble over the menus
    rig_set_debug(RIG_DEBUG_NONE);

    rig_ = rig_init(model);
    if (!rig_) {
        throw std::runtime_error("Hamlib has no rig model " + std::to_string(model));
    }

    try {
        // The dummy rig needs no port; given one, it tries to reach it over the network
        if (model != RIG_MODEL_DUMMY) {
            setConf(rig_, "rig_pathname", port);
        }
        if (model != RIG_MODEL_DUMMY && model != RIG_MODEL_NETRIGCTL) {
            setConf(rig_, "serial_speed", std::to_string(baud));
        }
        check(rig_open(rig_), ("open " + port).c_str());
    } catch (...) {
        rig_cleanup(rig_);
        throw;
    }
}

Cat::~Cat() {
    rig_close(rig_);
    rig_cleanup(rig_);
}

double Cat::frequency() {
    freq_t hz;
    check(rig_get_freq(rig_, RIG_VFO_CURR, &hz), "get frequency");
    return hz;
}

void Cat::setFrequency(double hz) {
    check(rig_set_freq(rig_, RIG_VFO_CURR, hz), "set frequency");
}

rmode_t Cat::mode() {
    rmode_t mode;
    pbwidth_t width;
    check(rig_get_mode(rig_, RIG_VFO_CURR, &mode, &width), "get mode");
    return mode;
}

void Cat::setMode(rmode_t mode, pbwidth_t width) {
    check(rig_set_mode(rig_, RIG_VFO_CURR, mode, width), "set mode");
}

bool Cat::transmitting() {
    ptt_t ptt;
    check(rig_get_ptt(rig_, RIG_VFO_CURR, &ptt), "get PTT");
    return ptt != RIG_PTT_OFF;
}

void Cat::setPtt(bool on) {
    check(rig_set_ptt(rig_, RIG_VFO_CURR, on ? RIG_PTT_ON : RIG_PTT_OFF), "set PTT");
}

int Cat::sMeter() {
    value_t value;
    check(rig_get_level(rig_, RIG_VFO_CURR, RIG_LEVEL_STRENGTH, &value), "get S-meter");
    return value.i;
}
