# OpenDock — especificación visual (la misma que Windows)

Valores en px lógicos (a 96 DPI / escala 1). Sacados del código de Windows (`notch.c`, `menubar.c`, `dock.c`, `pop.c`).

## Muelles
`a = (objetivo − x)·k − v·2√k·ζ`, masa 1, Euler semiimplícito con 2 subpasos de dt/2, dt ≤ 0,05 s.
Rebote: suave ζ 0,85 · normal ζ 0,62 · bouncy ζ 0,40.

## Materiales
| | OLED | Vidrio | Sistema oscuro | Sistema claro |
|---|---|---|---|---|
| fondo | #000000 | #1C1C1E al 52 % sobre el fondo desenfocado (16 px, saturación 1,5) | #1C1C1E | #F2F2F7 |
| texto / secundario / terciario | #FFF / #AEAEB2 / #6E6E73 | igual | igual | #000 / #6C6C70 / #A1A1A6 |
| tarjeta / tarjeta activa | #1C1C1E / #2C2C2E | #2C2C2E / #3A3A3C | igual que vidrio | #FFF / #E5E5EA |
Acentos: #0A84FF #BF5AF2 #FF375F #FF9F0A #30D158 #98989D (o el del sistema).
Fuente: la del sistema (Cantarell / Noto Sans / Inter); en Windows, Segoe UI Variable.

## Barra superior
Alto 28. Izquierda: icono de la app activa 13 px en x 16, nombre a +8, 13 px negrita.
Derecha (de izquierda a derecha): chevrón bandeja 16 sonido 16 Wi‑Fi 16 batería 18 hora 18 engranaje 16|borde.
Iconos de 14 px de alto óptico. Hora: "Jue 2 oct  14:05" (13 negrita).
Pasar el cursor: escala 1,16; pulsar 0,88 (k 520).
Las esquinas redondeadas de arriba empiezan bajo la barra y llevan su color/vidrio.

## Notch (pegado bajo la barra)
Oculto cuando no hay nada. Al aparecer baja desde el borde como una persiana (ancho inicial 0,45·W, alto 0).
Abrir: ancho k 380 ζ; alto k 420 min(1, ζ+0,1). Cerrar: k 300 ζ 1. El contenido aparece cuando h > 0,55·H.
Esquinas de abajo radio 16 (centro: 26); hombros cóncavos arriba de radio min(7, h/2).
- Aviso: 360×54. Icono 18 px en caja de 30 en x 12. Título 14 semibold en x 52, y 8. Cuerpo 12 en y 28, secundario. Hora a la derecha (16), terciario. Se queda 4,5 s.
- Discreto (No molestar): 112×34, campana 14 px + contador en píldora de acento.
- Mini notch: píldora 110×9 al tocar el borde de arriba (o la barra) en la zona central; sigue al cursor (k 240) y se imanta al centro a < 64 px. Quieto 450 ms → vista rápida 272×44 ("Notificaciones" + contador).
- Centro: 384 de ancho; cabecera 56 ("Notificaciones" 17 semibold, botones Borrar / silenciar / ajustes de 32); tarjetas 348×70 radio 16, separación 12, máximo 6 visibles y scroll de 60 px por paso (k 260).
  Pasar por una tarjeta: 1,035 y las demás 0,965 (k 420); a los 260 ms crece y muestra el cuerpo entero y los botones de acción (píldoras de 26).
  Papelera roja (#E5443C, 40 → 58 px) al acercarse al borde derecho; borrar: desliza y se desvanece en 0,26 s.

## Centro de control (pegado bajo la barra, a la derecha)
340×396, radio 24 abajo, hombros 8. Crece desde una píldora de 150 (ancho k 220, alto k 250).
Relleno 14, separación 10, dos columnas de 151. Wi‑Fi + Bluetooth (151×126), No molestar y Notificaciones (151×58),
Batería (312×60), Pantalla y Sonido (deslizadores de 26 px de alto), "Ocultar barra" y "Ajustes ›".
Cambio a Ajustes: los controles se alejan (escala 0,93, −12 % de ancho, se desvanecen) y la sección nueva entra desde +34 % (k 210 ζ 0,84).

## Dock
Iconos de 38 (30 / 38 / 46 / 56), separación 12, relleno 16 × 11, margen inferior 12, panel de 60 de alto, radio 0,30·alto.
Fondo del panel al 75 % (#000 OLED, #1C1C1E, #F2F2F7 claro), desenfoque suave 11 px.
Lupa: `1 + 0,5·exp(−((i − f)/1,55)²)` con k 520 ζ 0,66; crece hacia arriba desde la línea base (panel − 11).
Indicador: 3 px de alto a 5,5 px del borde; al frente 16 de ancho (0,95), abierta 6 (0,55).
Abrir una app: 3 saltos de 0,4 s (0,75·icono, ×0,62 cada uno); enfocar: 1 salto de 0,38·icono.
Ocultar: a la mitad (12 + 30) o del todo; baja a los 450 ms de salir el cursor (k 140 ζ 0,95), sube con k 260.
Reserva media altura del panel (las ventanas maximizadas terminan a 42 px del borde).
