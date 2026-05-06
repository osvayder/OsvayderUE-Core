import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  REQUIRED_OUTPUT_FIELDS,
  evaluationOutputSchema,
  validateEvaluationOutput,
  validateWorkflowInput
} from "./schemas.js";
import {
  buildDryRunReceipt,
  buildPrompt,
  evaluateWorkflowDryRun
} from "./sample-workflow.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");

async function main(argv) {
  const command = argv[2] || "--dry-run";

  if (command === "--dry-run") {
    const input = await readExample("mcp-tool-review-input.json");
    const evaluation = evaluateWorkflowDryRun(input);
    printJson(buildDryRunReceipt(input, evaluation));
    return;
  }

  if (command === "--eval-sample") {
    await runEvalSample();
    return;
  }

  if (command === "--self-test") {
    await runSelfTest();
    return;
  }

  throw new Error(`Unknown command: ${command}`);
}

async function runEvalSample() {
  const input = await readExample("verification-receipt-input.json");
  const realApiEnabled =
    process.env.OPENAI_GRANT_EVAL_REAL_API === "true" &&
    Boolean(process.env.OPENAI_API_KEY);

  if (!realApiEnabled) {
    const evaluation = evaluateWorkflowDryRun(input);
    printJson({
      mode: "safe-message",
      api_call_made: false,
      message:
        "Real API mode is disabled. Set OPENAI_API_KEY and OPENAI_GRANT_EVAL_REAL_API=true, then add an SDK-backed caller if maintainers choose to enable networked evaluation.",
      dry_run_receipt: buildDryRunReceipt(input, evaluation)
    });
    return;
  }

  printJson({
    mode: "safe-message",
    api_call_made: false,
    message:
      "OPENAI_GRANT_EVAL_REAL_API=true was set, but this dependency-free harness intentionally ships without an SDK caller. Install and review an OpenAI SDK integration before enabling networked evaluation.",
    prompt_preview: buildPrompt(input).slice(0, 800)
  });
}

async function runSelfTest() {
  const examples = [
    await readExample("mcp-tool-review-input.json"),
    await readExample("verification-receipt-input.json")
  ];

  for (const input of examples) {
    assert.deepEqual(validateWorkflowInput(input), []);
    const evaluation = evaluateWorkflowDryRun(input);
    assert.deepEqual(validateEvaluationOutput(evaluation), []);

    for (const field of REQUIRED_OUTPUT_FIELDS) {
      assert.ok(field in evaluation, `missing output field ${field}`);
    }
  }

  assert.equal(evaluationOutputSchema.required.length, REQUIRED_OUTPUT_FIELDS.length);
  assert.equal(evaluateWorkflowDryRun(examples[0]).safe_to_merge, true);

  printJson({
    status: "ok",
    examples_checked: examples.length,
    required_output_fields: REQUIRED_OUTPUT_FIELDS
  });
}

async function readExample(fileName) {
  const raw = await readFile(resolve(projectRoot, "examples", fileName), "utf8");
  return JSON.parse(raw);
}

function printJson(value) {
  process.stdout.write(`${JSON.stringify(value, null, 2)}\n`);
}

main(process.argv).catch((error) => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});
