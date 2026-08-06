const OPENAI_URL = "https://api.openai.com/v1/responses";
const TRANSCRIBE_URL = "https://api.openai.com/v1/audio/transcriptions";
const MAX_MESSAGE = 2000;
const MAX_AUDIO = 1000000;

function reply(body, status = 200) {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store" } });
}

function outputText(response) {
  return (response.output || [])
    .flatMap((item) => item.content || [])
    .filter((part) => part.type === "output_text")
    .map((part) => part.text)
    .join("");
}

function tabletText(text) {
  return text
    .replace(/[\u2018\u2019]/g, "'")
    .replace(/[\u201C\u201D]/g, '"')
    .replace(/[\u2013\u2014]/g, "-")
    .replace(/\u2026/g, "...")
    .replace(/\u2022/g, "*")
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[^\x09\x0A\x0D\x20-\x7E]+/gu, "?");
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/health") return reply({ ok: true });
    if (request.method !== "POST" || !["/chat", "/transcribe"].includes(url.pathname)) {
      return reply({ error: "Not found" }, 404);
    }
    if (!env.OPENAI_API_KEY || !env.DEVICE_TOKEN ||
        request.headers.get("Authorization") !== `Bearer ${env.DEVICE_TOKEN}`) {
      return reply({ error: "Unauthorized" }, 401);
    }

    if (url.pathname === "/transcribe") {
      const audio = await request.arrayBuffer();
      const bytes = new Uint8Array(audio);
      if (!audio.byteLength || audio.byteLength > MAX_AUDIO ||
          new TextDecoder().decode(bytes.slice(0, 4)) !== "RIFF" ||
          new TextDecoder().decode(bytes.slice(8, 12)) !== "WAVE") {
        return reply({ error: "Invalid WAV audio" }, 400);
      }
      const form = new FormData();
      form.append("file", new Blob([audio], { type: "audio/wav" }), "speech.wav");
      form.append("model", "gpt-4o-mini-transcribe");
      const upstream = await fetch(TRANSCRIBE_URL, {
        method: "POST",
        headers: { Authorization: `Bearer ${env.OPENAI_API_KEY}` },
        body: form,
      });
      const response = await upstream.json().catch(() => null);
      if (!upstream.ok || typeof response?.text !== "string") {
        return reply({ error: "OpenAI transcription failed" }, 502);
      }
      return reply({ text: tabletText(response.text) });
    }

    let body;
    try {
      body = await request.json();
    } catch {
      return reply({ error: "Invalid JSON" }, 400);
    }
    const message = typeof body.message === "string" ? body.message.trim() : "";
    if (!message || message.length > MAX_MESSAGE) return reply({ error: "Message must be 1-2000 characters" }, 400);
    if (body.previous_response_id && !/^resp_[A-Za-z0-9_-]+$/.test(body.previous_response_id)) {
      return reply({ error: "Invalid conversation id" }, 400);
    }

    const openaiBody = {
      model: "gpt-5.6-luna",
      reasoning: { effort: "high" },
      instructions: "You are a concise, helpful assistant on a small touchscreen device. Use plain ASCII text; avoid emoji and typographic punctuation.",
      input: message,
      max_output_tokens: 700,
      store: true,
    };
    if (body.previous_response_id) openaiBody.previous_response_id = body.previous_response_id;

    const upstream = await fetch(OPENAI_URL, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${env.OPENAI_API_KEY}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify(openaiBody),
    });
    const response = await upstream.json().catch(() => null);
    if (!upstream.ok || !response) return reply({ error: "OpenAI request failed" }, 502);
    const text = tabletText(outputText(response));
    if (!text) return reply({ error: "OpenAI returned no text" }, 502);
    return reply({ text, response_id: response.id });
  },
};

export { outputText, tabletText };
