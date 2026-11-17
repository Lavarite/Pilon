import copy
import http.client
import json
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "addons/assistant/workers"))
from litert_worker import Handler, Session, canonical


class Conversation:
    token_count = 47
    closed = False

    def close(self):
        self.closed = True


class Engine:
    def __init__(self):
        self.calls = []

    def create_conversation(self, **kwargs):
        self.calls.append(kwargs)
        return Conversation()


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.engine = Engine()
        self.session = Session(self.engine)
        self.body = {"reasoning_effort": "none", "messages": [
            {"role": "system", "content": "Stable instructions"},
            {"role": "user", "content": "Hello"}], "tools": []}

    def test_reuses_native_conversation_and_only_sends_new_input(self):
        _, history, cached = self.session.prepare(self.body)
        self.assertEqual(cached, 0)
        self.assertFalse(self.engine.calls[0]["automatic_tool_calling"])
        self.assertFalse(self.engine.calls[0]["thinking_config"].enable_thinking)
        self.assertTrue(self.engine.calls[0]["constrained_decoding_config"].enable)
        self.session.history = history + [canonical({"role": "assistant", "content": "Hello!"})]
        body = copy.deepcopy(self.body)
        body["messages"] += [{"role": "assistant", "content": [{"type": "text", "text": "Hello!"}]},
                             {"role": "user", "content": "Continue"}]
        prompt, _, cached = self.session.prepare(body)
        self.assertEqual(cached, 47)
        self.assertEqual(prompt, {"role": "user", "content": [{"type": "text", "text": "Continue"}]})
        self.assertEqual(len(self.engine.calls), 1)

    def test_tool_results_preserve_cache_and_combine_parallel_results(self):
        self.body["tools"] = [{"type": "function", "function": {"name": name, "parameters": {"type": "object"}}}
                              for name in ("first", "second")]
        _, history, _ = self.session.prepare(self.body)
        self.assertFalse(self.engine.calls[0]["automatic_tool_calling"])
        calls = [{"id": "transport-" + name, "type": "function", "function": {"name": name, "arguments": "{}"}}
                 for name in ("first", "second")]
        self.session.history = history + [canonical({"role": "assistant", "tool_calls": [
            {"function": {"name": name, "arguments": {}}} for name in ("first", "second")]})]
        self.body["messages"] += [{"role": "assistant", "tool_calls": calls},
                                  {"role": "tool", "tool_call_id": "transport-first", "content": "one"},
                                  {"role": "tool", "tool_call_id": "transport-second", "content": "two"}]
        prompt, _, cached = self.session.prepare(self.body)
        self.assertEqual(cached, 47)
        self.assertEqual(prompt["role"], "tool")
        self.assertEqual([p["name"] for p in prompt["content"]], ["first", "second"])
        self.assertEqual(len(self.engine.calls), 1)

    def test_structured_tool_results_are_not_double_encoded(self):
        result = {"exit_code": 0, "output": "first\nsecond", "truncated": True,
                  "full_output_path": "/tmp/output.json"}
        message = canonical({"role": "tool", "content": [{"type": "tool_response",
            "name": "ssh_exec", "response": json.dumps(result)}]})
        self.assertEqual(message["content"][0]["response"], result)
        self.assertEqual(canonical(message), message)
        text = canonical({"role": "tool", "content": [{"type": "tool_response",
            "name": "exec_command", "response": "Process exited with code 0\nOutput:\nhello"}]})
        self.assertIsInstance(text["content"][0]["response"], str)

    def test_thoughts_survive_tool_continuation_but_not_new_user_turn(self):
        self.body["reasoning_effort"] = "medium"
        self.body["tools"] = [{"type": "function", "function": {"name": "inspect",
            "parameters": {"type": "object"}}}]
        _, history, _ = self.session.prepare(self.body)
        self.session.history = history + [canonical({"role": "assistant",
            "channels": {"thought": "Still need to inspect the second remote project."},
            "tool_calls": [{"function": {"name": "inspect", "arguments": {}}}]})]
        self.body["messages"] += [{"role": "assistant", "reasoning_content": "Still need to inspect the second remote project.",
            "tool_calls": [{"id": "inspect-1", "function": {"name": "inspect", "arguments": "{}"}}]},
            {"role": "tool", "tool_call_id": "inspect-1", "content": "First project result"}]
        _, history, cached = self.session.prepare(self.body)
        self.assertEqual(cached, 47)
        self.assertIn("second remote project", history[-2]["channels"]["thought"])
        self.session.close()
        self.session.prepare(self.body)
        self.assertEqual(self.engine.calls[-1]["messages"][-1]["channels"], history[-2]["channels"])
        self.session.history = history + [canonical({"role": "assistant", "content": "Done"})]
        self.body["messages"] += [{"role": "assistant", "content": "Done"}, {"role": "user", "content": "New task"}]
        _, history, cached = self.session.prepare(self.body)
        self.assertEqual(cached, 0)
        self.assertTrue(all("channels" not in message for message in history))

    def test_changed_history_or_reasoning_cannot_reuse_stale_kv(self):
        for change in ("history", "reasoning", "tools"):
            with self.subTest(change=change):
                _, history, _ = self.session.prepare(self.body)
                self.session.history = history
                original = self.session.conversation
                body = copy.deepcopy(self.body)
                body["messages"].append({"role": "user", "content": "Continue"})
                if change == "history":
                    body["messages"][0]["content"] = "Different instructions"
                elif change == "reasoning":
                    body["reasoning_effort"] = "medium"
                else:
                    body["tools"] = [{"type": "function", "function": {"name": "new", "parameters": {"type": "object"}}}]
                _, _, cached = self.session.prepare(body)
                self.assertEqual(cached, 0)
                self.assertTrue(original.closed)

    def test_json_string_gateway_preserves_payload_and_cache(self):
        self.body["tools"] = [{"type": "function", "function": {"name": "call_tool", "parameters": {
            "type": "object", "properties": {"name": {"type": "string"}, "arguments": {"type": "string"}},
            "required": ["name", "arguments"], "additionalProperties": False}}}]
        _, history, _ = self.session.prepare(self.body)
        payload = json.dumps({"command": 'printf "a\\nb"', "target": "server"})
        arguments = {"name": "ssh_exec", "arguments": payload}
        self.session.history = history + [canonical({"role": "assistant", "tool_calls": [
            {"function": {"name": "call_tool", "arguments": arguments}}]})]
        self.body["messages"] += [{"role": "assistant", "tool_calls": [{"id": "gateway", "type": "function",
            "function": {"name": "call_tool", "arguments": json.dumps(arguments)}}]},
            {"role": "tool", "tool_call_id": "gateway", "content": "Command result"}]
        _, returned_history, cached = self.session.prepare(self.body)
        self.assertEqual(cached, 47)
        self.assertEqual(returned_history[-2]["tool_calls"][0]["function"]["arguments"]["arguments"], payload)
        self.assertEqual(len(self.engine.calls), 1)

    def test_empty_or_unmatched_tool_input_is_rejected(self):
        for messages in ([], [{"role": "assistant", "content": "Wrong final role"}],
                         [{"role": "tool", "tool_call_id": "missing", "content": "result"}]):
            with self.subTest(messages=messages), self.assertRaises(ValueError):
                self.session.prepare({"messages": messages})

    def test_close_discards_history_after_cancellation(self):
        self.session.prepare(self.body)
        old = self.session.conversation
        self.session.close()
        self.assertTrue(old.closed)
        self.assertEqual(self.session.history, [])
        self.assertIsNone(self.session.conversation)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.cancelled = threading.Event()
        self.closed = threading.Event()
        cancelled, closed = self.cancelled, self.closed

        class PendingConversation(Conversation):
            def send_message_async(self, _prompt, **_kwargs):
                cancelled.wait(5)
                return
                yield  # Keep the SDK's iterator contract while prefill waits.

            def cancel_process(self):
                cancelled.set()

            def close(self):
                closed.set()

        class PendingEngine:
            def create_conversation(self, **_kwargs):
                return PendingConversation()

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.api_key = "private-test-token"
        self.server.session = Session(PendingEngine())
        self.server.inference_lock = threading.Lock()
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.cancelled.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def connect(self):
        return http.client.HTTPConnection(*self.server.server_address, timeout=5)

    def test_health_requires_private_token(self):
        for headers, status in (({}, 401), ({"Authorization": "Bearer private-test-token"}, 200)):
            with self.subTest(status=status):
                connection = self.connect()
                connection.request("GET", "/health", headers=headers)
                response = connection.getresponse()
                self.assertEqual(response.status, status)
                response.read()
                connection.close()

    def test_disconnected_client_cancels_native_prefill_and_discards_session(self):
        connection = self.connect()
        connection.request("POST", "/v1/chat/completions", json.dumps({"stream": True,
            "reasoning_effort": "none", "messages": [{"role": "user", "content": "Hello"}]}),
            {"Authorization": "Bearer private-test-token", "Content-Type": "application/json"})
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertIn(b"assistant", response.readline())
        response.close()
        connection.close()
        self.assertTrue(self.cancelled.wait(3), "Native generation must be cancelled during prefill")
        self.assertTrue(self.closed.wait(3), "The incomplete KV state must be discarded")


class OwnerLifetimeTests(unittest.TestCase):
    worker_code = "from litert_worker import watch_owner; print('READY', flush=True); watch_owner()"
    source = str(pathlib.Path(__file__).resolve().parents[4] / "addons/assistant/workers")

    def test_owner_pipe_closure_exits_worker(self):
        worker = subprocess.Popen([sys.executable, "-c", self.worker_code], cwd=self.source,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(worker.stdout.readline().strip(), b"READY")
            self.assertIsNone(worker.poll())
            worker.stdin.close()
            self.assertEqual(worker.wait(timeout=5), 0)
        finally:
            if worker.poll() is None:
                worker.kill()
                worker.wait()
            worker.stdout.close()

    def test_owner_sigkill_does_not_leave_worker_running(self):
        owner_code = (
            "import subprocess,sys,time\n"
            f"worker=subprocess.Popen([sys.executable,'-c',{self.worker_code!r}],stdin=subprocess.PIPE,stdout=subprocess.PIPE)\n"
            "assert worker.stdout.readline().strip()==b'READY'\n"
            "print(worker.pid,flush=True)\n"
            "time.sleep(60)\n")
        owner = subprocess.Popen([sys.executable, "-c", owner_code], cwd=self.source, stdout=subprocess.PIPE)
        pid = None
        try:
            pid = int(owner.stdout.readline())
            owner.kill()
            owner.wait(timeout=5)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                status = pathlib.Path(f"/proc/{pid}/stat")
                try:
                    state = status.read_text().split(")", 1)[1].split()[0]
                except FileNotFoundError:
                    break
                if state == "Z":
                    break
                time.sleep(0.02)
            else:
                self.fail("The native service worker survived its owner's SIGKILL")
        finally:
            if owner.poll() is None:
                owner.kill()
                owner.wait()
            owner.stdout.close()
            if pid is not None:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    unittest.main()
