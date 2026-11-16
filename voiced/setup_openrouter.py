#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Sets voiced up to run all three stages through OpenRouter, asking as it goes.

    uv run setup_openrouter.py            (or: python3 setup_openrouter.py)

It asks for the API key (kept in .env next to this file, which git ignores), lets you pick the
speech-to-text, chat and text-to-speech models and a voice from OpenRouter's live lists, tries
each one with a short request, finds the sample rate of the speech model's PCM by playing it
to you, and writes voiced.toml. An existing voiced.toml keeps its listen address, token, system
prompt and [home_assistant] section.

--no-test skips the trial requests (each costs a fraction of a cent).
"""

import argparse
import getpass
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

BASE = "https://openrouter.ai/api/v1"
KEY_ENV = "OPENROUTER_API_KEY"
HERE = Path(__file__).resolve().parent
ENV_FILE = HERE / ".env"
CONFIG = HERE / "voiced.toml"
RATES = [8000, 16000, 22050, 24000, 44100, 48000]
PHRASE = "Okay, playing jazz on the kitchen speaker."
MENU_MAX = 60  # longer lists ask for a search word first


class Failed(Exception):
    pass


# --- HTTP ---------------------------------------------------------------------------------


def request(path, key=None, body=None, headers=None):
    """An open response for GET (no body) or POST. Raises Failed with the service's message."""
    all_headers = {"User-Agent": "voiced-setup"}
    if key:
        all_headers["Authorization"] = f"Bearer {key}"
    if isinstance(body, dict):
        body = json.dumps(body).encode()
        all_headers["Content-Type"] = "application/json"
    all_headers.update(headers or {})
    req = urllib.request.Request(BASE + path, data=body, headers=all_headers)
    try:
        return urllib.request.urlopen(req, timeout=60)
    except urllib.error.HTTPError as e:
        text = e.read().decode(errors="replace")
        try:
            text = json.loads(text)["error"]["message"]
        except (ValueError, KeyError, TypeError):
            pass
        raise Failed(f"HTTP {e.code}: {text[:400]}") from None
    except (urllib.error.URLError, TimeoutError) as e:
        raise Failed(f"can't reach openrouter.ai: {getattr(e, 'reason', e)}") from None


def get_json(path, key=None):
    with request(path, key) as r:
        return json.load(r)


def models(query):
    return get_json(f"/models?{query}")["data"]


# --- Asking -------------------------------------------------------------------------------


def ask(prompt, default=""):
    suffix = f" [{default}]" if default else ""
    try:
        answer = input(f"{prompt}{suffix}: ").strip()
    except EOFError:
        raise Failed("no more input") from None
    return answer or default


def yes(prompt, default=True):
    answer = ask(f"{prompt} ({'Y/n' if default else 'y/N'})").lower()
    return default if not answer else answer.startswith("y")


def pick(title, items, label=str, default=None, key=str):
    """A numbered menu. A number picks, any other text narrows the list, Enter takes the
    default. `key(item)` is what `default` is compared with and what a search matches."""
    print(f"\n{title}")
    shown = items
    while True:
        if len(shown) > MENU_MAX:
            print(f"  {len(shown)} to choose from.")
        else:
            for i, item in enumerate(shown, 1):
                mark = "*" if default is not None and key(item) == default else " "
                print(f" {mark}{i:3}  {label(item)}")
        hint = "number, or text to search"
        if len(shown) != len(items):
            hint += ", or 'all'"
        answer = ask(f"  {hint}", default or "")
        if not answer:
            continue
        if answer == "all":
            shown = items
            continue
        if answer.isdigit() and len(shown) <= MENU_MAX:
            if 1 <= int(answer) <= len(shown):
                return shown[int(answer) - 1]
            print("  No such number.")
            continue
        exact = [item for item in items if key(item) == answer]
        if exact:
            return exact[0]
        words = answer.lower().split()
        found = [item for item in items if all(w in label(item).lower() for w in words)]
        if not found:
            print("  Nothing matches.")
        elif len(found) == 1:
            return found[0]
        else:
            shown = found


# --- The key ------------------------------------------------------------------------------


def read_env_file():
    values = {}
    if ENV_FILE.exists():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip().removeprefix("export ")
            if "=" in line and not line.startswith("#"):
                name, value = line.split("=", 1)
                values[name.strip()] = value.strip().strip("\"'")
    return values


def save_key(key):
    lines = ENV_FILE.read_text().splitlines() if ENV_FILE.exists() else []
    lines = [l for l in lines if not l.strip().removeprefix("export ").startswith(KEY_ENV + "=")]
    lines.append(f"{KEY_ENV}={key}")
    ENV_FILE.touch(mode=0o600)
    ENV_FILE.chmod(0o600)
    ENV_FILE.write_text("\n".join(lines) + "\n")


def check_key(key):
    info = get_json("/key", key).get("data", {})
    parts = [f"key \"{info.get('label', '?')}\""]
    if info.get("limit_remaining") is not None:
        parts.append(f"${info['limit_remaining']:.2f} left of its limit")
    if info.get("is_free_tier"):
        parts.append("free tier (paid models need credits)")
    return ", ".join(parts)


def get_key():
    """Returns (key, whether it still has to be saved to .env)."""
    stored = read_env_file().get(KEY_ENV, "")
    for source, key in ((f"${KEY_ENV}", os.environ.get(KEY_ENV, "")), (".env", stored)):
        if not key:
            continue
        try:
            print(f"Using the key from {source}: {check_key(key)}.")
            return key, key != stored
        except Failed as e:
            print(f"The key from {source} was refused ({e}).")
    print("Create a key at https://openrouter.ai/settings/keys and paste it here (it won't show).")
    while True:
        try:
            key = getpass.getpass("API key: ").strip()
        except EOFError:
            raise Failed("no more input") from None
        if not key:
            continue
        try:
            print(f"Accepted: {check_key(key)}.")
            return key, True
        except Failed as e:
            print(f"Refused ({e}). Try again, or Ctrl-C to stop.")


# --- Trying the models --------------------------------------------------------------------


def wav_bytes(pcm, rate):
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return out.getvalue()


def try_speech(key, model, voice):
    """The request the daemon makes. Returns (pcm, content type, first-byte s, total s)."""
    start = time.monotonic()
    body = {"model": model, "input": PHRASE, "voice": voice, "response_format": "pcm"}
    with request("/audio/speech", key, body) as r:
        content_type = r.headers.get("Content-Type", "")
        pcm, first = bytearray(), None
        while chunk := r.read1(8192):
            first = first or time.monotonic() - start
            pcm += chunk
    if content_type.startswith(("application/json", "text/")):
        raise Failed(f"got {content_type} instead of audio: {bytes(pcm[:300]).decode(errors='replace')}")
    if len(pcm) < 2000:
        raise Failed(f"only {len(pcm)} bytes of audio came back")
    if pcm[:4] in (b"RIFF", b"OggS", b"fLaC") or pcm[:3] == b"ID3":
        raise Failed("the service returned an encoded file, not raw PCM; voiced needs PCM")
    return bytes(pcm[: len(pcm) & ~1]), content_type, first, time.monotonic() - start


def rate_from_header(content_type):
    for part in content_type.replace(" ", "").split(";")[1:]:
        name, _, value = part.partition("=")
        if name in ("rate", "sample_rate") and value.isdigit():
            return int(value)
    return None


def player():
    for command in (["afplay"], ["paplay"], ["aplay", "-q"], ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]):
        if shutil.which(command[0]):
            return command
    return None


def find_rate(pcm, guess):
    """Plays the PCM at a rate and moves up or down the usual ones until it sounds right."""
    command = player()
    if not command:
        print("  No audio player found (afplay, paplay, aplay, ffplay), so the rate can't be checked by ear.")
        return int(ask("  Sample rate of the model's PCM", str(guess)))
    rate = guess
    print(f'  You should hear: "{PHRASE}"')
    while True:
        with tempfile.NamedTemporaryFile(suffix=".wav") as f:
            f.write(wav_bytes(pcm, rate))
            f.flush()
            subprocess.run(command + [f.name], check=False)
        answer = ask(
            f"  At {rate} Hz. [y] sounds right  [s] too slow or deep  [f] too fast or high  [r] replay", "y"
        ).lower()
        if answer.isdigit():
            rate = int(answer)
        elif answer.startswith("y"):
            return rate
        elif answer.startswith(("s", "f")):
            # Too slow: the true rate is higher than the one it was played at.
            higher = answer.startswith("s")
            options = [r for r in RATES if (r > rate if higher else r < rate)]
            if not options:
                print("  No usual rate left that way; type a rate in Hz to try it.")
            else:
                rate = min(options) if higher else max(options)


def try_transcription(key, model, wav):
    """The daemon's multipart upload. Returns (text, seconds)."""
    boundary = uuid.uuid4().hex
    parts = b""
    for name, value in (("model", model), ("response_format", "json")):
        parts += f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode()
    parts += (
        f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="utterance.wav"\r\n'
        "Content-Type: audio/wav\r\n\r\n"
    ).encode()
    parts += wav + f"\r\n--{boundary}--\r\n".encode()
    start = time.monotonic()
    with request(
        "/audio/transcriptions", key, parts, {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    ) as r:
        reply = json.load(r)
    if "text" not in reply:
        raise Failed(f"no text in the reply: {json.dumps(reply)[:300]}")
    return reply["text"].strip(), time.monotonic() - start


REASONING = {"off": {"enabled": False}}  # other levels: {"effort": level}, as voiced sends


def try_chat(key, model, reasoning=None):
    """A streamed request with one tool, as the daemon sends. Returns (tool called, text,
    first-token s, total s)."""
    body = {
        "model": model,
        "stream": True,
        "max_tokens": 2000,
        "messages": [
            {"role": "system", "content": "You are a voice assistant. Use the tools to control things in the home."},
            {"role": "user", "content": "Which speakers are there?"},
        ],
        "tools": [
            {
                "type": "function",
                "function": {
                    "name": "list_speakers",
                    "description": "Lists the speakers in the home.",
                    "parameters": {"type": "object", "properties": {}},
                },
            }
        ],
    }
    if reasoning:
        body["reasoning"] = REASONING.get(reasoning, {"effort": reasoning})
    start = time.monotonic()
    first, called, text = None, None, ""
    with request("/chat/completions", key, body) as r:
        for raw in r:
            line = raw.decode(errors="replace").strip()
            if not line.startswith("data:") or line == "data: [DONE]":
                continue
            try:
                event = json.loads(line[5:])
            except ValueError:
                continue
            if "error" in event:
                raise Failed(str(event["error"].get("message", event["error"]))[:400])
            for choice in event.get("choices", []):
                delta = choice.get("delta", {})
                if delta.get("content") or delta.get("tool_calls"):
                    first = first or time.monotonic() - start
                text += delta.get("content") or ""
                for call in delta.get("tool_calls") or []:
                    called = called or call.get("function", {}).get("name")
    return called, text.strip(), first, time.monotonic() - start


# --- voiced.toml --------------------------------------------------------------------------


def toml_string(s):
    return json.dumps(s, ensure_ascii=False)  # JSON's escapes are TOML basic-string escapes


def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return str(v)
    if isinstance(v, list):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    return toml_string(str(v))


def toml_table(name, table):
    """A section kept from the old file, its subtables after its own keys."""
    lines = ["", f"[{name}]"]
    lines += [f"{k} = {toml_value(v)}" for k, v in table.items() if not isinstance(v, dict)]
    for k, v in table.items():
        if isinstance(v, dict):
            lines += toml_table(f"{name}.{k}", v)
    return lines


# Sections the script doesn't ask about but keeps from an existing voiced.toml.
KEPT_SECTIONS = ("think", "cache", "js", "mcp", "skills")


def render(kept, stt, llm, reasoning, tts, voice, rate):
    lines = [
        "# Written by setup_openrouter.py: all three stages through OpenRouter.",
        f"# The key is read from ${KEY_ENV} (see .env).",
        "",
        f"listen = {toml_string(kept.get('listen', '0.0.0.0:7391'))}",
        f"token = {toml_string(kept.get('token', ''))}",
    ]
    if "system_prompt" in kept:
        lines.append(f"system_prompt = {toml_string(kept['system_prompt'])}")
    for section, pairs in (
        ("stt", [("model", stt)]),
        ("llm", [("model", llm)] + ([("reasoning", reasoning)] if reasoning else [])),
        ("tts", [("model", tts), ("voice", voice), ("rate", rate)]),
    ):
        lines += ["", f"[{section}]", 'backend = "openai_compat"', f"base_url = {toml_string(BASE)}"]
        lines += [f"{k} = {v if isinstance(v, int) else toml_string(v)}" for k, v in pairs]
        lines.append(f"api_key_env = {toml_string(KEY_ENV)}")
    home = kept.get("home_assistant")
    if home:
        lines += toml_table("home_assistant", home)
    else:
        lines += [
            "",
            "# [home_assistant]",
            '# url = "http://homeassistant.local:8123"',
            '# token_env = "HOME_ASSISTANT_TOKEN"',
            '# play_service = "music_assistant.play_media"',
        ]
    for section in KEPT_SECTIONS:
        value = kept.get(section)
        if isinstance(value, list):  # an array of tables: [[mcp]]
            for item in value:
                table = toml_table(section, item)
                table[1] = f"[[{section}]]"
                lines += table
        elif value:
            lines += toml_table(section, value)
    return "\n".join(lines) + "\n"


def existing_config():
    """(settings to keep, the models an OpenRouter config already names)."""
    if not CONFIG.exists():
        return {}, {}
    try:
        old = tomllib.loads(CONFIG.read_text())
    except (tomllib.TOMLDecodeError, OSError) as e:
        print(f"{CONFIG.name} exists but can't be read as TOML ({e}); nothing is kept from it.")
        return {}, {}
    kept = {k: old[k] for k in ("listen", "token", "system_prompt", "home_assistant", *KEPT_SECTIONS) if k in old}
    current = {}
    for stage in ("stt", "llm", "tts"):
        section = old.get(stage, {})
        if section.get("base_url", "").startswith("https://openrouter.ai"):
            current[stage] = section.get("model")
            if stage == "llm":
                current["reasoning"] = section.get("reasoning")
            if stage == "tts":
                current["voice"] = section.get("voice")
                current["rate"] = section.get("rate")
    return kept, current


# --- The steps ----------------------------------------------------------------------------


def model_label(m):
    return f"{m['id']:<52} {m['name']}"


def chat_label(m):
    price = m.get("pricing", {})
    try:
        cost = f"${float(price['prompt']) * 1e6:g} in, ${float(price['completion']) * 1e6:g} out per M tokens"
    except (KeyError, ValueError):
        cost = ""
    return f"{m['id']:<52} {cost}"


def model_id(m):
    return m["id"]


def choose_speech(key, current, test):
    """Returns (model id, voice, rate, the trial PCM or None)."""
    speech = models("output_modalities=speech")
    while True:
        model = pick("Text to speech: which model?", speech, model_label, current.get("tts"), model_id)
        voices = model.get("supported_voices") or []
        if voices:
            default = current.get("voice") if current.get("voice") in voices else None
            voice = pick(f"Which voice? ({model['id']})", voices, default=default)
        else:
            voice = ask("This model lists no voices. Voice name", current.get("voice") or "")
        if not test:
            return model["id"], voice, int(ask("Sample rate of its PCM", str(current.get("rate") or 24000))), None
        print(f"\nAsking {model['id']} to speak...")
        try:
            pcm, content_type, first, total = try_speech(key, model["id"], voice)
        except Failed as e:
            print(f"  That didn't work: {e}")
            print("  Pick again.")
            continue
        streamed = "streamed" if total - first > 0.15 else "arrived in one piece"
        print(f"  {len(pcm)} bytes; first audio after {first * 1000:.0f} ms, all of it after {total * 1000:.0f} ms ({streamed}).")
        header_rate = rate_from_header(content_type)
        if header_rate:
            print(f"  The service says {header_rate} Hz ({content_type}).")
        rate = find_rate(pcm, header_rate or current.get("rate") or 24000)
        if yes(f"Keep {model['id']} with voice {voice}?"):
            return model["id"], voice, rate, wav_bytes(pcm, rate)


def choose_transcription(key, current, test, sample):
    transcription = models("output_modalities=transcription")
    while True:
        model = pick("Speech to text: which model?", transcription, model_label, current.get("stt"), model_id)
        if not test or sample is None:
            return model["id"]
        print(f"\nSending the spoken test phrase to {model['id']}...")
        try:
            text, seconds = try_transcription(key, model["id"], sample)
        except Failed as e:
            print(f"  That didn't work: {e}")
            print("  Pick again.")
            continue
        print(f'  Heard "{text}" in {seconds * 1000:.0f} ms.')
        print(f'  Said  "{PHRASE}"')
        if yes(f"Keep {model['id']}?"):
            return model["id"]


def choose_chat(key, current, test):
    """Returns (model id, reasoning setting or None)."""
    chat = [
        m
        for m in models("supported_parameters=tools")
        if not m["id"].endswith(":batch") and "text" in m.get("architecture", {}).get("output_modalities", ["text"])
    ]
    chat.sort(key=lambda m: -m.get("created", 0))
    print("\nThe chat model needs to call tools and answer fast: a small one (search haiku, flash, mini, nano) is a good start.")
    print("Add :nitro to an open model's id to have OpenRouter pick its fastest host.")
    saved = current.get("llm") or ""
    ids = {m["id"] for m in chat}
    listed = saved if saved in ids else saved.rsplit(":", 1)[0]  # "x:nitro" is listed as "x"
    while True:
        model = pick("Chat: which model?", chat, chat_label, listed or None, model_id)
        default = saved if listed == model["id"] and saved != listed else model["id"]
        name = ask("  Id to write (add :nitro for the fastest host)", default)
        reasoning = None
        if "reasoning" in model.get("supported_parameters", []):
            print("  This model can think before it answers, which delays the first word.")
            if yes("  Turn its thinking off?", default=current.get("reasoning", "off") == "off"):
                reasoning = "off"
        if not test:
            return name, reasoning
        print(f"Asking {name} a question that needs a tool...")
        try:
            called, text, first, total = try_chat(key, name, reasoning)
        except Failed as e:
            if reasoning and "reasoning" in str(e).lower():
                print(f"  Its thinking can't be turned off ({e}); trying it as it is.")
                reasoning = None
                try:
                    called, text, first, total = try_chat(key, name)
                except Failed as e2:
                    print(f"  That didn't work: {e2}\n  Pick again.")
                    continue
            else:
                print(f"  That didn't work: {e}\n  Pick again.")
                continue
        when = f"first token after {first * 1000:.0f} ms, done after {total * 1000:.0f} ms" if first else "nothing came back"
        if called:
            print(f"  It called {called}: {when}.")
        else:
            print(f'  It did not call the tool ({when}). It said: "{text[:120]}"')
        if yes(f"Keep {name}?", default=bool(called)):
            return name, reasoning


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-test", action="store_true", help="don't send trial requests to the chosen models")
    args = parser.parse_args()
    test = not args.no_test

    print("voiced through OpenRouter\n")
    key, unsaved = get_key()
    kept, current = existing_config()

    tts, voice, rate, sample = choose_speech(key, current, test)
    stt = choose_transcription(key, current, test, sample)
    llm, reasoning = choose_chat(key, current, test)

    text = render(kept, stt, llm, reasoning, tts, voice, rate)
    print(f"\n--- {CONFIG.name} ---\n{text}---")
    if CONFIG.exists():
        note = "its listen address, token, system prompt, [home_assistant], [think], [cache], [js] and [[mcp]] are kept" if kept else "nothing is kept from it"
        if not yes(f"{CONFIG} exists ({note}). Replace it?"):
            print("Left as it was. Nothing written.")
            return
    CONFIG.write_text(text)
    print(f"Wrote {CONFIG}.")
    if unsaved and yes(f"Save the key to {ENV_FILE} (git ignores it; only you can read it)?"):
        save_key(key)
        unsaved = False

    if unsaved:
        print(f"\nThe key was not saved: export {KEY_ENV} before starting the daemon.")
    print(
        f"\nRun the daemon (it reads .env itself):\n  cd {HERE}\n"
        "  cargo run --release -p voiced -- voiced.toml\n"
        "and in another terminal, a turn from a 16-bit mono WAV:\n"
        '  say -o command.wav --data-format=LEI16@16000 "What speakers are there?"   # macOS\n'
        "  cargo run --release -p voice-client -- --wav command.wav"
    )


if __name__ == "__main__":
    try:
        main()
    except Failed as e:
        sys.exit(f"\nStopped: {e}")
    except KeyboardInterrupt:
        sys.exit("\nStopped. Nothing written.")
