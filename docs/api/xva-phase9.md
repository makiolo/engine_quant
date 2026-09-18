# Fase 9 — XVA

`POST /v1/xva:calculate` (alias `/v1/xva`) accepts a client-owned `XvaRequest` and returns
EE/PFE/CVA/DVA/FVA/MVA/KVA plus artifact references and provenance. The operation is stateless;
request cancellation uses the same bounded CPU scheduler as pricing and risk.

The exposure graph is shared: Q drives EE, PFE, CVA and funding measures, while DVA uses the
explicit P cube tagged P. A missing P cube is accepted only as a documented approximation and is
reported in `provenance.approximations`. Q/P grids and context hashes must agree.

The baseline formulas are deliberately explainable: piecewise-constant hazard increments, clipped
recoveries, discounted expected positive/negative exposure, thresholded variation margin, funding
spread integration, initial-margin funding and a simple capital-factor KVA proxy. They are not a
regulatory capital implementation. Reduction is row/chunk bounded; an exposure cube is represented
by `ExposureCubeRef` (hash, shape, bytes, lineage) and is materialized only when requested.

Arrow output is pending. The `benches/xva_phase9.py` harness reports the estimated materialized and
streaming `f64` bytes and marks Arrow explicitly rather than claiming an Arrow buffer.
