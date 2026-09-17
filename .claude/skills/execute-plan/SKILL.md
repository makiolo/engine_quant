---
name: execute-plan
description: Ejecuta un documento PLAN_*.md de este repo (engine_quant) fase por fase, delegando cada fase a un subagente y actuando como orquestador que relega contexto/decisiones entre fases. Triggers - "implementa el plan", "ejecuta PLAN_X.md", "implementa las fases de", "sigue con el plan", nombre explicito de un PLAN_*.md de la raiz del repo.
---

# Ejecutar un PLAN_*.md fase por fase con subagentes orquestados

Este repo (`engine_quant`) documenta trabajo de ingenieria grande en documentos `PLAN_*.md` en la
raiz (`PLAN_GREEKS.md`, `PLAN_BACKWARD.md`, `PLAN_IMPROVE_NOTEBOOK.md`,
`PLAN_IMPROVE_NOTEBOOK2.md`, ...), cada uno con fases numeradas, criterios de aceptacion explicitos,
y a veces "forks" de diseno donde el propio documento dice "esto no lo decide el documento, lo
decide quien implemente". La convencion editorial de este repo (ver `PLAN_IMPROVE_NOTEBOOK.md §0`,
`PLAN_GREEKS.md §11`) es: **nada de un plan cuenta como hecho hasta que queda construido y
verificado de extremo a extremo**, y cada fase termina en su propio commit (ver `git log`: "Fase 6
PLAN_BACKWARD.md: ...", "Fase 3 PLAN_IMPROVE_NOTEBOOK.md: ..."). Este skill reproduce ese patron
siempre que se pida ejecutar uno de estos documentos, con el modelo que invoca el skill actuando de
**orquestador**: no implementa el las fases el mismo, coordina subagentes y lleva el hilo de
contexto entre ellos.

## 0. Antes de arrancar: checkpoint del arbol de trabajo

1. `git status` — si ya hay cambios sin commitear que correspondan a trabajo previo YA COMPLETADO
   (de este mismo plan en una sesion anterior, o de otro plan), **no los mezcles** con las fases que
   vas a ejecutar ahora. Si el volumen es significativo y no es obvio que sea seguro apilar mas
   trabajo encima, pregunta al usuario si commitear ese estado primero como checkpoint (una
   decision de alcance/riesgo real, no un detalle tecnico — usa `AskUserQuestion` si no esta ya
   resuelto por instrucciones previas del usuario en la conversacion).
2. Lee el documento del plan COMPLETO antes de tocar nada. Presta atencion especial a:
   - La tabla de "priorizacion sugerida" (ordena la ejecucion por ahi, no por el orden de aparicion
     de las fases en el documento — ver p.ej. `PLAN_IMPROVE_NOTEBOOK2.md §3`).
   - Que fases "bloquean" a otras (una fase de auditoria/cierre final tipicamente depende de que
     TODAS las demas esten terminadas primero — ejecutala literalmente al final).
   - Cualquier fork de diseno explicito ("esto no lo decide el documento") — el subagente de esa
     fase tiene que decidir y DOCUMENTAR la decision con justificacion, nunca preguntar de vuelta.
   - Fricciones/gaps ya conocidos de planes anteriores relacionados (este documento suele nombrar
     el plan del que nace, p.ej. "motivado por 08/09") — dale ese contexto al subagente en vez de
     dejar que lo redescubra leyendo commits.
3. Decide el orden final de ejecucion (normalmente la tabla de priorizacion) y confirma con el
   usuario el ritmo si el volumen/riesgo lo amerita (todas las fases seguidas de forma autonoma, o
   una a la vez con revision entre medias) — otra vez, pregunta solo si no esta ya resuelto.

## 1. Ejecucion: una fase = un subagente = un commit

Ejecuta las fases **secuencialmente en el mismo arbol de trabajo**, nunca en paralelo: las fases de
estos planes casi siempre tocan los mismos archivos centrales del motor (`greeks.cpp`,
`measure.cpp`, `quantdesk/*.py`, los crates de `rust/crates/engine-core`), y subagentes en
paralelo sobre el mismo working tree se pisarian entre si. Si en algun momento una fase concreta es
verificablemente independiente Y el usuario quiere paralelizar, usa `isolation: "worktree"` para esa
fase — no por defecto.

Para cada fase, en orden:

1. **Redacta el briefing de la fase** (el subagente arranca sin memoria de esta conversacion, dale
   todo lo que necesite):
   - Pega el texto integro de la seccion de esa fase del plan (problema, tareas de motor, tareas de
     notebook, criterio de aceptacion) — no lo resumas, el subagente necesita el detalle tecnico
     exacto (nombres de archivo, nombres de funcion, lineas aproximadas que el plan ya cita).
   - Un resumen corto de QUE decidieron las fases anteriores ya ejecutadas en esta misma sesion de
     orquestacion, si son relevantes para esta fase (p.ej. una decision de nombrado de API, una
     limitacion descubierta que otra fase deberia conocer). Esto es la parte de "dar contexto entre
     fases" — mantenla como una lista corta de hechos verificados, no controversial.
   - Instruccion explicita: si el plan deja un fork de diseno abierto, decide tu con criterio
     tecnico, documenta la decision y la razon (mismo patron que ya usa este repo: ver las
     secciones "Estado verificado / decision (sesion de implementacion de esta fase)" ya presentes
     en `PLAN_IMPROVE_NOTEBOOK.md`) — no le devuelvas la pregunta al orquestador salvo bloqueo real
     (ambiguedad que cambia el alcance de forma material, o algo que solo el usuario final puede
     decidir).
   - Instruccion explicita de verificacion en capas, de dentro hacia fuera, sin saltarse ninguna que
     aplique a lo que toca la fase: `cargo test -p engine-core --release` (Rust) →
     `cmake --build build --config Release` + `ctest --test-dir build -C Release` (C++) →
     `pytest clients/python/tests` (Python) → `jupyter nbconvert --to notebook --execute --inplace
     <notebook>.ipynb` (si la fase toca un notebook, confirmando 0 errores). No avances de capa si
     la anterior falla; no es aceptable "deberia compilar" sin ejecutarlo.
   - Instruccion explicita de **nunca aproximar/omitir en silencio** cuando algo no se puede
     resolver limpiamente — este es el criterio de diseno que TODOS estos planes repiten (ver
     `PLAN_GREEKS.md §7.4`, y la friccion 2/9 de `PLAN_IMPROVE_NOTEBOOK2.md`): si un caso no aplica,
     debe fallar explicito o quedar documentado como excluido, nunca devolver un numero silencioso o
     un `NaN` sin explicacion.
   - Instruccion de cierre: (a) actualizar el propio documento del plan anadiendo una seccion
     "**Estado verificado / decisiones tomadas (sesion de implementacion de esta fase)**" bajo la
     fase correspondiente, con el mismo nivel de detalle que ya usan las fases existentes de
     `PLAN_IMPROVE_NOTEBOOK.md` (que se verifico, que se corrigio del enunciado original si algo
     estaba mal, que se implemento capa por capa, resultados de test exactos); (b) actualizar
     `clients/python/notebooks/README.md` si la fase toca un notebook y su descripcion cambia;
     (c) un commit de git (`git add` selectivo, nunca `-A`/`.`, revisando que no se cuele nada
     sensible) con mensaje `Fase <N> <NOMBRE_PLAN>.md: <resumen de una linea>` mas el pie de
     atribucion que ya use el resto del historial de la sesion; (d) devolver un reporte conciso al
     orquestador con: que se implemento, que decision de diseno se tomo y por que, resultados de
     verificacion exactos (conteo de tests), el hash del commit, y cualquier sorpresa/limitacion
     descubierta que las fases siguientes deban conocer.
   - Tipo de agente: usa un agente fresco (no `fork`) para cada fase — el trabajo de motor/build es
     pesado y no necesitas que cargue el historial completo de la conversacion del orquestador,
     solo el briefing que le des. Si la fase es puramente de investigacion/decision sin tocar
     codigo, un `fork` puede ser mas barato.

2. **Lanza el subagente** (`Agent`, `subagent_type` por defecto o `general-purpose`) con ese
   briefing.

3. **Al recibir el reporte de esa fase**: verifica tu mismo los puntos criticos que te importan para
   orquestar bien la siguiente (no re-hagas el trabajo, pero si el reporte afirma algo que va a
   condicionar fases futuras, dale un vistazo — un `git log -1`/`git show --stat` rapido basta para
   confirmar que el commit existe y toca lo que dice tocar). Si el subagente reporta una regresion
   real (no un fallo preexistente ya conocido) o se salio del alcance de la fase, PARA la cadena y
   pidele al usuario que decida antes de seguir — no sigas apilando fases sobre una base rota.

4. **Actualiza tu propio resumen de contexto acumulado** (mentalmente, en el siguiente briefing) con
   los hechos nuevos de esa fase antes de redactar el briefing de la siguiente.

5. Repite con la siguiente fase del orden decidido en el paso 0.3.

## 2. Cierre del plan completo

- La fase final de auditoria/cierre (si el plan tiene una, tipicamente la de mayor numero, que
  "bloquea" en la tabla de priorizacion depende de todas las demas) se ejecuta literalmente al
  final, con un briefing que resume TODAS las decisiones tomadas en las fases anteriores (para que
  el subagente de auditoria sepa exactamente que patron reemplaza a que workaround en cada
  notebook/modulo).
- Cuando todas las fases del documento estan construidas y verificadas: informa al usuario con un
  resumen de cierre (una tabla fase → commit → resultado basta) y pregunta si quiere migrar el
  contenido a `PLAN.md` (la hoja de ruta maestra) — es la convencion editorial del repo
  ("cada fase se migra a PLAN.md solo despues de quedar construida y verificada de extremo a
  extremo"), pero es una decision sobre un documento compartido/central, asi que confirmala en vez
  de hacerla sola.
- No declares el plan "implementado" en tu propio resumen si alguna fase quedo en `skipped`/fuera de
  alcance por decision explicita documentada — repite tal cual esa decision, no la disuelvas en un
  "todo terminado" generico.
