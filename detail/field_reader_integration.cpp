/**
 * @file field_reader_integration.cpp
 * @brief Intégration du lecteur de champs
 */

#include "field_reader.h"

namespace qb::pg::detail {

// Définition du membre statique
ParamUnserializer FieldReader::unserializer;

/**
 * @brief Initialise le lecteur de champs
 * 
 * Cette fonction est appelée lors de l'initialisation du module
 * pour configurer le lecteur de champs.
 */
void initialize_field_reader() {
    // Rien à initialiser pour l'instant
}

} // namespace qb::pg::detail 