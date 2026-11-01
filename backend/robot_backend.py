#!/usr/bin/env python3
"""China-market conversational robot v1 backend gateway (ECS).

Loop: board POST /voice (WAV or raw 16kHz mono s16le) -> DashScope ASR
-> Qwen chat -> localhost Piper (127.0.0.1:5002) -> PCM audio back.
The board NEVER talks to Piper or to DashScope directly.

Security (non-negotiable, from the v1 draft):
  * DASHSCOPE_API_KEY comes ONLY from the environment (systemd
    EnvironmentFile /etc/robot-backend.env, mode 0600, created by the
    server owner). It is never written into this file, firmware, or chat.
  * Piper stays on 127.0.0.1:5002, security group does NOT open 5002.
  * This backend binds 0.0.0.0:<BACKEND_PORT> (default 8080) and is the
    ONLY public entry point. A device token (X-Device-Token) is checked
    against ROBOT_DEVICE_TOKEN when that env var is set; when unset the
    server still runs but logs a warning (dev only, never for a public ECS).
  * Security group must open BACKEND_PORT for the board's network; that
    step is done in the Alibaba console by the server owner, not by code.

Voice-confirmation rule (user, 2026-10-24): speech recognition often
mishears, so the robot first repeats back the understood question and
waits for confirmation before answering. CONFIRM_MODE=ask (default)
implements exactly that for a single device:
  1st utterance -> audio "你刚才问的是：<transcript>，对吗？"
  next utterance "对/没错/是的" -> Qwen answer to the stored question
  next utterance "不对/不是" -> clears the stored question, asks to repeat
  CONFIRM_MODE=direct skips this (loop-test bypass only).

Audio contract (from the verified Piper server):
  Piper returns raw PCM s32le 48kHz stereo, frame count % 3 == 0
  (byte length % 24 == 0). The board plays 16kHz mono s16le, so the
  backend down-converts: left channel, every 3rd frame, high 16 bits
  of each s32 sample (= value >> 16, full scale preserved). That is
  32 KB/s instead of 384 KB/s, which is what the ~55 KB/s board<->ECS
  link can actually carry.

Segmented answers (user approved 2026-10-25):
  The reply is split into segments of at most 45 characters (on
  sentence marks, then commas). POST /voice returns ONLY segment 0
  and the headers X-Segments-Total, X-Segment-Index (0), X-Session-Id
  and X-Audio-Format: s16le-16k-mono. The board then fetches the
  remaining segments with
      GET /voice_segment?session=<id>&index=<i>
  (same X-Device-Token auth), each response carrying the same headers.
  Sessions live in memory with a 180 s TTL and are dropped once the
  last segment has been served. A one-segment reply behaves exactly
  like the old single-response flow.

Endpoints:
  POST /voice            audio body (WAV, or raw s16le mono, default 16kHz)
  GET  /voice_segment    session=<id>&index=<i> -> one reply segment
  GET  /health           {"ok": true} -- no auth, for smoke tests only

Env: DASHSCOPE_API_KEY (required for ASR/chat), QWEN_CHAT_MODEL
(default qwen-plus), QWEN_ASR_MODEL (default qwen3-asr-flash),
PIPER_URL (default http://127.0.0.1:5002/tts), BACKEND_PORT (8080),
BACKEND_HOST (0.0.0.0), ROBOT_DEVICE_TOKEN, CONFIRM_MODE (ask|direct),
TTS_PROVIDER (piper|kokoro, default piper), KOKORO_URL (default
http://127.0.0.1:5003/tts).

TTS provider (user decision 2026-10-27, blind-test winner):
Kokoro-82M-v1.1-zh (voice zf_001) beat MeloTTS on phrasing and is
faster than real time, so it is the deployed provider. Piper stays
installed, running and untouched as the one-click fallback: set
TTS_PROVIDER=piper in /etc/robot-backend.env and restart
robot-backend -- no code change, no Piper change.

Stdlib only -- no pip dependencies, so the ECS venv is not required.
"""
import json
import os
import sys
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DASHSCOPE_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1"
PIPER_URL = os.environ.get("PIPER_URL", "http://127.0.0.1:5002/tts")
TTS_PROVIDER = os.environ.get("TTS_PROVIDER", "piper").lower()
KOKORO_URL = os.environ.get("KOKORO_URL", "http://127.0.0.1:5003/tts")
CHAT_MODEL = os.environ.get("QWEN_CHAT_MODEL", "qwen-plus")
ASR_MODEL = os.environ.get("QWEN_ASR_MODEL", "qwen3-asr-flash")
# Chat provider (v1 supports two, chosen by env; ASR is always DashScope,
# because an Anthropic-compatible chat API has no speech-to-text endpoint):
#   LLM_FORMAT=openai   (default, DashScope-compatible): LLM_BASE_URL +
#                       /chat/completions, Bearer key, LLM_MODEL or QWEN_CHAT_MODEL
#   LLM_FORMAT=anthropic: LLM_BASE_URL + /messages, x-api-key header,
#                       anthropic-version 2023-06-01, LLM_MODEL
#                       (e.g. base https://gpt-agent.cc/v1, model claude-sonnet-4-6)
LLM_FORMAT = os.environ.get("LLM_FORMAT", "openai").lower()
LLM_BASE_URL = os.environ.get("LLM_BASE_URL", DASHSCOPE_BASE).rstrip("/")
LLM_MODEL = os.environ.get("LLM_MODEL", CHAT_MODEL)
DEVICE_TOKEN = os.environ.get("ROBOT_DEVICE_TOKEN", "")
CONFIRM_MODE = os.environ.get("CONFIRM_MODE", "ask").lower()
# Reply length cap. Raised 90 -> 200 on 2026-10-25 (user approved the
# segmented-answer protocol): the reply is now cut into <=45 char
# segments, each synthesised and sent on its own, so the total text may
# be longer. What actually broke the board before was NOT the char
# count but the response SIZE: the board<->ECS link measures ~55 KB/s
# (board -> ECS upload 51.0 KB/s, ECS -> board download 54.9 KB/s),
# so 48k s32le stereo at 384 KB/s could never keep up (46.75 s to
# deliver 2.6 MB). Segments are therefore down-converted to 16k s16le
# mono at 32 KB/s, which the link can actually carry.
MAX_REPLY_CHARS = 400
# Max characters per segment (split on sentence marks, then commas).
MAX_SEGMENT_CHARS = 45
# Segmented-answer sessions live in memory only; the board fetches the
# remaining segments over the next couple of minutes.
SEG_SESSION_TTL = 180.0
# Debug bypass (user, 2026-10-25): when set, /voice NEVER calls ASR
# (zero Bailian ASR quota burned) and instead answers this fixed
# question directly, skipping the confirmation flow. The uploaded wav
# is still saved to /tmp/last-voice-debug.wav so the recording itself
# can be listened to and checked. Unset/empty = normal ASR path.
DEBUG_FIXED_QUESTION = os.environ.get("ROBOT_DEBUG_FIXED_QUESTION", "").strip()

# Single-device v1 state (pending confirmed question). Multi-device is post-v1.
_pending_question = {"text": None}

SYSTEM_PROMPT = (
    "你是无所不知的智能语音助手。用简短口语中文回答，"
    "不要用 Markdown、不要列序号超过 3 条，回答不超过 400 字，"
    "语气亲切、清楚、慢一点的感觉。只回答问题，不要反问。"
)

CONFIRM_WORDS = ("对", "没错", "是的", "对的", "嗯对")
DENY_WORDS = ("不对", "不是", "错了", "不是这个")


def _api_key():
    # Chat key: LLM_API_KEY wins, else the legacy DASHSCOPE_API_KEY.
    key = os.environ.get("LLM_API_KEY", "") or os.environ.get("DASHSCOPE_API_KEY", "")
    if not key:
        raise RuntimeError("LLM_API_KEY (or DASHSCOPE_API_KEY) is not set")
    return key


def _asr_key():
    key = os.environ.get("DASHSCOPE_API_KEY", "")
    if not key:
        raise RuntimeError("ASR needs DASHSCOPE_API_KEY (Bailian); "
                           "the Anthropic chat key cannot transcribe audio")
    return key


def _post_json(url, payload, timeout=60):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + _api_key(),
                 "Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def asr_transcribe(wav_bytes):
    """DashScope NATIVE multimodal-generation ASR (verified 2026-10-25).

    The compatible-mode /audio/transcriptions endpoint returns an empty
    404 for this account/model (verified on ECS); the native endpoint
    accepts qwen-audio-3.0-asr-flash with the audio as a data URI.
    parameters.format/sample_rate MUST be in the JSON body (in the URL
    query they are ignored and the call fails UNSUPPORTED_FORMAT).
    Response text path: output.output.sentence[0].text (output.text as fallback).
    """
    import base64
    payload = {
        "model": ASR_MODEL,
        "input": {"messages": [{"role": "user", "content": [
            {"audio": "data:audio/wav;base64," +
             base64.b64encode(wav_bytes).decode()},
            {"text": "请把这段语音转写成文字"}]}]},
        "parameters": {"format": "wav", "sample_rate": 16000},
    }
    req = urllib.request.Request(
        "https://dashscope.aliyuncs.com/api/v1/services/aigc/"
        "multimodal-generation/generation",
        data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + _asr_key(),
                 "Content-Type": "application/json",
                 "X-DashScope-SSE": "disable"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            data = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        # Surface DashScope's own error body in the backend log so a
        # board retry can be diagnosed by its code/message, not just 400.
        # Also keep the exact bytes that failed, for offline inspection
        # (header, silence level, re-encode test) — overwritten each time.
        try:
            with open("/tmp/last-voice-failed.wav", "wb") as f:
                f.write(wav_bytes)
        except Exception:
            pass
        raise RuntimeError("ASR HTTP %d: %s" % (
            e.code, e.read().decode(errors="replace")[:300]))
    out = data.get("output", {})
    # Native shape: output.output.sentence[0].text
    inner = out.get("output") or {}
    sentences = inner.get("sentence") or []
    # Non-streaming reply: sentence is a single dict, not a list
    # (agent fix on ECS 2026-10-25; list assumption raised KeyError(0)).
    if isinstance(sentences, dict):
        sentences = [sentences]
    if sentences:
        return (sentences[0].get("text") or "").strip()
    if out.get("text"):
        return out["text"].strip()
    choices = out.get("choices") or []
    if choices:
        content = choices[0].get("message", {}).get("content") or []
        texts = [c.get("text", "") for c in content if c.get("text")]
        if texts:
            return "".join(texts).strip()
    raise RuntimeError("ASR response has no text: %s" % json.dumps(data)[:300])


def _anthropic_reply(question):
    req = urllib.request.Request(
        LLM_BASE_URL + "/messages",
        data=json.dumps({"model": LLM_MODEL, "max_tokens": 1500,
                         "system": SYSTEM_PROMPT,
                         "messages": [{"role": "user", "content": question}]}).encode(),
        headers={"x-api-key": _api_key(), "anthropic-version": "2023-06-01",
                 "Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        data = json.loads(r.read().decode())
    parts = [b.get("text", "") for b in data.get("content", [])
             if b.get("type") == "text"]
    return "".join(parts).strip()


def qwen_reply(question):
    if LLM_FORMAT == "anthropic":
        text = _anthropic_reply(question)
    else:
        data = _post_json(LLM_BASE_URL + "/chat/completions", {
            "model": LLM_MODEL,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                         {"role": "user", "content": question}],
            "temperature": 0.7,
            # Measured 2026-10-25 19:03 on the compatible-mode endpoint:
            # qwen3.7-plus with thinking on costs 8.44 s / 544 tokens
            # (446 of them reasoning); with enable_thinking=false it is
            # 1.24 s / 92 tokens. The OpenAI-compatible gateway accepts
            # and honours the flag, so the reply quality holds while the
            # voice round trip drops from ~9.4 s to ~2.2 s. Only the
            # openai-format branch is affected; anthropic is untouched.
            "enable_thinking": False,
        }, timeout=120)
        text = data["choices"][0]["message"]["content"].strip()
    if len(text) > MAX_REPLY_CHARS:  # truncate at a sentence boundary
        cut = text[:MAX_REPLY_CHARS]
        for sep in ("。", "！", "？", "\n"):
            i = cut.rfind(sep)
            if i > 20:
                cut = cut[:i + 1]
                break
        text = cut
    return text


def piper_synth(text):
    req = urllib.request.Request(
        PIPER_URL, data=json.dumps({"text": text}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=180) as r:
        data = r.read()
    if not data or len(data) % 24 != 0:
        raise RuntimeError("Piper audio failed contract: len=%d" % len(data))
    return data


def to_wav(raw_s16, sample_rate=16000):
    """Wrap raw s16le mono in a minimal WAV header (for the ASR API)."""
    import struct
    data_len = len(raw_s16)
    hdr = (b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE" +
           b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sample_rate,
                                 sample_rate * 2, 2, 16) +
           b"data" + struct.pack("<I", data_len))
    return hdr + raw_s16


def split_segments(text, max_len=MAX_SEGMENT_CHARS):
    """Cut a reply into segments of at most max_len characters.

    Primary split on sentence marks (。？！；), keeping the mark with its
    sentence so each segment plays as a complete breath group. A segment
    that is still too long is cut again on the nearest comma (，、：,)
    at or before max_len; if there is no comma at all it is hard-cut.
    """
    if not text:
        return []
    sentences = []
    buf = ""
    for ch in text:
        buf += ch
        if ch in "。？！；":
            sentences.append(buf)
            buf = ""
    if buf:
        sentences.append(buf)
    segments = []
    for s in sentences:
        s = s.strip()
        while len(s) > max_len:
            cut = 0
            for i in range(min(max_len, len(s) - 1), 0, -1):
                if s[i] in "，、：,":
                    cut = i + 1
                    break
            if not cut:
                cut = max_len
            segments.append(s[:cut])
            s = s[cut:].strip()
        if s:
            segments.append(s)
    return segments


def to_16k_mono_s16(pcm_s32_stereo_48k):
    """Piper PCM (s32le 48 kHz stereo) -> raw s16le 16 kHz mono.

    Exactly the transform the board expects:
      * left channel only  -- one 48k frame is 8 bytes (2 ch x 4 bytes),
        the left sample lives in bytes [0:4] and its HIGH 16 bits are
        bytes [2:4], so pcm[2::8] is the left channel already scaled
        from s32 to s16 (value >> 16, full scale preserved).
      * every 3rd frame  -- 48 kHz -> 16 kHz.
    The result is always an even number of bytes (whole s16 samples).
    """
    # One 48k frame is 8 bytes (2 ch x 4 bytes). The LEFT sample is
    # bytes [0:4] of the frame, little-endian s32, so its high 16 bits
    # are bytes [2] (low) and [3] (high) of that frame -- together the
    # s16 pattern (value >> 16, full scale preserved).
    lo = pcm_s32_stereo_48k[2::8]      # low  byte of each frame's high16
    hi = pcm_s32_stereo_48k[3::8]      # high byte of each frame's high16
    # Decimate 48 kHz -> 16 kHz: keep every 3rd frame.
    lo = lo[0::3]
    hi = hi[0::3]
    out = bytearray(len(lo) * 2)
    out[0::2] = lo
    out[1::2] = hi
    return bytes(out)


def kokoro_synth(text):
    """One text -> Kokoro service -> raw s16le 16 kHz mono.

    The Kokoro service already returns the board's production format
    (16 kHz mono s16le, produced from its 24 kHz output), so unlike
    the Piper path there is no down-conversion step here.
    """
    req = urllib.request.Request(
        KOKORO_URL, data=json.dumps({"text": text}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=180) as r:
        data = r.read()
    if not data or len(data) % 2 != 0:
        raise RuntimeError("Kokoro audio failed contract: len=%d" % len(data))
    return data


def synth_segment_audio(segment_text):
    """One segment -> TTS provider -> 16 kHz mono s16le bytes."""
    if TTS_PROVIDER == "kokoro":
        return kokoro_synth(segment_text)
    return to_16k_mono_s16(piper_synth(segment_text))


# Segmented-answer sessions. sid -> {"segments": [str], "ts": float}
# Only the segment TEXTS are stored; the audio for segment i is
# synthesised on demand when the board asks for it. That keeps the
# first response fast (one Piper call instead of N) and the memory
# footprint tiny.
_seg_sessions = {}


def _seg_prune(now=None):
    now = now if now is not None else time.time()
    dead = [sid for sid, v in _seg_sessions.items()
            if now - v["ts"] > SEG_SESSION_TTL]
    for sid in dead:
        del _seg_sessions[sid]
    return dead


def _seg_new(segments):
    _seg_prune()
    sid = os.urandom(8).hex()
    _seg_sessions[sid] = {"segments": segments, "ts": time.time()}
    return sid


def _seg_get(sid, index):
    """Return (audio_bytes, total) or (None, None) if unknown/expired."""
    _seg_prune()
    v = _seg_sessions.get(sid)
    if not v:
        return None, None
    v["ts"] = time.time()  # sliding TTL
    if index < 0 or index >= len(v["segments"]):
        return None, None
    audio = synth_segment_audio(v["segments"][index])
    total = len(v["segments"])
    if index == total - 1:
        # last segment served -> drop the session
        del _seg_sessions[sid]
    return audio, total


def handle_voice(body):
    """Returns (reply_text, transcript, segment_texts).

    The AUDIO is no longer produced here: each segment is synthesised
    on demand (see _seg_get), so the first response only waits for one
    Piper call instead of one per segment.
    """
    wav = body if body[:4] == b"RIFF" else to_wav(body)
    if DEBUG_FIXED_QUESTION:
        # ASR bypass: prove record -> upload -> download -> playback
        # and the LLM + Piper channel without spending ASR quota.
        try:
            with open("/tmp/last-voice-debug.wav", "wb") as f:
                f.write(wav)
        except Exception:
            pass
        text = qwen_reply(DEBUG_FIXED_QUESTION)
        return text, DEBUG_FIXED_QUESTION, split_segments(text)
    transcript = asr_transcribe(wav)
    if not transcript:
        text = "没听清楚，能再说一遍吗？"
        return text, transcript, split_segments(text)
    if CONFIRM_MODE == "ask":
        if _pending_question["text"] is None:
            _pending_question["text"] = transcript
            text = "你刚才问的是：%s，对吗？" % transcript
            return text, transcript, split_segments(text)
        if transcript.startswith(CONFIRM_WORDS) or transcript in CONFIRM_WORDS:
            q = _pending_question["text"]
            _pending_question["text"] = None
            text = qwen_reply(q)
            return text, transcript, split_segments(text)
        if transcript.startswith(DENY_WORDS):
            _pending_question["text"] = None
            text = "好的，那请再说一遍。"
            return text, transcript, split_segments(text)
        _pending_question["text"] = transcript  # new question, confirm again
        text = "你刚才问的是：%s，对吗？" % transcript
        return text, transcript, split_segments(text)
    text = qwen_reply(transcript)
    return text, transcript, split_segments(text)


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1 (not the BaseHTTPRequestHandler default 1.0): the board's
    # esp_http_client only finalises the download on a clean transaction
    # finish; with 1.0 + connection close it never reported the audio
    # length, so the board always failed its play_len check (never
    # purple, even for 200 responses). All responses below carry an
    # explicit Content-Length, as 1.1 keep-alive requires.
    protocol_version = "HTTP/1.1"

    def _auth_ok(self):
        if DEVICE_TOKEN and self.headers.get("X-Device-Token") != DEVICE_TOKEN:
            self.send_error(403, "bad device token")
            return False
        return True

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            self._send_json(200, {"ok": True})
            return
        if parsed.path == "/voice_segment":
            if not self._auth_ok():
                return
            q = urllib.parse.parse_qs(parsed.query)
            sid = (q.get("session") or [""])[0]
            try:
                index = int((q.get("index") or ["-1"])[0])
            except ValueError:
                index = -1
            if not sid:
                self.send_error(400, "missing session")
                return
            try:
                audio, total = _seg_get(sid, index)
            except Exception as e:
                print("voice_segment error:", e, file=sys.stderr)
                self.send_error(502, str(e))
                return
            if audio is None:
                self.send_error(404, "unknown or expired session")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("X-Audio-Format", "s16le-16k-mono")
            self.send_header("X-Segments-Total", str(total))
            self.send_header("X-Segment-Index", str(index))
            self.send_header("X-Session-Id", sid)
            self.send_header("Content-Length", str(len(audio)))
            self.end_headers()
            self.wfile.write(audio)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/voice":
            self.send_error(404)
            return
        if not self._auth_ok():
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
            # 3 MB cap: firmware records max 60 s at 16 kHz mono = 1.9 MB;
            # base64 inflates the native-ASR payload ~1.33x, so keep margin
            # under DashScope's request-body limit.
            if length <= 0 or length > 3 * 1024 * 1024:
                self.send_error(400, "bad audio length")
                return
            body = self.rfile.read(length)
            reply_text, transcript, seg_texts = handle_voice(body)
            if not seg_texts:
                seg_texts = [reply_text or "没听清楚"]
            sid = _seg_new(seg_texts)
            audio = synth_segment_audio(seg_texts[0])
            total = len(seg_texts)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("X-Audio-Format", "s16le-16k-mono")
            self.send_header("X-Segments-Total", str(total))
            self.send_header("X-Segment-Index", "0")
            self.send_header("X-Session-Id", sid)
            self.send_header("X-Transcript", urllib.parse.quote(transcript))
            self.send_header("X-Reply-Text", urllib.parse.quote(reply_text))
            self.send_header("Content-Length", str(len(audio)))
            self.end_headers()
            self.wfile.write(audio)
        except Exception as e:
            print("voice error:", e, file=sys.stderr)
            self.send_error(502, str(e))

    def _send_json(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        print(*a, file=sys.stderr)


if __name__ == "__main__":
    if not DEVICE_TOKEN:
        print("WARNING: ROBOT_DEVICE_TOKEN unset -- dev mode only", file=sys.stderr)
    host = os.environ.get("BACKEND_HOST", "0.0.0.0")
    port = int(os.environ.get("BACKEND_PORT", "8080"))
    print("robot backend on %s:%d confirm=%s llm=%s:%s asr=%s" %
          (host, port, CONFIRM_MODE, LLM_FORMAT, LLM_MODEL, ASR_MODEL), file=sys.stderr)
    ThreadingHTTPServer((host, port), Handler).serve_forever()
