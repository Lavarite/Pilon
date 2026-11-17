#!/usr/bin/env python3
"""Private LiteRT-LM inference transport. Codex owns tools and orchestration.

The SDK's stock server discards its Conversation after each request. Retain one
conversation here and feed only new messages while its history and settings
match. SDK protocol/metadata helpers are pinned by litert-requirements.lock.
"""

import argparse
import copy
import hmac
import json
import os
import select
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import litert_lm
from litert_lm_builder.litertlm_builder import TfLiteModelType
from litert_lm_cli import model as model_info
from litert_lm_cli.commands import openai_handler as protocol
from litert_lm.interfaces import ConstrainedDecodingConfig
from litert_lm._ffi import LiteRtLmConstraintProviderType


def canonical(message):
    """Compare semantic history despite transport IDs and text part grouping."""
    result = {"role": "assistant" if message.get("role") == "model" else message.get("role")}
    content = message.get("content") or []
    if isinstance(content, str):
        content = [{"type": "text", "text": content}]
    parts = []
    for part in content:
        if part.get("type") == "text" and parts and parts[-1].get("type") == "text":
            parts[-1]["text"] += part.get("text", "")
        elif part.get("type") != "text" or part.get("text"):
            parts.append(copy.deepcopy(part))
    for part in parts:
        # Responses/Chat transports serialize tool results as strings. Restore
        # structured results before the SDK renders Gemma's tool-response
        # syntax, rather than quoting JSON (and escaping its output) again.
        if part.get("type") == "tool_response" and isinstance(part.get("response"), str):
            try:
                structured = json.loads(part["response"])
                if isinstance(structured, (dict, list)):
                    part["response"] = structured
            except ValueError:
                pass
    if parts:
        result["content"] = parts
    if message.get("tool_calls"):
        result["tool_calls"] = [{"type": "function", "function": {
            "name": call["function"]["name"], "arguments": call["function"]["arguments"]}}
            for call in message["tool_calls"]]
    if message.get("channels"):
        result["channels"] = copy.deepcopy(message["channels"])
    return result


def joined_prompt(messages):
    # Several results from a parallel tool call belong to the same tool turn.
    if not messages or any(m["role"] != messages[0]["role"] for m in messages):
        return None
    if messages[0]["role"] not in ("user", "tool"):
        return None
    return {"role": messages[0]["role"], "content": [p for m in messages for p in m.get("content", [])]}


class Session:
    def __init__(self, engine):
        self.engine = engine
        self.conversation = None
        self.history = []
        self.settings = None

    def close(self):
        if self.conversation is not None:
            self.conversation.close()
        self.conversation = None
        self.history = []
        self.settings = None

    def prepare(self, body):
        messages = body.get("messages")
        if not isinstance(messages, list) or not messages:
            raise ValueError("A nonempty messages array is required.")
        names = protocol._build_name_by_tool_call_id_map(messages)
        history = []
        for message in messages:
            translated = protocol._translate_openai_message(message, names)
            if message.get("role") == "assistant" and message.get("reasoning_content"):
                translated["channels"] = {"thought": message["reasoning_content"]}
            history.append(canonical(translated))
        # Gemma requires thoughts within a tool sequence, but not thoughts from
        # completed user turns. A changed prefix correctly rebuilds the cache.
        last_user = max((i for i, m in enumerate(history) if m["role"] == "user"), default=-1)
        for message in history[:last_user]:
            message.pop("channels", None)
        settings = {k: body.get(k) for k in ("tools", "reasoning_effort", "temperature", "top_p", "top_k", "seed")}
        prompt = None
        if self.conversation is not None and settings == self.settings and history[:len(self.history)] == self.history:
            prompt = joined_prompt(history[len(self.history):])
        reused = prompt is not None
        cached = self.conversation.token_count if reused else 0
        if not reused:
            self.close()
            # Prefill old turns once when starting or switching a conversation.
            first = len(history) - 1
            while first > 0 and history[first - 1]["role"] == history[-1]["role"]:
                first -= 1
            prompt = joined_prompt(history[first:])
            if prompt is None:
                raise ValueError("The request must end with a user or tool message.")
            self.conversation = self.engine.create_conversation(
                messages=history[:first],
                tools=[protocol._ProxyTool(t) for t in body.get("tools", [])] or None,
                automatic_tool_calling=False,
                thinking_config=protocol._parse_thinking_config(body),
                sampler_config=protocol._parse_sampler_config(body),
                # Native discovery supplies typed operation schemas. Keep
                # constrained decoding enabled as the advertised tool set grows.
                constrained_decoding_config=ConstrainedDecodingConfig(
                    enable=True, provider=LiteRtLmConstraintProviderType.LL_GUIDANCE),
                filter_channel_content_from_kv_cache=False,
            )
            self.settings = copy.deepcopy(settings)
        return prompt, history, cached


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def authorized(self):
        return hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + self.server.api_key)

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.authorized():
            self.reply(401, {"error": {"message": "Unauthorized"}})
        elif self.path == "/health":
            self.reply(200, {"status": "ok"})
        else:
            self.reply(404, {"error": {"message": "Unknown endpoint"}})

    def do_POST(self):
        if not self.authorized():
            self.reply(401, {"error": {"message": "Unauthorized"}})
            return
        if self.path != "/v1/chat/completions":
            self.reply(404, {"error": {"message": "Unknown endpoint"}})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 96 * 1024 * 1024:
                raise ValueError("Invalid request size")
            body = json.loads(self.rfile.read(size))
            if not isinstance(body, dict) or body.get("stream") is not True:
                raise ValueError("Streaming chat completion required")
        except (ValueError, TypeError) as error:
            self.reply(400, {"error": {"message": str(error)}})
            return
        # One model slot, matching Pilon's single active assistant turn.
        with self.server.inference_lock:
            self.generate(body)

    def generate(self, body):
        session = self.server.session
        stop = threading.Event()
        disconnected = threading.Event()
        monitor = None
        chunks = None
        started = False
        try:
            prompt, history, cached = session.prepare(body)
            conv = session.conversation

            def watch_disconnect():
                while not stop.wait(0.1):
                    try:
                        readable, _, _ = select.select([self.connection], [], [], 0)
                        if readable and not self.connection.recv(1, socket.MSG_PEEK):
                            disconnected.set()
                            conv.cancel_process()
                            return
                    except OSError:
                        disconnected.set()
                        conv.cancel_process()
                        return

            monitor = threading.Thread(target=watch_disconnect, daemon=True)
            monitor.start()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            started = True
            response_id = "chatcmpl_" + uuid.uuid4().hex

            def event(delta=None, finish=None, usage=None):
                chunk = {"id": response_id, "object": "chat.completion.chunk", "created": int(time.time()),
                         "model": "pilon-local", "choices": [] if usage else [
                             {"index": 0, "delta": delta or {}, "finish_reason": finish}]}
                if usage is not None:
                    chunk["usage"] = usage
                self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
                self.wfile.flush()

            event({"role": "assistant"})
            response = {"role": "assistant", "content": [], "tool_calls": []}
            chunks = conv.send_message_async(prompt, max_output_tokens=body.get("max_completion_tokens"))
            for chunk in chunks:
                if disconnected.is_set():
                    raise InterruptedError("Request cancelled")
                text = "".join(p.get("text", "") for p in chunk.get("content", []) if p.get("type") == "text")
                if text:
                    response["content"].append({"type": "text", "text": text})
                    event({"content": text})
                thought = "".join(chunk.get("channels", {}).values())
                for channel, value in chunk.get("channels", {}).items():
                    channels = response.setdefault("channels", {})
                    channels[channel] = channels.get(channel, "") + value
                if thought and body.get("reasoning_effort") != "none":
                    event({"reasoning_content": thought})
                for call in chunk.get("tool_calls", []):
                    index = len(response["tool_calls"])
                    response["tool_calls"].append(call)
                    event({"tool_calls": [{"index": index, "id": "call_" + uuid.uuid4().hex,
                        "type": "function", "function": {"name": call["function"]["name"],
                        "arguments": json.dumps(call["function"]["arguments"])}}]})
            if disconnected.is_set():
                raise InterruptedError("Request cancelled")
            session.history = history + [canonical(response)]
            usage = protocol._compute_token_usage(conv)
            usage["prompt_tokens"] += cached
            usage["total_tokens"] += cached
            usage["prompt_tokens_details"] = {"cached_tokens": cached}
            event(finish="tool_calls" if response["tool_calls"] else "stop")
            event(usage=usage)
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except Exception as error:
            if session.conversation is not None:
                session.conversation.cancel_process()
            if chunks is not None:
                try:
                    for _ in chunks:
                        pass
                except Exception:
                    pass
            # A cancelled/failed turn must never be reused as completed history.
            stop.set()
            if monitor is not None:
                monitor.join()
                monitor = None
            session.close()
            try:
                if started:
                    self.wfile.write(b"data: " + json.dumps({"error": {"message": str(error)}}).encode() + b"\n\n")
                    self.wfile.flush()
                else:
                    self.reply(400, {"error": {"message": str(error)}})
            except OSError:
                pass
        finally:
            stop.set()
            if monitor is not None:
                monitor.join()


def watch_owner():
    while os.read(0, 1):
        pass
    os._exit(0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--context", type=int, default=8192)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--cache-dir", required=True)
    parser.add_argument("--backend", choices=("cpu", "gpu"))
    parser.add_argument("--exit-on-stdin-close", action="store_true")
    args = parser.parse_args()
    if args.exit_on_stdin_close:
        # A transient user service outlives its systemd-run client. Pilon owns
        # this pipe; EOF also covers crashes and SIGKILL, without an orphan model.
        threading.Thread(target=watch_owner, daemon=True).start()
    key = os.environ.get("PILON_LITERT_API_KEY")
    if not key:
        parser.error("PILON_LITERT_API_KEY is required")
    os.makedirs(args.cache_dir, mode=0o700, exist_ok=True)
    litert_lm.set_min_log_severity(litert_lm.LogSeverity.ERROR)
    package = model_info.Model.from_model_reference(os.path.abspath(args.model))
    backend = model_info.parse_backend(args.backend, model_obj=package, cpu_thread_count=args.threads)
    vision = model_info.parse_backend(None, model_obj=package, target_model_types={TfLiteModelType.VISION_ENCODER.value}, label="vision")
    audio = model_info.parse_backend(None, model_obj=package, target_model_types={TfLiteModelType.AUDIO_ENCODER_HW.value}, label="audio")
    with litert_lm.Engine(args.model, backend=backend, vision_backend=vision, audio_backend=audio,
                         max_num_tokens=args.context, cache_dir=args.cache_dir, enable_benchmark=True,
                         use_ringbuffers_local_attention=True) as engine:
        with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
            server.api_key = key
            server.session = Session(engine)
            server.inference_lock = threading.Lock()
            try:
                server.serve_forever()
            finally:
                server.session.close()


if __name__ == "__main__":
    main()
