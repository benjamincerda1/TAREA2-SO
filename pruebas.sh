#!/bin/sh
# pruebas del torneo, correr con: sh pruebas.sh  (o make test)
# cada prueba tiene un limite de tiempo, si el programa se cuelga se avisa

make || exit 1
ok=0
mal=0

bien() {
    echo "  ok: $1"
    ok=$((ok + 1))
}

falla() {
    echo "  MAL: $1"
    mal=$((mal + 1))
}

revisar() {
    # $1 = nombre de la prueba, $2 = codigo de salida, $3 = archivo con la salida
    # 137 = timeout lo tuvo que matar a la fuerza, o sea se colgo
    if [ "$2" -eq 137 ]; then
        falla "$1 se quedo colgado"
    elif grep -q "fin del torneo" "$3"; then
        bien "$1"
    else
        falla "$1 no termino bien (ver $3)"
    fi
}

# revisa que cada tablero impreso en $2 tenga sus 7 filas seguidas, sin lineas de otros hilos al medio
tableros_enteros() {
    cortados=$(awk '/^  \[mesa/ {dentro = 1; filas = 0; next}
                    dentro { if ($0 ~ /^    /) { filas++; if (filas == 7) dentro = 0 }
                             else { print; dentro = 0 } }' "$2" | wc -l)
    if [ "$cortados" -eq 0 ]; then
        bien "$1: ningun tablero cortado"
    else
        falla "$1: $cortados tableros cortados por otras lineas (ver $2)"
    fi
}

# config con retardos cortos para las pruebas que no necesitan ver jugar
sed 's/^retardo_min .*/retardo_min 5/; s/^retardo_max .*/retardo_max 20/; s/^max_movimientos .*/max_movimientos 60/' config.txt > config_rapida.txt

echo "== 1. torneo normal (6 jugadores, 2 mesas) =="
timeout -s KILL 60 ./torneo 6 2 config.txt > salida_normal.txt
revisar "normal" $? salida_normal.txt
grep "termino la partida" salida_normal.txt
empiezan=$(grep -c "empieza la partida" salida_normal.txt)
terminan=$(grep -c "termino la partida" salida_normal.txt)
if [ "$empiezan" -eq 3 ] && [ "$terminan" -eq 3 ]; then
    bien "se jugaron las 3 partidas y todas terminaron"
else
    falla "empezaron $empiezan partidas y terminaron $terminan (se esperaban 3)"
fi
tableros_enteros "normal" salida_normal.txt

echo "== 2. numero impar de jugadores =="
timeout -s KILL 60 ./torneo 5 2 config.txt > salida_impar.txt
revisar "impar" $? salida_impar.txt
if grep -q "sin rival" salida_impar.txt; then
    bien "$(grep 'sin rival' salida_impar.txt)"
else
    falla "nadie quedo sin rival con 5 jugadores"
fi

echo "== 3. caidas de fichas (prob_falla 20, con y sin el mutex) =="
timeout -s KILL 90 ./torneo 10 3 config_fallas.txt > salida_fallas.txt
revisar "fallas" $? salida_fallas.txt
simuladas=$(grep -c 'se cae la ficha' salida_fallas.txt)
detectadas=$(grep -c 'el juez detecto que la ficha' salida_fallas.txt)
echo "  caidas simuladas:  $simuladas ($(grep -c 'se cae la ficha.*con el mutex' salida_fallas.txt) con el mutex, $(grep -c 'se cae la ficha.*sin el mutex' salida_fallas.txt) sin el mutex)"
echo "  caidas detectadas: $detectadas"
if [ "$simuladas" -gt 0 ] && [ "$simuladas" -eq "$detectadas" ]; then
    bien "el juez detecto todas las caidas"
else
    falla "caidas simuladas ($simuladas) y detectadas ($detectadas) no calzan"
fi
grep "por abandono" salida_fallas.txt | grep "termino la partida"

echo "== 4. ctrl+c mientras todavia llegan jugadores =="
# manda ctrl+c al segundo, si 20 s despues sigue vivo lo mata
timeout -s INT -k 20 1 ./torneo 20 2 config.txt > salida_ctrlc1.txt
revisar "ctrl+c llegando" $? salida_ctrlc1.txt
grep -c "rechazado" salida_ctrlc1.txt | sed 's/^/  rechazados: /'

echo "== 5. ctrl+c cuando ya se emparejaron todos y solo quedan partidas =="
# con 4 jugadores todos llegan antes de 2 s. uso fichas lentas para que
# las partidas sigan jugandose cuando llega el ctrl+c a los 4 s
sed 's/^retardo_min .*/retardo_min 300/; s/^retardo_max .*/retardo_max 600/; s/^max_movimientos .*/max_movimientos 2000/' config.txt > config_lento.txt
timeout -s INT -k 20 4 ./torneo 4 2 config_lento.txt > salida_ctrlc2.txt
revisar "ctrl+c al final" $? salida_ctrlc2.txt
if grep -q "cancelada" salida_ctrlc2.txt; then
    grep "cancelada" salida_ctrlc2.txt
else
    falla "no se cancelo ninguna partida"
fi

echo "== 6. nunca hay mas de M mesas jugando a la vez =="
for m in 1 3; do
    timeout -s KILL 120 ./torneo 30 $m config_rapida.txt > salida_mesas$m.txt
    revisar "30 jugadores, M=$m" $? salida_mesas$m.txt
    maximo=$(sed -n 's/^maximo de mesas jugando a la vez: \([0-9]*\).*/\1/p' salida_mesas$m.txt)
    if [ -n "$maximo" ] && [ "$maximo" -le $m ]; then
        bien "M=$m: como maximo hubo $maximo mesas a la vez"
    else
        falla "M=$m: hubo ${maximo:-?} mesas a la vez"
    fi
done

echo "== 7. tableros enteros en consola con muchas mesas a la vez =="
sed 's/^retardo_min .*/retardo_min 0/; s/^retardo_max .*/retardo_max 1/; s/^max_movimientos .*/max_movimientos 30/' config.txt > config_estres.txt
timeout -s KILL 120 ./torneo 120 40 config_estres.txt > salida_estres.txt
revisar "estres" $? salida_estres.txt
tableros_enteros "estres" salida_estres.txt

echo "== 8. argumentos y configs malas (tiene que salir con error, sin colgarse) =="
printf 'perro 9 9\n' > config_mala.txt
printf 'perro 5 0\nperro 5 1\n' > config_pocos.txt
for args in "" "0 2" "4 0" "abc 2" "5x 2" "4 2 no_existe.txt" "4 2 config_mala.txt" "4 2 config_pocos.txt"; do
    timeout -s KILL 10 ./torneo $args > /dev/null 2>&1
    codigo=$?
    if [ "$codigo" -ne 0 ] && [ "$codigo" -ne 137 ]; then
        bien "./torneo $args -> error"
    else
        falla "./torneo $args -> codigo $codigo"
    fi
done
timeout -s KILL 10 ./torneo 1 1 config_rapida.txt > salida_uno.txt
revisar "1 solo jugador" $? salida_uno.txt

if command -v valgrind > /dev/null; then
    echo "== 9. valgrind (memory leaks), normal y con ctrl+c =="
    valgrind --leak-check=full --error-exitcode=1 ./torneo 4 2 config_fallas.txt > /dev/null 2> salida_valgrind.txt
    if [ $? -eq 0 ] && grep -q "All heap blocks were freed" salida_valgrind.txt; then
        bien "valgrind con caidas: sin errores ni leaks"
    else
        falla "valgrind con caidas (ver salida_valgrind.txt)"
    fi
    # --foreground: sin eso timeout + valgrind no le entrega bien el ctrl+c al programa
    timeout --foreground -s INT -k 30 4 valgrind --leak-check=full --error-exitcode=1 ./torneo 20 2 config.txt > salida_valgrind_ctrlc_out.txt 2> salida_valgrind_ctrlc.txt
    if grep -q "fin del torneo" salida_valgrind_ctrlc_out.txt && grep -q "All heap blocks were freed" salida_valgrind_ctrlc.txt \
       && grep -q "ERROR SUMMARY: 0 errors" salida_valgrind_ctrlc.txt; then
        bien "valgrind con ctrl+c: sin errores ni leaks"
    else
        falla "valgrind con ctrl+c (ver salida_valgrind_ctrlc.txt)"
    fi
else
    echo "(no esta valgrind, me salto esa prueba)"
fi

rm -f config_rapida.txt config_lento.txt config_estres.txt config_mala.txt config_pocos.txt

echo
echo "pruebas ok: $ok   pruebas mal: $mal"
[ "$mal" -eq 0 ]
