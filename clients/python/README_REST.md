# `QuantRestClient`

`quantdesk.rest` is a small, stateless client for the implemented REST v1 contract in
[`docs/api/openapi.v1.yaml`](../../docs/api/openapi.v1.yaml). The complete `QuantContext`
is kept by the notebook (not by a server session) and is returned by each context mutation.

```python
from quantdesk.rest import Context, HullWhite1F, IRSwap, Market, QuantRestClient

client = QuantRestClient("http://127.0.0.1:8080")
ctx = Context.new(
    "ctx-notebook",
    engine={"execution": {"device": "auto", "precision": "fp64"}},
    pricing={"n_paths": 5_000, "n_steps": 208, "seed": 7},
)
ctx, market = client.add_market(ctx, "eur", Market(pillars=[1, 2], zero_rates=[.02, .021]))
ctx, model = client.add_model(ctx, "hw", HullWhite1F(a=.1, b=.03, sigma=.01, r0=.02))
ctx, swap = client.add_product(
    ctx, "swap", IRSwap(1_000_000, .025, payment_times=[1, 2], accruals=[1, 1])
)

pricing = client.price(ctx, swap, model=model, market=market, measures=["PV"])
```

The wire requests follow the real implementation: the three `add_*` methods batch a
typed `add_*` command through `POST /v1/context:apply`; `price` uses `POST /v1/prices`;
portfolio, scenarios, risk, and XVA use `/v1/portfolios:price`, `/v1/scenarios:run`,
`/v1/risk:calculate`, and `/v1/xva:calculate`. `price_batch` is the convenience alias
for `price_portfolio` and therefore receives an existing `portfolio_id`—it does not
invent the draft endpoint described in older plan text.

HTTP errors with `application/problem+json` are raised as `QuantRestError` and expose a
typed `ProblemDetails` instance through `.problem`. Tests can inject a transport with
`request(method, path, payload)`; no `requests` or `httpx` dependency is required.
