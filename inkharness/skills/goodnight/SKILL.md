---
name: goodnight
description: Bedtime routine. Pauses every speaker that is playing, then says goodnight. Use when the user says goodnight or that they're going to bed.
---
Pause every speaker that is playing in one run_js call, with this script as it is:

```js
const playing = tools.list_speakers().split("\n")
  .filter(l => l.endsWith(" | playing"))
  .map(l => l.split(" | ")[0]);
for (const s of playing) tools.media_control({speaker: s, action: "pause"});
playing.length
```

It returns how many speakers it paused. Then say goodnight in a few words, and mention the
speakers only if it paused any, for example "I paused two speakers. Goodnight!"

Without run_js, call list_speakers, then media_control with action pause for each speaker whose
state is playing. Without list_speakers, say you can't reach the speakers, and goodnight.
