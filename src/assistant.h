#pragma once
// Kevin's Orb — the voice Assistant's engine: record a question, send it to OpenAI to be
// turned into text, ask ChatGPT for a short spoken-style answer, turn the answer into speech,
// play it. The screen is assistant_view.*.
//
// OpenAI, three calls per question (models tested on Torrey's key, 2026-10-26):
//   speech -> text   /v1/audio/transcriptions   gpt-transcribe
//   the answer       /v1/chat/completions       gpt-6-luna
//   text -> speech   /v1/audio/speech           gpt-4o-mini-tts, raw 24 kHz PCM
// The key is OPENAI_API_KEY in src/secrets.h, which .gitignore keeps off GitHub. Any of the
// ASSIST_MODEL_* names below can be overridden there too, when OpenAI renames one.
//
// Threads: the screen (core 1) asks; the network task (core 0) does everything that waits
// on the microphone or the network, one stage per call of assist_step(), and owns every
// buffer. The screen reads the state, the level meter and the texts through the getters.
#include <stdint.h>
#include <stddef.h>

enum AssistState : uint8_t {
    AS_NOKEY,      // no key in secrets.h
    AS_IDLE,       // waiting for a push
    AS_LISTENING,  // recording
    AS_HEARD,      // recording done; transcribing next
    AS_THINKING,   // asking ChatGPT
    AS_VOICING,    // fetching the spoken answer
    AS_SPEAKING,   // playing it (the text is up too)
    AS_ANSWERED,   // done; the text stays up
    AS_ERROR,      // something went wrong; the message says what
};

bool        assist_has_key();
AssistState assist_state();
int         assist_level();                 // microphone level 0..100 while listening
// Copies of the last question, answer and error message (plain ASCII, ready to show).
void        assist_texts(char *q, size_t qCap, char *a, size_t aCap, char *err, size_t errCap);

// Screen (core 1):
void assist_push();      // idle/answered/error -> listen; listening -> stop and send; speaking -> stop talking
void assist_open();      // the screen came up: a fresh conversation
void assist_close();     // the screen went away: stop, and let the network task tidy up

// Network task (core 0): one stage. Returns true when the state changed.
bool assist_step();

// The simulator (no network task) also needs the recording fed and the audio played.
#if !defined(ARDUINO)
void assist_sim_capture(const int16_t *pcm16k, int n);   // SDL microphone callback
#endif
