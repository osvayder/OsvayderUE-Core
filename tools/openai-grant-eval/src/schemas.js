export const REQUIRED_OUTPUT_FIELDS = [
  "risk_level",
  "grant_relevance",
  "review_summary",
  "required_followups",
  "verification_gaps",
  "safe_to_merge"
];

export const riskLevels = ["low", "medium", "high"];
export const grantRelevanceLevels = ["low", "medium", "high"];

export const evaluationOutputSchema = {
  type: "object",
  required: REQUIRED_OUTPUT_FIELDS,
  additionalProperties: false,
  properties: {
    risk_level: {
      type: "string",
      enum: riskLevels
    },
    grant_relevance: {
      type: "string",
      enum: grantRelevanceLevels
    },
    review_summary: {
      type: "string",
      minLength: 1
    },
    required_followups: {
      type: "array",
      items: {
        type: "string"
      }
    },
    verification_gaps: {
      type: "array",
      items: {
        type: "string"
      }
    },
    safe_to_merge: {
      type: "boolean"
    }
  }
};

export function validateEvaluationOutput(output) {
  const errors = [];

  if (!output || typeof output !== "object" || Array.isArray(output)) {
    return ["output must be an object"];
  }

  for (const field of REQUIRED_OUTPUT_FIELDS) {
    if (!(field in output)) {
      errors.push(`missing required field: ${field}`);
    }
  }

  if ("risk_level" in output && !riskLevels.includes(output.risk_level)) {
    errors.push(`risk_level must be one of: ${riskLevels.join(", ")}`);
  }

  if (
    "grant_relevance" in output &&
    !grantRelevanceLevels.includes(output.grant_relevance)
  ) {
    errors.push(
      `grant_relevance must be one of: ${grantRelevanceLevels.join(", ")}`
    );
  }

  if ("review_summary" in output && typeof output.review_summary !== "string") {
    errors.push("review_summary must be a string");
  }

  if (
    "required_followups" in output &&
    !isStringArray(output.required_followups)
  ) {
    errors.push("required_followups must be an array of strings");
  }

  if (
    "verification_gaps" in output &&
    !isStringArray(output.verification_gaps)
  ) {
    errors.push("verification_gaps must be an array of strings");
  }

  if ("safe_to_merge" in output && typeof output.safe_to_merge !== "boolean") {
    errors.push("safe_to_merge must be a boolean");
  }

  return errors;
}

export function validateWorkflowInput(input) {
  const errors = [];

  if (!input || typeof input !== "object" || Array.isArray(input)) {
    return ["input must be an object"];
  }

  for (const field of ["workflow_type", "title", "change_summary"]) {
    if (typeof input[field] !== "string" || input[field].trim() === "") {
      errors.push(`${field} must be a non-empty string`);
    }
  }

  for (const field of [
    "public_surface",
    "evidence",
    "known_constraints"
  ]) {
    if (!isStringArray(input[field])) {
      errors.push(`${field} must be an array of strings`);
    }
  }

  if (!input.verification || typeof input.verification !== "object") {
    errors.push("verification must be an object");
  } else {
    if (!isStringArray(input.verification.commands_run)) {
      errors.push("verification.commands_run must be an array of strings");
    }

    if (!isStringArray(input.verification.manual_checks)) {
      errors.push("verification.manual_checks must be an array of strings");
    }
  }

  return errors;
}

function isStringArray(value) {
  return Array.isArray(value) && value.every((item) => typeof item === "string");
}
