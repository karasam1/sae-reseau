#!/bin/bash

# Configuration
TFTP_ROOT=".tftp"
TEST_FILE="stress_test_final.bin"
SERVER_IP="127.0.0.1"

# Nettoyage
rm -f $TEST_FILE
mkdir -p $TFTP_ROOT

echo "--- 1. Préparation du fichier (20 Mo) ---"
dd if=/dev/urandom of=$TFTP_ROOT/$TEST_FILE bs=1M count=20 2>/dev/null
SOURCE_MD5=$(md5sum $TFTP_ROOT/$TEST_FILE | awk '{print $1}')
echo "MD5 Source : $SOURCE_MD5"

echo "--- 2. Lancement de client ---"
# On lance le client
./client $SERVER_IP get $TEST_FILE &
CLIENT_PID=$!

sleep 0.2

if ps -p $CLIENT_PID > /dev/null; then
    echo "--- 3. PERTURBATION : Gel du client (SIGSTOP) ---"
    kill -STOP $CLIENT_PID # Simulation de congestion dans le réseau
    sleep 6
    echo "--- 4. RÉTABLISSEMENT : Reprise du client (SIGCONT) ---"
    kill -CONT $CLIENT_PID # Reveiller le client
else
    echo "Erreur : Le transfert est déjà fini (trop rapide sur localhost)."
    exit 1
fi

echo "Attente de la fin du transfert..."
wait $CLIENT_PID

echo "--- 5. Vérification finale ---"
if [ -f "$TEST_FILE" ]; then
    FINAL_MD5=$(md5sum $TEST_FILE | awk '{print $1}')
    echo "Source : $SOURCE_MD5"
    echo "Reçu   : $FINAL_MD5"
    if [ "$SOURCE_MD5" == "$FINAL_MD5" ]; then
        echo -e "\033[0;32m✅ RÉSULTAT : SUCCÈS ! (La résilience est prouvée)\033[0m"
    else
        echo -e "\033[0;31m❌ RÉSULTAT : ÉCHEC (Fichier corrompu)\033[0m"
    fi
else
    echo -e "\033[0;31m❌ RÉSULTAT : ÉCHEC (Fichier non reçu)\033[0m"
fi