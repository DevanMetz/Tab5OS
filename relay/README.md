# Tab5 OpenAI relay

This Worker keeps the OpenAI API key off the tablet and public repository. The tablet authenticates with a separate revocable device token.

```powershell
npx wrangler login
npx wrangler secret put OPENAI_API_KEY --config relay/wrangler.jsonc
npx wrangler deploy --config relay/wrangler.jsonc
.\tools\provision_chat.ps1 -RelayUrl "https://<worker>.workers.dev/chat"
```

The provisioning script creates a device token, uploads it to Cloudflare, and writes the ignored `main/chat_secrets.h` without printing the token. Rebuild and flash the firmware afterward.

Run the relay check with:

```powershell
node --test relay/worker.test.mjs
```
