# Devlog

Registro sesión a sesión — qué se probó, qué anduvo, qué no, y por qué. Ver el roadmap por
tiers en el [README](README.md).

## 2026-09-19 — Arranque

Repo creado. El roadmap de 6 tiers viene de investigar el estado real del problema: los issues
oficiales de redroid-doc (#126, #172, #168, #535) llevan entre 1 y 4 años abiertos, todos con la
misma respuesta del maintainer ("los drivers VA-API ya están, el componente Codec2 lo tenés que
escribir vos") y ningún resultado publicado.

Punto de partida importante que cambia el cálculo de dificultad respecto a lo que la comunidad
parece asumir: redroid corre como contenedor Docker privilegiado, no como VM. Eso significa que
no hay virtio-gpu de por medio en el límite del contenedor — el kernel/DRM es literalmente el
del host. El spike del Tier 3 (¿un buffer gralloc exporta un dma-buf que VA-API importe
zero-copy?) parte de mejor posición que si redroid corriera virtualizado.

Próximo paso: Tier 0 — confirmar cómo redroid/scrcpy eligen el encoder hoy, y qué entrypoints de
encode VA-API soporta el hardware real que se va a usar.
