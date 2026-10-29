#!/bin/sh
# pruebas del torneo, correr con: sh pruebas.sh
# cada prueba tiene un limite de tiempo, si el programa se cuelga se avisa

make || exit 1
ok=0
mal=0

revisar() {
    # $1 = nombre de la prueba, $2 = codigo de salida, $3 = archivo con la salida
    # 137 = timeout lo tuvo que matar a la fuerza, o sea se colgo
    if [ "$2" -eq 137 ]; then
        echo "  MAL: $1 se quedo colgado"
        mal=$((mal + 1))
    elif grep -q "fin del torneo" "$3"; then
        echo "  ok: $1"
        ok=$((ok + 1))
    else
        echo "  MAL: $1 no termino bien (ver $3)"
        mal=$((mal + 1))
    fi
}

echo "== 1. torneo normal (6 jugadores, 2 mesas) =="
timeout -s KILL 60 ./torneo 6 2 config.txt > salida_normal.txt
revisar "normal" $? salida_normal.txt
grep "termino la partida" salida_normal.txt

echo "== 2. numero impar de jugadores =="
timeout -s KILL 60 ./torneo 5 2 config.txt > salida_impar.txt
revisar "impar" $? salida_impar.txt
grep "sin rival" salida_impar.txt

echo "== 3. caidas de fichas (prob_falla 20) =="
timeout -s KILL 60 ./torneo 6 2 config_fallas.txt > salida_fallas.txt
revisar "fallas" $? salida_fallas.txt
echo "  caidas simuladas:  $(grep -c 'se cae la ficha' salida_fallas.txt)"
echo "  caidas detectadas: $(grep -c 'se detecto que la ficha' salida_fallas.txt)"
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
grep "cancelada" salida_ctrlc2.txt || echo "  MAL: no se cancelo ninguna partida"
rm -f config_lento.txt

if command -v valgrind > /dev/null; then
    echo "== 6. valgrind (memory leaks) =="
    valgrind --leak-check=full --error-exitcode=1 ./torneo 4 2 config_fallas.txt > /dev/null 2> salida_valgrind.txt
    grep "ERROR SUMMARY\|definitely lost\|All heap blocks" salida_valgrind.txt
else
    echo "(no esta valgrind, me salto esa prueba)"
fi

echo
echo "pruebas ok: $ok   pruebas mal: $mal"
