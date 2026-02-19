#ifndef TFTP_ERRORS_H
#define TFTP_ERRORS_H

// --- CODES D'ERREUR STANDARDS (RFC 1350) ---
#define ERR_NOT_DEFINED        0  // Erreur non définie (voir message)
#define ERR_FILE_NOT_FOUND     1  // Fichier non trouvé
#define ERR_ACCESS_VIOLATION   2  // Violation d'accès (droits)
#define ERR_DISK_FULL          3  // Disque plein ou dépassement de quota
#define ERR_ILLEGAL_OP         4  // Opération TFTP illégale
#define ERR_UNKNOWN_TID        5  // Transfer Identifier inconnu (mauvais port)
#define ERR_FILE_EXISTS        6  // Le fichier existe déjà
#define ERR_NO_SUCH_USER       7  // Utilisateur inconnu

// --- TABLEAU DE MESSAGES POUR FACILITER LES LOGS ---
// Ce tableau permet au serveur de faire : tftp_error_messages[ERR_FILE_NOT_FOUND]
static const char *tftp_error_messages[] = {
    "Erreur non definie",
    "Fichier introuvable",
    "Acces refuse",
    "Espace disque insuffisant",
    "Operation non valide",
    "Identifiant de transfert (TID) incorrect",
    "Le fichier existe deja",
    "Utilisateur inconnu"
};

#endif // TFTP_ERRORS_H