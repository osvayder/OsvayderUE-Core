import { validateWorkflowInput } from "./schemas.js";

export function buildPrompt(input) {
  const errors = validateWorkflowInput(input);
  if (errors.length > 0) {
    throw new Error(`Invalid workflow input: ${errors.join("; ")}`);
  }

  return [
    "You are reviewing a public open-source maintainer workflow for grant readiness.",
    "Return only compact JSON matching the requested schema.",
    "",
    `Workflow type: ${input.workflow_type}`,
    `Title: ${input.title}`,
    `Change summary: ${input.change_summary}`,
    `Public surface: ${input.public_surface.join(", ")}`,
    `Evidence: ${input.evidence.join(", ")}`,
    `Known constraints: ${input.known_constraints.join(", ")}`,
    `Commands run: ${input.verification.commands_run.join(", ") || "none"}`,
    `Manual checks: ${input.verification.manual_checks.join(", ") || "none"}`,
    "",
    "Required JSON fields: risk_level, grant_relevance, review_summary, required_followups, verification_gaps, safe_to_merge."
  ].join("\n");
}

export function evaluateWorkflowDryRun(input) {
  const errors = validateWorkflowInput(input);
  if (errors.length > 0) {
    return {
      risk_level: "high",
      grant_relevance: "medium",
      review_summary: `Input is incomplete: ${errors.join("; ")}.`,
      required_followups: ["Fix the workflow input shape before review."],
      verification_gaps: ["Dry-run could not evaluate malformed input."],
      safe_to_merge: false
    };
  }

  const verificationGaps = [];
  const requiredFollowups = [];
  const constraints = input.known_constraints.join(" ").toLowerCase();

  if (input.verification.commands_run.length === 0) {
    verificationGaps.push("No automated verification command was provided.");
  }

  if (input.verification.manual_checks.length === 0) {
    verificationGaps.push("No manual public-readiness check was provided.");
  }

  if (!constraints.includes("no api calls") && !constraints.includes("no real api")) {
    requiredFollowups.push("State whether default execution avoids real API calls.");
  }

  if (!constraints.includes("secret") && !constraints.includes("private")) {
    requiredFollowups.push("Document secret and private-data exclusion rules.");
  }

  const safeToMerge =
    verificationGaps.length === 0 && requiredFollowups.length === 0;

  return {
    risk_level: safeToMerge ? "low" : "medium",
    grant_relevance: "high",
    review_summary: safeToMerge
      ? "Dry-run found a public, reproducible workflow with explicit safety constraints."
      : "Dry-run found a grant-relevant workflow, but follow-up evidence is still needed before relying on it for public review.",
    required_followups: requiredFollowups,
    verification_gaps: verificationGaps,
    safe_to_merge: safeToMerge
  };
}

export function buildDryRunReceipt(input, evaluation) {
  return {
    mode: "dry-run",
    api_call_made: false,
    workflow_type: input.workflow_type,
    model_intent: process.env.OPENAI_MODEL || "gpt-4.1-mini",
    evaluation,
    prompt_preview: buildPrompt(input).slice(0, 800)
  };
}
