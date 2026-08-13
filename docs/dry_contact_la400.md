# Dry contact protocol — LA400 discrete open/close/stop controls

**This fork's `drycontact` protocol is specific to operators with discrete
OPEN/CLOSE command inputs** — built for a LiftMaster LA400UL gate with its
accessory expansion board (and applicable to commercial door operators with
3-button station terminals). It is NOT interchangeable with upstream's
drycontact behavior: upstream drives everything through the single button
control (SBC) input, this fork drives open/close through the discrete pins
and reserves SBC for toggle/stop. Do not flash this firmware on an opener
wired only for SBC — open/close would pulse unconnected pins.

No configuration changes versus upstream: the standard
`v32board_drycontact.yaml` builds it, with `discrete_open_pin: GPIO26` (SD)
and `discrete_close_pin: GPIO25` (SO) as already defined there.

## Why: single-button control is a guessing game

The SBC wire cycles the operator: open → stop → close → stop → open. What a
pulse does depends on state the ratgdo can only partially observe, so Home
Assistant's Open button can stop a moving gate, Close falls back to a toggle
(or is silently dropped when the state is unknown), and any externally-issued
command (RF remote, keypad) desynchronizes the cycle.

With discrete controls, Open and Close are **direction-absolute**: they do
what the button says regardless of current state, including reversing
mid-travel. The cover in Home Assistant behaves like a Security+ door:

| Command | Upstream drycontact (SBC only)       | This fork                           |
| ------- | ------------------------------------ | ----------------------------------- |
| Open    | pulse SBC — may stop or close        | always opens (reverses if closing)  |
| Close   | TOGGLE fallback, or dropped          | always closes (reverses if opening) |
| Toggle  | pulse SBC                            | pulse SBC (unchanged)               |
| Stop    | pulse SBC while believed moving      | pulse SBC (unchanged)               |

## Wiring (LiftMaster LA400UL)

Requires the LA400UL expansion board (K1D8387), which provides the
SBC/OPN/CLS/STP command inputs and two DIP-configurable AUX relays used to
feed gate position back to the ratgdo's limit switch inputs.

```
      ratgdo32                       LA400UL expansion board
 ┌───────────────┐                 ┌─────────────────────────┐
 │             G ├────────┬───────►│ AUX RELAY 1 COM         │
 │            SO ├────────│───────►│ CLS         (CONTROLS)  │
 │            SD ├────────│───────►│ OPN         (CONTROLS)  │
 │            TL │        └───────►│ AUX RELAY 2 COM         │
 │            TC │◄────────────────┤ AUX RELAY 2 NC          │
 │            TO │◄────────────────┤ AUX RELAY 1 NO          │
 │       red ctrl├─────────────────►│ SBC        (CONTROLS)  │
 │       wht gnd ├─────────────────►│ COM        (CONTROLS)  │
 └───────────────┘                 └─────────────────────────┘

 AUX RELAY 1 DIP switches: OFF-OFF-ON  → energizes at open limit
 AUX RELAY 2 DIP switches: OFF-ON-OFF  → energizes when not at close limit
```

- **SD/SO** are the ratgdo32's status output pins (GPIO26/GPIO25), used as
  discrete open/close outputs. They connect directly to the OPN/CLS inputs —
  no relays needed.
- **AUX relay feedback** gives the firmware real open/closed state: AUX1's NO
  contact closes at the open limit (→ TO input); AUX2's NC contact closes at
  the close limit (→ TC input).
- **Keep the SBC wiring** — it still serves Toggle and Stop.
- **Do not touch the STP terminal.** On the LA400UL it is a normally-closed
  safety loop: breaking it halts and inhibits the operator entirely.
- Party mode wiring (a separate output to the operator's reset-button input)
  is unaffected — it uses its own pin.

## Behavior changes versus upstream

1. **Open/Close pulse only the discrete pins.** The SBC pulse upstream always
   adds is suppressed for these two actions so the operator never receives
   two conflicting commands at once. Toggle and Stop still use SBC.
2. **Close is sent directly.** Upstream refuses a true CLOSE unless a
   Chamberlain-style obstruction sensor is detected, degrading to TOGGLE or
   dropping the command. The stop-first sequence for close-while-opening is
   also skipped — these operators reverse on an opposing command, and the
   limit-switch state machine never reports STOPPED, which would deadlock the
   deferred close.
3. **Optimistic state on commands.** A mid-travel reversal produces no limit
   switch event, so the commanded direction is reported immediately. A
   self-issued Stop/Toggle while moving reports STOPPED at that instant,
   freezing the time-based position estimate where the operator actually
   halted.
4. **Travel-time watchdog.** If no limit switch confirms arrival within the
   configured opening/closing duration plus 3 s (e.g. the operator was
   stopped by an RF remote), the state expires to *unknown* instead of
   showing "opening"/"closing" forever. Purely display — it never issues
   commands. Set the *Opening duration* and *Closing duration* entities to
   your measured travel time to arm it; at the default 0 it stays off.
5. **No assumed end states.** The duration-based "probably reached
   open/closed" fallbacks are disabled — limit switches are authoritative.

Security+ builds are unaffected: all changes are inside
`PROTOCOL_DRYCONTACT` guards or the dry contact protocol itself.
