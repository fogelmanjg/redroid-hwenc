# redroid-hwenc

**Idiomas:** [English](README.md) | Español | [中文](README.zh-CN.md)

> El idioma oficial de este proyecto es el **inglés**. Este archivo se ofrece por comodidad y
> puede no estar perfectamente actualizado o traducido — ante cualquier duda, [README.md](README.md)
> (inglés) es la fuente de verdad.

Encoder de video por hardware (H.264/H.265, vía VA-API) para [redroid](https://github.com/remote-android/redroid-doc).

## El problema

BlueStacks, Nox y MuMu no tienen equivalente real en Linux desktop. [redroid](https://github.com/remote-android/redroid-doc)
es lo más cercano — Android corriendo en un contenedor Docker, con GPU passthrough para el
renderizado — pero el streaming de esa pantalla (scrcpy, o cualquier consumo de video dentro de
una app Android) sigue cayendo en un **encoder de software puro**
(`OMX.google.h264.encoder` / `c2.android.avc.encoder`). `gpuMode=host` acelera el renderizado
(OpenGL/Vulkan vía Mesa), nunca el encoder.

Este pedido lleva **abierto desde 2022** en el repo oficial, sin que nadie lo haya resuelto y
publicado:

- [remote-android/redroid-doc#126](https://github.com/remote-android/redroid-doc/issues/126) — AMD VA-API (OMX/Codec2)
- [remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172) — mismo pedido, AMD
- [remote-android/redroid-doc#168](https://github.com/remote-android/redroid-doc/issues/168) — mismo pedido, Intel
- [remote-android/redroid-doc#535](https://github.com/remote-android/redroid-doc/issues/535) — pedido de H.265, comentario de septiembre 2025 sin respuesta

El maintainer del proyecto (`zhouziyang`) siempre contesta lo mismo: los drivers VA-API ya están
empaquetados en redroid, pero el componente Codec2/OMX que los use para encodear queda como
tarea para la comunidad. En 4 años, nadie lo terminó y publicó.

## Por qué ahora

No es que sea imposible — es que la intersección de "le importa este problema específico" y
"está dispuesto a meterse en AOSP/Codec2/VA-API" es rara. La mayoría de la gente en esos issues
son usuarios pidiendo el feature, no gente dispuesta a escribirlo. Este repo es el intento de
resolverlo en público, en tiers de dificultad creciente, documentando el proceso a medida que
avanza (ver [DEVLOG.md](DEVLOG.md), en inglés).

## Roadmap (por dificultad, no por tiempo)

Cada tier asume que el anterior está resuelto. El ⭐ marca el checkpoint de mayor apalancamiento.

- [x] **Tier 0 — Investigación pura.** Cómo redroid/scrcpy eligen el encoder hoy (¿por
      capacidad vía `MediaCodecList`, o nombre hardcodeado?). Confirmar entrypoints de encode
      VA-API disponibles (`VAEntrypointEncSlice`) en el host real.
- [x] **Tier 1 — Lectura/mapeo.** Estructura de un componente Codec2 (tomando
      [`android_external_v4l2_codec2`](https://gitcode.com/pi-plus/android_external_v4l2_codec2)
      como referencia de plomería, no de lógica de hardware — eso es V4L2, acá va VA-API).
      Superficie de API de **encode** VA-API (no decode).
- [x] **Tier 2 — Prototipo nativo aislado.** Programa standalone (host, sin Android) que
      encodea H.264 vía VA-API sobre `/dev/dri/renderD*`.
- [ ] **Tier 3 — ⭐ El spike que decide todo.** ¿Un buffer gralloc exporta un `dma-buf` fd que
      VA-API pueda importar zero-copy, dentro de un contenedor con los mismos privilegios que
      redroid? Nadie lo confirmó en 4 años de issues. redroid corre como contenedor (no VM) — sin
      virtio-gpu de por medio, mejor punto de partida del que la comunidad asume.
- [ ] **Tier 4 — Esqueleto Codec2 en Android.** Componente que Android reconoce y lista
      (`dumpsys media.c2`) como `c2.hardware.encoder.h264`, sin encoding real todavía.
- [ ] **Tier 5 — Integración real.** Tier 3 + Tier 2 conectados dentro de los callbacks del
      componente del Tier 4. El objetivo final.
- [ ] **Tier 6 (condicional a Tier 0).** Si el selector de codec de redroid/scrcpy resulta estar
      hardcodeado en vez de por capacidad: parchearlo.

Cada tier, aunque no se llegue más lejos, ya es una contribución publicable — nada de esto quedó
documentado por nadie hasta ahora.

## Estado actual

Arrancando. Ver [DEVLOG.md](DEVLOG.md) (en inglés) para el progreso real, sesión a sesión.

## Contribuir

Sin CLA, sin fricción — Apache-2.0 llano. Si te interesa este problema, un PR o un comentario en
un issue vale más que pedir permiso primero.

## Licencia

Apache License 2.0 — ver [LICENSE](LICENSE). Misma licencia que AOSP (`frameworks/av`), a
propósito: si algo de esto eventualmente vale la pena subirlo río arriba, no hay fricción legal.
