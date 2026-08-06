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
