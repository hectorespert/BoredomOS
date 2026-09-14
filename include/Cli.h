
#ifndef BOREDOMOS_CLI_H
#define BOREDOMOS_CLI_H

// The console (and, once a valid frame arrives, MAVLink -- see src/cli.cpp's
// mode detection) is fixed to the USB CDC port. CLI_SERIAL was overridable
// only to dodge add-console-cli's now-deleted bench build, where the radio
// link took over Serial; there is no longer a build where that collision can
// happen, so this is no longer a build-time choice.

#define CLI_SERIAL Serial

#endif //BOREDOMOS_CLI_H
