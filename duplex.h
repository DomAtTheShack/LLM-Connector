// duplex.h - Duplex mode: transmit and receive in turns until the user quits
#ifndef HAMPROOF_DUPLEX_H
#define HAMPROOF_DUPLEX_H

#include <string>
#include <vector>

class Cat;

// Alternates Tx and Rx turns, starting with Tx:
//   Tx: each line typed goes on air at Enter; Tab ends the turn once it has all been sent
//   Rx: decodes until the other station's carrier drops, or Tab to cut it short
// Esc (or Ctrl-C) quits from either. txArgs/rxArgs are the full minimodem command lines;
// rxArgs must include --rx-one. With radio, PTT is keyed over CAT for each Tx turn;
// without, the radio is expected to key itself on VOX.
// Returns the exit status for the program: 0 if the user quit, 1 if something failed
int runDuplex(const std::vector<std::string>& txArgs, const std::vector<std::string>& rxArgs,
              Cat* radio);

#endif //HAMPROOF_DUPLEX_H
