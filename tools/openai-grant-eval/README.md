# OpenAI Grant Eval Harness

Small maintainer-side example harness for showing how OpenAI API credits could support public PR, release, and verification review workflows.

The default path is intentionally offline:

- `npm run dry-run` produces structured JSON without `OPENAI_API_KEY`.
- `npm test` validates the example inputs and output shape without network access.
- `npm run eval:sample` does not call the API by default. It prints a safe enablement message and a dry-run receipt.

## Output Shape

Every evaluation result uses this JSON shape:

```json
{
  "risk_level": "low",
  "grant_relevance": "high",
  "review_summary": "Dry-run found a public, reproducible workflow with explicit safety constraints.",
  "required_followups": [],
  "verification_gaps": [],
  "safe_to_merge": true
}
```

## Usage

```powershell
npm install
npm test
npm run dry-run
npm run eval:sample
```

## Real API Mode

This package ships with no runtime dependencies and no SDK caller. That keeps public grant-readiness checks reproducible and safe in CI or maintainer dry-runs.

If maintainers later choose to spend OpenAI API credits on live evaluation, use `.env.example` as the starting point and add a reviewed SDK-backed caller. Keep real API mode opt-in, bounded, and disabled by default.

Required safety expectations for any future networked mode:

- Never include secrets, private assets, private logs, or workstation-specific paths in prompts.
- Keep the model, token budget, and workflow input explicit in review.
- Preserve `npm run dry-run` and `npm test` as no-key, no-network commands.

## Examples

- `examples/mcp-tool-review-input.json` models public PR review of an MCP tool change.
- `examples/verification-receipt-input.json` models public release verification receipt review.
