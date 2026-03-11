#!/bin/bash

# Configuration
TFTP_DIR="./.tftp"
SERVER_IP="127.0.0.1"
FILES=("client1.bin" "client2.bin" "client3.bin")

echo "--- 1. Nettoyage et Préparation ---"
mkdir -p "$TFTP_DIR"
rm -f client1.bin client2.bin client3.bin

for f in "${FILES[@]}"; do
    dd if=/dev/urandom of="$TFTP_DIR/$f" bs=1M count=20 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "Fichier $TFTP_DIR/$f créé avec succès."
    else
        echo "Erreur lors de la création de $f"
        exit 1
    fi
    md5sum "$TFTP_DIR/$f" > "$f.md5_src"
done

echo "--- 2. Lancement des transferts SIMULTANÉS ---"
# Lancement en arrière-plan
for f in "${FILES[@]}"; do
    ./client $SERVER_IP get $f &
done

echo "Attente de la fin des transferts..."
wait

echo -e "\n--- 3. Vérification de l'étanchéité (MD5) ---"
SUCCESS_COUNT=0
for f in "${FILES[@]}"; do
    if [ -f "$f" ]; then
        SRC_MD5=$(cat "$f.md5_src" | awk '{print $1}')
        REC_MD5=$(md5sum "$f" | awk '{print $1}')
        
        if [ "$SRC_MD5" == "$REC_MD5" ]; then
            echo -e "Client $f : \033[0;32m✅ OK\033[0m"
            ((SUCCESS_COUNT++))
        else
            echo -e "Client $f : \033[0;31m❌ ÉCHEC (MD5 différent)\033[0m"
        fi
    else
        echo -e "Client $f : \033[0;31m❌ ÉCHEC (Fichier non reçu)\033[0m"
    fi
done

if [ $SUCCESS_COUNT -eq 3 ]; then
    echo -e "\n\033[0;32mRÉSULTAT GLOBAL : VÉRITABLE SUCCÈS\033[0m"
else
    echo -e "\n\033[0;31mRÉSULTAT GLOBAL : ÉCHEC\033[0m"
fi