#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Compares voice pipelines on the same spoken commands: cost, tokens, latency, and whether the
right thing happened in the home.

    cargo build -p inkharness -p voice-client
    uv run compare_pipelines.py --out results.json          (macOS: the commands are made with `say`)
    cargo clean

The pipelines:

    3  speech-to-text → model → text-to-speech     (what inkharness.toml runs today)
    2  the model hears the recording → text-to-speech        ([llm] input = "audio")
    1  the model hears and speaks                            (input = "audio", output = "audio")

Each one runs as a real inkharness with a fake Home Assistant that records the actions it is asked
for (no real speaker is touched). voice-client sends every command on a fresh connection, after
one warm-up turn per pipeline. Latency is inkharness's own, from the end of speech: first audio is
when the reply starts playing, total when the turn is over. Costs are what OpenRouter charged,
looked up per request through its generation endpoint (the speech cache is off, so every sentence
is paid for). Needs OPENROUTER_API_KEY in .env; a full run with --repeat 2 costs about 30 cents, half of
it gpt-audio. --summarize prints the table again from saved result files.
"""

import argparse
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
OPENROUTER = "https://openrouter.ai/api/v1"

# --- The pipelines ------------------------------------------------------------------------

STT = """
[stt]
backend = "openai_compat"
base_url = "https://openrouter.ai/api/v1"
model = "openai/whisper-large-v3-turbo"
api_key_env = "OPENROUTER_API_KEY"
"""

TTS = """
[tts]
backend = "openai_compat"
base_url = "https://openrouter.ai/api/v1"
model = "speechify/simba-3.5"
voice = "beth"
rate = 24000
api_key_env = "OPENROUTER_API_KEY"
"""


def llm(model, hears=False, speaks=False, reasoning=None):
    lines = ["[llm]", 'backend = "openai_compat"', f'base_url = "{OPENROUTER}"',
             f'model = "{model}"', 'api_key_env = "OPENROUTER_API_KEY"']
    if reasoning:
        lines.append(f'reasoning = "{reasoning}"')
    if hears:
        lines.append('input = "audio"')
    if speaks:
        lines += ['output = "audio"', 'voice = "alloy"', "audio_rate = 24000"]
    return "\n" + "\n".join(lines) + "\n"


PIPELINES = {
    "3-qwen": ("3", "whisper → qwen3.6-35b-a3b → simba", STT + llm("qwen/qwen3.6-35b-a3b:nitro", reasoning="off") + TTS),
    "2-gemini": ("2", "gemini-3.5-flash-lite (hears) → simba", llm("google/gemini-3.5-flash-lite", hears=True) + TTS),
    # Its reasoning can't be turned off ("mandatory for this endpoint"); minimal is the least.
    "2-gemini-min": ("2", "gemini-3.5-flash-lite, reasoning minimal (hears) → simba",
                     llm("google/gemini-3.5-flash-lite", hears=True, reasoning="minimal") + TTS),
    "2-qwen-omni": ("2", "qwen3.8-omni-flash (hears) → simba", llm("qwen/qwen3.8-omni-flash", hears=True, reasoning="off") + TTS),
    "2-gpt-audio-mini": ("2", "gpt-audio-mini (hears) → simba", llm("openai/gpt-audio-mini", hears=True) + TTS),
    "1-gpt-audio-mini": ("1", "gpt-audio-mini (hears, speaks)", llm("openai/gpt-audio-mini", hears=True, speaks=True)),
    "1-gpt-audio": ("1", "gpt-audio (hears, speaks)", llm("openai/gpt-audio", hears=True, speaks=True)),
}

# --- The commands -------------------------------------------------------------------------


def called(domain, service, entity, **data):
    """The check that an action went to Home Assistant, with these values."""
    def check(calls, reply):
        for d, s, body in calls:
            if (d, s) != (domain, service) or body.get("entity_id") != entity:
                continue
            if all(value(body.get(k)) for k, value in data.items()):
                return True
        return False
    return check


def nothing_called_and(test):
    return lambda calls, reply: not calls and test(reply.lower())


COMMANDS = [
    ("pause", "Pause the kitchen speaker.", called("media_player", "media_pause", "media_player.kitchen")),
    ("jazz", "Play some jazz in the living room.", called("music_assistant", "play_media", "media_player.living_room",
                                                          media_id=lambda v: "jazz" in str(v).lower())),
    ("volume", "Set the bedroom volume to twenty percent.", called("media_player", "volume_set", "media_player.bedroom",
                                                                   volume_level=lambda v: v is not None and abs(v - 0.2) < 0.01)),
    ("next", "Skip to the next song in the kitchen.", called("media_player", "media_next_track", "media_player.kitchen")),
    ("stop", "Stop the music in the living room.", lambda c, r: called("media_player", "media_stop", "media_player.living_room")(c, r)
     or called("media_player", "media_pause", "media_player.living_room")(c, r)),
    ("resume", "Resume the music in the bedroom.", called("media_player", "media_play", "media_player.bedroom")),
    ("speakers", "What speakers do we have?", nothing_called_and(lambda r: all(w in r for w in ("kitchen", "living room", "bedroom")))),
    ("math", "What is seventeen times twenty three?", nothing_called_and(lambda r: "391" in r.replace(",", "") or "three hundred ninety-one" in r)),
]
WARMUP = "Hello there."

# --- A fake Home Assistant ----------------------------------------------------------------

STATES = [
    {"entity_id": "media_player.kitchen", "state": "playing", "attributes": {"friendly_name": "Kitchen"}},
    {"entity_id": "media_player.living_room", "state": "playing", "attributes": {"friendly_name": "Living Room"}},
    {"entity_id": "media_player.bedroom", "state": "paused", "attributes": {"friendly_name": "Bedroom"}},
    {"entity_id": "light.kitchen", "state": "on", "attributes": {"friendly_name": "Kitchen Light"}},
]


class FakeHome(BaseHTTPRequestHandler):
    calls = []  # (domain, service, data), cleared per command

    def do_GET(self):
        self.reply(STATES if self.path == "/api/states" else {"message": "API running."})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0)
        parts = self.path.strip("/").split("/")  # api/services/<domain>/<service>
        if len(parts) == 4 and parts[:2] == ["api", "services"]:
            FakeHome.calls.append((parts[2], parts[3], json.loads(body or b"{}")))
        self.reply([])

    def reply(self, value):
        data = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


# --- Running ------------------------------------------------------------------------------


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def key():
    if os.environ.get("OPENROUTER_API_KEY"):
        return os.environ["OPENROUTER_API_KEY"]
    for line in (HERE / ".env").read_text().splitlines():
        if line.startswith("OPENROUTER_API_KEY="):
            return line.split("=", 1)[1].strip().strip("\"'")
    sys.exit("OPENROUTER_API_KEY isn't set or in .env")


def speak(text, path):
    subprocess.run(["say", "-o", str(path), "--data-format=LEI16@16000", text], check=True)


TURN = re.compile(r'turn: (?P<heard>".*?") -> (?P<reply>".*") heard_secs=')


def field(line, name, kind=float):
    m = re.search(rf"\b{name}=(\S+)", line)
    return kind(m.group(1)) if m else None


class Daemon:
    """inkharness on its own port, its log in a file."""

    def __init__(self, config, workdir, binary):
        self.port = free_port()
        self.config = Path(workdir) / "inkharness.toml"
        self.config.write_text(config.replace("{port}", str(self.port)))
        self.log_path = Path(workdir) / "inkharness.log"
        self.log = open(self.log_path, "w")
        # INKHARNESS_HOME: no global config folder to layer under this one.
        env = dict(os.environ, NO_COLOR="1", EVAL_HA_TOKEN="eval", RUST_LOG="info", INKHARNESS_HOME=str(workdir))
        # The working directory is this folder, so inkharness reads the key from .env here (and
        # the skills in skills/, as the board's daemon does).
        self.proc = subprocess.Popen([binary, "serve", "--config", str(self.config), "--http", ""], cwd=HERE,
                                     stdout=self.log, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + 30
        while "inkharness is up" not in self.log_path.read_text():
            if self.proc.poll() is not None or time.time() > deadline:
                raise RuntimeError("inkharness didn't start:\n" + self.log_path.read_text()[-2000:])
            time.sleep(0.1)

    def turns(self):
        return [l for l in self.log_path.read_text().splitlines() if "turn: " in l or "turn failed" in l]

    def stop(self):
        self.proc.terminate()
        self.proc.wait(10)
        self.log.close()


def run_command(daemon, client, wav, out):
    before = len(daemon.turns())
    FakeHome.calls = []
    started = time.time()
    proc = subprocess.run([client, "--addr", f"127.0.0.1:{daemon.port}", "--wav", str(wav), "--out", str(out), "--fast"],
                          capture_output=True, text=True, timeout=90)
    wall_ms = round((time.time() - started) * 1000)
    deadline = time.time() + 5
    while len(daemon.turns()) <= before and time.time() < deadline:
        time.sleep(0.05)
    lines = daemon.turns()[before:]
    return lines, list(FakeHome.calls), proc, wall_ms


def generation(api_key, gen_id):
    """OpenRouter's record of one request: cost and tokens. It can take a few seconds to appear."""
    for attempt in range(15):
        req = urllib.request.Request(f"{OPENROUTER}/generation?id={gen_id}",
                                     headers={"Authorization": f"Bearer {api_key}", "User-Agent": "inkharness-eval"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.load(r)["data"]
        except urllib.error.HTTPError as e:
            if e.code != 404:
                return {"error": f"HTTP {e.code}"}
        time.sleep(1 + attempt * 0.5)
    return {"error": "not found"}


def unquote(debug):
    """A string as Rust's Debug wrote it."""
    try:
        return json.loads(debug)
    except ValueError:
        return debug[1:-1]


def parse_turn(line):
    m = TURN.search(line)
    row = {k: field(line, k) for k in ("heard_secs", "stt_ms", "first_token_ms", "first_audio_ms", "total_ms", "reply_secs")}
    row["heard"] = unquote(m.group("heard")) if m else None
    row["reply"] = unquote(m.group("reply")) if m else None
    req = re.search(r"\brequests=(\S*)", line)
    row["requests"] = [r.split(":", 1) for r in req.group(1).split(",") if r] if req else []
    return row


def price(rows, api_key):
    """Fills in each turn's cost and tokens per stage from the generation endpoint."""
    for row in rows:
        stages = {s: {"cost": 0.0, "requests": 0, "prompt_tokens": 0, "completion_tokens": 0} for s in ("stt", "llm", "tts")}
        unpriced = 0
        for stage, gen_id in row.get("requests", []):
            g = generation(api_key, gen_id) if gen_id != "?" else {"error": "no id"}
            st = stages.setdefault(stage, {"cost": 0.0, "requests": 0, "prompt_tokens": 0, "completion_tokens": 0})
            st["requests"] += 1
            if "error" in g or g.get("usage") is None:
                unpriced += 1
                continue
            st["cost"] += g["usage"]
            st["prompt_tokens"] += g.get("native_tokens_prompt") or 0
            st["completion_tokens"] += g.get("native_tokens_completion") or 0
        row["stages"] = stages
        row["cost"] = sum(s["cost"] for s in stages.values())
        row["unpriced"] = unpriced


def summarize(name, shape, label, rows):
    ok = [r for r in rows if r.get("total_ms") is not None]

    def med(k):  # over the turns that got that far (0: never did, e.g. no audio)
        values = [r[k] for r in ok if r.get(k)]
        return round(statistics.median(values)) if values else None

    mean = lambda f: statistics.mean([f(r) for r in ok]) if ok else 0.0
    return {
        "pipeline": name, "shape": shape, "label": label, "turns": len(rows), "answered": len(ok),
        "correct": sum(1 for r in rows if r.get("correct")),
        "first_audio_ms_median": med("first_audio_ms"), "total_ms_median": med("total_ms"), "stt_ms_median": med("stt_ms"),
        "silent_replies": sum(1 for r in ok if not r.get("first_audio_ms")),
        "first_audio_ms_max": max((r["first_audio_ms"] for r in ok if r.get("first_audio_ms")), default=None),
        "cost_per_turn": mean(lambda r: r.get("cost", 0.0)),
        "cost_stt": mean(lambda r: r["stages"]["stt"]["cost"]), "cost_llm": mean(lambda r: r["stages"]["llm"]["cost"]),
        "cost_tts": mean(lambda r: r["stages"]["tts"]["cost"]),
        "llm_requests": mean(lambda r: r["stages"]["llm"]["requests"]),
        "llm_prompt_tokens": mean(lambda r: r["stages"]["llm"]["prompt_tokens"]),
        "llm_completion_tokens": mean(lambda r: r["stages"]["llm"]["completion_tokens"]),
        "tts_chars": mean(lambda r: len(r.get("reply") or "") if r["stages"]["tts"]["requests"] else 0),
        "unpriced": sum(r.get("unpriced", 0) for r in rows),
    }


def table(summaries):
    head = ("| Pipeline | Stages | Correct | Transcript | First audio | Turn | $/turn | STT | Model | TTS | Model requests | Model tokens in/out |\n"
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    rows = [head]
    for s in summaries:
        stt = f"{s['stt_ms_median']} ms" if s.get("stt_ms_median") else "–"
        rows.append(f"| {s['label']} | {s['shape']} | {s['correct']}/{s['turns']} | {stt} | {s['first_audio_ms_median']} ms | "
                    f"{s['total_ms_median']} ms | ${s['cost_per_turn']:.5f} | ${s['cost_stt']:.6f} | ${s['cost_llm']:.5f} | "
                    f"${s['cost_tts']:.5f} | {s['llm_requests']:.1f} | {s['llm_prompt_tokens']:.0f} / {s['llm_completion_tokens']:.0f} |")
    return "\n".join(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated pipelines: " + ", ".join(PIPELINES))
    ap.add_argument("--repeat", type=int, default=1, help="times to send each command")
    ap.add_argument("--profile", default="debug", help="the cargo profile inkharness was built with")
    ap.add_argument("--out", default="pipeline-results.json")
    ap.add_argument("--summarize", nargs="+", metavar="RESULTS", help="only print the table of earlier runs' result files")
    args = ap.parse_args()
    if args.summarize:
        summaries = []
        for path in args.summarize:
            for name, p in json.loads(Path(path).read_text())["pipelines"].items():
                summaries.append(summarize(name, p["shape"], p["label"], p["turns"]))
        print(table(summaries))
        return
    names = args.only.split(",") if args.only else list(PIPELINES)
    target = HERE / "target" / args.profile
    inkharness, client = target / "inkharness", target / "voice-client"
    for b in (inkharness, client):
        if not b.exists():
            sys.exit(f"{b} is missing: cargo build -p inkharness -p voice-client")
    api_key = key()

    home = ThreadingHTTPServer(("127.0.0.1", 0), FakeHome)
    threading.Thread(target=home.serve_forever, daemon=True).start()
    common = f"""listen = "127.0.0.1:{{port}}"
token = ""

[home_assistant]
url = "http://127.0.0.1:{home.server_port}"
token_env = "EVAL_HA_TOKEN"
play_service = "music_assistant.play_media"

[cache]
tts_entries = 0
"""
    results, summaries = {}, []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        wavs = {}
        for cid, text, _ in COMMANDS + [("warmup", WARMUP, None)]:
            wavs[cid] = tmp / f"{cid}.wav"
            speak(text, wavs[cid])
        for name in names:
            shape, label, stages = PIPELINES[name]
            work = tmp / name
            work.mkdir()
            print(f"\n== {name}: {label}", flush=True)
            daemon = Daemon(common + stages, work, str(inkharness))
            rows = []
            try:
                run_command(daemon, str(client), wavs["warmup"], work / "warmup.wav")
                for rep in range(args.repeat):
                    for cid, text, check in COMMANDS:
                        lines, calls, proc, wall_ms = run_command(daemon, str(client), wavs[cid], work / f"{cid}-{rep}.wav")
                        line = next((l for l in lines if "turn: " in l), None)
                        row = parse_turn(line) if line else {"error": (lines or [proc.stdout[-300:] + proc.stderr[-300:]])[-1]}
                        row.update(command=cid, said=text, calls=calls, client_wall_ms=wall_ms)
                        row["correct"] = bool(line) and check(calls, row.get("reply") or "")
                        mark = "ok " if row["correct"] else "BAD"
                        print(f"  {mark} {cid:9} first audio {row.get('first_audio_ms')} ms, total {row.get('total_ms')} ms: "
                              f"{(row.get('reply') or row.get('error') or '')[:90]!r} {[(d, s) for d, s, _ in calls]}", flush=True)
                        rows.append(row)
            finally:
                daemon.stop()
            price(rows, api_key)
            results[name] = {"shape": shape, "label": label, "config": common + stages, "turns": rows}
            summaries.append(summarize(name, shape, label, rows))
            print(f"  ${summaries[-1]['cost_per_turn']:.5f}/turn", flush=True)
    home.shutdown()
    Path(args.out).write_text(json.dumps({"summaries": summaries, "pipelines": results}, indent=1))
    print("\n" + table(summaries))
    print(f"\nPer turn: {args.out}")


if __name__ == "__main__":
    main()
