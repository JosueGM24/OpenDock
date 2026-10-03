# OpenDock para GNOME Shell

Extensión de GNOME Shell (GJS, GNOME 45–50) que lleva el aspecto y el comportamiento de
OpenDock al escritorio: esquinas redondeadas, un notch que reemplaza los banners de
notificación, un dock inferior con lupa y ocultación, y una barra superior restyleada.
Ver `linux/PLAN.md` y `linux/DESIGN.md` para la arquitectura y la especificación visual
completas (las mismas constantes que la versión de Windows).

## Instalación

### Desde el .zip (recomendado para probar)

1. Descarga `opendock@josuegm24.github.io.shell-extension.zip` desde los
   [artefactos de CI](https://github.com/JosueGM24/OpenDock/actions) o constrúyelo tú
   mismo:

   ```sh
   bash linux/gnome-extension/build.sh
   ```

2. Instálalo:

   ```sh
   gnome-extensions install --force dist/opendock@josuegm24.github.io.shell-extension.zip
   ```

3. Reinicia GNOME Shell (en Xorg: Alt+F2, `r`, Enter; en Wayland: cierra sesión y vuelve
   a entrar) y actívala:

   ```sh
   gnome-extensions enable opendock@josuegm24.github.io
   ```

### Desde el AUR (Arch Linux / Manjaro)

```sh
yay -S gnome-shell-extension-opendock
gnome-extensions enable opendock@josuegm24.github.io
```

El `PKGBUILD` de este paquete está en `linux/gnome-extension/PKGBUILD`.

## Ajustes

Abre `gnome-extensions prefs opendock@josuegm24.github.io` o desde la app de
Extensiones de GNOME. Se puede elegir el material (OLED, vidrio o seguir al sistema),
el estilo de rebote de los muelles (suave, normal, bouncy), el radio de las esquinas,
el modo de ocultación del dock (nunca, a la mitad, del todo), el tamaño de icono del
dock y el sonido de notificación (los mismos 13 sonidos que la versión de Windows, en
`sounds/`).

## Qué hace cada pieza

- **Esquinas redondeadas**: cuatro máscaras Cairo por monitor, por encima de las
  ventanas, que siguen `monitors-changed` y el radio configurado.
- **Notch**: sustituye los banners nativos de `Main.messageTray` (se restauran al
  desactivar la extensión) y anima la apertura/cierre con el mismo integrador de
  muelles que Windows (`a = (objetivo − x)·k − v·2√k·ζ`). Pulsar el notch abre el
  centro de notificaciones (tarjetas de 348×70, construidas a partir de las
  notificaciones que ya mantiene GNOME).
- **Dock**: favoritos + aplicaciones abiertas (`AppFavorites` / `Shell.AppSystem`),
  lupa gaussiana al pasar el cursor, rebote de 3 saltos al abrir, indicador de apps en
  ejecución y ocultación automática con reserva de espacio de trabajo
  (`Main.layoutManager.addChrome(..., {affectsStruts: true})`).
- **Barra superior**: 28 px de alto, color del material y reloj con el formato
  "Jue 2 oct  14:05". El Centro de Control de GNOME (Quick Settings) se mantiene tal
  cual; solo se restylea.
- **Sonidos**: reproducidos en proceso con GStreamer (sin lanzar binarios externos);
  si el typelib de Gst no está disponible, la extensión sigue funcionando sin sonido.

## Límites conocidos

- GNOME Shell no permite clientes `layer-shell`; todo lo anterior se hace con la API
  pública (y alguna privada, como la señal `notification-added` de
  `MessageTray.Source`) de GNOME Shell. Un cambio grande en el shell entre versiones
  podría requerir ajustes — por eso cada pieza se activa de forma independiente y un
  fallo en una no impide desactivar las demás.
- El seguimiento fino del cursor del mini-notch (imantado a 64 px, vista rápida tras
  450 ms quieto) está simplificado respecto a la especificación: se implementa con
  "enter/leave" sobre una franja central bajo la barra en vez de seguir el cursor
  píxel a píxel.
- Algunas micro-interacciones del centro de notificaciones (la papelera que crece al
  acercarse al borde, el cuerpo completo que aparece a los 260 ms de pasar el cursor)
  están simplificadas para mantener el código robusto en el shell headless usado en
  CI; los tamaños, colores y el comportamiento principal (activar / borrar) sí siguen
  la especificación.
- Las pruebas de integración corren con `gnome-shell --headless --virtual-monitor` en
  contenedores Fedora y Arch (ver `linux/gnome-extension/ci/`); algunas aserciones
  (capturas de pantalla, `org.gnome.Shell.Eval`) dependen de que esas dos piezas estén
  disponibles en el contenedor y se omiten con un aviso si no lo están, en vez de hacer
  fallar toda la prueba.

## Desarrollo

```sh
bash linux/gnome-extension/build.sh        # copia sonidos, compila schemas, empaqueta
bash linux/gnome-extension/ci/run-test.sh  # prueba local (requiere gnome-shell headless)
```
