## ADDED Requirements

### Requirement: The USB port chooses its protocol from what arrives on it

The USB port SHALL start every boot in CLI mode. It SHALL leave CLI mode for MAVLink
mode when, and only when, a complete frame passing its checksum has been received. A
byte that merely looks like a frame header SHALL NOT be enough.

The change SHALL be one-way for the remainder of that boot: no inactivity timer, no
escape sequence and no command SHALL return the port to CLI mode. Only a reset does.

#### Scenario: A terminal types at the port

- **WHEN** a host opens the USB port and types text, including bytes that happen to
  match a frame header
- **THEN** the port stays in CLI mode
- **AND** the text is interpreted as commands

#### Scenario: A ground station connects

- **WHEN** a ground station opens the USB port and its first complete frame is received
  and passes its checksum
- **THEN** the port enters MAVLink mode
- **AND** the remaining bytes already buffered are processed as MAVLink, not as command
  text

#### Scenario: Line noise on an open port

- **WHEN** bytes arrive that do not form a checksum-valid frame
- **THEN** the port stays in CLI mode
- **AND** the CLI remains usable

#### Scenario: The switch does not reverse

- **WHEN** the port is in MAVLink mode and the ground station disconnects
- **THEN** the port stays in MAVLink mode until the board is reset
- **AND** text sent to it afterwards is not executed as a command

### Requirement: The CLI is unreachable while the port is in MAVLink mode

Once the USB port is in MAVLink mode the firmware SHALL NOT execute commands from it
and SHALL NOT emit CLI text on it. Anything a reader needs from the CLI in that state
SHALL require a reset first.

#### Scenario: Commands sent after the switch

- **WHEN** command text is sent to a port already in MAVLink mode
- **THEN** no command runs and no firmware state changes
- **AND** no CLI text is emitted that a frame parser would have to discard

#### Scenario: Recovering the CLI

- **WHEN** the board is reset with no ground station sending frames
- **THEN** the port is in CLI mode again and commands are executed

### Requirement: The console CLI answers on the USB port while it is in CLI mode

The firmware SHALL accept text commands and emit text replies on the USB CDC port at
115200 while that port is in CLI mode. The same port SHALL also carry MAVLink, per
the `mavlink-link` capability, once it has switched modes. No other port SHALL carry
the CLI, and the choice of port SHALL NOT be a build-time option.

#### Scenario: Host opens the USB port after boot

- **WHEN** a host opens the USB CDC port at 115200 on a board that has received no
  MAVLink frame since reset
- **THEN** it can send a command and receive a text reply

#### Scenario: One port, both protocols

- **WHEN** any build of the firmware is flashed
- **THEN** the USB port carries the CLI and, after a valid frame, MAVLink
- **AND** no build flag moves the CLI to another port

## REMOVED Requirements

### Requirement: The console CLI is carried on the USB CDC port

**Reason**: The requirement forbade MAVLink on the USB port in the flight build and
made the console's port a build-time choice, naming a single definition a build could
override to move it. This change makes the USB port carry both protocols at runtime,
selected by what arrives on it rather than by what was flashed, so both the
prohibition and the build-time choice cease to hold.

**Migration**: Replaced by *The console CLI answers on the USB port while it is in
CLI mode*, which keeps the "commands in, text replies out, at 115200" behaviour
verbatim and states the new mode-dependent scope. The mode-switching behaviour itself
is covered by *The USB port chooses its protocol from what arrives on it*, above.
Builds that overrode `CLI_SERIAL` no longer need to: every build carries the CLI and
MAVLink on the same USB port, distinguished by mode, not by build flag.
