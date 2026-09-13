## MODIFIED Requirements

### Requirement: Where memory is reserved is not externally observable

Whether a task stack, a queue structure or a queued item is reserved when the image is
linked or obtained while the firmware runs SHALL NOT be observable in the firmware's
external behaviour. In the normal configuration, the message set, rates and identity
are those the `mavlink-link` capability defines, and the housekeeping record keeps its
cadence and its fields. The reduced configuration defined by the `fault-recovery`
capability is a distinct, observable state and is not governed by this requirement.

This governs steady-state behaviour. What a producer observes when a queue is
saturated is governed by *A declared queue depth is backed*, and is deliberately
different.

#### Scenario: A ground station sees no difference

- **WHEN** a ground station is connected to the link and the firmware is in the normal
  configuration
- **THEN** it receives the message set, at the rates and with the identity that the
  `mavlink-link` capability defines
- **AND** nothing in that traffic reveals where the firmware's memory was reserved
