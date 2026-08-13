# Pipeline debugger

The Windows tray menu exposes **Pipeline debugger...** for inspecting the live
semantic cleanup path. The debugger is diagnostic UI only: its contents remain
in process memory, reset when a new dictation starts, and are never persisted.

The four panes show:

1. the latest exact Nemotron partial or final hypothesis;
2. the exact versioned JSON sent to the rewrite worker, including
   `readOnlyContext`, `editableTail`, and `newAsrText`, plus the queue duration
   and approximate `newAsrText` word/byte size;
3. the last worker response and the controller decision (`Accepted`,
   `Preserved raw`, `Rejected`, `Ignored`, timeout, or worker error);
4. the effective composed text used by the overlay and final insertion.

The current request and the last completed response are kept separately. A new
pending request therefore does not hide the response and commit decision that
preceded it. **Copy complete snapshot** copies all four panes for a bug report.

The debugger deliberately does not store a transcript history or write log
files. Close hides the window without affecting recording or cleanup.

The live scheduler enforces at least 8,000 ms between rewrite dispatches and
also waits for a 1,200 ms quiet period after a newer stable ASR update. During
continuous speech, the eight-second interval acts as the collection deadline.
Only one rewrite runs at a time; no parallel cleanup requests are issued.

The worker has a ten-second generation budget and the controller a
twelve-second process deadline. A timeout or worker error consumes the covered
stable ASR span as raw text, so the same failing span cannot remain in the
backlog and grow into every later request. A replacement that repeats any
six-token run from `readOnlyContext` is rejected before it can be frozen.
