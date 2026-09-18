"""Small dependency-free checks for the versioned Phase 0 fixture envelope."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def test_dataset_matrix_is_s_m_l_and_does_not_materialize_cartesian_product():
    payload = json.loads((ROOT / "fixtures" / "v1" / "datasets.json").read_text(encoding="utf-8"))
    assert payload["schema"] == "quant.benchmark-datasets/v1"
    cases = payload["cases"]
    assert [case["case_id"] for case in cases] == ["S", "M", "L"]
    assert all(case["materialize_cartesian_product"] is False for case in cases)


def test_numerical_fixtures_have_version_and_non_silent_oracle():
    for fixture in sorted((ROOT / "fixtures" / "v1").glob("*.json")):
        if fixture.name == "datasets.json":
            continue
        payload = json.loads(fixture.read_text(encoding="utf-8"))
        assert payload["schema"] == "quant.baseline-fixture/v1"
        assert payload["version"] == 1
        assert payload["oracle"]
        assert payload["provenance"]
