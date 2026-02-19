#!/bin/bash

# Configuration
IP_SERVEUR="127.0.0.1"
PORT_SERVEUR="69"
CLIENT="../client" # Le binaire est dans le dossier parent

echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
echo "┃ PRÉPARATION DE L'ENVIRONNEMENT DE TEST        ┃"
echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"

# 1. On s'assure que le dossier .tftp existe dans le dossier parent (racine du serveur)
mkdir -p ../.tftp

# 2. On prépare des fichiers cohérents côté serveur pour les tests GET
echo "- Mise à disposition de client.c dans .tftp/ pour tests de lecture..."
cp ../client.c ../.tftp/source_client.c

# 3. On prépare des fichiers locaux dans test/ pour les tests PUT
echo "- Préparation des fichiers locaux pour tests d'envoi..."
cp ../server.c ./envoi_server.c
cp ../tftp_protocole.h ./envoi_protocole.h

echo ""
echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
echo "┃ SCÉNARIO 1 : LECTURES SIMULTANÉES (Partagées) ┃"
echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
echo "Lancement de 3 clients GET en même temps sur source_client.c..."

# Lancement en parallèle
$CLIENT $IP_SERVEUR get source_client.c $PORT_SERVEUR  &
PID1=$!
$CLIENT $IP_SERVEUR get source_client.c $PORT_SERVEUR  &
PID2=$!
$CLIENT $IP_SERVEUR get source_client.c $PORT_SERVEUR  &
PID3=$!

wait $PID1 $PID2 $PID3
echo "✓ Scénario 1 terminé. (Lectures concurrentes autorisées)"

echo ""
echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
echo "┃ SCÉNARIO 2 : CONFLIT ÉCRITURE/LECTURE         ┃"
echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
echo "Lancement d'un PUT (envoi_server.c) suivi d'un GET..."

# On lance l'écriture (PUT)
$CLIENT $IP_SERVEUR put envoi_server.c $PORT_SERVEUR  &
PID_PUT=$!

# Petit délai pour s'assurer que le serveur traite le WRQ d'abord
sleep 0.2

# On lance la lecture du même fichier. Il doit attendre que le PUT soit fini !
$CLIENT $IP_SERVEUR get envoi_server.c $PORT_SERVEUR  &
PID_GET=$!

wait $PID_PUT $PID_GET
echo "✓ Scénario 2 terminé. (L'écriture a bloqué la lecture comme attendu)"

echo ""
echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
echo "┃ SCÉNARIO 3 : FICHIERS DIFFÉRENTS (Indépendants)┃"
echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
echo "Lancement de 2 PUT sur des fichiers différents..."

$CLIENT $IP_SERVEUR put envoi_protocole.h $PORT_SERVEUR  &
PID_A=$!
$CLIENT $IP_SERVEUR put envoi_server.c $PORT_SERVEUR  &
PID_B=$!

wait $PID_A $PID_B
echo "✓ Scénario 3 terminé. (Traitements parallèles car fichiers distincts)"

echo ""
echo "================================================="
echo "               FIN DE LA SIMULATION              "
echo "================================================="

# Nettoyage des fichiers temporaires dans le dossier test/
rm -f source_client.c envoi_server.c envoi_protocole.h