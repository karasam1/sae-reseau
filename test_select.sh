#!/bin/bash

# Configuration
SERVER_IP="127.0.0.1"
PORT=69
REPO=".tftp"
CLIENT_BIN="./client"
SERVER_BIN="./server_select"
CLIENT_SRC="client.c"
SERVER_SRC="server_select.c"

# Couleurs pour la lisibilité
VERT='\033[0;32m'
ROUGE='\033[0;31m'
JAUNE='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}==========================================================${NC}"
echo -e "${CYAN}    COMPILATION ET PROTOCOLE DE TEST : SERVER_SELECT     ${NC}"
echo -e "${CYAN}==========================================================${NC}"

# 1. Compilation automatique
echo -e "\n${JAUNE}[1/5] Vérification et Compilation des sources...${NC}"

# Fonction de compilation pour éviter la répétition
compile_file() {
    local src=$1
    local bin=$2
    if [ -f "$src" ]; then
        echo -n "Compilation de $src... "
        gcc -Wall "$src" -o "$bin"
        if [ $? -eq 0 ]; then
            echo -e "${VERT}SUCCÈS${NC}"
        else
            echo -e "${ROUGE}ERREUR DE COMPILATION${NC}"
            exit 1
        fi
    else
        echo -e "${ROUGE}ERREUR : Fichier source $src introuvable.${NC}"
        exit 1
    fi
}

compile_file "$SERVER_SRC" "$SERVER_BIN"
compile_file "$CLIENT_SRC" "$CLIENT_BIN"

# 2. Préparation et Génération des ressources
echo -e "\n${JAUNE}[2/5] Préparation de l'environnement .tftp/...${NC}"

if [ ! -d "$REPO" ]; then
    mkdir -p "$REPO"
fi

# Nettoyage des anciennes traces
rm -f archivo_A.txt archivo_B.txt

# Génération des fichiers de test
echo "CONTENU_UNIQUE_A_$(date +%s)" > "$REPO/archivo_A.txt"
echo "CONTENU_UNIQUE_B_$(date +%s)" > "$REPO/archivo_B.txt"
echo -e "${VERT}[OK] Environnement prêt.${NC}"

# 3. Test de Parallélisme (Multiplexage)
echo -e "\n${JAUNE}[3/5] Test : Téléchargements simultanés...${NC}"
echo -e "Note : Assurez-vous que le serveur est déjà lancé dans un autre terminal."

$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID1=$!
$CLIENT_BIN $SERVER_IP get archivo_B.txt $PORT > /dev/null &
PID2=$!

wait $PID1 $PID2
echo -e "${VERT}[OK] Les requêtes parallèles ont été traitées.${NC}"

# 4. Test d'Exclusion Mutuelle (Lock)
echo -e "\n${JAUNE}[4/5] Test : Verrouillage (Accès concurrent sur archivo_A.txt)${NC}"

$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID_LOCK=$!

sleep 0.2 # Pause pour laisser le temps au serveur de verrouiller

echo -e "Tentative du Client 2 (devrait échouer)..."
ERREUR_MSG=$($CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT 2>&1)

if [[ $ERREUR_MSG == *"busy"* ]] || [[ $ERREUR_MSG == *"Error"* ]] || [[ $ERREUR_MSG == *"lock"* ]]; then
    echo -e "${VERT}[SUCCÈS] Le serveur a bien refusé l'accès concurrent.${NC}"
else
    echo -e "${ROUGE}[ALERTE] Le serveur n'a pas appliqué le verrouillage.${NC}"
fi

wait $PID_LOCK

# 5. Vérification finale et nettoyage
echo -e "\n${JAUNE}[5/5] Vérification de l'intégrité...${NC}"
TEST_FINAL=true

for f in "archivo_A.txt" "archivo_B.txt"; do
    if [ -f "$f" ] && diff "$REPO/$f" "$f" > /dev/null; then
        echo -e "${VERT}[OK] $f : Reçu sans corruption.${NC}"
    else
        echo -e "${ROUGE}[ÉCHEC] $f : Erreur de réception.${NC}"
        TEST_FINAL=false
    fi
done

echo -e "\n${CYAN}==========================================================${NC}"
if [ "$TEST_FINAL" = true ]; then
    echo -e "${VERT}RÉSULTAT DU TEST : RÉUSSI (SUCCESS)${NC}"
    # Nettoyage automatique des fichiers téléchargés
    rm archivo_A.txt archivo_B.txt
    echo -e "${NC}Fichiers de test nettoyés.${NC}"
else
    echo -e "${ROUGE}RÉSULTAT DU TEST : ÉCHOUÉ (FAILED)${NC}"
fi
echo -e "${CYAN}==========================================================${NC}"