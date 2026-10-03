#!/usr/bin/env python3
"""
Comprueba capturas de pantalla (grim) tomadas en la prueba funcional de
sway sin cabeza. Cada "--check" es una aserción independiente; si falla
imprime por qué y termina con código distinto de 0.

Uso:
  check_screenshot.py captura.png --width 1280 --height 800 --radius 16 \
      --bg 255 255 255 --check corners
  check_screenshot.py captura.png --check bar --bar-height 28
  check_screenshot.py captura.png --check notch
"""
import argparse
import sys
from PIL import Image


def casi(c1, c2, tolerancia=24):
    return all(abs(a - b) <= tolerancia for a, b in zip(c1, c2))


def es_negro(px, tolerancia=24):
    return casi(px[:3], (0, 0, 0), tolerancia)


def check_corners(img, w, h, radius, bg, top=0):
    """Cada esquina: el píxel extremo (pegado al borde físico) debe ser
    oscuro (la máscara: negra, o del color de la barra arriba); un píxel
    cerca del borde interior del cuarto de círculo debe mostrar el fondo
    (la zona transparente deja verlo). 'top' es el alto de la barra: las
    esquinas de arriba empiezan debajo de ella."""
    extremos = [
        (1, top + 1, "superior izquierda"),
        (w - 2, top + 1, "superior derecha"),
        (1, h - 2, "inferior izquierda"),
        (w - 2, h - 2, "inferior derecha"),
    ]
    interiores = [
        (radius - 3, top + radius - 3, "superior izquierda"),
        (w - radius + 2, top + radius - 3, "superior derecha"),
        (radius - 3, h - radius + 2, "inferior izquierda"),
        (w - radius + 2, h - radius + 2, "inferior derecha"),
    ]
    ok = True
    for x, y, nombre in extremos:
        px = img.getpixel((x, y))
        if not es_negro(px, 40):
            print(f"FALLO: esquina {nombre}: píxel extremo ({x},{y}) = {px}, esperaba oscuro")
            ok = False
        else:
            print(f"ok: esquina {nombre}: píxel extremo ({x},{y}) negro {px}")
    for x, y, nombre in interiores:
        px = img.getpixel((x, y))
        if not casi(px[:3], bg):
            print(f"FALLO: esquina {nombre}: píxel interior ({x},{y}) = {px}, esperaba fondo {bg}")
            ok = False
        else:
            print(f"ok: esquina {nombre}: píxel interior ({x},{y}) = fondo {px}")
    return ok


def check_bar(img, w, bar_height):
    """La banda superior (0..bar_height) debe ser de la barra en toda su anchura."""
    muestra_izq = img.getpixel((4, bar_height // 2))
    muestra_centro = img.getpixel((w // 2, bar_height // 2))
    muestra_der = img.getpixel((w - 4, bar_height // 2))
    # Del color de la barra (#1C1C1E, DESIGN.md "Sistema oscuro") en toda su
    # anchura; negro puro sería el fondo de un X sin pintar, no la barra.
    color_barra = (0x1C, 0x1C, 0x1E)
    ok = all(casi(m[:3], color_barra, 12) for m in (muestra_izq, muestra_centro, muestra_der))
    print(f"{'ok' if ok else 'FALLO'}: banda superior del color de la barra: {muestra_izq}, {muestra_centro}, {muestra_der}")
    # justo debajo de la barra no debería tener el mismo color exacto que dentro,
    # salvo que el fondo del escritorio coincida (poco probable con blanco puro)
    return ok


def check_notch(img, w):
    """Tras `notify-send`, debe verse una región oscura y redondeada
    centrada bajo la barra (el aviso)."""
    cx = w // 2
    # Buscamos en una franja vertical bajo la barra algún píxel oscuro
    # distinto del fondo (blanco) cerca del centro horizontal.
    encontrado = False
    for y in range(28, 120):
        px = img.getpixel((cx, y))
        if es_negro(px, 60) or (px[0] < 80 and px[1] < 80 and px[2] < 80):
            encontrado = True
            print(f"ok: notch detectado en ({cx},{y}) = {px}")
            break
    if not encontrado:
        print("FALLO: no se detectó el notch (región oscura bajo la barra)")
    return encontrado


PANEL = (0x1C, 0x1C, 0x1E)   # fondo del centro / de la barra (DESIGN.md)
TARJETA = (0x2C, 0x2C, 0x2E)  # tarjeta del centro


def muestra(img, x, y, esperado, nombre, tol=10):
    px = img.getpixel((x, y))
    ok = casi(px[:3], esperado, tol)
    print(f"{'ok' if ok else 'FALLO'}: {nombre} en ({x},{y}) = {px}, esperaba {esperado}")
    return ok


def check_center(img, w, bar_height, con_tarjeta):
    """Centro de notificaciones abierto: panel de 384 de ancho centrado bajo
    la barra; la primera tarjeta empieza tras la cabecera de 56."""
    cx = w // 2
    ok = muestra(img, cx - 192 + 8, bar_height + 28, PANEL, "panel del centro (cabecera)")
    y_tarjeta = bar_height + 56 + 35
    if con_tarjeta:
        ok &= muestra(img, cx - 60, y_tarjeta, TARJETA, "primera tarjeta")
    else:
        px = img.getpixel((cx, y_tarjeta))
        vacio = not casi(px[:3], TARJETA, 6)
        print(f"{'ok' if vacio else 'FALLO'}: sin tarjetas en ({cx},{y_tarjeta}) = {px}")
        ok &= vacio
    return ok


def check_closed(img, w, bar_height, bg):
    """Nada bajo la barra en el centro (centro y notch cerrados)."""
    return muestra(img, w // 2, bar_height + 120, bg, "fondo bajo la barra (centro cerrado)")


def check_mini(img, w, bar_height):
    """Pastilla del mini notch (110×9) justo bajo la barra, en el centro."""
    return muestra(img, w // 2, bar_height + 3, PANEL, "pastilla del mini notch")


def check_quick(img, w, bar_height):
    """Vista rápida (272×44) bajo la barra: su fondo a la izquierda del texto."""
    return muestra(img, w // 2 - 130, bar_height + 30, PANEL, "vista rápida")


def check_dock(img, w, h):
    """El panel del dock debe verse como una franja oscura cerca del
    borde inferior de la pantalla."""
    cx = w // 2
    y = h - 15
    px = img.getpixel((cx, y))
    ok = px[0] < 100 and px[1] < 100 and px[2] < 100
    print(f"{'ok' if ok else 'FALLO'}: dock en ({cx},{y}) = {px}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imagen")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=800)
    ap.add_argument("--radius", type=int, default=16)
    ap.add_argument("--bar-height", type=int, default=28)
    ap.add_argument("--top-offset", type=int, default=0,
                    help="alto de la barra sobre las esquinas de arriba (corners)")
    ap.add_argument("--bg", type=int, nargs=3, default=[255, 255, 255])
    ap.add_argument("--check", required=True, choices=["corners", "bar", "notch", "dock", "center", "center-empty",
                             "closed", "mini", "quick"])
    args = ap.parse_args()

    img = Image.open(args.imagen).convert("RGB")

    if args.check == "corners":
        ok = check_corners(img, args.width, args.height, args.radius, tuple(args.bg),
                           args.top_offset)
    elif args.check == "bar":
        ok = check_bar(img, args.width, args.bar_height)
    elif args.check == "notch":
        ok = check_notch(img, args.width)
    elif args.check == "dock":
        ok = check_dock(img, args.width, args.height)
    elif args.check in ("center", "center-empty"):
        ok = check_center(img, args.width, args.bar_height, args.check == "center")
    elif args.check == "closed":
        ok = check_closed(img, args.width, args.bar_height, tuple(args.bg))
    elif args.check == "mini":
        ok = check_mini(img, args.width, args.bar_height)
    elif args.check == "quick":
        ok = check_quick(img, args.width, args.bar_height)
    else:
        ok = False

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
