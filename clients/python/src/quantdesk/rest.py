"""Thin, stateless Python client for the implemented Engine Quant REST v1 API.

The client deliberately keeps a :class:`QuantContext` snapshot on the caller side.  It
does not create a session or retain server handles: every calculation request contains
the complete context and resource references returned by ``context:apply``.

This module only uses the Python standard library.  The native ``engine`` extension and
the existing typed facade remain independent and can be used alongside this client.
"""

from __future__ import annotations

import copy
import hashlib
import json
import uuid
from dataclasses import dataclass, field
from typing import Any, Mapping, Protocol, Sequence
from urllib.error import HTTPError
from urllib.request import Request, urlopen


JSON = Any


def _jsonable(value: Any) -> Any:
    """Convert a small typed spec (or a pydantic model) to JSON-compatible values."""
    if isinstance(value, Mapping):
        return {str(k): _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    if hasattr(value, "to_dict") and callable(value.to_dict):
        return _jsonable(value.to_dict())
    if hasattr(value, "to_params") and callable(value.to_params):
        return _jsonable(value.to_params())
    if hasattr(value, "model_dump") and callable(value.model_dump):
        return _jsonable(value.model_dump(exclude_none=True))
    if hasattr(value, "__dataclass_fields__"):
        return {name: _jsonable(getattr(value, name)) for name in value.__dataclass_fields__}
    return value


def _sorted_json(value: Any) -> Any:
    """Sort JSON object keys recursively, as serde_json's default map serializer does."""
    if isinstance(value, Mapping):
        return {key: _sorted_json(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        return [_sorted_json(item) for item in value]
    return value


def _hash_value(value: Any) -> str:
    raw = json.dumps(_sorted_json(_jsonable(value)), separators=(",", ":"), ensure_ascii=False).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


@dataclass(frozen=True)
class ResourceRef:
    """Client-owned reference to a versioned resource in a context."""

    id: str
    hash: str
    kind: str
    version: int

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ResourceRef":
        return cls(str(value["id"]), str(value["hash"]), str(value["kind"]), int(value["version"]))

    def to_dict(self) -> dict[str, Any]:
        return {"id": self.id, "hash": self.hash, "kind": self.kind, "version": self.version}


@dataclass(frozen=True)
class QuantContext:
    """Immutable local snapshot of the REST ``ContextEnvelope``."""

    context_id: str
    revision: int = 0
    parent_hash: str | None = None
    context_hash: str | None = None
    engine: Mapping[str, Any] = field(default_factory=dict)
    markets: Mapping[str, Any] = field(default_factory=dict)
    models: Mapping[str, Any] = field(default_factory=dict)
    products: Mapping[str, Any] = field(default_factory=dict)
    portfolios: Mapping[str, Any] = field(default_factory=dict)
    pricing: Mapping[str, Any] = field(default_factory=dict)
    runs: Sequence[Mapping[str, Any]] = field(default_factory=tuple)
    branch_id: str = "main"
    lineage: Sequence[str] = field(default_factory=tuple)
    _extra: Mapping[str, Any] = field(default_factory=dict, repr=False, compare=False)

    @classmethod
    def new(
        cls,
        context_id: str | None = None,
        *,
        engine: Mapping[str, Any] | None = None,
        pricing: Mapping[str, Any] | None = None,
        branch_id: str = "main",
    ) -> "QuantContext":
        context = cls(
            context_id=context_id or f"ctx-{uuid.uuid4().hex}",
            engine=dict(engine or {}),
            pricing=dict(pricing or {}),
            branch_id=branch_id,
        )
        return context.with_hash()

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "QuantContext":
        data = dict(value)
        known = {
            "schema", "context_id", "revision", "parent_hash", "context_hash", "engine", "markets",
            "models", "products", "portfolios", "pricing", "runs", "branch_id", "lineage",
        }
        if data.get("schema", "quant.context/v1") != "quant.context/v1":
            raise ValueError(f"unsupported context schema: {data.get('schema')!r}")
        return cls(
            context_id=str(data["context_id"]),
            revision=int(data.get("revision", 0)),
            parent_hash=_hash_or_none(data.get("parent_hash")),
            context_hash=_hash_or_none(data.get("context_hash")),
            engine=copy.deepcopy(data.get("engine", {})),
            markets=copy.deepcopy(data.get("markets", {})),
            models=copy.deepcopy(data.get("models", {})),
            products=copy.deepcopy(data.get("products", {})),
            portfolios=copy.deepcopy(data.get("portfolios", {})),
            pricing=copy.deepcopy(data.get("pricing", {})),
            runs=tuple(copy.deepcopy(data.get("runs", []))),
            branch_id=str(data.get("branch_id", "main")),
            lineage=tuple(_hash_or_none(item) or "" for item in data.get("lineage", [])),
            _extra={k: copy.deepcopy(v) for k, v in data.items() if k not in known},
        )

    def to_dict(self, *, include_schema: bool = True) -> dict[str, Any]:
        # Keep the same field names/order as QuantContext in quant-domain.  ``None`` hash
        # fields are omitted because the Rust DTO uses skip_serializing_if for those fields.
        result: dict[str, Any] = {}
        if include_schema:
            result["schema"] = "quant.context/v1"
        result.update(
            {
                "context_id": self.context_id,
                "revision": self.revision,
            }
        )
        if self.parent_hash is not None:
            result["parent_hash"] = self.parent_hash
        if self.context_hash is not None:
            result["context_hash"] = self.context_hash
        result.update(
            {
                "engine": _jsonable(self.engine),
                "markets": _jsonable(self.markets),
                "models": _jsonable(self.models),
                "products": _jsonable(self.products),
                "portfolios": _jsonable(self.portfolios),
                "pricing": _jsonable(self.pricing),
                "runs": _jsonable(list(self.runs)),
                "branch_id": self.branch_id,
                "lineage": list(self.lineage),
            }
        )
        result.update(copy.deepcopy(dict(self._extra)))
        return result

    def computed_hash(self) -> str:
        value = self.to_dict()
        value.pop("context_hash", None)
        return _hash_value(value)

    def with_hash(self) -> "QuantContext":
        return dataclass_replace(self, context_hash=self.computed_hash())

    def verify_hash(self) -> bool:
        return self.context_hash is not None and self.context_hash == self.computed_hash()

    def with_run(self, record: Mapping[str, Any]) -> "QuantContext":
        """Return a locally recorded run snapshot without storing result arrays."""
        runs = list(self.runs)
        runs.append(dict(record))
        next_context = dataclass_replace(
            self,
            runs=tuple(runs),
            parent_hash=self.context_hash,
            context_hash=None,
            revision=self.revision + 1,
        )
        return next_context.with_hash()


# ``Context`` is the short name used by notebook examples.
Context = QuantContext


@dataclass(frozen=True, init=False)
class MarketSpec:
    """Open market payload; providers may add curve/surface fields over time."""

    values: Mapping[str, Any]

    def __init__(self, values: Mapping[str, Any] | None = None, **kwargs: Any) -> None:
        merged = dict(values or {})
        merged.update(kwargs)
        object.__setattr__(self, "values", merged)

    def to_dict(self) -> dict[str, Any]:
        return _jsonable(self.values)


Market = MarketSpec


@dataclass(frozen=True)
class HullWhite1F:
    a: float
    b: float
    sigma: float
    r0: float

    def to_dict(self) -> dict[str, Any]:
        return {"a": self.a, "b": self.b, "sigma": self.sigma, "r0": self.r0}

    to_params = to_dict


@dataclass(frozen=True)
class IRSwap:
    notional: float
    fixed_rate: float | str
    payment_times: Sequence[float]
    accruals: Sequence[float]
    start: float = 0.0

    def to_dict(self) -> dict[str, Any]:
        value: dict[str, Any] = {
            "notional": self.notional,
            "start": self.start,
            "payment_times": list(self.payment_times),
            "accruals": list(self.accruals),
        }
        if self.fixed_rate != "PAR":
            value["fixed_rate"] = self.fixed_rate
        return value

    to_params = to_dict


@dataclass(frozen=True)
class ScenarioSet:
    id: str
    scenarios: Sequence[Mapping[str, Any]]
    chunk_size: int | None = None

    def to_dict(self) -> dict[str, Any]:
        value: dict[str, Any] = {"id": self.id, "scenarios": [_jsonable(x) for x in self.scenarios]}
        if self.chunk_size is not None:
            value["chunk_size"] = self.chunk_size
        return value


@dataclass(frozen=True)
class ProblemDetails:
    type: str
    title: str
    status: int
    code: str
    trace_id: str
    detail: str | None = None
    field_errors: Sequence[Mapping[str, Any]] = field(default_factory=tuple)

    @classmethod
    def from_dict(cls, value: Mapping[str, Any], status: int | None = None) -> "ProblemDetails":
        return cls(
            type=str(value.get("type", "about:blank")),
            title=str(value.get("title", "Request error")),
            status=int(value.get("status", status or 500)),
            code=str(value.get("code", "http_error")),
            trace_id=str(value.get("trace_id", "")),
            detail=value.get("detail"),
            field_errors=tuple(value.get("field_errors", ())),
        )

    def to_dict(self) -> dict[str, Any]:
        value = {
            "type": self.type,
            "title": self.title,
            "status": self.status,
            "code": self.code,
            "trace_id": self.trace_id,
        }
        if self.detail is not None:
            value["detail"] = self.detail
        if self.field_errors:
            value["field_errors"] = list(self.field_errors)
        return value


class QuantRestError(RuntimeError):
    """Typed error for an RFC 9457-style ``application/problem+json`` response."""

    def __init__(self, problem: ProblemDetails):
        self.problem = problem
        super().__init__(f"{problem.code} ({problem.status}): {problem.detail or problem.title}")

    @property
    def status(self) -> int:
        return self.problem.status

    @property
    def code(self) -> str:
        return self.problem.code


@dataclass(frozen=True)
class TransportResponse:
    status: int
    headers: Mapping[str, str] = field(default_factory=dict)
    body: bytes | str | Mapping[str, Any] | None = None


class Transport(Protocol):
    def request(self, method: str, path: str, payload: Mapping[str, Any] | None = None) -> Any: ...


class UrlLibTransport:
    """Small urllib transport used when the caller does not inject a fake transport."""

    def __init__(self, base_url: str, timeout: float = 30.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, payload: Mapping[str, Any] | None = None) -> TransportResponse:
        body = None if payload is None else json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
        request = Request(
            self.base_url + (path if path.startswith("/") else "/" + path),
            data=body,
            method=method,
            headers={"Accept": "application/json, application/problem+json", "Content-Type": "application/json"},
        )
        try:
            with urlopen(request, timeout=self.timeout) as response:
                return TransportResponse(response.status, dict(response.headers.items()), response.read())
        except HTTPError as error:
            return TransportResponse(error.code, dict(error.headers.items()), error.read())


def _hash_or_none(value: Any) -> str | None:
    if value is None:
        return None
    return str(value)


def dataclass_replace(value: Any, **changes: Any) -> Any:
    # A tiny local equivalent avoids importing dataclasses.replace into the public API.
    data = {name: getattr(value, name) for name in value.__dataclass_fields__}
    data.update(changes)
    return type(value)(**data)


def _context(value: QuantContext | Mapping[str, Any]) -> QuantContext:
    return value if isinstance(value, QuantContext) else QuantContext.from_dict(value)


def _spec(value: Any) -> dict[str, Any]:
    result = _jsonable(value)
    if not isinstance(result, dict):
        raise TypeError("resource specs must serialize to a JSON object")
    return result


def _ref(value: ResourceRef | Mapping[str, Any] | str, *, kind: str | None = None, context: QuantContext | None = None) -> ResourceRef:
    if isinstance(value, ResourceRef):
        return value
    if isinstance(value, Mapping):
        return ResourceRef.from_dict(value)
    if context is None or kind is None:
        raise TypeError("a string resource id requires kind and context")
    collection = getattr(context, kind + "s")
    if value not in collection:
        raise KeyError(f"{kind} not found in context: {value}")
    return ResourceRef(str(value), _hash_value(collection[value]), kind, context.revision)


def _operation_id(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex}"


class QuantRestClient:
    """Thin facade over the implemented REST v1 endpoints.

    ``transport`` is injectable for deterministic unit tests.  It may implement
    ``request(method, path, payload)`` or be a callable with that same signature.
    """

    def __init__(self, base_url: str = "https://quant.example", *, transport: Any = None, timeout: float = 30.0):
        self.base_url = base_url.rstrip("/")
        self.transport: Any = transport or UrlLibTransport(self.base_url, timeout=timeout)

    def _request(self, method: str, path: str, payload: Mapping[str, Any] | None = None) -> dict[str, Any]:
        if hasattr(self.transport, "request"):
            response = self.transport.request(method, path, payload)
        elif callable(self.transport):
            response = self.transport(method, path, payload)
        else:
            raise TypeError("transport must expose request() or be callable")
        status, body = self._unpack_response(response)
        if status >= 400:
            problem = ProblemDetails.from_dict(body if isinstance(body, Mapping) else {}, status)
            raise QuantRestError(problem)
        if body is None:
            return {}
        if not isinstance(body, Mapping):
            raise ValueError("REST response must be a JSON object")
        return dict(body)

    @staticmethod
    def _unpack_response(response: Any) -> tuple[int, Any]:
        if isinstance(response, TransportResponse):
            status, body = response.status, response.body
        elif isinstance(response, tuple) and len(response) == 3:
            status, _headers, body = response
        elif isinstance(response, Mapping) and "status" in response and "body" in response:
            status, body = response["status"], response["body"]
        elif isinstance(response, Mapping):
            status, body = 200, response
        else:
            status, body = getattr(response, "status", 200), getattr(response, "body", response)
        if isinstance(body, (bytes, bytearray)):
            body = body.decode("utf-8")
        if isinstance(body, str):
            body = json.loads(body) if body.strip() else None
        return int(status), body

    def context_apply(
        self,
        context: QuantContext | Mapping[str, Any],
        commands: Sequence[Mapping[str, Any]],
        *,
        operation_id: str | None = None,
        resource: Mapping[str, Any] | None = None,
    ) -> tuple[QuantContext, ResourceRef | None]:
        payload: dict[str, Any] = {
            "context": _context(context).to_dict(),
            "operation_id": operation_id or _operation_id("context"),
            "commands": [_jsonable(command) for command in commands],
        }
        if resource is not None:
            payload["resource"] = _jsonable(resource)
        response = self._request("POST", "/v1/context:apply", payload)
        if "context" not in response:
            raise ValueError("context:apply response has no context")
        ref = response.get("resource_ref")
        return QuantContext.from_dict(response["context"]), (ResourceRef.from_dict(ref) if ref else None)

    def _add(self, context: QuantContext | Mapping[str, Any], kind: str, resource_id: str, spec: Any, operation_id: str | None) -> tuple[QuantContext, ResourceRef | None]:
        value = _spec(spec)
        return self.context_apply(
            context,
            [{"kind": f"add_{kind}", "id": resource_id, "spec": value}],
            operation_id=operation_id,
        )

    def add_market(self, context: QuantContext | Mapping[str, Any], resource_id: str, spec: Any, *, operation_id: str | None = None) -> tuple[QuantContext, ResourceRef | None]:
        return self._add(context, "market", resource_id, spec, operation_id)

    def add_model(self, context: QuantContext | Mapping[str, Any], resource_id: str, spec: Any, *, operation_id: str | None = None) -> tuple[QuantContext, ResourceRef | None]:
        return self._add(context, "model", resource_id, spec, operation_id)

    def add_product(self, context: QuantContext | Mapping[str, Any], resource_id: str, spec: Any, *, operation_id: str | None = None) -> tuple[QuantContext, ResourceRef | None]:
        return self._add(context, "product", resource_id, spec, operation_id)

    def add_products(
        self,
        context: QuantContext | Mapping[str, Any],
        products: Mapping[str, Any] | Sequence[tuple[str, Any]] | Sequence[Mapping[str, Any]],
        *,
        operation_id: str | None = None,
    ) -> tuple[QuantContext, list[ResourceRef]]:
        if isinstance(products, Mapping):
            items = list(products.items())
        else:
            items = []
            for item in products:
                if isinstance(item, Mapping) and "id" in item and "spec" in item:
                    items.append((str(item["id"]), item["spec"]))
                elif isinstance(item, (tuple, list)) and len(item) == 2:
                    items.append((str(item[0]), item[1]))
                else:
                    raise TypeError("add_products expects (id, spec) pairs or {'id', 'spec'} mappings")
        commands = [{"kind": "add_product", "id": resource_id, "spec": _spec(spec)} for resource_id, spec in items]
        next_context, last_ref = self.context_apply(context, commands, operation_id=operation_id)
        refs = [ResourceRef(resource_id, _hash_value(_spec(spec)), "product", next_context.revision) for resource_id, spec in items]
        if refs and last_ref is not None:
            refs[-1] = last_ref
        return next_context, refs

    def price(
        self,
        context: QuantContext | Mapping[str, Any],
        product: ResourceRef | Mapping[str, Any] | str | Sequence[ResourceRef | Mapping[str, Any] | str],
        *,
        model: ResourceRef | Mapping[str, Any] | str | None = None,
        market: ResourceRef | Mapping[str, Any] | str | None = None,
        measures: Sequence[Any],
        operation_id: str | None = None,
        pricing: Mapping[str, Any] | None = None,
        execution: Mapping[str, Any] | None = None,
        output: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        ctx = _context(context)
        values = list(product) if isinstance(product, (list, tuple)) else [product]
        payload: dict[str, Any] = {
            "context": ctx.to_dict(),
            "operation_id": operation_id or _operation_id("price"),
            "products": [_ref(item, kind="product", context=ctx).to_dict() for item in values],
            "measures": [_measure(item) for item in measures],
        }
        if market is not None:
            payload["market"] = _ref(market, kind="market", context=ctx).to_dict()
        if model is not None:
            payload["model"] = _ref(model, kind="model", context=ctx).to_dict()
        for key, value in (("pricing", pricing), ("execution", execution), ("output", output)):
            if value is not None:
                payload[key] = _jsonable(value)
        return self._request("POST", "/v1/prices", payload)

    def price_batch(
        self,
        context: QuantContext | Mapping[str, Any],
        portfolio_id: str | ResourceRef,
        *,
        model: ResourceRef | Mapping[str, Any] | str,
        market: ResourceRef | Mapping[str, Any] | str,
        operation_id: str | None = None,
        pricing: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        return self.price_portfolio(context, portfolio_id, model=model, market=market, operation_id=operation_id, pricing=pricing)

    def price_portfolio(
        self,
        context: QuantContext | Mapping[str, Any],
        portfolio_id: str | ResourceRef,
        *,
        model: ResourceRef | Mapping[str, Any] | str,
        market: ResourceRef | Mapping[str, Any] | str,
        operation_id: str | None = None,
        pricing: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        ctx = _context(context)
        portfolio = portfolio_id.id if isinstance(portfolio_id, ResourceRef) else str(portfolio_id)
        payload: dict[str, Any] = {
            "context": ctx.to_dict(),
            "operation_id": operation_id or _operation_id("portfolio-price"),
            "portfolio_id": portfolio,
            "market": _ref(market, kind="market", context=ctx).to_dict(),
            "model": _ref(model, kind="model", context=ctx).to_dict(),
        }
        if pricing is not None:
            payload["pricing"] = _jsonable(pricing)
        return self._request("POST", "/v1/portfolios:price", payload)

    def run_scenarios(
        self,
        context: QuantContext | Mapping[str, Any],
        portfolio_id: str | ResourceRef,
        scenarios: ScenarioSet | Mapping[str, Any],
        *,
        model: ResourceRef | Mapping[str, Any] | str,
        market: ResourceRef | Mapping[str, Any] | str,
        operation_id: str | None = None,
        pricing: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        ctx = _context(context)
        portfolio = portfolio_id.id if isinstance(portfolio_id, ResourceRef) else str(portfolio_id)
        payload: dict[str, Any] = {
            "context": ctx.to_dict(),
            "operation_id": operation_id or _operation_id("scenarios"),
            "portfolio_id": portfolio,
            "market": _ref(market, kind="market", context=ctx).to_dict(),
            "model": _ref(model, kind="model", context=ctx).to_dict(),
            "scenarios": _jsonable(scenarios),
        }
        if pricing is not None:
            payload["pricing"] = _jsonable(pricing)
        return self._request("POST", "/v1/scenarios:run", payload)

    def calculate_risk(
        self,
        context: QuantContext | Mapping[str, Any],
        portfolio_id: str | ResourceRef,
        *,
        model: ResourceRef | Mapping[str, Any] | str,
        market: ResourceRef | Mapping[str, Any] | str,
        measures: Sequence[Any] | None = None,
        factors: Sequence[Mapping[str, Any]] | None = None,
        bump_scheme: str | None = None,
        bump_size: float | None = None,
        tolerances: Mapping[str, float] | None = None,
        pricing: Mapping[str, Any] | None = None,
        operation_id: str | None = None,
    ) -> dict[str, Any]:
        ctx = _context(context)
        portfolio = portfolio_id.id if isinstance(portfolio_id, ResourceRef) else str(portfolio_id)
        payload: dict[str, Any] = {
            "context": ctx.to_dict(),
            "operation_id": operation_id or _operation_id("risk"),
            "portfolio_id": portfolio,
            "market": _ref(market, kind="market", context=ctx).to_dict(),
            "model": _ref(model, kind="model", context=ctx).to_dict(),
        }
        if measures is not None:
            payload["measures"] = [_risk_measure(item) for item in measures]
        if factors is not None:
            payload["factors"] = [_jsonable(item) for item in factors]
        for key, value in (("bump_scheme", bump_scheme), ("bump_size", bump_size), ("tolerances", tolerances), ("pricing", pricing)):
            if value is not None:
                payload[key] = _jsonable(value)
        return self._request("POST", "/v1/risk:calculate", payload)

    def calculate_xva(
        self,
        context: QuantContext | Mapping[str, Any],
        portfolio_id: str | ResourceRef,
        *,
        model: ResourceRef | Mapping[str, Any] | str,
        market: ResourceRef | Mapping[str, Any] | str,
        measures: Sequence[str] | None = None,
        confidence_level: float | None = None,
        netting_sets: Sequence[Mapping[str, Any]] | None = None,
        collateral_agreements: Sequence[Mapping[str, Any]] | None = None,
        counterparty_default: Mapping[str, Any] | None = None,
        own_default: Mapping[str, Any] | None = None,
        funding: Mapping[str, Any] | None = None,
        q_exposure: Mapping[str, Any] | None = None,
        p_exposure: Mapping[str, Any] | None = None,
        materialize_exposure: bool | None = None,
        memory_budget_bytes: int | None = None,
        pricing: Mapping[str, Any] | None = None,
        operation_id: str | None = None,
    ) -> dict[str, Any]:
        ctx = _context(context)
        portfolio = portfolio_id.id if isinstance(portfolio_id, ResourceRef) else str(portfolio_id)
        payload: dict[str, Any] = {
            "context": ctx.to_dict(),
            "operation_id": operation_id or _operation_id("xva"),
            "portfolio_id": portfolio,
            "market": _ref(market, kind="market", context=ctx).to_dict(),
            "model": _ref(model, kind="model", context=ctx).to_dict(),
        }
        optional = {
            "measures": [_xva_measure(item) for item in measures] if measures is not None else None,
            "confidence_level": confidence_level,
            "netting_sets": netting_sets,
            "collateral_agreements": collateral_agreements,
            "counterparty_default": counterparty_default,
            "own_default": own_default,
            "funding": funding,
            "q_exposure": q_exposure,
            "p_exposure": p_exposure,
            "materialize_exposure": materialize_exposure,
            "memory_budget_bytes": memory_budget_bytes,
            "pricing": pricing,
        }
        payload.update({key: _jsonable(value) for key, value in optional.items() if value is not None})
        return self._request("POST", "/v1/xva:calculate", payload)

    def health_live(self) -> dict[str, Any]:
        return self._request("GET", "/health/live")

    def health_ready(self) -> dict[str, Any]:
        return self._request("GET", "/health/ready")


def _measure(value: Any) -> Any:
    if isinstance(value, str):
        return {"name": value, "params": {}}
    if isinstance(value, Mapping):
        return _jsonable(value)
    if hasattr(value, "to_spec"):
        spec = value.to_spec()
        if isinstance(spec, (tuple, list)) and len(spec) >= 2:
            return {"name": spec[0], "params": _jsonable(spec[1])}
    return _jsonable(value)


def _risk_measure(value: Any) -> str:
    value = value[0] if isinstance(value, (tuple, list)) and value else value
    if not isinstance(value, str):
        value = getattr(value, "measure_name", str(value))
    aliases = {"PV": "pv", "DV01": "pv01", "DELTA": "delta", "RHO": "rho", "VEGA": "vega", "ALL_GREEKS": "all_greeks"}
    return aliases.get(str(value).upper(), str(value).lower())


def _xva_measure(value: Any) -> str:
    """Normalize convenience names to the lower-case OpenAPI XVA enum."""
    value = getattr(value, "value", value)
    return str(value).lower()


__all__ = [
    "Context", "QuantContext", "ResourceRef", "Market", "MarketSpec", "HullWhite1F", "IRSwap",
    "ScenarioSet", "ProblemDetails", "QuantRestError", "TransportResponse", "UrlLibTransport", "QuantRestClient",
]
