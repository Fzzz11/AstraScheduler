#!/usr/bin/env python3
"""AstraScheduler 受限 benchmark regression gate（AST-051 / R-091 / D-143）。

只对 standard profile 的 artifact 做 gate；回归判定必须同时满足：
  1) primary metric 劣化幅度超过 policy 允许的 practical effect；
  2) candidate 与 baseline 的 bootstrap 95% CI 排除零效应（CI 不重叠且方向为劣化）。
环境 metadata（cpu_model/compiler/build_type）不兼容时拒绝自动判定（exit 3），
quick/exploratory profile 的 baseline 不得用于 gate（exit 3）。

退出码：0 = 无回归；1 = 检出回归；2 = 输入/解析错误或缺少 policy case；3 = 环境/profile 不兼容。
"""

from __future__ import annotations

import argparse
import json
import sys

EXIT_OK = 0
EXIT_REGRESSION = 1
EXIT_INPUT_ERROR = 2
EXIT_ENV_INCOMPATIBLE = 3

GATE_ENV_KEYS = ("cpu_model", "compiler", "build_type")


def load_json(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"cannot load artifact {path}: {exc}", file=sys.stderr)
        raise SystemExit(EXIT_INPUT_ERROR)


def env_of(artifact: dict) -> dict:
    env = artifact.get("env")
    if not isinstance(env, dict):
        print("artifact missing env metadata; refusing to judge", file=sys.stderr)
        raise SystemExit(EXIT_ENV_INCOMPATIBLE)
    return env


def check_env(candidate: dict, baseline: dict) -> None:
    cand_env, base_env = env_of(candidate), env_of(baseline)
    for key in GATE_ENV_KEYS:
        cv, bv = cand_env.get(key), base_env.get(key)
        if cv in (None, "unknown") or bv in (None, "unknown"):
            print(f"env metadata {key} unknown; refusing to judge", file=sys.stderr)
            raise SystemExit(EXIT_ENV_INCOMPATIBLE)
        if cv != bv:
            print(
                f"environment mismatch {key}: candidate={cv!r} baseline={bv!r}; "
                "refusing to attribute to code",
                file=sys.stderr,
            )
            raise SystemExit(EXIT_ENV_INCOMPATIBLE)


def check_profile(candidate: dict, baseline: dict) -> None:
    if baseline.get("profile") != "standard":
        print(
            f"baseline profile is {baseline.get('profile')!r}; only 'standard' may gate",
            file=sys.stderr,
        )
        raise SystemExit(EXIT_ENV_INCOMPATIBLE)
    if candidate.get("profile") != "standard":
        print(
            f"candidate profile is {candidate.get('profile')!r}; only 'standard' may gate",
            file=sys.stderr,
        )
        raise SystemExit(EXIT_ENV_INCOMPATIBLE)


def comparable_workers(case: dict) -> list:
    return [w for w in case.get("workers", []) if w.get("comparable", False)]


def main() -> int:
    parser = argparse.ArgumentParser(description="AstraScheduler benchmark regression gate")
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--policy", required=True)
    args = parser.parse_args()

    candidate = load_json(args.candidate)
    baseline = load_json(args.baseline)
    policy = load_json(args.policy)

    check_env(candidate, baseline)
    check_profile(candidate, baseline)

    default_effect = float(policy.get("default_effect_pct", 5.0))
    policy_cases = policy.get("cases", {})

    base_cases = {c.get("name"): c for c in baseline.get("cases", [])}
    cand_cases = {c.get("name"): c for c in candidate.get("cases", [])}

    verdicts = []
    any_regression = False
    for name, policy_entry in policy_cases.items():
        if name not in base_cases:
            print(f"policy case {name!r} missing in baseline artifact", file=sys.stderr)
            raise SystemExit(EXIT_INPUT_ERROR)
        if name not in cand_cases:
            print(f"policy case {name!r} missing in candidate artifact", file=sys.stderr)
            raise SystemExit(EXIT_INPUT_ERROR)

        effect_pct = float(policy_entry.get("effect_pct", default_effect))
        base_workers = comparable_workers(base_cases[name])
        cand_workers = comparable_workers(cand_cases[name])
        if len(base_workers) != len(cand_workers):
            print(f"case {name!r}: worker matrix mismatch", file=sys.stderr)
            raise SystemExit(EXIT_INPUT_ERROR)

        for idx, (bw, cw) in enumerate(zip(base_workers, cand_workers)):
            brep = bw.get("repetitions")
            crep = cw.get("repetitions")
            if not isinstance(brep, dict) or not isinstance(crep, dict):
                print(f"case {name!r} worker#{idx}: missing repetitions block", file=sys.stderr)
                raise SystemExit(EXIT_INPUT_ERROR)
            b_med = float(brep["median_ns"])
            c_med = float(crep["median_ns"])
            b_raw = brep.get("raw_ns", [])
            c_raw = crep.get("raw_ns", [])
            if len(b_raw) < 2 or len(c_raw) < 2:
                print(f"case {name!r} worker#{idx}: raw repetitions not retained", file=sys.stderr)
                raise SystemExit(EXIT_INPUT_ERROR)

            threshold = b_med * (1.0 + effect_pct / 100.0)
            effect_exceeded = c_med > threshold
            ci_excludes_zero = float(crep["ci95_lo_ns"]) > float(brep["ci95_hi_ns"])
            regression = effect_exceeded and ci_excludes_zero
            if regression:
                any_regression = True
            verdicts.append(
                {
                    "case": name,
                    "worker": cw.get("workers", idx),
                    "baseline_median_ns": b_med,
                    "candidate_median_ns": c_med,
                    "effect_pct": effect_pct,
                    "effect_exceeded": effect_exceeded,
                    "ci_excludes_zero": ci_excludes_zero,
                    "regression": regression,
                    "raw_baseline_ns": b_raw,
                    "raw_candidate_ns": c_raw,
                }
            )

    out = {
        "schema_version": 1,
        "tool": "bench_compare",
        "policy": args.policy,
        "baseline": args.baseline,
        "candidate": args.candidate,
        "verdict": "regression" if any_regression else "ok",
        "cases": verdicts,
    }
    print(json.dumps(out, indent=2))
    return EXIT_REGRESSION if any_regression else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
