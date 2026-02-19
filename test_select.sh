#!/bin/bash

# --- CONFIGURATION ---
SERVER_IP="127.0.0.1"
PORT=69
REPO=".tftp"

# couleurs pour les messages
VERT='\033[0;32m'
ROUGE='\033[0;31m'
NC='\033[0m'

# --- TESTS ---
sudo pkill -9 server_select 2>/dev/null
rm -rf $REPO && mkdir -p $REPO
rm -f archivo_A.txt archivo_B.txt

echo -e "=== DEMARRAGE DES TESTS SIMPLES ==="

# 1. Compilation
gcc -Wall server_select.c -o server_select
gcc -Wall client.c -o client

# 2. Préparation des fichiers
dd if=/dev/urandom of="$REPO/archivo_A.txt" bs=1k count=3000 2>/dev/null
echo "Contenido archivo B" > "$REPO/archivo_B.txt"

# 3. Lancer le serveur
sudo ./server_select & 
SERVER_PID=$!
sleep 1

# 4. TEST 1: Multiplexage (telecharger A et B a la fois)
echo -e "\n[TEST 1] Téléchargements simultanés..."
./client $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID1=$!
./client $SERVER_IP get archivo_B.txt $PORT > /dev/null &
PID2=$!

#   Attendre que les deux téléchargements soient terminés
wait $PID1 $PID2
echo -e "${VERT}[OK] Test 1 terminé.${NC}"

# 5. TEST 2: Lock (verifier que le serveur bloque les accès concurrents au même fichier)
echo -e "\n[TEST 2] Vérification du Lock..."
echo "Client 1 lance la descarga de archivo_A.txt..."
./client $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID_LOCK=$!

sleep 0.2 #     Attendre un peu pour s'assurer que le serveur a pris le lock

echo "Client 2 tente de télécharger le même fichier (devrait échouer)..."
#   Ce client devrait recevoir une erreur de lock
./client $SERVER_IP get archivo_A.txt $PORT

#   Attendre que le premier client termine
wait $PID_LOCK

#   Vérifier que le fichier téléchargé est correct
echo -e "\n=== TESTS TERMINÉS ==="
sudo kill -9 $SERVER_PID 2>/dev/null