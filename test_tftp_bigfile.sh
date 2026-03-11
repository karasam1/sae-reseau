#!/bin/bash

# Configuration
SERVER_IP="127.0.0.1"
PORT=69
FILE_NAME="test_big_file.bin"
FILE_SIZE_MB=200
CLIENT_EXE="./client"
SERVER_DIR="./.tftp"

# Couleurs pour la sortie
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # Sans Couleur

echo "-------------------------------------------------------"
echo "  TFTP BIGFILE TEST SUITE"
echo "-------------------------------------------------------"

# 1. Preparation: Creation d'un fichier volumineux sur le serveur
echo -e "[1/4] Génération du fichier de ${FILE_SIZE_MB}MB..."
mkdir -p "$SERVER_DIR"

# Génération d'un fichier de 200 Mo rempli de données aléatoires
dd if=/dev/urandom of="$SERVER_DIR/$FILE_NAME" bs=1M count=$FILE_SIZE_MB status=none

# 2. Obtenez le hachage MD5 original
ORIGINAL_MD5=$(md5sum "$SERVER_DIR/$FILE_NAME" | awk '{ print $1 }')
echo "      MD5 Original: $ORIGINAL_MD5"

# 3. Exécution du transfert
echo -e "[2/4] Lancement du transfert TFTP (GET)..."
$CLIENT_EXE $SERVER_IP get $FILE_NAME $PORT

# 4. Vérifier si le fichier est arrivé.
if [ ! -f "$FILE_NAME" ]; then
    echo -e "${RED}[ERREUR] Le fichier n'a pas été téléchargé.${NC}"
    exit 1
fi

# 5. Comparer l'intégrité
echo -e "[3/4] Vérification de l'intégrité (MD5)..."
DOWNLOADED_MD5=$(md5sum "$FILE_NAME" | awk '{ print $1 }')
echo "      MD5 Reçu:     $DOWNLOADED_MD5"

if [ "$ORIGINAL_MD5" == "$DOWNLOADED_MD5" ]; then
    echo -e "${GREEN}[SUCCÈS] L'intégrité du fichier est parfaite !${NC}"
else
    echo -e "${RED}[ÉCHEC] Les fichiers sont différents (Corruption).${NC}"
    exit 1
fi

# 6. Nettoyage
echo -e "[4/4] Nettoyage des fichiers de test..."
rm "$SERVER_DIR/$FILE_NAME"
rm "$FILE_NAME"

echo "-------------------------------------------------------"
echo -e "${GREEN}TEST TERMINÉ AVEC SUCCÈS ✓${NC}"
echo "-------------------------------------------------------"