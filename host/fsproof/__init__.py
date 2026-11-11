# SPDX-License-Identifier: Apache-2.0
"""fsproof -- conformance bench for the Passerelle drivers (7.2, 7.3, 7.4).

Public surface: the differential test, the content exerciser, the directed
case table and the client profiles that make the comparison objective.
"""

from __future__ import annotations

from fsproof.cases import CaseResult, all_passed, case_names, run_cases, run_all_cases
from fsproof.clients import PROFILES, ClientProfile, describe_profiles, get_profile
from fsproof.diff import (
    DiffResult,
    Divergence,
    compare,
    load_regression,
    reduce_sequence,
    run_diff,
    run_local_diff,
    save_regression,
)
from fsproof.exercise import ExerciseResult, exercise_plan, run_exercise
from fsproof.gen import SequenceGenerator, generate_sequence, long_name_actions
from fsproof.model import Action, Entry, Snapshot, entry_from_path, hash_file, scan_tree
from fsproof.replay import StepResult, failures, replay

__all__ = [
    "PROFILES",
    "Action",
    "CaseResult",
    "ClientProfile",
    "DiffResult",
    "Divergence",
    "Entry",
    "ExerciseResult",
    "SequenceGenerator",
    "Snapshot",
    "StepResult",
    "all_passed",
    "case_names",
    "compare",
    "describe_profiles",
    "entry_from_path",
    "exercise_plan",
    "failures",
    "generate_sequence",
    "get_profile",
    "hash_file",
    "load_regression",
    "long_name_actions",
    "reduce_sequence",
    "replay",
    "run_all_cases",
    "run_cases",
    "run_diff",
    "run_exercise",
    "run_local_diff",
    "save_regression",
    "scan_tree",
]
