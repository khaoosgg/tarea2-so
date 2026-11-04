#!/bin/bash
# test_config.sh — Casos borde del lector de config.txt y de los argumentos.
# Uso: make test-config   (o: bash tests/test_config.sh)
# Cada caso indica el codigo de salida esperado: 1 = rechazado, 0 = aceptado.
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fallas=0
caso() {   # caso <esperado> <descripcion> <contenido>
    printf "$3" > "$TMP/c.txt"
    ./torneo 2 1 "$TMP/c.txt" > /dev/null 2> "$TMP/err.txt"
    local r=$?
    if [ "$r" -eq "$1" ]; then printf "  OK    %-40s (exit %d)\n" "$2" "$r"
    else printf "  FALLA %-40s (exit %d, esperado %d)\n" "$2" "$r" "$1"; fallas=$((fallas+1)); fi
}
echo "[config] Debe rechazar (exit 1):"
caso 1 "valor vacio con espacios"       'delay_min_ms = \n'
caso 1 "valor vacio sin espacios"       'delay_min_ms=\n'
caso 1 "linea sin '='"                  'seed\n'
caso 1 "linea sin clave ('= 5')"        '= 5\n'
caso 1 "texto en vez de numero"         'seed = abc\n'
caso 1 "numero con basura ('300ms')"    'delay_max_ms = 300ms\n'
caso 1 "duracion negativa"              'delay_min_ms = -5\n'
caso 1 "numero desbordado"              'seed = 99999999999999999999\n'
caso 1 "retardo min > max"              'delay_min_ms = 400\ndelay_max_ms = 100\n'
caso 1 "llegada min > max"              'arrival_min_ms = 50\narrival_max_ms = 10\n'
caso 1 "puma fuera del tablero"         'puma_start = 28\n'
caso 1 "solo 11 perros"                 'dog_start = 16,17,18,19,20,21,22,23,24,25,26\n'
caso 1 "13 perros"                      'dog_start = 15,16,17,18,19,20,21,22,23,24,25,26,27\n'
caso 1 "coma doble en dog_start"        'dog_start = 16,,17,18,19,20,21,22,23,24,25,26,27\n'
caso 1 "coma final en dog_start"        'dog_start = 16,17,18,19,20,21,22,23,24,25,26,27,\n'
caso 1 "perro repetido"                 'dog_start = 16,16,18,19,20,21,22,23,24,25,26,27\n'
caso 1 "perro sobre el puma"            'puma_start = 16\n'
caso 1 "fail_role invalido"             'fail_role = gato\n'
caso 1 "max_moves = 0"                  'max_moves = 0\n'
echo "[config] Debe aceptar (exit 0):"
caso 0 "clave desconocida (solo aviso)" 'color = azul\ndelay_min_ms=0\ndelay_max_ms=2\n'
caso 0 "fin de linea de Windows (CRLF)" 'seed = 7\r\ndelay_min_ms = 0\r\ndelay_max_ms = 2\r\n'
caso 0 "comentarios, espacios, vacias"  '  # hola\n\n  seed   =   9   # fin\ndelay_min_ms=0\ndelay_max_ms=2\n'
caso 0 "log_file vacio = solo consola"  'log_file =\ndelay_min_ms=0\ndelay_max_ms=2\n'

echo "[args] Debe rechazar (exit 1):"
arg() {   # arg <descripcion> <argumentos...>
    local d=$1; shift
    ./torneo "$@" > /dev/null 2>&1
    local r=$?
    if [ "$r" -eq 1 ]; then printf "  OK    %-40s (exit 1)\n" "$d"
    else printf "  FALLA %-40s (exit %d)\n" "$d" "$r"; fallas=$((fallas+1)); fi
}
arg "sin argumentos"
arg "falta M"                 4
arg "N < 2"                   1 1
arg "M = 0"                   4 0
arg "M negativo"              4 -2
arg "N no numerico ('4x')"    4x 2
arg "config inexistente"      4 2 noexiste.txt
arg "argumentos de sobra"     4 2 a b

[ "$fallas" -eq 0 ] && echo "RESULTADO: todo OK" || echo "RESULTADO: $fallas FALLAS"
[ "$fallas" -eq 0 ]
