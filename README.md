# QB PostgreSQL Module (qbm-pgsql)

Le module PostgreSQL pour le framework QB C++ Actor fournit une implémentation asynchrone et performante pour les bases de données PostgreSQL. Il offre une API complète pour les opérations SQL intégrée de manière transparente avec l'architecture événementielle du framework QB Actor.

## Fonctionnalités

- **API Asynchrone**: Opérations non-bloquantes intégrées avec la boucle d'événements de QB
- **Transactions**: Support ACID complet, transactions imbriquées (savepoints), isolation configurable
- **Requêtes Préparées**: Mise en cache et réutilisation des requêtes préparées
- **Gestion des Connexions**: Pooling, reconnexion automatique, timeout configurable
- **Sécurité**: Support de SSL/TLS, authentifications multiples (MD5, SCRAM-SHA-256)
- **Conception Moderne**: Interface fluide chainable, typage fort, gestion RAII des ressources

## Prérequis

- Framework QB C++ Actor
- Compilateur compatible C++17
- CMake 3.14+
- OpenSSL (pour connexions sécurisées et authentification SCRAM)

## Intégration

```cmake
# Dans votre CMakeLists.txt
qb_use_module(pgsql)
```

## Utilisation de Base

### Connexion

```cpp
#include <pgsql/pgsql.h>

// Création d'une connexion
qb::pg::tcp::database db("tcp://user:password@localhost:5432[mydb]");

// Connexion à la base
if (!db.connect()) {
    std::cerr << "Échec de connexion: " << db.error().message << std::endl;
    return 1;
}

// Lancement de la boucle d'événements
qb::io::async::run();
```

### Requête Simple

```cpp
// Exécution d'une requête simple
db.execute("SELECT * FROM users",
    [](auto& tr, qb::pg::results results) {
        for (const auto& row : results) {
            std::cout << "ID: " << row["id"].as<int>() << std::endl;
            std::cout << "Nom: " << row["name"].as<std::string>() << std::endl;
        }
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Erreur: " << err.message << std::endl;
    }
);
```

### Requête Préparée

```cpp
// Préparation d'une requête (le typage est automatique)
db.prepare("insert_user", "INSERT INTO users (username, email) VALUES ($1, $2) RETURNING id")
  .execute("insert_user", {"john_doe", "john@example.com"},
    [](auto& tr, qb::pg::results results) {
        std::cout << "ID utilisateur inséré: " << results[0][0].as<int>() << std::endl;
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Erreur d'insertion: " << err.message << std::endl;
    }
);
```

### Utilisation de params

```cpp
// Création de paramètres explicites
qb::pg::params params;
params.add("john_doe");
params.add("john@example.com");

db.execute("insert_user", std::move(params),
    [](auto& tr, qb::pg::results results) {
        // Traitement des résultats
    }
);

// Alternative plus concise
db.execute("SELECT * FROM users WHERE id > $1", {100},
    [](auto& tr, qb::pg::results results) {
        // Traitement des résultats
    }
);
```

## Gestion des Transactions

### Transaction Simple

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        // Les requêtes sont exécutées dans une transaction
        tr.execute("INSERT INTO users (username) VALUES ('user1')")
          .execute("INSERT INTO users (username) VALUES ('user2')");
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Erreur de transaction: " << err.message << std::endl;
    }
);
```

### Transaction avec Savepoint

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO users (username) VALUES ('user1')");
        
        // Création d'un savepoint
        tr.savepoint("user_2",
            [](qb::pg::transaction& tr) {
                // Cette partie peut être annulée indépendamment
                tr.execute("INSERT INTO users (username) VALUES ('user2')");
                
                // Force un rollback du savepoint
                throw std::runtime_error("Annulation intentionnelle");
            }
        );
        
        // Cette partie sera toujours exécutée même si le savepoint échoue
        tr.execute("INSERT INTO users (username) VALUES ('user3')");
    }
);
```

### Chaînage d'Opérations

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO logs (message) VALUES ('Début')")
          .then([](qb::pg::transaction& tr) {
              // Exécuté seulement si l'opération précédente réussit
              return tr.execute("UPDATE stats SET count = count + 1");
          })
          .success([](qb::pg::transaction& tr) {
              // Exécuté si toutes les opérations précédentes réussissent
              std::cout << "Transaction réussie" << std::endl;
          })
          .error([](const qb::pg::error::db_error& err) {
              // Exécuté si une opération échoue
              std::cerr << "Échec: " << err.message << std::endl;
          });
    }
);
```

## API Synchrone avec Await

```cpp
// Exécution d'une requête et attente du résultat
auto status = db.execute("SELECT * FROM users WHERE id = $1", {1}).await();

if (status) {
    auto results = status.results();
    if (results.rows_count() > 0) {
        std::cout << "Utilisateur: " << results[0]["username"].as<std::string>() << std::endl;
    }
} else {
    std::cerr << "Erreur: " << status.error().message << std::endl;
}
```

## Traitement des Résultats

```cpp
db.execute("SELECT * FROM users",
    [](auto& tr, qb::pg::results results) {
        // Informations sur les colonnes
        for (const auto& col : results.columns()) {
            std::cout << "Colonne: " << col.name << std::endl;
        }
        
        // Accès par index
        for (size_t i = 0; i < results.rows_count(); ++i) {
            std::cout << "Ligne " << i << ": " << results[i]["username"].as<std::string>() << std::endl;
        }
        
        // Itération sur les lignes
        for (const auto& row : results) {
            std::cout << "Utilisateur: " << row["username"].as<std::string>() << std::endl;
            std::cout << "Email: " << row["email"].as<std::string>() << std::endl;
        }
    }
);
```

## Types de Données Supportés

- Types numériques: int16, int32, int64, float, double
- Texte: text, varchar, char
- Binaire: bytea
- JSON et JSONB
- Date et temps: timestamp, date, time
- Booléen
- Types tableau
- Types personnalisés

```cpp
// Exemple avec différents types
db.execute("SELECT * FROM data",
    [](auto& tr, qb::pg::results results) {
        for (const auto& row : results) {
            // Types simples
            int id = row["id"].as<int>();
            std::string name = row["name"].as<std::string>();
            bool active = row["active"].as<bool>();
            
            // Date/Heure
            auto created_at = row["created_at"].as<std::chrono::system_clock::time_point>();
            
            // Tableaux
            auto numbers = row["numbers"].as<std::vector<int>>();
            
            // JSON
            auto data = row["data"].as<qb::json>();
        }
    }
);
```

## Optimisations et Performances

- Mise en cache et réutilisation des requêtes préparées
- Optimisation des conversions de types
- Gestion efficace de la mémoire
- Paramètres binaires pour transfert optimisé

## Sécurité

- Sanitisation automatique des paramètres contre les injections SQL
- Support SSL/TLS pour les communications chiffrées
- Gestion détaillée des erreurs
- Méthodes d'authentification sécurisées

## Exemple d'Utilisation Avancée

```cpp
// Requête avec CTE et agrégation
db.execute(R"(
    WITH user_stats AS (
        SELECT user_id, COUNT(*) as order_count, SUM(amount) as total_spent
        FROM orders
        GROUP BY user_id
    )
    SELECT u.name, u.email, s.order_count, s.total_spent
    FROM users u
    JOIN user_stats s ON u.id = s.user_id
    WHERE s.total_spent > $1
    ORDER BY s.total_spent DESC
)", {1000.0},
    [](auto& tr, qb::pg::results results) {
        for (const auto& row : results) {
            std::cout << "Client: " << row["name"].as<std::string>() << std::endl;
            std::cout << "Commandes: " << row["order_count"].as<int>() << std::endl;
            std::cout << "Total: " << row["total_spent"].as<double>() << " €" << std::endl;
        }
    }
);
```

## Licence

Le module PostgreSQL QB est sous licence Apache 2.0.