# Slime melee presentation replacement

Original melee particle and audio cue definitions are preserved in this directory. Original texture and WAV files remain in place; the live melee cue definitions no longer reference the old WAV variants or melee_arc sprite. Shared death, spawn warning, Bleed, Burn, Slow, and Mark effects remain unchanged.

The replacement uses existing runtime cue IDs to preserve event routing. Melee hit remains emitted only after a successful range check. The 0.35-second cyan ground sector uses velocity facing and a short forward wave so it follows the attack's fixed direction. Cyan droplets and a brief spark present successful hits.

Audio was authored with tools/audio/author_slime.py using deterministic elastic FM resonance and filtered noise. Stable Audio generation was attempted first: native imports required execution outside the sandbox, then the pinned small-sfx model download returned HTTP 401 (gated model access). Procedural authoring was selected to avoid blocking on unavailable credentials. This is not model-generated audio.

Final WAVs: mono, 48 kHz, 24-bit PCM. Windup: 0.35 s, -6.34 dBFS peak. Hit: 0.19492 s after conservative tail trimming, -8.81 dBFS peak. Both passed finalize.py decode, finite/non-silent, channel, sample rate and peak validation. Final subjective timbre and in-game visual readability require listening/viewing.
