//
// Created by dooli on 2026/10/06.
//

#ifndef HAMPROOF_OPTIONS_H
#define HAMPROOF_OPTIONS_H

// A menu option: the value used for decoding plus the text shown in the menu
struct Option {
    float value;
    const char* label;
};

const Option modes[] = {
    {0,  "Receiving/Rx RTTY"},
    {1,  "Transmitting/Tx RTTY"},
    {2,  "Receiving/Rx Bell202 1200 bps"},
    {3,  "Transmitting/Tx Bell202 1200 bps"},
    {4,  "Duplex RTTY"},
    {5,  "Duplex Bell202 1200 bps"},
};

const Option catBaudRates[] = {
    {4800.0f,   "4800"},
    {9600.0f,   "9600"},
    {19200.0f,  "19200"},
    {38400.0f,  "38400"},
    {115200.0f, "115200"},
};
const int defaultCatBaud = 2;   // 19200

const Option baudRates[] = {
    {45.45f, "45.45  (amateur standard, 60 WPM)"},
    {50.0f,  "50     (European commercial / weather)"},
    {75.0f,  "75     (100 WPM)"},
    {100.0f, "100"},
    {110.0f, "110"},
    {300.0f, "300"},
};
const Option centreFreqs[] = {
    {1000.0f, "1000 Hz"},
    {1360.0f, "1360 Hz  (classic low tones 1275/1445)"},
    {1500.0f, "1500 Hz"},
    {2210.0f, "2210 Hz  (classic high tones 2125/2295)"},
};
const Option shifts[] = {
    {170.0f, "170 Hz  (amateur standard)"},
    {85.0f,  "85 Hz   (narrow)"},
    {200.0f, "200 Hz"},
    {425.0f, "425 Hz  (commercial / maritime)"},
    {450.0f, "450 Hz  (commercial / weather)"},
    {850.0f, "850 Hz  (old military / press)"},
};
// minimodem's -c confidence threshold: how many times stronger one tone must be than the
// other, averaged over a character, before it counts as a signal. Noise sits around 1-2.
const Option squelchLevels[] = {
    {1.5f, "Off     (1.5, minimodem default - prints on noise)"},
    {2.5f, "Low     (2.5)"},
    {3.5f, "Medium  (3.5)"},
    {5.0f, "High    (5.0, strong signals only)"},
    {8.0f, "Max     (8.0, very clean signals only)"},
};
const int defaultSquelch = 2;   // Medium

#endif //HAMPROOF_OPTIONS_H