# Switching behavior

## Input qualification

There are six physical alarm inputs. Each has a configurable enable flag, name,
ATEM source number (1–6), polarity, and priority (1 is highest, 6 is lowest).
Enabled alarms must map to distinct sources. GPIO assignments come from the
compiled board profile and are not editable in the web page.

Every 10 ms the main loop samples the electrical input level. `activeHigh=true`
means GPIO HIGH represents lock; `false` means GPIO LOW represents lock. This is
the level **after the electrical interface**, not necessarily the receiver's
unconditioned alarm level. In the recommended optocoupler circuit, receiver HIGH
produces GPIO LOW and therefore uses `activeHigh=false`.

An asserted level must persist continuously for the acquisition time before
becoming qualified. A qualified lock remains present until deassertion persists
for the loss time. A return to the previous level cancels the pending change.
Defaults are 1,000 ms acquire and 2,000 ms release. Both may be configured from
0 through 60,000 ms. A disabled channel never qualifies.

The dashboard reports the raw level, qualified lock and time remaining for a
pending change. During the release delay a receiver still counts as qualified.
These are software timing targets; network/web handling and the ESP32 scheduler
can add latency. This is not a hard real-time video-loss detector.

## One or no qualified lock

With exactly one qualified receiver, select its mapped input immediately after
qualification. With none, select the configured fallback. The initial fallback
is ATEM Black (0); it can instead be input 1–6. A fallback is deliberately allowed
to be a prepared slate or source without a receiver alarm.

Automatic selection changes Program directly, producing a cut. It waits for an
already-reported ATEM transition to finish before sending another automatic
Program selection. It does not change the configured ATEM transition style or
rate, and does not clear Fade to black automatically.

“Immediately” describes the policy decision after input qualification. Actual
automatic requests are spaced at least 500 ms apart and wait for synchronized
feedback, transport availability, and completion of a reported transition.

## Several qualified locks

The first selected receiver is the one with the lowest configured priority
number; ties use physical alarm-channel order.

### Rotate locked inputs (default)

When the count first reaches two or more, begin a full dwell interval, initially
300 seconds. Keep the currently selected qualified receiver for that interval,
then select the next qualified receiver in alarm-channel order, wrapping from
channel 6 to channel 1. Skip disabled or unqualified channels.

For example, with alarms 1, 3 and 6 qualified and the default mapping/priorities,
the order is input 1, then input 3 five minutes later, then input 6 five minutes
after that, and back to input 1 five minutes later. An additional receiver joining
does not reset an interval already running. Thus repeated joins cannot prevent
other receivers getting a turn.

If the selected receiver loses qualification, choose an available receiver
without waiting out the remaining dwell. Start a full dwell on the replacement.
If only one remains, keep that receiver and stop the competition timer. When a
second qualified receiver appears again, begin a new full dwell interval.

### Input priority

Always select the qualified receiver with the lowest priority number, with
alarm-channel order resolving equal priorities. A newly qualified higher-priority
receiver takes over immediately. This policy does not use the dwell interval.

### Selected source after delay

Keep the currently selected qualified receiver while two or more receivers
remain qualified for the entire configured interval. At expiry, select the
configured multiple-lock source, even if that source has no alarm lock of its
own. It may be Black or input 1–6.

Stay on that conflict source while at least two receivers remain qualified. If
competition drops to one, select the remaining receiver. If it drops to zero,
select the no-lock fallback. A later return to multiple locks starts a new full
delay. Once the delay has expired, a latched state prevents a timer rollover
after prolonged uptime from accidentally leaving the conflict output.

This policy measures **continuous concurrent qualified locks**. It does not
remember separate, nonoverlapping lock events within the previous five minutes.

## Operator control, failures and restart

Monitoring stays active while automation is paused. Automatic selection starts
paused on every boot, and saving configuration pauses it and requalifies all
inputs. Resume requires synchronized ATEM feedback and starts a fresh selection
cycle using the currently qualified alarms.

Manual Program, Cut, Auto transition and Fade to black pause automatic selection.
Preview selection does not. An observed unexpected Program change made by another
controller also pauses automation by default. This can be disabled in settings
if this controller is intended to continually enforce its selection. Program
feedback identifies the source, not the panel that selected it: an external
selection of the exact source this controller just requested is indistinguishable
from confirmation of its own request. It cannot detect every external button press.

Pausing with unacknowledged automatic commands abandons the current UDP session,
preventing additional retries from that session. It cannot retract a datagram
already sent, in the network, or applied by the ATEM. A manual request that coincides with this
reset returns a reconnect message and is not queued; wait for fresh feedback and
repeat the manual action.

During loss of connectivity or incomplete feedback, the controller stops issuing
new switching commands. Manual commands are not queued for later replay. After
reconnection, it obtains fresh ATEM state and reevaluates current qualified locks.
Only one automatic Program decision remains unresolved at a time. An obsolete
unacknowledged target causes the session to reset instead of being followed by
another queued selection. An ACK without matching Program feedback for two
seconds also triggers reconnection for fresh state. Three unconfirmed attempts
for the same desired source pause automation; reconnecting does not replenish
that budget. The transport README describes its separate packet retry limits.

The last Program baseline is retained over feedback gaps and reconnects so an
unexpected changed source can pause automation when operator precedence is
enabled. A reconnect matching this controller's last possibly-applied target is
recognized as an expected own result. Hardware acceptance must include
interruptions and external-controller takeover on the actual switcher.

The ESP32's GPIOs do not carry video and are never driven as alarm outputs by this
firmware. Receiver signal validity, compatible video format and working HDMI/SDI
cables remain separate prerequisites for a usable Program picture.
