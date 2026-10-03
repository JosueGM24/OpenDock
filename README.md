# OpenDock

*(antes CornerRadius)* Notch con tus notificaciones, barra superior, centro de control, dock
y esquinas redondeadas para Windows 10/11. Si tenías CornerRadius instalada, al abrir
OpenDock la cierra, conserva tus ajustes y ocupa su lugar.

## Esquinas redondeadas
Esquinas redondeadas por software para la pantalla de tu laptop.
Dibuja una máscara negra antialiasada en cada esquina de cada monitor. En pantallas
OLED el negro equivale a píxel apagado; en LCD se ve igual de negro que el bisel.

## Uso
Ejecuta `OpenDock.exe`. La primera vez se abre la **configuración**; pulsa
**Instalar en este equipo** y queda fija: se copia a `%LOCALAPPDATA%\Programs\OpenDock`,
arranca con Windows, aparece en el menú Inicio y se desinstala desde
Configuración → Aplicaciones. No necesita permisos de administrador.

- Clic en el icono de la bandeja → configuración · clic derecho → menú.
- Volver a abrir el .exe (o buscar "OpenDock" en Inicio) también abre la configuración.

| Atajo | Acción |
|---|---|
| Ctrl+Alt+R | Activar / desactivar |
| Ctrl+Alt+RePág / AvPág | Radio +2 / −2 px |

En el panel: rueda del ratón o flechas = radio ±1, Espacio = activar, Esc = cerrar.

CLI: `--radius 16` · `--settings` · `--exit` · `--uninstall`

## Avisos tipo notch
En vez de las notificaciones de Windows, los cambios (atajos, instalación…) se muestran
en una "isla" negra que crece desde el borde superior y se recoge sola.
- Arrástrala: pegada arriba es un notch; suéltala más abajo y queda flotante. Recuerda la posición.
- Clic = abrir configuración. Con el ratón encima no se cierra.
- No aparece con juegos o presentaciones a pantalla completa. Se desactiva en el panel.

**Centro de notificaciones**: clic en el notch → baja un panel con tus notificaciones
reales de Windows (WhatsApp, Teams, correo…): icono, app, hora, título y texto.
Clic en una = abrir la app · × = quitarla de la lista · "Borrar" · engranaje = ajustes.
Se leen en solo lectura de la base local de Windows (`wpndatabase.db`); nada sale del equipo.

**Mini notch**: lleva el cursor al borde superior y aparece una pastilla que te sigue con
rebote y se imanta al centro (y a la posición guardada). Déjalo quieto un instante y se
abre la vista rápida con el contador; clic = centro de notificaciones.

**"No molestar" de Windows**: es lo que hace que Windows no saque sus propios banners y
deje los avisos en su centro, de donde los lee el notch. Windows no permite que una app lo
active (su estado solo se puede leer), así que OpenDock lo muestra en la ficha del centro de
control y, si el espejo está activo y "No molestar" apagado, lo sugiere una vez al día.

**Notificaciones ofuscadas**: en vez del contenido, llega un notch pequeñito con una
campanita que se balancea y un contador; clic = abrir el centro con el detalle.
Para que Windows no muestre además su banner de la esquina, activa su **No molestar**
(las notificaciones siguen llegando al centro de Windows y al notch). OpenDock ya no
toca `ShowBanner` en el registro: Windows no lo relee en vivo. Al arrancar deshace los
cambios que hicieron versiones anteriores.

**Sonido**: al llegar una notificación al notch puede sonar el sonido de Windows o uno de
los trece de Material Design (por defecto, "Eco suave"), con su propio volumen, aunque
tengas No molestar activado.

**Estilo**: "Notch" (pegado al borde superior) o "Flotante" (una tarjeta separada del borde).

**Diseño** (pestaña Notch): material OLED / vidrio translúcido / sistema (claro u oscuro),
tamaño compacto / normal / grande, rebote suave / normal / bouncy, y acento (Windows o 6 colores).

**Capturas en el notch**: al hacer Win+Shift+S / ImpPant aparece "Captura copiada" con
miniatura y tamaño; si además se guarda en *Capturas de pantalla*, clic = abrir el archivo.
Solo reacciona a imágenes sin ventana dueña (Recortes) o de ShareX/Greenshot/Lightshot,
no a "copiar imagen" de un navegador. El DIB del portapapeles se valida antes de leerlo.
Para quitar el aviso propio de Windows: Configuración → Sistema → Notificaciones → Recortes → desactivar.

## Barra superior
Panel → General → "Barra superior" (o Ctrl+Alt+B, o el menú de la bandeja).
Una franja fina arriba, como la de macOS, con el notch en medio: logo (abre ajustes), app
activa, volumen, Wi‑Fi, batería, fecha/hora (abre el centro de notificaciones) y el
**centro de control** (Wi‑Fi, batería, No molestar, notificaciones, brillo y volumen reales).
Se registra como AppBar: Windows le reserva el espacio y las ventanas maximizadas quedan
debajo. Se oculta sola con apps a pantalla completa. Con "Ocultar reloj de Windows" se quita
la hora de la barra de tareas (ajuste oficial, se restaura al quitar la barra o salir).

## Material común
Panel → General → **Material** (OLED · Vidrio · Sistema) se aplica a la vez al notch, la
barra superior y el dock: OLED es negro puro; Vidrio desenfoca lo que hay detrás (cada uno
con su vidrio propio, por eso con Vidrio quedan fuera de capturas); Sistema sigue el tema
claro/oscuro de Windows.

## Dock
Panel → pestaña **Dock** (o Ctrl+Alt+D, o el menú de la bandeja; clic derecho en el dock abre
sus ajustes). Un dock flotante abajo con tus apps **ancladas** a la barra de tareas más las
que tengas **abiertas**: una barrita blanca corta marca las que corren y una larga y brillante
la que está al frente. Al pasar el cursor, el icono bajo él crece (1,5×) y los vecinos un poco,
con rebote; el dock sigue centrado y crece simétrico. Como el Dock de macOS, reserva su
franja: las ventanas maximizadas terminan encima de él en vez de quedar tapadas. Al abrir Inicio o Buscar (tecla Windows) el dock
baja y se desvanece: Windows dibuja ese menú pegado a su propia barra y no se puede mover.
Clic: abre la app con tres saltos que se van apagando, o la enfoca con un salto corto si ya
está abierta (clic de nuevo la minimiza). Los iconos se cargan una vez a alta resolución y se
guardan en caché; todo se lanza vía explorer.exe (sin extensiones de shell en el proceso).

Ajustes: **desenfoque del fondo** (no · suave · intenso; vidrio propio como el del notch, por
eso con desenfoque el dock queda fuera de capturas), **opacidad** (30 · 55 · 75 · 92 %) y
**tamaño de iconos** (pequeño · mediano · grande · enorme).
"Desactivar barra de tareas": la de Windows desaparece mientras el dock está activo
(medido: Explorer pasa de 2,29 % a 0,68 % de un núcleo y usa 33 MB menos, porque deja de
dibujarla) (se pone
en autoocultar y se ocultan sus ventanas) y vuelve tal cual al quitarlo, al salir o al
desinstalar. Se oculta solo a pantalla completa.

## Seguridad
- **Sin red, sin telemetría, sin admin** (`asInvoker`). Todo vive en `HKCU`.
- **Mitigaciones de proceso** al arrancar: DLLs solo desde System32 (anti DLL-hijacking
  desde Descargas), bloqueo de imágenes remotas/baja integridad, sin puntos de extensión
  heredados (AppInit), sin código dinámico, solo fuentes del sistema, solo binarios
  firmados por Microsoft, heap que termina ante corrupción.
- **Sin DLL plantables**: las DLL del sistema que no son KnownDLLs (version, winmm, dwmapi,
  wlanapi, bthprops) no se importan de forma estática: `lazy.c` las carga solo desde System32
  la primera vez que se usan (con MSVC, `/DEPENDENTLOADFLAG:0x800`). Así una DLL dejada junto al
  .exe en Descargas o en %TEMP% no se carga antes de que actúen las mitigaciones.
- **Nada de líneas de comandos a mano**: todo lo que se abre (apps del dock, avisos, rutas, URI)
  pasa por `App_ShellOpen`, que rechaza comillas, caracteres de control y modificadores; los
  protocolos de los avisos excluyen los peligrosos (file, shell, search, its, ms-* de Office…).
- **Entradas ajenas acotadas**: XML de avisos con tope de etiquetas, logos solo de rutas locales
  (nunca \servidor, que filtraría credenciales NTLM), WAV con trozos validados, y ningún
  mensaje de ventana lleva punteros.
- **Binario endurecido**: ASLR de alta entropía, DEP y stack protector. Las versiones
  publicadas se compilan con MSVC en GitHub Actions y llevan además Control Flow Guard, CET
  (shadow stack), CRT estática y DLL dependientes solo de System32; `tools/check_pe.py`
  lo comprueba en cada compilación y la versión no se publica si falta algo.
- **Bandeja propia acotada**: como mucho 8 iconos por proceso, solo de ventanas que existen,
  y al pulsar un icono solo se envían mensajes de aplicación.
- **Desinstalación limpia**: el ayudante corre en una carpeta propia de %TEMP% y la borra al
  terminar.
- Configuración validada al leerla; `--cleanup` nunca borra rutas recibidas por CLI.
- La copia instalada se escribe byte a byte, sin el `Zone.Identifier` de la descarga.

### Quitar el aviso de SmartScreen
Solo se consigue **firmando** el exe con un certificado de confianza pública
(ver `sign.ps1`; lo más barato es Azure Artifact Signing, ~10 USD/mes). Además conviene:
1. Enviar el exe a Microsoft como falso positivo: https://www.microsoft.com/wdsi/filesubmission
2. Publicar el SHA-256 junto a cada descarga (`sign.ps1` genera `OpenDock.exe.sha256`).
3. No empaquetar con UPX ni similares (dispara heurísticas de antivirus).

## Detalles técnicos
- Win32 puro en C, estático (~1,7 MB: incluye SQLite para leer las notificaciones).
- Módulos: `corner_radius.c` (esquinas, bandeja), `panel.c`, `notch.c`, `winnotif.c`, `install.c`, `gfx.c` (SDF).
- 4 ventanas layered por monitor, click-through, topmost, fuera de Alt+Tab; se reutilizan al cambiar el radio (sin parpadeo).
- DPI Per-Monitor V2; excluidas de capturas (`WDA_EXCLUDEFROMCAPTURE`).
- Se reconstruye al cambiar resolución, escala, monitores o al reiniciar explorer.

## Limitaciones conocidas
- Juegos en pantalla completa *exclusiva* pueden taparlo (borderless sí funciona).
- Windows 11 esconde iconos nuevos de la bandeja en `^`; para fijarlo: Configuración →
  Personalización → Barra de tareas → Otros iconos → OpenDock.
- Al desinstalar queda `%TEMP%\OpenDock-uninstall.exe` (el ayudante que borra la carpeta).

## Compilar
- MSYS2 / Linux / WSL: `./build.sh` (mingw-w64).
- Windows: `build.bat` desde Developer Command Prompt (MSVC), con `sqlite3.c` (amalgamación)
  junto al código o `SQLITE_DIR` apuntando a vcpkg (`sqlite3:x64-windows-static`).
- Versiones: `git tag v2.0.0 && git push --tags` → `.github/workflows/release.yml` compila con
  MSVC, comprueba las protecciones y publica el .exe con su SHA-256 en GitHub Releases.
- Comprobar un .exe: `py tools/check_pe.py OpenDock.exe --strict`.

## Código abierto
OpenDock es software libre con licencia [MIT](LICENSE): puedes usarlo, estudiarlo,
modificarlo y redistribuirlo. Está escrito en C contra la API de Win32, sin frameworks ni
dependencias en tiempo de ejecución; cada pieza vive en su archivo (`notch.c`, `menubar.c`,
`dock.c`, `tray.c`, `winnotif.c`, `panel.c`). El logo se genera con `make_icon.py` (Pillow).
Los sonidos de `sounds/` conservan su propia licencia (CC BY 4.0, ver abajo).

## Créditos

Los sonidos de notificación "Nota", "Eco suave", "Alerta", "Destello", "Cascada", "Aviso",
"Ambiente", "Confirmar", "Logro", "Brindis", "Fanfarria", "Fiesta" y "Completado" son de
[Material Design Sound Resources](https://m2.material.io/design/sound/sound-resources.html),
© Google, con licencia [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Se han
convertido a mono, recortado y normalizado (carpeta `sounds/`).
