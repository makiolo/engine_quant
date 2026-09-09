# Procedencia de `XLCALL.H` / `XLCALL.CPP`

Estos dos ficheros son el **Microsoft Excel Developer's Toolkit** (`XLCALL.H` v15.0,
`XLCALL.CPP`): el header y la implementación de referencia que Microsoft distribuye
libremente para que cualquier XLL de terceros pueda hablar con Excel vía su C API
(`Excel12`/`Excel12v`), sin depender de una `.lib` de importación con problemas de
arquitectura (`XLCALL.CPP` resuelve el punto de entrada `MdCallBack12` en tiempo de
ejecución vía `GetProcAddress(GetModuleHandle(NULL), "MdCallBack12")`, no hay enlazado
estático contra Excel).

Vendorizados sin modificar desde el mirror público
[`xlladdins/xll24`](https://github.com/xlladdins/xll24) (misma copia que usan
prácticamente todos los add-ins XLL de código abierto: xlw, xll12/xll22/xll24, etc.),
commit de la rama `master` en el momento de esta integración (PLAN.md, Fase 4, §7.8).

No se vendoriza nada más de `xll24` (su framework de `AddIn`/registro por macros):
`clients/excel` implementa su propio registro explícito (`xlAutoOpen` + `xlfRegister`,
PLAN.md §5.4 "registro explícito centralizado", el mismo principio que ya sigue el
registry C++) en vez de depender de esa capa de terceros.
