import assert from "node:assert/strict";
import test from "node:test";
import worker, { outputText } from "./worker.mjs";

const env = { OPENAI_API_KEY: "openai", DEVICE_TOKEN: "device" };

test("extracts assistant text", () => {
  assert.equal(outputText({ output: [{ content: [{ type: "output_text", text: "hello" }] }] }), "hello");
});

test("rejects unauthorized clients", async () => {
  const response = await worker.fetch(new Request("https://relay/chat", { method: "POST" }), env);
  assert.equal(response.status, 401);
});

test("proxies a valid conversation turn", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (_url, init) => {
    const body = JSON.parse(init.body);
    assert.equal(body.previous_response_id, "resp_previous");
    return Response.json({ id: "resp_next", output: [{ content: [{ type: "output_text", text: "Hi!" }] }] });
  };
  try {
    const request = new Request("https://relay/chat", {
      method: "POST",
      headers: { Authorization: "Bearer device", "Content-Type": "application/json" },
      body: JSON.stringify({ message: "Hello", previous_response_id: "resp_previous" }),
    });
    const response = await worker.fetch(request, env);
    assert.deepEqual(await response.json(), { text: "Hi!", response_id: "resp_next" });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("transcribes a valid WAV upload", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    assert.equal(url, "https://api.openai.com/v1/audio/transcriptions");
    assert.equal(init.body.get("model"), "gpt-4o-mini-transcribe");
    assert.equal(init.body.get("file").type, "audio/wav");
    return Response.json({ text: "voice works" });
  };
  try {
    const wav = new Uint8Array(44);
    wav.set(new TextEncoder().encode("RIFF"), 0);
    wav.set(new TextEncoder().encode("WAVE"), 8);
    const request = new Request("https://relay/transcribe", {
      method: "POST",
      headers: { Authorization: "Bearer device", "Content-Type": "audio/wav" },
      body: wav,
    });
    const response = await worker.fetch(request, env);
    assert.deepEqual(await response.json(), { text: "voice works" });
  } finally {
    globalThis.fetch = originalFetch;
  }
});
