from __future__ import annotations

import json

import pytest

from quantdesk.rest import (
    HullWhite1F,
    IRSwap,
    Market,
    ProblemDetails,
    QuantContext,
    QuantRestClient,
    QuantRestError,
    ResourceRef,
    ScenarioSet,
)


class FakeTransport:
    def __init__(self):
        self.calls = []
        self.responses = {}

    def request(self, method, path, payload=None):
        self.calls.append((method, path, payload))
        response = self.responses.get(path)
        if callable(response):
            return response(payload)
        return response or {"context": payload["context"]}


def test_context_mutations_are_one_stateless_apply_and_keep_refs():
    transport = FakeTransport()
    client = QuantRestClient("http://fake", transport=transport)
    root = QuantContext.new("ctx-1", pricing={"n_paths": 4, "n_steps": 2, "seed": 7})

    def apply(payload):
        context = dict(payload["context"])
        context["revision"] += 1
        context["markets"] = {"eur": payload["commands"][0]["spec"]}
        context["context_hash"] = "sha256:server"
        return {
            "context": context,
            "resource_ref": {"id": "eur", "hash": "sha256:market", "kind": "market", "version": 1},
        }

    transport.responses["/v1/context:apply"] = apply
    ctx, market = client.add_market(root, "eur", Market(pillars=[1.0], zero_rates=[0.02]), operation_id="op-1")

    assert market == ResourceRef("eur", "sha256:market", "market", 1)
    method, path, payload = transport.calls[0]
    assert (method, path, payload["operation_id"]) == ("POST", "/v1/context:apply", "op-1")
    assert payload["commands"] == [{"kind": "add_market", "id": "eur", "spec": {"pillars": [1.0], "zero_rates": [0.02]}}]
    assert ctx.context_hash == "sha256:server"


def test_price_payload_uses_openapi_pricing_operation_shape():
    transport = FakeTransport()
    client = QuantRestClient(transport=transport)
    ctx = QuantContext.new("ctx-1")
    context = ctx.to_dict()
    context["products"] = {"swap": IRSwap(1_000_000, 0.02, [1, 2], [1, 1]).to_dict()}
    context["models"] = {"hw": HullWhite1F(0.1, 0.03, 0.01, 0.02).to_dict()}
    context["markets"] = {"eur": Market(pillars=[1.0], zero_rates=[0.02]).to_dict()}
    ctx = QuantContext.from_dict(context)

    transport.responses["/v1/prices"] = {"context": ctx.to_dict(), "result": {"PV": 1.0}, "provenance": {}}
    result = client.price(ctx, "swap", model="hw", market="eur", measures=["PV"], operation_id="p-1")
    assert result["result"] == {"PV": 1.0}
    payload = transport.calls[0][2]
    assert set(payload) == {"context", "operation_id", "products", "measures", "market", "model"}
    assert payload["measures"] == [{"name": "PV", "params": {}}]
    assert payload["products"][0]["id"] == "swap"


def test_batch_scenarios_risk_and_xva_paths_match_real_endpoints():
    transport = FakeTransport()
    client = QuantRestClient(transport=transport)
    ctx = QuantContext.new("ctx-1")
    ctx = QuantContext.from_dict({**ctx.to_dict(), "markets": {"m": {}}, "models": {"o": {}}, "portfolios": {"p": {}}})
    for path in ("/v1/portfolios:price", "/v1/scenarios:run", "/v1/risk:calculate", "/v1/xva:calculate"):
        transport.responses[path] = {}

    client.price_batch(ctx, "p", model="o", market="m", operation_id="b-1")
    assert transport.calls[-1][1] == "/v1/portfolios:price"
    assert set(transport.calls[-1][2]) == {"context", "operation_id", "portfolio_id", "market", "model"}

    client.run_scenarios(ctx, "p", ScenarioSet("s", [{"id": "up"}]), model="o", market="m", operation_id="s-1")
    assert transport.calls[-1][1] == "/v1/scenarios:run"
    assert transport.calls[-1][2]["scenarios"]["id"] == "s"

    client.calculate_risk(ctx, "p", model="o", market="m", measures=["PV", "DV01"], operation_id="r-1")
    assert transport.calls[-1][2]["measures"] == ["pv", "pv01"]

    client.calculate_xva(ctx, "p", model="o", market="m", measures=["ee", "cva"], operation_id="x-1")
    assert transport.calls[-1][1] == "/v1/xva:calculate"
    assert transport.calls[-1][2]["measures"] == ["ee", "cva"]


def test_problem_details_are_typed():
    transport = FakeTransport()
    transport.responses["/v1/health"] = (429, {}, {"type": "x", "title": "Too Many", "status": 429, "code": "queue_full", "trace_id": "t", "detail": "full"})
    client = QuantRestClient(transport=transport)
    with pytest.raises(QuantRestError) as error:
        client._request("GET", "/v1/health")
    assert error.value.problem == ProblemDetails("x", "Too Many", 429, "queue_full", "t", "full")
